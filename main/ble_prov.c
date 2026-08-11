/* BLE Wi-Fi provisioning. See ble_prov.h for the shape and the two rules.
 *
 * Protocol, briefly. The phone drives; the cube only ever answers.
 *
 *   1. Phone reads PUBKEY  -> [ver=1][65-byte P-256 point]
 *   2. Phone writes its own 65-byte point to SESSION
 *   3. Both derive Z by ECDH, then
 *        key = HKDF-SHA256(ikm = Z, salt = the 6 digits on screen, info = "facet-prov-v1")
 *      The cube cannot tell yet whether the phone used the right digits.
 *   4. Phone writes a sealed HELLO to CMD. If it opens, the digits matched and
 *      the session is authenticated; if not, it was the wrong code.
 *   5. Everything after that is sealed both ways.
 *
 * The 6 digits are not carrying secrecy — the ECDH does that, and a passive
 * listener learns nothing. They authenticate: they stop a phone that never saw
 * the screen from completing the exchange. An active man-in-the-middle who
 * relays both halves could brute-force 10^6 offline; killing that needs SPAKE2
 * and is not worth it here. Written down rather than overclaimed.
 *
 * Long reads and long writes are left to the ATT layer — Chrome and NimBLE both
 * fragment transparently — so only STATE is notified, and it is 4 bytes. That
 * removed an entire hand-rolled chunking protocol that would otherwise have had
 * to survive a 23-byte MTU.
 */

#include "ble_prov.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "mbedtls/ecp.h"
#include "mbedtls/ecdh.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/gcm.h"
#include "mbedtls/md.h"

static const char *TAG = "ble_prov";

/* ---- timing ------------------------------------------------------------ */

#define ADV_TIMEOUT_MS     180000   /* nobody connected                      */
#define AUTH_TIMEOUT_MS     60000   /* connected but never proved the code   */
#define IDLE_TIMEOUT_MS    180000   /* authenticated but gone quiet          */
#define DONE_LINGER_MS      15000   /* let the phone see the good news       */
#define MAX_AUTH_ATTEMPTS       5

/* ---- wire format ------------------------------------------------------- */

#define PROTO_VER        1
#define PUBKEY_LEN      65          /* uncompressed P-256 point              */
#define KEY_LEN         32
#define NONCE_LEN       12
#define TAG_LEN         16
#define HELLO_MAGIC     "FACETPRV"  /* 8 bytes, not NUL-terminated on the wire */

/* phone -> cube */
#define CMD_HELLO       0x01
#define CMD_SCAN        0x02
#define CMD_JOIN        0x03
#define CMD_WIFI_OFF    0x04
#define CMD_BYE         0x05

/* cube -> phone, in DATA */
#define RSP_APLIST      0x81

/* STATE byte 1 */
#define ERR_NONE        0
#define ERR_BAD_CODE    1
#define ERR_LOCKED_OUT  2
#define ERR_JOIN_FAILED 3
#define ERR_INTERNAL    4

/* DATA is read as one attribute; ATT caps an attribute at 512 bytes, and the
 * seal costs NONCE_LEN + TAG_LEN on top of the plaintext. */
#define DATA_CAP        512
#define DATA_PT_CAP     (DATA_CAP - NONCE_LEN - TAG_LEN)
#define CMD_CAP         256

/* ---- UUIDs -------------------------------------------------------------
 * f9a3000N-0b45-4f7e-9c2a-6d1e8b3c7a51. BLE_UUID128_INIT takes bytes
 * little-endian, i.e. the text form reversed, which puts the varying byte at
 * index 12. The web page hard-codes the text form; keep the two in step. */
#define FACET_UUID(n) BLE_UUID128_INIT(                                     \
    0x51, 0x7a, 0x3c, 0x8b, 0x1e, 0x6d, 0x2a, 0x9c,                         \
    0x7e, 0x4f, 0x45, 0x0b, (n), 0x00, 0xa3, 0xf9)

static const ble_uuid128_t s_uuid_svc     = FACET_UUID(0x01);
static const ble_uuid128_t s_uuid_pubkey  = FACET_UUID(0x02);
static const ble_uuid128_t s_uuid_session = FACET_UUID(0x03);
static const ble_uuid128_t s_uuid_cmd     = FACET_UUID(0x04);
static const ble_uuid128_t s_uuid_data    = FACET_UUID(0x05);
static const ble_uuid128_t s_uuid_state   = FACET_UUID(0x06);

/* ---- session -----------------------------------------------------------
 * Everything that only exists while a session does lives in one PSRAM block.
 * Internal SRAM is the binding constraint on this board and none of this needs
 * to be DMA-capable — the GATT layer copies through mbufs. */
typedef struct {
    uint8_t  our_pub[PUBKEY_LEN];
    uint8_t  key[KEY_LEN];
    char     code[7];               /* six ASCII digits + NUL                */

    uint8_t  data[DATA_CAP];        /* sealed, ready for a GATT read         */
    uint16_t data_len;

    uint8_t  cmd[CMD_CAP];          /* sealed, straight off the host task    */
    uint16_t cmd_len;

    uint64_t tx_ctr;
    uint64_t rx_ctr;                /* highest accepted; replays are dropped */

    mbedtls_ecp_group grp;
    mbedtls_mpi       d;            /* our private scalar                    */

    int      auth_attempts;
    bool     have_key;
} session_t;

static session_t *s_sess;

static volatile bool  s_running;    /* stack is up, or coming up/going down  */
static volatile bool  s_cmd_pending;
static ble_prov_state_t s_state = BLE_PROV_OFF;
static uint8_t  s_err;
static uint8_t  s_data_seq;         /* bumped so the phone knows to re-read  */

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_state_handle;
static uint8_t  s_own_addr_type;

static int64_t  s_t_state;          /* when the current state was entered    */
static bool     s_scan_wanted;      /* a SCAN arrived; waiting on main.c     */

static void adv_start(void);

/* ---- small helpers ----------------------------------------------------- */

static void set_state(ble_prov_state_t st, int64_t now)
{
    if (s_state == st) return;
    s_state  = st;
    s_t_state = now;
    ESP_LOGI(TAG, "state -> %d", (int)st);
}

/* STATE is deliberately plaintext: the phone needs to see "wrong code" and
 * "joining" before a key exists, and none of it is secret. */
static void state_notify(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    uint8_t v[4] = { (uint8_t)s_state, s_err, s_data_seq, 0 };
    struct os_mbuf *om = ble_hs_mbuf_from_flat(v, sizeof(v));
    if (om) ble_gatts_notify_custom(s_conn, s_state_handle, om);
}

static int f_rng(void *ctx, unsigned char *out, size_t len)
{
    (void)ctx;
    esp_fill_random(out, len);
    return 0;
}

/* ---- sealing -----------------------------------------------------------
 * [nonce 12][ciphertext][tag 16]. The nonce is carried rather than recomputed
 * so a dropped message desynchronises nothing; the counter inside it is still
 * checked, so replays are refused. Direction is in byte 0 so the two sides can
 * never collide on a nonce under the same key. */

static void nonce_make(uint8_t *n, uint8_t dir, uint64_t ctr)
{
    memset(n, 0, NONCE_LEN);
    n[0] = dir;
    for (int i = 0; i < 8; i++) n[NONCE_LEN - 1 - i] = (uint8_t)(ctr >> (8 * i));
}

static bool seal(const uint8_t *pt, size_t pt_len,
                 uint8_t *out, size_t out_cap, size_t *out_len)
{
    if (!s_sess || !s_sess->have_key) return false;
    if (pt_len + NONCE_LEN + TAG_LEN > out_cap) return false;

    uint8_t nonce[NONCE_LEN];
    nonce_make(nonce, 0x00, ++s_sess->tx_ctr);

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES,
                                 s_sess->key, KEY_LEN * 8) == 0;
    if (ok) {
        ok = mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, pt_len,
                                       nonce, NONCE_LEN, NULL, 0,
                                       pt, out + NONCE_LEN,
                                       TAG_LEN, out + NONCE_LEN + pt_len) == 0;
    }
    mbedtls_gcm_free(&gcm);
    if (!ok) return false;

    memcpy(out, nonce, NONCE_LEN);
    *out_len = pt_len + NONCE_LEN + TAG_LEN;
    return true;
}

static bool unseal(const uint8_t *in, size_t in_len,
                   uint8_t *pt, size_t pt_cap, size_t *pt_len)
{
    if (!s_sess || !s_sess->have_key) return false;
    if (in_len < NONCE_LEN + TAG_LEN) return false;

    size_t ct_len = in_len - NONCE_LEN - TAG_LEN;
    if (ct_len > pt_cap) return false;

    /* Direction byte must say "from the phone", and the counter must advance.
     * Both checks are cheap and both close off replay. */
    if (in[0] != 0x01) return false;
    uint64_t ctr = 0;
    for (int i = 0; i < 8; i++) ctr = (ctr << 8) | in[NONCE_LEN - 8 + i];
    if (ctr <= s_sess->rx_ctr) return false;

    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    bool ok = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES,
                                 s_sess->key, KEY_LEN * 8) == 0;
    if (ok) {
        ok = mbedtls_gcm_auth_decrypt(&gcm, ct_len, in, NONCE_LEN, NULL, 0,
                                      in + NONCE_LEN + ct_len, TAG_LEN,
                                      in + NONCE_LEN, pt) == 0;
    }
    mbedtls_gcm_free(&gcm);
    if (!ok) return false;

    s_sess->rx_ctr = ctr;
    *pt_len = ct_len;
    return true;
}

/* ---- key agreement ----------------------------------------------------- */

static bool derive_key(const uint8_t *peer_pub)
{
    mbedtls_ecp_point Qp;
    mbedtls_mpi z;
    mbedtls_ecp_point_init(&Qp);
    mbedtls_mpi_init(&z);

    bool ok = mbedtls_ecp_point_read_binary(&s_sess->grp, &Qp,
                                            peer_pub, PUBKEY_LEN) == 0;
    /* Refuse a point that is not actually on the curve. Skipping this is the
     * classic invalid-curve attack and it costs one call. */
    if (ok) ok = mbedtls_ecp_check_pubkey(&s_sess->grp, &Qp) == 0;
    if (ok) ok = mbedtls_ecdh_compute_shared(&s_sess->grp, &z, &Qp,
                                             &s_sess->d, f_rng, NULL) == 0;

    uint8_t secret[32];
    if (ok) ok = mbedtls_mpi_write_binary(&z, secret, sizeof(secret)) == 0;

    if (ok) {
        const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
        static const char info[] = "facet-prov-v1";
        ok = md && mbedtls_hkdf(md,
                                (const uint8_t *)s_sess->code, strlen(s_sess->code),
                                secret, sizeof(secret),
                                (const uint8_t *)info, sizeof(info) - 1,
                                s_sess->key, KEY_LEN) == 0;
    }

    memset(secret, 0, sizeof(secret));
    mbedtls_mpi_free(&z);
    mbedtls_ecp_point_free(&Qp);

    if (ok) {
        s_sess->have_key = true;
        s_sess->tx_ctr = 0;
        s_sess->rx_ctr = 0;
    } else {
        ESP_LOGW(TAG, "key agreement failed");
    }
    return ok;
}

/* ---- payloads ---------------------------------------------------------- */

/* [RSP_APLIST][cur_len][cur_ssid][count][ len, ssid, rssi, flags ] * count
 * Stops early rather than overflowing: a scan list is advisory, and a phone
 * that sees 11 of 14 networks is still useful. */
static void build_aplist(void)
{
    ble_prov_ap_t aps[16];
    int n = ble_prov_get_aps(aps, (int)(sizeof(aps) / sizeof(aps[0])));

    uint8_t pt[DATA_PT_CAP];
    size_t  o = 0;

    pt[o++] = RSP_APLIST;

    const char *cur = ble_prov_current_ssid();
    size_t cur_len = cur ? strnlen(cur, 32) : 0;
    pt[o++] = (uint8_t)cur_len;
    memcpy(pt + o, cur, cur_len);
    o += cur_len;

    size_t count_at = o++;          /* backfilled once we know what fitted   */
    uint8_t written = 0;

    for (int i = 0; i < n; i++) {
        size_t sl = strnlen(aps[i].ssid, 32);
        if (o + 1 + sl + 2 > sizeof(pt)) break;
        pt[o++] = (uint8_t)sl;
        memcpy(pt + o, aps[i].ssid, sl);
        o += sl;
        pt[o++] = (uint8_t)aps[i].rssi;
        pt[o++] = aps[i].secure ? 1 : 0;
        written++;
    }
    pt[count_at] = written;

    size_t sealed = 0;
    if (seal(pt, o, s_sess->data, DATA_CAP, &sealed)) {
        s_sess->data_len = (uint16_t)sealed;
        s_data_seq++;
        ESP_LOGI(TAG, "aplist: %u of %d networks, %u bytes sealed",
                 written, n, (unsigned)sealed);
    } else {
        s_sess->data_len = 0;
        ESP_LOGW(TAG, "aplist seal failed");
    }
    memset(pt, 0, sizeof(pt));
}

/* ---- GATT -------------------------------------------------------------- */

static int gatt_access(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)attr; (void)arg;
    const ble_uuid_t *u = ctxt->chr->uuid;

    if (ble_uuid_cmp(u, &s_uuid_pubkey.u) == 0) {
        if (!s_sess) return BLE_ATT_ERR_UNLIKELY;
        uint8_t v[1 + PUBKEY_LEN];
        v[0] = PROTO_VER;
        memcpy(v + 1, s_sess->our_pub, PUBKEY_LEN);
        return os_mbuf_append(ctxt->om, v, sizeof(v)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ble_uuid_cmp(u, &s_uuid_state.u) == 0) {
        uint8_t v[4] = { (uint8_t)s_state, s_err, s_data_seq, 0 };
        return os_mbuf_append(ctxt->om, v, sizeof(v)) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    if (ble_uuid_cmp(u, &s_uuid_data.u) == 0) {
        if (!s_sess || !s_sess->data_len) return BLE_ATT_ERR_UNLIKELY;
        return os_mbuf_append(ctxt->om, s_sess->data, s_sess->data_len) == 0
                   ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    /* Both writes only copy and flag. Everything expensive — ECDH, AES,
     * anything that could block — happens in ble_prov_poll() on the main task,
     * because this runs on the NimBLE host task with a 3.5 KB stack. */
    if (ble_uuid_cmp(u, &s_uuid_session.u) == 0) {
        if (!s_sess) return BLE_ATT_ERR_UNLIKELY;
        uint16_t len = 0;
        uint8_t peer[PUBKEY_LEN];
        if (ble_hs_mbuf_to_flat(ctxt->om, peer, sizeof(peer), &len) != 0 ||
            len != PUBKEY_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        memcpy(s_sess->cmd, peer, PUBKEY_LEN);
        s_sess->cmd_len = PUBKEY_LEN;
        s_cmd_pending = true;            /* poll() tells them apart by state */
        return 0;
    }

    if (ble_uuid_cmp(u, &s_uuid_cmd.u) == 0) {
        if (!s_sess) return BLE_ATT_ERR_UNLIKELY;
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, s_sess->cmd, CMD_CAP, &len) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        s_sess->cmd_len = len;
        s_cmd_pending = true;
        return 0;
    }

    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_uuid_svc.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = &s_uuid_pubkey.u,  .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &s_uuid_session.u, .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &s_uuid_cmd.u,     .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_WRITE },
            { .uuid = &s_uuid_data.u,    .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = &s_uuid_state.u,   .access_cb = gatt_access,
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
              .val_handle = &s_state_handle },
            { 0 },
        },
    },
    { 0 },
};

/* ---- GAP --------------------------------------------------------------- */

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    (void)arg;
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "phone connected");
        } else {
            adv_start();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "phone disconnected (reason 0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        /* A session outlives one connection: a phone that drops mid-flow can
         * come back and re-do the exchange without the user re-arming it. */
        if (s_running && s_state != BLE_PROV_DONE) adv_start();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_running) adv_start();
        break;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU now %d", ev->mtu.value);
        break;

    default:
        break;
    }
    return 0;
}

static void adv_start(void)
{
    if (!s_running) return;

    /* Flags plus one 128-bit UUID is 20 of the 31 available bytes, so the name
     * goes in the scan response. Web Bluetooth filters on the UUID, so that is
     * the half that must be in the advertisement itself. */
    struct ble_hs_adv_fields adv = { 0 };
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = (ble_uuid128_t *)&s_uuid_svc;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&adv) != 0) {
        ESP_LOGW(TAG, "adv fields rejected");
        return;
    }

    struct ble_hs_adv_fields rsp = { 0 };
    rsp.name = (uint8_t *)"Facet Cube";
    rsp.name_len = 10;
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);

    struct ble_gap_adv_params p = { 0 };
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;

    int rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                               &p, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "adv start failed (%d)", rc);
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 ||
        ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    adv_start();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();               /* returns when nimble_port_stop() lands */
    nimble_port_freertos_deinit();
}

/* ---- lifecycle --------------------------------------------------------- */

bool ble_prov_start(void)
{
    if (s_running) return true;

    s_sess = heap_caps_calloc(1, sizeof(session_t), MALLOC_CAP_SPIRAM);
    if (!s_sess) {
        /* PSRAM is 7 MB free in practice; if this fails something is very
         * wrong and pulling BLE up on internal SRAM instead would be worse. */
        ESP_LOGE(TAG, "no PSRAM for the session");
        return false;
    }

    uint32_t r = esp_random() % 1000000u;
    snprintf(s_sess->code, sizeof(s_sess->code), "%06u", (unsigned)r);

    mbedtls_ecp_group_init(&s_sess->grp);
    mbedtls_mpi_init(&s_sess->d);
    mbedtls_ecp_point Q;
    mbedtls_ecp_point_init(&Q);

    bool ok = mbedtls_ecp_group_load(&s_sess->grp, MBEDTLS_ECP_DP_SECP256R1) == 0;
    if (ok) ok = mbedtls_ecdh_gen_public(&s_sess->grp, &s_sess->d, &Q,
                                         f_rng, NULL) == 0;
    size_t olen = 0;
    if (ok) ok = mbedtls_ecp_point_write_binary(&s_sess->grp, &Q,
                                                MBEDTLS_ECP_PF_UNCOMPRESSED,
                                                &olen, s_sess->our_pub,
                                                PUBKEY_LEN) == 0 &&
                 olen == PUBKEY_LEN;
    mbedtls_ecp_point_free(&Q);

    if (!ok) {
        ESP_LOGE(TAG, "keygen failed");
        mbedtls_mpi_free(&s_sess->d);
        mbedtls_ecp_group_free(&s_sess->grp);
        heap_caps_free(s_sess);
        s_sess = NULL;
        return false;
    }

    s_running     = true;            /* set before init: NVS must be gated
                                        from the first moment the radio can
                                        touch flash, not after it succeeds */
    s_conn        = BLE_HS_CONN_HANDLE_NONE;
    s_cmd_pending = false;
    s_scan_wanted = false;
    s_err         = ERR_NONE;
    s_data_seq    = 0;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed (%d)", (int)err);
        s_running = false;
        mbedtls_mpi_free(&s_sess->d);
        mbedtls_ecp_group_free(&s_sess->grp);
        heap_caps_free(s_sess);
        s_sess = NULL;
        return false;
    }

    ble_hs_cfg.sync_cb  = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    /* The service table is re-registered on every start. ble_gatts_reset()
     * first, or a second session would add a duplicate copy of the service. */
    ble_gatts_reset();
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_svcs) != 0 || ble_gatts_add_svcs(s_svcs) != 0) {
        ESP_LOGE(TAG, "GATT registration failed");
        nimble_port_deinit();
        s_running = false;
        mbedtls_mpi_free(&s_sess->d);
        mbedtls_ecp_group_free(&s_sess->grp);
        heap_caps_free(s_sess);
        s_sess = NULL;
        return false;
    }
    ble_svc_gap_device_name_set("Facet Cube");

    nimble_port_freertos_init(host_task);

    set_state(BLE_PROV_ADV, 0);
    s_t_state = 0;                   /* poll() stamps it on the first call   */
    ESP_LOGI(TAG, "provisioning open, code %s", s_sess->code);
    return true;
}

void ble_prov_stop(void)
{
    if (!s_running) return;

    ESP_LOGI(TAG, "tearing down");

    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
    }
    ble_gap_adv_stop();

    /* blecent's order. nimble_port_stop() waits for the host task to leave
     * nimble_port_run(); only then is it safe to free the controller. */
    int rc = nimble_port_stop();
    if (rc == 0) {
        nimble_port_deinit();
    } else {
        ESP_LOGE(TAG, "nimble_port_stop failed (%d) — leaving the stack up", rc);
    }

    if (s_sess) {
        mbedtls_mpi_free(&s_sess->d);
        mbedtls_ecp_group_free(&s_sess->grp);
        memset(s_sess, 0, sizeof(*s_sess));   /* key material                */
        heap_caps_free(s_sess);
        s_sess = NULL;
    }

    s_cmd_pending = false;
    s_scan_wanted = false;
    s_state = BLE_PROV_OFF;
    s_running = false;               /* last: unblocks NVS only once the
                                        controller is genuinely gone */
}

bool ble_prov_active(void)          { return s_running; }
ble_prov_state_t ble_prov_state(void) { return s_state; }
bool ble_prov_nvs_blocked(void)     { return s_running; }

const char *ble_prov_code(void)
{
    return (s_sess && s_running) ? s_sess->code : "------";
}

/* ---- command handling (main task) -------------------------------------- */

static void handle_command(int64_t now)
{
    uint8_t pt[CMD_CAP];
    size_t  n = 0;

    /* Before a key exists the only thing CMD/SESSION can carry is the phone's
     * public point. */
    if (!s_sess->have_key) {
        if (s_sess->cmd_len != PUBKEY_LEN) {
            ESP_LOGW(TAG, "unexpected %u bytes before key agreement",
                     s_sess->cmd_len);
            return;
        }
        if (derive_key(s_sess->cmd)) {
            set_state(BLE_PROV_LINKED, now);
        } else {
            s_err = ERR_INTERNAL;
            set_state(BLE_PROV_ERR, now);
        }
        state_notify();
        return;
    }

    if (!unseal(s_sess->cmd, s_sess->cmd_len, pt, sizeof(pt), &n) || n < 1) {
        /* Before AUTHED this is the wrong 6 digits, which is a user error and
         * expected. After AUTHED it is a corrupt or replayed frame. */
        if (s_state != BLE_PROV_AUTHED) {
            s_sess->auth_attempts++;
            s_err = (s_sess->auth_attempts >= MAX_AUTH_ATTEMPTS)
                        ? ERR_LOCKED_OUT : ERR_BAD_CODE;
            ESP_LOGW(TAG, "bad code (attempt %d)", s_sess->auth_attempts);
            if (s_sess->auth_attempts >= MAX_AUTH_ATTEMPTS) {
                set_state(BLE_PROV_ERR, now);
            }
        } else {
            ESP_LOGW(TAG, "dropping an unopenable frame");
        }
        state_notify();
        return;
    }

    uint8_t op = pt[0];

    if (s_state != BLE_PROV_AUTHED) {
        /* The first frame that opens proves the phone saw the screen. It must
         * be HELLO and nothing else, so a malformed first frame cannot slip
         * past as an accidental authentication. */
        if (op != CMD_HELLO || n != 1 + 8 ||
            memcmp(pt + 1, HELLO_MAGIC, 8) != 0) {
            s_err = ERR_BAD_CODE;
            state_notify();
            return;
        }
        s_err = ERR_NONE;
        s_sess->auth_attempts = 0;
        set_state(BLE_PROV_AUTHED, now);
        ESP_LOGI(TAG, "phone authenticated");
        state_notify();
        return;
    }

    switch (op) {
    case CMD_SCAN:
        ESP_LOGI(TAG, "scan requested");
        s_scan_wanted = true;
        ble_prov_request_scan();
        break;

    case CMD_JOIN: {
        /* [op][ssid_len][ssid][pass_len][pass] */
        if (n < 2) break;
        size_t sl = pt[1];
        if (2 + sl + 1 > n) break;
        size_t pl = pt[2 + sl];
        if (2 + sl + 1 + pl > n) break;
        if (sl > 32 || pl > 63) break;

        char ssid[33], pass[64];
        memcpy(ssid, pt + 2, sl);        ssid[sl] = '\0';
        memcpy(pass, pt + 3 + sl, pl);   pass[pl] = '\0';

        ESP_LOGI(TAG, "join requested for a %u-char ssid", (unsigned)sl);
        set_state(BLE_PROV_JOINING, now);
        state_notify();
        ble_prov_submit(ssid, pass);
        memset(pass, 0, sizeof(pass));
        break;
    }

    case CMD_WIFI_OFF:
        ESP_LOGI(TAG, "disconnect requested");
        ble_prov_request_wifi_off();
        break;

    case CMD_BYE:
        set_state(BLE_PROV_DONE, now);
        break;

    default:
        ESP_LOGW(TAG, "unknown command 0x%02x", op);
        break;
    }

    state_notify();
    memset(pt, 0, sizeof(pt));
}

/* ---- main-loop service ------------------------------------------------- */

void ble_prov_poll(int64_t now)
{
    if (!s_running) return;
    if (s_t_state == 0) s_t_state = now;

    if (s_cmd_pending) {
        s_cmd_pending = false;
        handle_command(now);
    }

    int64_t age = now - s_t_state;
    switch (s_state) {
    case BLE_PROV_ADV:
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            set_state(BLE_PROV_LINKED, now);
        } else if (age > ADV_TIMEOUT_MS) {
            ESP_LOGI(TAG, "nobody paired, closing");
            ble_prov_stop();
        }
        break;

    case BLE_PROV_LINKED:
        if (age > AUTH_TIMEOUT_MS) {
            ESP_LOGI(TAG, "no authentication, closing");
            ble_prov_stop();
        }
        break;

    case BLE_PROV_AUTHED:
        if (age > IDLE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "idle, closing");
            ble_prov_stop();
        }
        break;

    case BLE_PROV_DONE:
        if (age > DONE_LINGER_MS) ble_prov_stop();
        break;

    case BLE_PROV_ERR:
        if (age > DONE_LINGER_MS) ble_prov_stop();
        break;

    default:
        break;
    }
}

void ble_prov_scan_ready(void)
{
    if (!s_running || !s_sess || !s_scan_wanted) return;
    s_scan_wanted = false;
    build_aplist();
    state_notify();
}

/* ---- memory probe ------------------------------------------------------
 * The gate on this whole feature: does tearing NimBLE down actually give the
 * memory back? A leak would compound, one session at a time, until the board
 * wedged — and that failure would look like anything but BLE.
 *
 * Both figures use the SAME cap set on purpose. telemetry_row() logs
 * esp_get_free_internal_heap_size() (8BIT|DMA|INTERNAL) beside
 * heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), which is a strictly
 * larger pool — so its "minimum" can read *higher* than its "free", and it
 * cannot be used to judge whether a fix worked. Don't repeat that here.
 *
 * Pass: `after` returns to within ~1 KB of `before` on every cycle, with no
 * downward drift across the three. */
#define PROBE_CAPS (MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)

void ble_prov_mem_probe(void)
{
    ESP_LOGW(TAG, "=== BLE memory probe: 3 cycles ===");
    size_t first_before = heap_caps_get_free_size(PROBE_CAPS);

    for (int i = 1; i <= 3; i++) {
        size_t before = heap_caps_get_free_size(PROBE_CAPS);

        bool ok = ble_prov_start();
        vTaskDelay(pdMS_TO_TICKS(1500));      /* let advertising settle      */
        size_t during = heap_caps_get_free_size(PROBE_CAPS);
        size_t during_min = heap_caps_get_minimum_free_size(PROBE_CAPS);

        ble_prov_stop();
        vTaskDelay(pdMS_TO_TICKS(1000));      /* let the host task exit      */
        size_t after = heap_caps_get_free_size(PROBE_CAPS);

        ESP_LOGW(TAG,
                 "cycle %d: start=%s before=%u during=%u after=%u "
                 "leaked=%d cost_while_up=%d min_during=%u",
                 i, ok ? "ok" : "FAILED",
                 (unsigned)before, (unsigned)during, (unsigned)after,
                 (int)before - (int)after,
                 (int)before - (int)during,
                 (unsigned)during_min);
    }

    size_t last_after = heap_caps_get_free_size(PROBE_CAPS);
    ESP_LOGW(TAG, "=== probe done: net drift over 3 cycles = %d bytes ===",
             (int)first_before - (int)last_after);
}

void ble_prov_join_result(bool joined)
{
    if (!s_running) return;
    if (joined) {
        s_err = ERR_NONE;
        set_state(BLE_PROV_DONE, s_t_state);
        s_t_state = 0;               /* re-stamped by the next poll()        */
        ESP_LOGI(TAG, "join succeeded");
    } else {
        /* Stay open. Retrying a mistyped password without re-pairing is the
         * whole reason the session survives a join attempt. */
        s_err = ERR_JOIN_FAILED;
        set_state(BLE_PROV_AUTHED, 0);
        s_t_state = 0;
        ESP_LOGI(TAG, "join failed, session stays open for a retry");
    }
    state_notify();
}
