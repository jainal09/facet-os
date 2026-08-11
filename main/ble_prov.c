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
#define AUTH_TIMEOUT_MS    180000   /* connected but never proved the code   */
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
#define CMD_RESCAN      0x06
#define CMD_JOIN_SAVED  0x07
#define CMD_FORGET      0x08

/* cube -> phone, in DATA */
#define RSP_APLIST      0x81

/* STATE byte 1 */
#define ERR_NONE        0
#define ERR_BAD_CODE    1
#define ERR_LOCKED_OUT  2
/* 3 was ERR_JOIN_FAILED, retired: the radio is down before the join happens, so
 * the outcome shows on the panel rather than on the phone. Left as a gap so the
 * codes stay stable across versions. */
#define ERR_INTERNAL    4
#define ERR_NO_RESCAN   5   /* not enough headroom to restart Wi-Fi */

/* DATA is read as one attribute; ATT caps an attribute at 512 bytes, and the
 * seal costs NONCE_LEN + TAG_LEN on top of the plaintext. */
#define DATA_CAP        512
#define DATA_PT_CAP     (DATA_CAP - NONCE_LEN - TAG_LEN)
#define CMD_CAP         256
#define CMD_QUEUE       4    /* frames buffered between main-loop passes */

/* Worst case (14 x 32-char SSIDs) only 12 entries fit one 512-byte attribute;
 * real SSIDs are far shorter, so the snapshot holds more than will ever be
 * sent and build_aplist() stops cleanly when it runs out of room. */
#define AP_SNAPSHOT_MAX 16

/* Long enough for the STATE notification to reach the phone before the radio
 * goes down, short enough not to feel like a hang. */
#define HANDOFF_LINGER_MS 1200

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

    /* Two slots, published by flipping an index. NimBLE re-invokes the read
     * callback for EVERY Read Blob Request and slices at `offset`
     * (ble_gatts.c ble_gatts_val_access), so a >MTU payload is served across
     * several callbacks spanning 60-200 ms — while the main task may be sealing
     * a new one into the same memory. Writing in place hands the phone the head
     * of one ciphertext and the tail of another, which fails the GCM tag. */
    uint8_t  data[2][DATA_CAP];
    uint16_t data_len[2];
    volatile uint8_t data_idx;      /* published last; host task reads once   */

    /* A QUEUE, not a buffer. GATT writes arrive on the NimBLE host task and
     * the main loop only drains them every 20 ms, so a phone that writes twice
     * in quick succession — HELLO then SCAN, which is exactly what the page
     * does — puts the second frame where the first has not been read yet. With
     * a single slot the first is lost silently and the second is judged in its
     * place: the HELLO never gets examined and a correct pairing code is
     * reported as wrong. */
    struct { uint8_t buf[CMD_CAP]; volatile uint16_t len; } cmdq[CMD_QUEUE];
    volatile uint8_t cmdq_head;     /* written by the host task */
    volatile uint8_t cmdq_tail;     /* written by the main loop */

    /* Separate from cmd[] on purpose. The page writes SESSION and CMD back to
     * back, both land on the host task within milliseconds, and the main loop
     * only polls every 20 ms — so sharing one buffer meant the sealed HELLO
     * overwrote the public key before it was ever read. The key was then never
     * derived, the HELLO could not open, and the user was told "wrong code"
     * while typing the right one. */
    uint8_t  peer_pub[PUBKEY_LEN];
    uint8_t  last_peer[PUBKEY_LEN];  /* tells a re-handshake from a replay    */

    /* The scan list as it was when pairing began. Wi-Fi is deinitialised for
     * the duration of a session, so there is nothing to rescan against. */
    ble_prov_ap_t aps[AP_SNAPSHOT_MAX];
    int      ap_count;
    char     cur_ssid[33];

    char     want_ssid[33];         /* handed back to main.c at teardown     */
    char     want_pass[64];
    bool     have_handoff;
    bool     use_saved;        /* join with the stored password, not one sent */

    uint64_t tx_ctr;
    uint64_t rx_ctr;                /* highest accepted; replays are dropped */

    mbedtls_ecp_group grp;
    mbedtls_mpi       d;            /* our private scalar                    */

    int      auth_attempts;
    bool     have_key;
    bool     authed;           /* proved the code once; survives HANDOFF   */
    bool     lockout;          /* terminal; close as soon as poll() sees it */
} session_t;

static session_t *s_sess;

static volatile bool  s_running;    /* stack is up, or coming up/going down  */
static volatile bool  s_key_pending;
static ble_prov_state_t s_state = BLE_PROV_OFF;
static uint8_t  s_err;
static uint8_t  s_data_seq;         /* bumped so the phone knows to re-read  */

static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;   /* host task writes */
static uint16_t s_state_handle;
static uint8_t  s_own_addr_type;

static int64_t  s_t_state;          /* when the current state was entered    */

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
        /* Anything staged was sealed under the key that just died — unreadable
         * now, and advertising it as fresh makes the phone fetch a payload it
         * can never decrypt. Drop it and reset the cue. */
        s_sess->data_len[0] = s_sess->data_len[1] = 0;
        s_data_seq = 0;
        /* Reset the replay floor ONLY for a peer point we have not seen. The
         * SESSION write is plaintext on the air, so replaying a captured one
         * would otherwise re-zero rx_ctr and let every frame captured after it
         * be replayed too. A genuine re-handshake always brings a fresh
         * ephemeral point, so this costs nothing legitimate. */
        if (memcmp(s_sess->last_peer, peer_pub, PUBKEY_LEN) != 0) {
            memcpy(s_sess->last_peer, peer_pub, PUBKEY_LEN);
            s_sess->tx_ctr = 0;
            s_sess->rx_ctr = 0;
        }
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
    const ble_prov_ap_t *aps = s_sess->aps;
    int n = s_sess->ap_count;

    uint8_t pt[DATA_PT_CAP];
    size_t  o = 0;

    pt[o++] = RSP_APLIST;

    const char *cur = s_sess->cur_ssid;
    size_t cur_len = strnlen(cur, 32);
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
        /* bit0 = needs a password, bit1 = we already have one */
        pt[o++] = (uint8_t)((aps[i].secure ? 1 : 0) | (aps[i].saved ? 2 : 0));
        written++;
    }
    pt[count_at] = written;

    uint8_t slot = (uint8_t)(s_sess->data_idx ^ 1);
    size_t sealed = 0;
    if (seal(pt, o, s_sess->data[slot], DATA_CAP, &sealed)) {
        s_sess->data_len[slot] = (uint16_t)sealed;
        s_sess->data_idx = slot;    /* publish only once it is complete */
        s_data_seq++;
        ESP_LOGI(TAG, "aplist: %u of %d networks, %u bytes sealed",
                 written, n, (unsigned)sealed);
    } else {
        ESP_LOGW(TAG, "aplist seal failed");
    }
    memset(pt, 0, sizeof(pt));
}

/* ---- GATT -------------------------------------------------------------- */

static int gatt_access(uint16_t conn, uint16_t attr,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn; (void)arg;
    const ble_uuid_t *u = ctxt->chr->uuid;

    /* Which characteristic, by handle and by our own name for it. The phone
     * side reports only an opaque ATT code, so without this there is no way to
     * tell "asked for the wrong thing" from "asked at the wrong time". */
    /* Any GATT traffic is the user still being there. Refreshing only on
     * commands meant the timer ran flat out through the pick-a-network and
     * type-a-password step, which sends nothing at all — the session would
     * expire underneath someone mid-password and simply go quiet. */
    s_t_state = 0;   /* poll() re-stamps it on its next pass */

    {
        const char *nm = "?";
        if      (!ble_uuid_cmp(u, &s_uuid_pubkey.u))  nm = "PUBKEY";
        else if (!ble_uuid_cmp(u, &s_uuid_session.u)) nm = "SESSION";
        else if (!ble_uuid_cmp(u, &s_uuid_cmd.u))     nm = "CMD";
        else if (!ble_uuid_cmp(u, &s_uuid_data.u))    nm = "DATA";
        else if (!ble_uuid_cmp(u, &s_uuid_state.u))   nm = "STATE";
        ESP_LOGI(TAG, "gatt op=%d attr=%u chr=%s", ctxt->op, attr, nm);
    }

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
        /* Read the index ONCE: the main task may flip it mid-transaction. */
        uint8_t i = s_sess ? s_sess->data_idx : 0;
        if (!s_sess || !s_sess->data_len[i]) {
            /* The phone asked for a list we do not have. Logged because from
             * the phone this is an opaque GATT error code with no context. */
            ESP_LOGW(TAG, "DATA read with nothing staged (sess=%d)",
                     s_sess ? 1 : 0);
            return BLE_ATT_ERR_UNLIKELY;
        }
        return os_mbuf_append(ctxt->om, s_sess->data[i], s_sess->data_len[i]) == 0
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
        memcpy(s_sess->peer_pub, peer, PUBKEY_LEN);
        s_key_pending = true;
        return 0;
    }

    if (ble_uuid_cmp(u, &s_uuid_cmd.u) == 0) {
        if (!s_sess) return BLE_ATT_ERR_UNLIKELY;
        uint8_t head = s_sess->cmdq_head;
        uint8_t next = (uint8_t)((head + 1) % CMD_QUEUE);
        if (next == s_sess->cmdq_tail) {
            ESP_LOGW(TAG, "command queue full, dropping a frame");
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        uint16_t len = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, s_sess->cmdq[head].buf,
                                CMD_CAP, &len) != 0) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        s_sess->cmdq[head].len = len;
        /* Release: everything above must be visible before the index that
         * exposes it. Without the barrier the compiler is free to publish
         * cmdq_head first and the main task reads a half-filled slot. */
        __atomic_store_n(&s_sess->cmdq_head, next, __ATOMIC_RELEASE);
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
         * come back and re-do the exchange without the user re-arming it.
         * Except after a lockout — re-advertising there would hand an attacker
         * a fresh connection to keep grinding from, which is the whole thing
         * the attempt limit exists to stop. */
        if (s_running && s_state != BLE_PROV_DONE && s_state != BLE_PROV_ERR) {
            adv_start();
        }
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

bool ble_prov_start(const ble_prov_ap_t *aps, int n, const char *current)
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

    /* Take the scan list now; Wi-Fi is about to be, or already is, gone. */
    if (n > AP_SNAPSHOT_MAX) n = AP_SNAPSHOT_MAX;
    if (aps && n > 0) {
        memcpy(s_sess->aps, aps, (size_t)n * sizeof(ble_prov_ap_t));
        s_sess->ap_count = n;
    }
    if (current) snprintf(s_sess->cur_ssid, sizeof(s_sess->cur_ssid), "%.32s", current);

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
    s_key_pending = false;
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
    if (rc != 0) {
        /* The controller is still running. Freeing s_sess here would pull it
         * out from under a live host task, and clearing s_running below would
         * tell main.c that NVS and Wi-Fi are safe while they are not. Leave
         * everything up; poll() retries. */
        ESP_LOGE(TAG, "nimble_port_stop failed (%d) — session stays up", rc);
        return;
    }
    nimble_port_deinit();

    if (s_sess) {
        mbedtls_mpi_free(&s_sess->d);
        mbedtls_ecp_group_free(&s_sess->grp);
        memset(s_sess, 0, sizeof(*s_sess));   /* key material                */
        heap_caps_free(s_sess);
        s_sess = NULL;
    }

    s_key_pending = false;
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

static void handle_command(const uint8_t *frame, uint16_t frame_len, int64_t now)
{
    uint8_t pt[CMD_CAP];
    size_t  n = 0;

    /* Not the user's fault, so it must not count as a wrong-code attempt:
     * this is a frame that arrived before key agreement completed. */
    if (!s_sess->have_key) {
        ESP_LOGW(TAG, "command before key agreement — dropping %u bytes",
                 frame_len);
        return;
    }

    /* A sealed HELLO is exactly this long. Anything else is not a code
     * attempt, and counting it lets any peer that writes junk burn the limit
     * and kill an open session. */
    const bool hello_shaped = (frame_len == NONCE_LEN + 1 + 8 + TAG_LEN);
    if (!unseal(frame, frame_len, pt, sizeof(pt), &n) || n < 1) {
        /* Before AUTHED this is the wrong 6 digits, which is a user error and
         * expected. After AUTHED it is a corrupt or replayed frame. */
        if (!s_sess->authed && hello_shaped) {
            s_sess->auth_attempts++;
            s_err = (s_sess->auth_attempts >= MAX_AUTH_ATTEMPTS)
                        ? ERR_LOCKED_OUT : ERR_BAD_CODE;
            ESP_LOGW(TAG, "bad code (attempt %d)", s_sess->auth_attempts);
            /* The page fires CMD_SCAN straight after HELLO without waiting for
             * a verdict it cannot know yet. Both land in the queue, and judging
             * both charged two attempts for one wrong code — three tries and
             * the user was locked out. Drop the rest of the batch. */
            s_sess->cmdq_tail = s_sess->cmdq_head;
            if (s_sess->auth_attempts >= MAX_AUTH_ATTEMPTS) {
                /* Six digits is only 10^6 — trivial to grind over a radio link
                 * if guessing is free. The limit is what makes the code worth
                 * anything, so it ends the SESSION rather than just refusing
                 * the frame: stop advertising here, and gap_event() will not
                 * re-arm it on the disconnect that follows. Getting back in
                 * needs a physical press of PAIR and a fresh code. */
                set_state(BLE_PROV_ERR, now);
                ble_gap_adv_stop();
                ESP_LOGW(TAG, "locked out after %d attempts — session over",
                         MAX_AUTH_ATTEMPTS);
                /* Terminal immediately. Lingering left s_running true, so the
                 * user's first press of PAIR only tore the dead session down
                 * and a second was needed to start a new one. */
                s_sess->lockout = true;
            }
        } else {
            ESP_LOGW(TAG, "dropping an unopenable frame");
        }
        state_notify();
        return;
    }

    uint8_t op = pt[0];

    if (!s_sess->authed) {
        /* Keyed on "has never authenticated", not "is not AUTHED right now":
         * after CMD_JOIN the state is HANDOFF, and testing the state made every
         * frame drained in that window count as a wrong code. */
        if (op != CMD_HELLO || n != 1 + 8 ||
            memcmp(pt + 1, HELLO_MAGIC, 8) != 0) {
            /* Logged: this path was silent, and its silence cost a whole
             * debugging round. A frame that decrypts but is not HELLO means the
             * phone got ahead of itself, not that the code was wrong. */
            ESP_LOGW(TAG, "pre-auth frame op=0x%02x len=%u is not HELLO",
                     op, (unsigned)n);
            s_err = ERR_BAD_CODE;
            state_notify();
            return;
        }
        s_err = ERR_NONE;
        s_sess->auth_attempts = 0;
        s_sess->authed = true;
        set_state(BLE_PROV_AUTHED, now);
        ESP_LOGI(TAG, "phone authenticated");
        state_notify();
        return;
    }

    switch (op) {
    case CMD_SCAN:
        /* Serves the snapshot taken at start(). There is no live rescan: Wi-Fi
         * is deinitialised for the whole session so there is nothing to scan
         * with. Harmless to call repeatedly — the phone uses it to re-read the
         * list after a reconnect. */
        ESP_LOGI(TAG, "list requested (%d networks in snapshot)", s_sess->ap_count);
        build_aplist();
        break;

    case CMD_JOIN: {
        /* [op][ssid_len][ssid][pass_len][pass] */
        /* Each of these was a bare `break`, which fell through to the
         * unchanged state_notify() and let the page read [AUTHED, err=0] as
         * "Ready." — a malformed frame reported to the user as success. */
        if (n < 2 || pt[1] > 32) { s_err = ERR_INTERNAL; break; }
        size_t sl = pt[1];
        if (2 + sl + 1 > n) { s_err = ERR_INTERNAL; break; }
        size_t pl = pt[2 + sl];
        if (2 + sl + 1 + pl > n || pl > 63) { s_err = ERR_INTERNAL; break; }

        char ssid[33], pass[64];
        memcpy(ssid, pt + 2, sl);        ssid[sl] = '\0';
        memcpy(pass, pt + 3 + sl, pl);   pass[pl] = '\0';

        ESP_LOGI(TAG, "join requested for a %u-char ssid", (unsigned)sl);
        /* Stash and close. The join itself cannot happen while the radio is up,
         * so poll() notifies the phone, lingers long enough for that to land,
         * then hands off and tears BLE down. */
        snprintf(s_sess->want_ssid, sizeof(s_sess->want_ssid), "%s", ssid);
        snprintf(s_sess->want_pass, sizeof(s_sess->want_pass), "%s", pass);
        s_sess->have_handoff = true;
        set_state(BLE_PROV_HANDOFF, now);
        memset(pass, 0, sizeof(pass));
        break;
    }

    case CMD_JOIN_SAVED: {
        /* [op][ssid_len][ssid] — no password on the wire at all. */
        if (n < 2 || pt[1] > 32) { s_err = ERR_INTERNAL; break; }
        size_t sl = pt[1];
        if (2 + sl > n) { s_err = ERR_INTERNAL; break; }
        char ssid[33];
        memcpy(ssid, pt + 2, sl); ssid[sl] = '\0';
        ESP_LOGI(TAG, "rejoin \"%s\" using stored credentials", ssid);
        snprintf(s_sess->want_ssid, sizeof(s_sess->want_ssid), "%.32s", ssid);
        s_sess->want_pass[0] = '\0';
        s_sess->have_handoff = true;
        s_sess->use_saved    = true;
        set_state(BLE_PROV_HANDOFF, now);
        break;
    }

    case CMD_FORGET: {
        /* Deliberately does NOT end the session. The whole point is to escape a
         * bad stored password, so the user must still be connected afterwards
         * to type the right one. The actual NVS erase is deferred to main.c for
         * after teardown — a flash erase with the controller running from flash
         * is exactly what §7g forbids. */
        if (n < 2 || pt[1] > 32) { s_err = ERR_INTERNAL; break; }
        size_t fl = pt[1];
        if (2 + fl > n) { s_err = ERR_INTERNAL; break; }
        char fssid[33];
        memcpy(fssid, pt + 2, fl); fssid[fl] = '\0';
        ESP_LOGI(TAG, "forget \"%s\"", fssid);
        ble_prov_forget(fssid);
        for (int i = 0; i < s_sess->ap_count; i++) {
            if (strncmp(s_sess->aps[i].ssid, fssid, 33) == 0) s_sess->aps[i].saved = false;
        }
        build_aplist();                 /* re-send so the phone drops the offer */
        break;
    }

    case CMD_WIFI_OFF:
        /* An empty SSID is the agreed "stay off" hand-off. */
        ESP_LOGI(TAG, "disconnect requested");
        s_sess->want_ssid[0] = '\0';
        s_sess->want_pass[0] = '\0';
        s_sess->have_handoff = true;
        set_state(BLE_PROV_HANDOFF, now);
        break;

    case CMD_BYE:
        set_state(BLE_PROV_DONE, now);
        break;

    case CMD_RESCAN: {
        /* Blocks the main loop for a few seconds: Wi-Fi has to be brought up,
         * scanned with, and taken down again. Well inside the 30 s task WDT,
         * and the link survives because NimBLE runs on its own task. */
        ESP_LOGI(TAG, "rescan requested");
        int n = ble_prov_rescan(s_sess->aps, AP_SNAPSHOT_MAX);
        if (n < 0) {
            s_err = ERR_NO_RESCAN;
            ESP_LOGW(TAG, "rescan refused — not enough free internal memory");
        } else {
            s_sess->ap_count = n;
            s_err = ERR_NONE;
            build_aplist();     /* bumps data_seq, so the phone re-reads */
        }
        break;
    }

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

    /* Key agreement before commands, always, and both in the same pass: the
     * two writes arrive together and the HELLO is meaningless until the key
     * exists. Ordering this the other way is what made pairing fail. */
    if (s_key_pending) {
        s_key_pending = false;
        if (derive_key(s_sess->peer_pub)) {
            set_state(BLE_PROV_LINKED, now);
        } else {
            s_err = ERR_INTERNAL;
            set_state(BLE_PROV_ERR, now);
        }
        state_notify();
    }

    /* Drain everything queued since the last pass, in arrival order. */
    while (s_sess &&
           s_sess->cmdq_tail != __atomic_load_n(&s_sess->cmdq_head,
                                                __ATOMIC_ACQUIRE)) {
        uint8_t t = s_sess->cmdq_tail;
        /* Any command is activity. Without this the idle timeout counts from
         * when AUTHED was entered and never restarts, so a session dies mid-use
         * while the user is reading the list or typing a password. */
        s_t_state = now;
        handle_command(s_sess->cmdq[t].buf, s_sess->cmdq[t].len, now);
        s_sess->cmdq_tail = (uint8_t)((t + 1) % CMD_QUEUE);
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
            /* Back to advertising with the SAME code rather than tearing the
             * session down. The timer starts when the phone connects — before
             * the user has even been shown the code — so someone who walked off
             * to read it should find the cube still waiting. */
            ESP_LOGI(TAG, "no authentication, back to advertising");
            if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            }
            set_state(BLE_PROV_ADV, now);
        }
        break;

    case BLE_PROV_AUTHED:
        if (age > IDLE_TIMEOUT_MS) {
            ESP_LOGI(TAG, "idle, closing");
            ble_prov_stop();
        }
        break;

    case BLE_PROV_HANDOFF:
        /* Give the STATE notification time to reach the phone, then hand the
         * credentials over and take the radio down. main.c must wait for
         * ble_prov_active() to go false before touching Wi-Fi. */
        if (age > HANDOFF_LINGER_MS && s_sess && s_sess->have_handoff) {
            char ssid[33], pass[64];
            bool saved = s_sess->use_saved;
            snprintf(ssid, sizeof(ssid), "%.32s", s_sess->want_ssid);
            snprintf(pass, sizeof(pass), "%.63s", s_sess->want_pass);
            s_sess->have_handoff = false;
            set_state(BLE_PROV_DONE, now);
            state_notify();
            ble_prov_stop();          /* frees s_sess, hence the local copies */
            ble_prov_submit(ssid, saved ? NULL : pass);
            memset(pass, 0, sizeof(pass));
        }
        break;

    case BLE_PROV_DONE:
        if (age > DONE_LINGER_MS) ble_prov_stop();
        break;

    case BLE_PROV_ERR:
        if ((s_sess && s_sess->lockout) || age > DONE_LINGER_MS) ble_prov_stop();
        break;

    default:
        break;
    }
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

        bool ok = ble_prov_start(NULL, 0, "");
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

