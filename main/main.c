/*
 * Funnel-profile firmware — Waveshare ESP32-S3-Touch-AMOLED-2.16
 *
 * No VPN stack. Wi-Fi STA and ordinary outbound HTTPS, so TLS cost is transient
 * per call instead of a permanent set of task stacks.
 *
 * Apps are built on open and freed on close; see docs/ARCHITECTURE.md.
 *
 * Keys
 *   left  (IO18, GPIO18) : HOME/SETUP = screen off/on   PET = feed
 *   mid   (PWR)          : toggle Wi-Fi setup           PET = play
 *                          not a GPIO — read from the AXP2101 PWRKEY IRQ
 *   right (BOOT, GPIO0)  : short = open pet             PET = rest
 *                          long  = back to HOME
 *
 * Everything sits in a safe area: the 480x480 AMOLED has heavily rounded
 * corners and curved cover glass, so content within ~55px of an edge is
 * clipped or unreadable at an angle.
 */

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <inttypes.h>
#include <math.h>
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_netif.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_panel_ops.h"
#include "esp_pm.h"
#include "esp_sntp.h"
#include "esp_task_wdt.h"
#include "esp_app_desc.h"
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <sys/time.h>
#include "bsp/display.h"

#include "lvgl.h"
/* not pulled in by lvgl.h — needed to invalidate a decoded image whose file
 * changed underneath it (the wallpaper is always the same path) */
#include "src/misc/cache/instance/lv_image_cache.h"
#include "bsp/esp-bsp.h"
#include "ml_draw_buf_cfg.h"

#include "credentials.h"
#include "hud_fonts.h"
#include "cJSON.h"
#include "ble_prov.h"

static const char *TAG = "funnel";

/* A new lock-screen photo every six hours: four API calls a day against
 * Unsplash's 50/hour demo limit, and one ~400 KB download — small enough that
 * it never competes with anything, frequent enough that the desk clock does
 * not look like the same picture forever. */
#define WALLPAPER_PERIOD_MS (6 * 60 * 60 * 1000)

#define STATS_PERIOD_MS   (15 * 1000)
#define BATTERY_PERIOD_MS (2 * 1000)
#define HTTPS_PERIOD_MS   (45 * 1000)
#define PET_TICK_MS       (180 * 1000)  /* one stat point of decay (~5 h to empty) */

/* Physical layout, per the label on the back of the board:
 *   leftmost  = BOOT / minus   GPIO0    (the strap; an ordinary pulled-up
 *                                        input after boot, so it doubles as a
 *                                        user key — just don't hold it
 *                                        through a reset)
 *   middle    = PWR            AXP2101 PWRKEY, not a GPIO at all
 *   rightmost = plus           GPIO18
 */
#define KEY_LEFT_GPIO     GPIO_NUM_0    /* BOOT / minus */
#define KEY_RIGHT_GPIO    GPIO_NUM_18   /* plus */
#define LONG_PRESS_MS     800

/* Safe area for the rounded/curved 480x480 panel.
 * The keyboard is the tightest fit: 424 wide with a 44px bottom margin puts
 * its bottom corners at (28,436), inside the corner arc even at r=110. */
#define CONTENT_W         364           /* centred column: x 58..422 */
#define TOP_MARGIN        30
#define BOTTOM_MARGIN     44
#define KB_W              424
#define KB_H              188

/* AXP2101 PMU */
#define AXP2101_ADDR          0x34
#define AXP_REG_STATUS1       0x00      /* bit3: battery present */
#define AXP_REG_STATUS2       0x01      /* bits[6:5]: 01=charging */
#define AXP_REG_GAUGE_CTRL    0x18      /* bit3: fuel-gauge module enable */
#define AXP_REG_ADC_CH_CTRL   0x30      /* bit0: battery voltage ADC enable */
#define AXP_REG_ADC_DATA_H    0x34
#define AXP_REG_ADC_DATA_L    0x35
#define AXP_REG_INTEN2        0x41      /* bit3: PWRKEY short-press IRQ enable */
#define AXP_REG_INTSTS2       0x49      /* bit3: PWRKEY short press, write 1 to clear */
#define AXP_REG_BAT_DET_CTRL  0x68      /* bit0: battery detection enable */
#define AXP_REG_BAT_PERCENT   0xA4
#define AXP_PKEY_SHORT_BIT    3

static EventGroupHandle_t s_evt;
#define WIFI_CONNECTED_BIT BIT0

/* Apps are built when opened and destroyed when closed, so only the running
 * app costs RAM. That removes the widget ceiling entirely — the cost of a
 * switch is one rebuild, not a reboot. */
enum { APP_CONTROL = 0, APP_MUSIC, APP_POMO, APP_PET, APP_COUNT };
#define APP_DRAWER (-1)
#define APP_LOCK   (-2)
#define AUTO_LOCK_MS  60000     /* unlocked and idle -> lock */
#define LOCK_SLEEP_MS 15000     /* locked and idle   -> backlight off */
static volatile int s_app = APP_DRAWER;
/* Sentinel must not collide with any real app id — APP_LOCK is -2, so -2 as
 * "nothing pending" silently swallowed every lock request. */
#define APP_NONE   (-100)
static volatile int s_req_app = APP_NONE;
static lv_timer_t  *s_app_timer;             /* owned by the running app */

/* Switching is always deferred to the main task: tearing a screen down from
 * inside one of its own touch callbacks would free the object mid-event. */
static void app_request(int idx) { s_req_app = idx; }

/* ---- home screen ---- */
static lv_obj_t *s_scr_home;
static lv_obj_t *s_status_label;
static lv_obj_t *s_batt_bar;
static lv_obj_t *s_batt_label;
static lv_obj_t *s_bolt_label;
static lv_obj_t *s_fps_label;
static lv_obj_t *s_events_label;

/* ---- setup screen ---- */
static lv_obj_t *s_scr_setup;

/* ---- pet screen (widgets are declared with the scene, further down) ---- */
static lv_obj_t *s_scr_pet;

static volatile uint32_t s_refr_count;
static volatile uint32_t s_last_fps_x10;

/* shared state */
static volatile int  s_batt_pct = -1;       /* -1 = no battery present */
static volatile int  s_batt_mv;
static volatile bool s_batt_charging;
static volatile bool s_wifi_up;
static volatile bool s_screen_on = true;

/* pet state (persisted) */
static int s_food = 80, s_fun = 75, s_nrg = 90;
static uint32_t s_pet_age_min;
static volatile bool s_pet_dirty;

/* Wi-Fi credentials in use, and a pending change from the setup screen */
static char s_ssid[33];
static char s_pass[65];
/* Set only by an explicit disconnect. Auto-reconnect must respect it, or the
 * station is back on the network a second after the user asked it not to be. */
static bool s_wifi_disabled;
static char s_ip[16];
static volatile int s_wifi_reason;      /* last disconnect reason, for the log */
static int  s_wifi_tries;               /* drives the reconnect backoff */
static bool s_wifi_diag_done;           /* one scan dump per boot when joining fails */

/* BLE pairing. The radios cannot both be initialised (HARDWARE.md §7g), so a
 * session tears the Wi-Fi driver down and rebuilds it afterwards. */
static volatile bool s_req_ble_on;      /* CONTROL: start pairing  */
static volatile bool s_req_ble_off;     /* CONTROL: stop pairing   */
static bool s_wifi_torn_down;           /* driver deinitialised for a session */
static bool s_ble_forget;               /* discard stored creds after teardown */
/* GOT_IP runs on the esp_event task. Both the credential commit and the known
 * table belong to the main task — one is a flash write that BLE may be gating,
 * the other is a shared array with no lock. Stage here, act in the main loop. */
static volatile bool s_req_creds_save;
static volatile bool s_req_known_remember;
static bool s_ble_handoff;              /* credentials waiting to be applied  */
/* Rescan brings Wi-Fi up only to look around. Without this the STA_START event
 * immediately calls esp_wifi_connect(), and a scan cannot run while a connect
 * is in flight — it returns zero networks, which reads as "no wi-fi in range"
 * rather than "the radio was busy". */
static volatile bool s_wifi_scan_only;
static char s_ble_ssid[33];
static char s_ble_pass[64];
/* Credentials are not trusted until they actually work.
 *
 * A half-typed password on the on-screen keyboard used to be committed to NVS
 * immediately, and NVS wins over the compiled-in values — so one abandoned
 * attempt permanently overrode a working config, and the only symptom was
 * reason=201 forever against an SSID that does not exist. Hold new credentials
 * as pending, commit on GOT_IP, and fall back to the built-in ones if they keep
 * failing. */
static volatile bool s_creds_pending;
static volatile bool s_req_sntp;
static volatile bool s_req_wake;
static bool s_time_synced;

/* scan results — compact copies so the raw records can be freed immediately */
#define MAX_APS 14
typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} ap_entry_t;
static ap_entry_t s_aps[MAX_APS];
static int s_ap_count;

/* rolling event log on the home screen */
#define LOG_LINES 3
static char s_log[LOG_LINES][40];
static SemaphoreHandle_t s_log_mtx;

static i2c_master_dev_handle_t s_axp;

/* lock screen widgets */
/* X-Art-Accent from the last download, 0 when the header was absent. Declared
 * up here because sp_fetch_art() reads it and sits above the wallpaper code
 * that owns the download sink. */
static uint32_t s_dl_accent;

/* Declared up here because sp_fetch_art() swaps the cover in itself — it sits
 * above the screen code but needs the widgets and the PSRAM buffer. */
static lv_obj_t *s_sp_art, *s_sp_art_ph;
static char s_sp_art_shown[160];  /* which art the widgets are showing */
static uint8_t *s_sp_art_buf;            /* PSRAM, SP_ART_BYTES */
static lv_image_dsc_t s_sp_art_dsc;      /* points into s_sp_art_buf + header */
static bool asset_fetch_mem(const char *url, const char *bearer,
                            uint8_t *buf, size_t cap, size_t *out_len);

static lv_obj_t *s_lock_time, *s_lock_date, *s_lock_batt;
static lv_obj_t *s_lock_sweep, *s_lock_batt_arc;

/* Now-playing panel on the lock screen. Hidden unless Spotify actually has an
 * active device, so the screen stays as sparse as it was whenever there is
 * nothing to say. s_lock_np_up is read by wall_service() on the network task: a
 * wallpaper download and an album-art download must not overlap, because the
 * wallpaper fetch is already the largest transient consumer of internal SRAM on
 * this board and the margin is thinner than it has ever been. */
static lv_obj_t *s_lock_np, *s_lock_np_art, *s_lock_np_ph, *s_lock_np_track;
static lv_obj_t *s_lock_np_prev, *s_lock_np_play, *s_lock_np_next;
static volatile bool s_lock_np_up;
static uint32_t s_lock_np_bg;    /* last scrim colour, 0 = unset */
static lv_obj_t *s_lock_rule;    /* divider under the clock; moves with it */
static int s_sweep_deg;

/* App state store + power state — defined further down, used from above */
/* Known-network table lives further down with the other credential code, but
 * the Wi-Fi event handler confirms a join before that point. */
static void known_remember(const char *ssid, const char *pass);
static const char *known_pass(const char *ssid);

static void store_init_dirs(void);
static bool store_save(const char *id, const void *data, size_t len);
static bool store_load(const char *id, void *data, size_t len);

static bool s_sd_ok;
static uint32_t s_tele_rows;
static bool s_doze;
static char s_wall_credit[48];      /* Unsplash photographer, shown in CONTROL */

/* Set if any task had to fall back to an internal stack. That silently gives
 * back the ~8 KB that moving stacks to PSRAM bought, and the only symptom would
 * be a floor regression with no visible cause — so it is surfaced in the
 * periodic status line rather than left to be re-derived from a heap
 * distribution later. */
static bool s_stack_fallback;

/* Wallpaper pool state. Declared up here rather than beside the fetch code
 * because both the network task and the CONTROL screen are defined long before
 * it, and both need to see it. */
#define WALL_SLOTS      12
#define WALL_DIR        BSP_SD_MOUNT_POINT "/assets"

static volatile bool s_req_wallpaper;
static uint16_t s_wall_have;         /* bitmask of slots holding a usable PNG */
static int      s_wall_slot = -1;    /* slot currently on screen, -1 = none */
static int64_t  s_wall_last;         /* last download attempt, success or not */

/* Download progress, so the UI can show what a fetch is actually doing.
 * "Fetching wallpaper" as a one-shot log line read as permanently stuck: it
 * never cleared, and it appeared up to a full network cycle before anything
 * happened. */
typedef enum { DL_IDLE = 0, DL_QUERY, DL_IMAGE, DL_OK, DL_FAIL } dl_state_t;
static volatile dl_state_t s_dl_state;
static volatile int      s_dl_pct;
static volatile int      s_dl_kb;
static volatile int64_t  s_dl_total;
static volatile int64_t  s_dl_got;
static int64_t           s_dl_ended_ms;   /* when OK/FAIL landed, to auto-clear */
static char              s_dl_theme[80];

/* One tick of the wallpaper pool. Defined with the asset code further down but
 * driven from the network task, which is declared long before it. */
static void wall_service(void);

/* Streaming download to the card, optionally with a bearer. Defined with the
 * asset code; the Spotify app needs it for album art and sits above it. */
static bool asset_fetch_auth(const char *url, const char *path, const char *bearer);
static void power_set_doze(bool doze);
static int  battery_drain_mv_h(void);

/* The pool that actually matters, and the ONE definition of it.
 *
 * esp_get_free_internal_heap_size() reports 8BIT|DMA|INTERNAL, but
 * heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) watermarks a superset
 * that also counts 32-bit-only IRAM. Logging one against the other produced
 * rows where min exceeded free — impossible for a single pool — and silently
 * overstated the floor. Everything below uses identical caps so the numbers are
 * comparable, and largest-block is included because exhaustion and
 * fragmentation look the same in a free-size column and need opposite fixes. */
#define MEM_CAPS (MALLOC_CAP_8BIT | MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL)

static inline uint32_t hp_free(void)    { return heap_caps_get_free_size(MEM_CAPS); }
static inline uint32_t hp_min(void)     { return heap_caps_get_minimum_free_size(MEM_CAPS); }
static inline uint32_t hp_largest(void) { return heap_caps_get_largest_free_block(MEM_CAPS); }

/* Short build id, so a CSV row can be attributed to a binary. Without it the log
 * silently mixes every firmware that ever ran, and a row from deleted code reads
 * as a live measurement. */
static const char *build_id(void) {
    static char id[9];
    if (!id[0]) {
        const esp_app_desc_t *d = esp_app_get_description();
        for (int i = 0; i < 4; i++) {
            static const char hex[] = "0123456789abcdef";
            id[i * 2]     = hex[d->app_elf_sha256[i] >> 4];
            id[i * 2 + 1] = hex[d->app_elf_sha256[i] & 0xF];
        }
    }
    return id;
}

static int64_t now_ms(void) {
    return (int64_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static void log_event(const char *fmt, ...) {
    char line[40];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (s_log_mtx && xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < LOG_LINES - 1; i++) {
            memcpy(s_log[i], s_log[i + 1], sizeof(s_log[0]));
        }
        snprintf(s_log[LOG_LINES - 1], sizeof(s_log[0]), "%s", line);
        xSemaphoreGive(s_log_mtx);
    }
}

static void log_mem(const char *tag2) {
    ESP_LOGI(TAG,
        "MEM[%s] free=%u min=%u largest=%u psram=%u",
        tag2, (unsigned)hp_free(), (unsigned)hp_min(), (unsigned)hp_largest(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* The LVGL lock, bounded.
 *
 * An infinite wait here turns any stall on the LVGL side into a total freeze of
 * the main loop, and that is exactly what happened once: every task blocked,
 * both cores idle, no panic and no watchdog. A bounded wait degrades to one
 * dropped UI update plus a loud log line, which is recoverable and diagnosable.
 * The underlying mutex is recursive, so calling this from a callback already
 * running under the lock is safe. */
#define UI_LOCK_MS 2000

static bool ui_lock(void) {
    if (bsp_display_lock(UI_LOCK_MS)) return true;
    ESP_LOGE(TAG, "LVGL lock timed out after %d ms — UI update skipped", UI_LOCK_MS);
    return false;
}

/* No draw-buffer reservation: holding one across bsp_display_new() starved
 * spi_bus_initialize() of DMA memory. Hook kept for visibility. */
#define DRAW_BUF_BYTES (480 * ML_DRAW_BUF_HEIGHT * 2)

void ml_pre_draw_buf_alloc(void) {
    ESP_LOGI(TAG, "draw buffers: internal_largest=%u (need 2x %u)",
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)DRAW_BUF_BYTES);
}

/* ---------------- persistent state ---------------- */

/* Set to 1 for one flash to discard saved Wi-Fi credentials. A half-typed
 * password on the on-screen keyboard had been committed to NVS, and NVS wins
 * over the compiled-in values, so the device chased an SSID that does not exist.
 * Combined with save-on-success below, one boot restores a correct saved pair. */
#define CREDS_RESET 0

static void creds_load(void) {
#if CREDS_RESET
    {
        nvs_handle_t h;
        if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGW(TAG, "CREDS_RESET: discarded saved Wi-Fi credentials");
        }
    }
#endif
    snprintf(s_ssid, sizeof(s_ssid), "%s", WIFI_SSID);
    snprintf(s_pass, sizeof(s_pass), "%s", WIFI_PASSWORD);

    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    char ssid[33];
    size_t len = sizeof(ssid);
    if (nvs_get_str(h, "ssid", ssid, &len) == ESP_OK && ssid[0]) {
        snprintf(s_ssid, sizeof(s_ssid), "%s", ssid);
        char pass[65];
        len = sizeof(pass);
        if (nvs_get_str(h, "pass", pass, &len) == ESP_OK) {
            snprintf(s_pass, sizeof(s_pass), "%s", pass);
        }
        ESP_LOGI(TAG, "using NVS credentials for \"%s\"", s_ssid);
    }
    nvs_close(h);
}

static void creds_save(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGW(TAG, "could not open NVS to save credentials");
        return;
    }
    nvs_set_str(h, "ssid", ssid);
    nvs_set_str(h, "pass", pass);
    nvs_commit(h);
    nvs_close(h);
}

typedef struct {
    uint16_t ver;
    int16_t  food, fun, nrg;
    uint32_t age_min;
} pet_blob_t;
#define PET_BLOB_VER 1

static void pet_load(void) {
    pet_blob_t b;
    if (store_load("pet", &b, sizeof(b)) && b.ver == PET_BLOB_VER) {
        s_food = clampi(b.food, 0, 100);
        s_fun  = clampi(b.fun,  0, 100);
        s_nrg  = clampi(b.nrg,  0, 100);
        s_pet_age_min = b.age_min;
    }
    if (s_food + s_fun + s_nrg == 0) {
        s_food = s_fun = s_nrg = 70;
        ESP_LOGI(TAG, "pet had run flat — starting it fresh");
    }
    ESP_LOGI(TAG, "pet restored: food=%d fun=%d nrg=%d age=%lumin",
             s_food, s_fun, s_nrg, (unsigned long)s_pet_age_min);
}

static void pet_save(void) {
    pet_blob_t b = { .ver = PET_BLOB_VER, .food = s_food, .fun = s_fun,
                     .nrg = s_nrg, .age_min = s_pet_age_min };
    store_save("pet", &b, sizeof(b));
    s_pet_dirty = false;
}

/* ---------------- Wi-Fi ---------------- */

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (s_wifi_scan_only) return;      /* scanning only; do not associate */
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        if (s_creds_pending) {
            /* Credentials that never worked. The phone is already gone by now
             * (the radio came down to let Wi-Fi up), so the panel is the only
             * place this can be said. */
            log_event(d->reason == 201 ? "join failed: not found"
                                       : "join failed: rejected");
        }
        s_wifi_up = false;
        s_ip[0] = '\0';
        xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT);
        /* Record the reason and let the main loop own reconnection.
         *
         * This used to retry here, which was wrong twice over. It slept for a
         * second *inside an event callback*, stalling the whole Wi-Fi event task
         * on every disconnect; and it raced the main loop's keep-alive nudge, so
         * two paths called esp_wifi_connect() and the logs filled with
         * "sta is connecting, return error". With an AP that has genuinely gone
         * away (reason 201, NO_AP_FOUND) that repeated several times a second
         * for hours. One owner, with backoff, is the fix. */
        s_wifi_reason = d->reason;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_up = true;
        log_event("wifi " IPSTR, IP2STR(&e->ip_info.ip));
        if (!s_time_synced) s_req_sntp = true;
        /* Every confirmed join, not just credentials that arrived over BLE.
         * Gating this on s_creds_pending meant a device that had been working
         * since before the table existed had an EMPTY table — so the phone
         * showed nothing as saved and asked for a password the cube held. */
        s_req_known_remember = true;
        /* It works — now it is safe to remember. */
        if (s_creds_pending) {
            s_creds_pending = false;
            s_req_creds_save = true;
            ESP_LOGI(TAG, "credentials for \"%s\" confirmed and saved", s_ssid);
        }
        xEventGroupSetBits(s_evt, WIFI_CONNECTED_BIT);
    }
}

static void wifi_apply_config(const char *ssid, const char *pass) {
    wifi_config_t wc = { .sta = { .threshold.authmode = WIFI_AUTH_OPEN } };
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%.31s", ssid);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%.63s", pass);
    if (pass[0]) {
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    }
    esp_wifi_set_config(WIFI_IF_STA, &wc);
}

/* Wi-Fi comes up in two halves because the driver has to be torn down and
 * rebuilt for a BLE pairing session — the two radios cannot both be
 * initialised on this board (HARDWARE.md §7g). Only the driver half cycles.
 *
 * The once-per-boot half must never run twice: nothing here is ever
 * deinitialised, and a second esp_netif_create_default_wifi_sta() leaks the
 * first netif and then asserts. */
static void wifi_init_once(void) {
    s_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));
}

static esp_err_t wifi_driver_up(void) {
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    /* Not ESP_ERROR_CHECK: the rescan path calls this while BLE holds ~26 KB,
     * where NO_MEM is a legitimate answer rather than a bug. Aborting there
     * would turn "cannot rescan right now" into a reboot. */
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_init failed (%s), wifi stays down",
                 esp_err_to_name(err));
        return err;
    }

    /* The default WIFI_STORAGE_FLASH mirrors every esp_wifi_set_config() into
     * NVS, which is a flash erase. That stalls a BLE controller running from
     * flash, and there is no auto-suspend available on this board's XMC part.
     * Nothing reads the mirror back — creds_load() uses our own "wifi"
     * namespace and wifi_apply_config() is called explicitly right here. */
    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_apply_config(s_ssid, s_pass);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(s_doze ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM);

    /* Reset the backoff so the rejoin after a session is immediate rather than
     * up to 60 s late. */
    s_wifi_tries     = 0;
    s_wifi_diag_done = false;
    s_wifi_torn_down = false;
    /* ESP_LOGx needs a literal format string, so this cannot be a ternary. */
    if (s_wifi_scan_only) ESP_LOGI(TAG, "WiFi up for a scan only");
    else                  ESP_LOGI(TAG, "WiFi connecting to \"%s\"...", s_ssid);
    return ESP_OK;
}

/* Frees ~53.5 KB of 8BIT|DMA|INTERNAL — the single largest block of that pool
 * on this board. esp_wifi_stop() alone frees 96 bytes; the pools are allocated
 * at init, so only a deinit returns them. */
static void wifi_driver_down(void) {
    if (s_wifi_torn_down) return;

    /* Set the flag and clear s_wifi_up BEFORE deinit, never as a consequence of
     * a disconnect event: a deinit does not reliably deliver one, so s_wifi_up
     * would stay true with no driver underneath it. Four subsystems believe
     * that flag — the Spotify poll, the asset fetch, and two status screens —
     * and one of them would start a TLS handshake mid-advert. */
    s_wifi_torn_down = true;
    s_wifi_up        = false;
    s_ip[0]          = '\0';
    xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT);

    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    ESP_LOGI(TAG, "wifi driver down (internal free %u)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_DMA |
                                               MALLOC_CAP_INTERNAL));
}

static void wifi_init(void) {
    wifi_init_once();
    wifi_driver_up();
}

/* The one thing ble_prov.c asks of us. Called from ble_prov_poll() immediately
 * before the radio goes down, so it may only stash — Wi-Fi must not be touched
 * until ble_prov_active() reads false. The main loop does the rest. */
/* Defined below with the other Wi-Fi primitives; C needs it before use and
 * moving the rescan callback down would separate it from wifi_driver_up/down,
 * which is where it belongs. */
static void wifi_scan_now(void);

/* Wi-Fi's pools are ~53.5 KB and a live BLE session leaves ~41 KB, so this
 * normally refuses. The check is on the REAL number at the moment of asking
 * rather than a compile-time assumption, which means it starts working by
 * itself if the Wi-Fi buffer counts are ever trimmed. */
/* Derived from the buffer counts rather than hardcoded, so it follows any
 * future trim of ESP_WIFI_STATIC_*_BUFFER_NUM without anyone remembering to
 * revisit it — the day those come down, this starts succeeding on its own.
 *
 * The earlier constant was wrong in an instructive way: it came from what
 * esp_wifi_deinit() *frees* (~53.5 KB) on a device that has been associated and
 * running, which is an upper bound including buffers accumulated at runtime.
 * What init actually needs up front is the static pools plus the driver task:
 * 8 static RX + 16 static TX at ~1.6 KB each is 38.4 KB, plus a 6,656-byte task
 * stack — about 45 KB. Measuring the wrong end of the lifecycle overstated the
 * gap by three times. */
#define WIFI_STATIC_BUFS ((CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM + \
                           CONFIG_ESP_WIFI_STATIC_TX_BUFFER_NUM) * 1600u)
/* Overhead beyond the static pools, MEASURED rather than estimated: a rescan
 * at 8+8 buffers took free from 42,503 to 2,156, i.e. 40,347 total against
 * 25,600 of static pools — so ~14.7 KB of driver task stack, management short
 * buffers, dynamic RX and descriptors. An earlier 8 KB guess let the gate pass
 * with ~1.5 KB to spare, which is the razor-thin condition this project has
 * twice been burned by. */
#define WIFI_INIT_OVERHEAD (15u * 1024u)
#define WIFI_INIT_NEED     (WIFI_STATIC_BUFS + WIFI_INIT_OVERHEAD)
/* Below this much slack the rescan is technically possible and not worth doing:
 * anything else allocating during the ~5 s window would fail. */
#define WIFI_RESCAN_MARGIN (8u * 1024u)

int ble_prov_rescan(ble_prov_ap_t *out, int max) {
    const uint32_t need = WIFI_INIT_NEED;
    uint32_t have = hp_free();
    if (have < need) {
        ESP_LOGW(TAG, "rescan refused: %u free, need ~%u (short by %u)",
                 (unsigned)have, (unsigned)need, (unsigned)(need - have));
        return -1;
    }
    if (have - need < WIFI_RESCAN_MARGIN) {
        /* Loud on purpose. It will work, and it will keep working right up
         * until something else allocates during the window. Trim
         * ESP_WIFI_STATIC_TX_BUFFER_NUM further to buy margin. */
        ESP_LOGW(TAG, "rescan margin THIN: %u free, need ~%u, only %u spare",
                 (unsigned)have, (unsigned)need, (unsigned)(have - need));
    }

    ESP_LOGI(TAG, "rescan: bringing wifi up beside BLE (%u free)", (unsigned)have);
    s_wifi_scan_only = true;
    esp_err_t err = wifi_driver_up();
    if (err == ESP_OK) {
        /* Let the driver finish starting. esp_wifi_start() returns before the
         * radio is ready and an immediate scan comes back empty. */
        vTaskDelay(pdMS_TO_TICKS(300));
        wifi_scan_now();
    }
    wifi_driver_down();
    s_wifi_scan_only = false;
    if (err != ESP_OK) return -1;

    int n = (s_ap_count < max) ? s_ap_count : max;
    for (int i = 0; i < n; i++) {
        snprintf(out[i].ssid, sizeof(out[i].ssid), "%.32s", s_aps[i].ssid);
        out[i].rssi   = s_aps[i].rssi;
        out[i].secure = s_aps[i].secure;
        /* Must be recomputed, not inherited. `out` aliases the live snapshot,
         * which already carries saved bits for the PREVIOUS list — leaving them
         * alone attaches them to whatever SSID now occupies that index. */
        out[i].saved  = known_pass(out[i].ssid) != NULL;
    }
    ESP_LOGI(TAG, "rescan found %d networks", n);
    return n;
}

/* ---- known networks -------------------------------------------------
 * The device used to remember exactly one network, so "saved" could only ever
 * mean the one it was last on — useless for rejoining anywhere else. This keeps
 * a small table instead, the way a phone does.
 *
 * NVS, not the SD card: these are plaintext passwords and the card is
 * removable. KNOWN_MAX x sizeof(known_net_t) is under 1 KB, which NVS holds
 * comfortably as a single blob. */
#define KNOWN_MAX 8
typedef struct { char ssid[33]; char pass[64]; } known_net_t;
static known_net_t s_known[KNOWN_MAX];
static int  s_known_count;
static bool s_known_dirty;              /* needs writing once BLE is down */

static void known_load(void) {
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = sizeof(s_known);
    if (nvs_get_blob(h, "known", s_known, &sz) == ESP_OK) {
        s_known_count = (int)(sz / sizeof(known_net_t));
        if (s_known_count > KNOWN_MAX) s_known_count = KNOWN_MAX;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d known networks", s_known_count);
}

/* Deferred: an NVS commit is a flash erase, and §7g forbids that while the BLE
 * controller is executing from flash. */
static void known_flush(void) {
    if (!s_known_dirty || ble_prov_nvs_blocked()) return;
    nvs_handle_t h;
    if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t e = nvs_set_blob(h, "known", s_known,
                               (size_t)s_known_count * sizeof(known_net_t));
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e == ESP_OK) {
        s_known_dirty = false;
        ESP_LOGI(TAG, "known networks saved (%d)", s_known_count);
    } else {
        /* Dirty flag stays up so the next pass retries. Clearing it here would
         * drop the credential silently, and the user would discover it by being
         * asked for a password they had already given. */
        ESP_LOGW(TAG, "known networks NOT saved: %s", esp_err_to_name(e));
    }
}

static const char *known_pass(const char *ssid) {
    for (int i = 0; i < s_known_count; i++) {
        if (strncmp(s_known[i].ssid, ssid, sizeof(s_known[i].ssid)) == 0) {
            return s_known[i].pass;
        }
    }
    return NULL;
}

static void known_remember(const char *ssid, const char *pass) {
    if (!ssid[0]) return;
    for (int i = 0; i < s_known_count; i++) {
        if (strncmp(s_known[i].ssid, ssid, sizeof(s_known[i].ssid)) != 0) continue;
        /* Move to the end on every join, making eviction least-recently-joined
         * rather than first-ever-added — otherwise "home", added first and used
         * daily, is the first casualty of a ninth network joined once. */
        known_net_t e = s_known[i];
        bool same = strncmp(e.pass, pass, sizeof(e.pass)) == 0;
        snprintf(e.pass, sizeof(e.pass), "%.63s", pass);
        memmove(&s_known[i], &s_known[i + 1],
                (size_t)(s_known_count - i - 1) * sizeof(known_net_t));
        s_known[s_known_count - 1] = e;
        if (!same || i != s_known_count - 1) s_known_dirty = true;
        return;
    }
    if (s_known_count == KNOWN_MAX) {
        memmove(&s_known[0], &s_known[1], (KNOWN_MAX - 1) * sizeof(known_net_t));
        s_known_count--;
    }
    snprintf(s_known[s_known_count].ssid, sizeof(s_known[0].ssid), "%.32s", ssid);
    snprintf(s_known[s_known_count].pass, sizeof(s_known[0].pass), "%.63s", pass);
    s_known_count++;
    s_known_dirty = true;
}

void ble_prov_forget(const char *ssid) {
    for (int i = 0; i < s_known_count; i++) {
        if (strncmp(s_known[i].ssid, ssid, sizeof(s_known[i].ssid)) == 0) {
            memmove(&s_known[i], &s_known[i + 1],
                    (size_t)(s_known_count - i - 1) * sizeof(known_net_t));
            s_known_count--;
            s_known_dirty = true;
            break;
        }
    }
    /* Outside the loop deliberately. Nested inside the table-match branch this
     * was a silent no-op for a network that was the boot credential but had
     * never been added — which was every network on a device that had been
     * working since before the table existed. */
    if (strncmp(s_ssid, ssid, sizeof(s_ssid)) == 0) {
        s_ssid[0] = '\0';
        s_pass[0] = '\0';
        s_ble_forget = true;
        /* Nothing is configured now; say so rather than leaving the reconnect
         * owner hammering an empty SSID forever. */
        s_wifi_disabled = true;
    }
}

void ble_prov_submit(const char *ssid, const char *pass) {
    snprintf(s_ble_ssid, sizeof(s_ble_ssid), "%.32s", ssid);
    if (pass) {
        snprintf(s_ble_pass, sizeof(s_ble_pass), "%.63s", pass);
    } else {
        /* Rejoin with what we already hold for THAT network — any of the
         * known ones, not merely whichever was joined last. */
        const char *k = known_pass(s_ble_ssid);
        if (!k) {
            /* Joining with an empty password would fail, and worse would
             * OVERWRITE the stored credential with "" on the way. Refuse. */
            ESP_LOGW(TAG, "no stored password for \"%s\" — refusing", s_ble_ssid);
            s_ble_ssid[0] = '\0';
            s_ble_pass[0] = '\0';
            s_ble_handoff = true;
            return;
        }
        snprintf(s_ble_pass, sizeof(s_ble_pass), "%.63s", k);
        ESP_LOGI(TAG, "rejoining \"%s\" with stored credentials", s_ble_ssid);
    }
    s_ble_handoff = true;
}

/* Blocking scan — runs on the main task, never in the LVGL task */
static void wifi_scan_now(void) {
    s_ap_count = 0;

    wifi_scan_config_t sc = { .show_hidden = false };
    if (esp_wifi_scan_start(&sc, true) != ESP_OK) {
        ESP_LOGW(TAG, "scan failed to start");
        return;
    }

    uint16_t num = 0;
    esp_wifi_scan_get_ap_num(&num);
    if (num == 0) return;
    if (num > 32) num = 32;

    wifi_ap_record_t *recs = heap_caps_malloc(num * sizeof(wifi_ap_record_t),
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!recs) {
        esp_wifi_clear_ap_list();
        return;
    }
    if (esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
        for (int i = 0; i < num && s_ap_count < MAX_APS; i++) {
            if (recs[i].ssid[0] == '\0') continue;
            bool dup = false;
            for (int j = 0; j < s_ap_count; j++) {
                if (strcmp(s_aps[j].ssid, (char *)recs[i].ssid) == 0) { dup = true; break; }
            }
            if (dup) continue;
            snprintf(s_aps[s_ap_count].ssid, sizeof(s_aps[0].ssid), "%s", (char *)recs[i].ssid);
            s_aps[s_ap_count].rssi = recs[i].rssi;
            s_aps[s_ap_count].secure = recs[i].authmode != WIFI_AUTH_OPEN;
            s_ap_count++;
        }
    }
    free(recs);
    ESP_LOGI(TAG, "scan found %d networks", s_ap_count);
}

/* ---------------- HTTPS (Funnel profile) ---------------- */

/* ---------------- network latency bench (temporary) ----------------
 *
 * A Spotify remote has to feel instant, and our HTTPS calls currently take
 * ~1.5 s. Almost none of that is the request: it is DNS, TCP, and above all
 * verifying a certificate chain with asymmetric crypto on a 240 MHz Xtensa.
 *
 * Aimed at api.spotify.com deliberately, with no Authorization header. The 401
 * costs the same as a 200 everywhere that matters, and it exercises Spotify's
 * real cert chain, cipher suite, edge node and network path — which is what
 * actually sets the number. No credentials needed to measure any of this.
 *
 * Measures the same call two ways: a fresh client each time (what the firmware
 * does today) against one handle reused with keep-alive.
 */
#define NET_BENCH 0        /* flip to 1 to re-measure; see HARDWARE.md 7f */
#define NET_BENCH_URL "https://api.spotify.com/v1/me/player"
#define NET_BENCH_N   5

#if NET_BENCH
static void net_bench_run(void) {
    int64_t cold[NET_BENCH_N], warm[NET_BENCH_N];

    ESP_LOGW(TAG, "=== net bench: %d cold calls (new connection each) ===", NET_BENCH_N);
    for (int i = 0; i < NET_BENCH_N; i++) {
        esp_http_client_config_t cfg = {
            .url = NET_BENCH_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
        };
        int64_t t0 = esp_timer_get_time();
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        int code = 0, clen = 0;
        if (c) {
            esp_http_client_perform(c);
            code = esp_http_client_get_status_code(c);
            clen = (int)esp_http_client_get_content_length(c);
            esp_http_client_cleanup(c);
        }
        cold[i] = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGW(TAG, "  cold[%d] HTTP %d  %d B  %lld ms", i, code, clen, (long long)cold[i]);
        vTaskDelay(pdMS_TO_TICKS(300));
    }

    ESP_LOGW(TAG, "=== net bench: %d warm calls (one connection reused) ===", NET_BENCH_N);
    {
        esp_http_client_config_t cfg = {
            .url = NET_BENCH_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .keep_alive_enable = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        for (int i = 0; i < NET_BENCH_N; i++) {
            int64_t t0 = esp_timer_get_time();
            int code = 0, clen = 0;
            if (c) {
                esp_http_client_perform(c);
                code = esp_http_client_get_status_code(c);
                clen = (int)esp_http_client_get_content_length(c);
            }
            warm[i] = (esp_timer_get_time() - t0) / 1000;
            ESP_LOGW(TAG, "  warm[%d] HTTP %d  %d B  %lld ms", i, code, clen, (long long)warm[i]);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (c) esp_http_client_cleanup(c);
    }

    ESP_LOGW(TAG, "=== net bench: 8 calls at a 3 s poll cadence (one connection) ===");
    {
        esp_http_client_config_t cfg = {
            .url = NET_BENCH_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .keep_alive_enable = true,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        for (int i = 0; i < 8; i++) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            int64_t t0 = esp_timer_get_time();
            int code = 0, clen = 0;
            if (c) {
                esp_http_client_perform(c);
                code = esp_http_client_get_status_code(c);
                clen = (int)esp_http_client_get_content_length(c);
            }
            ESP_LOGW(TAG, "  poll[%d] HTTP %d  %d B  %lld ms", i, code, clen,
                     (long long)((esp_timer_get_time() - t0) / 1000));
        }
        if (c) esp_http_client_cleanup(c);
    }

    int64_t cs = 0, ws = 0;
    for (int i = 0; i < NET_BENCH_N; i++) { cs += cold[i]; ws += warm[i]; }
    /* the first warm call still pays for the handshake, so the steady-state
     * figure is the mean of the rest — that is what a poll would actually cost */
    int64_t ws_steady = 0;
    for (int i = 1; i < NET_BENCH_N; i++) ws_steady += warm[i];

    ESP_LOGW(TAG, "=== net bench result: cold mean %lld ms | warm mean %lld ms | "
                  "warm steady-state %lld ms | heap %u ===",
             (long long)(cs / NET_BENCH_N), (long long)(ws / NET_BENCH_N),
             (long long)(ws_steady / (NET_BENCH_N - 1)),
             (unsigned)hp_free());
}
#endif

/* The network task.
 *
 * Once polled a stand-in "speech to text" endpoint every 45 s to characterise
 * TLS cost. That measurement is long done and recorded in HARDWARE.md 7f, so the
 * poll was pure waste: a TLS handshake, a log line and a UI string, several
 * times a minute, for information nobody read.
 *
 * Its stack lives in PSRAM. As plain xTaskCreate it took 8 KB of internal SRAM —
 * the scarcest pool on the board — for a task that does nothing time-critical
 * and never runs with the cache disabled.
 */
static void net_task(void *arg) {
    while (1) {
        xEventGroupWaitBits(s_evt, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        /* Wallpapers are this task's only remaining job. Off the main loop, so
         * a slow download cannot stall button handling or the app switcher. */
        wall_service();

        /* A wallpaper request breaks the wait, so pressing Fetch acts now
         * instead of sitting until the next cycle. */
        int period = s_doze ? (10 * 60 * 1000) : HTTPS_PERIOD_MS;
        for (int w = 0; w < period / 100 && !s_req_wallpaper; w++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
}

/* ---------------- AXP2101: fuel gauge + PWRKEY ---------------- */

static esp_err_t axp_read(uint8_t reg, uint8_t *val) {
    return i2c_master_transmit_receive(s_axp, &reg, 1, val, 1, 100);
}

static esp_err_t axp_write(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_axp, buf, 2, 100);
}

static esp_err_t axp_set_bit(uint8_t reg, uint8_t bit) {
    uint8_t v;
    esp_err_t err = axp_read(reg, &v);
    if (err != ESP_OK) return err;
    return axp_write(reg, (uint8_t)(v | (1u << bit)));
}

static void pmu_init(void) {
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "no I2C bus, PMU features disabled");
        return;
    }
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dev, &s_axp) != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 not reachable, PMU features disabled");
        s_axp = NULL;
        return;
    }
    /* all three are needed before 0xA4 reports anything but zero */
    axp_set_bit(AXP_REG_GAUGE_CTRL, 3);
    axp_set_bit(AXP_REG_BAT_DET_CTRL, 0);
    axp_set_bit(AXP_REG_ADC_CH_CTRL, 0);
    /* PWRKEY short press -> IRQ status bit we poll */
    axp_set_bit(AXP_REG_INTEN2, AXP_PKEY_SHORT_BIT);
    axp_write(AXP_REG_INTSTS2, 1u << AXP_PKEY_SHORT_BIT);
    /* The PMU powers up at 25 mA, which barely charges the cell at all.
     * 400 mA is what Waveshare's own AXP2101 example programs. */
    uint8_t icc = 0, cv = 0, ctrl = 0;
    axp_read(0x62, &icc);
    axp_write(0x62, (uint8_t)((icc & 0xE0) | 10));      /* step 10 = 400 mA */
    axp_read(0x62, &icc); axp_read(0x64, &cv); axp_read(0x18, &ctrl);
    int step = icc & 0x1F;
    int ma = (step <= 8) ? step * 25 : 200 + (step - 8) * 100;
    ESP_LOGI(TAG, "AXP2101 ready — charge current %d mA (ICC=0x%02x) CV=0x%02x CTRL=0x%02x",
             ma, icc, cv, ctrl);
}

static void battery_poll(void) {
    if (!s_axp) return;

    uint8_t st1 = 0, st2 = 0, pct = 0, hi = 0, lo = 0;
    if (axp_read(AXP_REG_STATUS1, &st1) != ESP_OK) return;
    axp_read(AXP_REG_STATUS2, &st2);
    s_batt_charging = ((st2 >> 5) & 0x03) == 0x01;

    if (!((st1 >> 3) & 0x01)) {          /* no battery on the connector */
        s_batt_pct = -1;
        s_batt_mv = 0;
        return;
    }
    if (axp_read(AXP_REG_ADC_DATA_H, &hi) == ESP_OK &&
        axp_read(AXP_REG_ADC_DATA_L, &lo) == ESP_OK) {
        s_batt_mv = ((hi & 0x1F) << 8) | lo;
    }
    if (axp_read(AXP_REG_BAT_PERCENT, &pct) == ESP_OK && pct > 0 && pct <= 100) {
        s_batt_pct = pct;
    } else if (s_batt_mv > 2500) {
        s_batt_pct = clampi((s_batt_mv - 3300) * 100 / 900, 0, 100);
    } else {
        s_batt_pct = 0;
    }
}

static bool pmu_pwrkey_pressed(void) {
    if (!s_axp) return false;
    uint8_t sts = 0;
    if (axp_read(AXP_REG_INTSTS2, &sts) != ESP_OK) return false;
    if (sts & (1u << AXP_PKEY_SHORT_BIT)) {
        axp_write(AXP_REG_INTSTS2, 1u << AXP_PKEY_SHORT_BIT);  /* write 1 to clear */
        return true;
    }
    return false;
}


/* ---------------- QMI8658 IMU: screen autorotate ----------------
 *
 * The 6-axis IMU sits on the same I2C bus as the PMU and the touch panel.
 * Only the accelerometer is needed to know which way is down; the gyro stays
 * powered off. Rotation is applied at the panel (MADCTL via
 * bsp_display_rotation_set) which turns the entire framebuffer, and the touch
 * axes are re-mapped to match. The display is square, so nothing has to be
 * re-laid out.
 */

#define QMI_REG_WHOAMI   0x00
#define QMI_REG_CTRL1    0x02
#define QMI_REG_CTRL2    0x03
#define QMI_REG_CTRL7    0x08
#define QMI_REG_AX_L     0x35
#define QMI_TILT_TH      6500      /* ~0.4 g at 16384 LSB/g (+-2 g range) */
#define QMI_TILT_MARGIN  3000      /* ~0.18 g the winning axis must lead by */
#define QMI_VOTES_NEEDED 8         /* x100 ms of agreement before rotating */

extern esp_lcd_touch_handle_t bsp_touch_handle(void);

static i2c_master_dev_handle_t s_imu;
static int s_rot;          /* quarter turns from native, 0..3 */
static int s_rot_cand;
static int s_base_rot;     /* what the accelerometer alone suggests */
/* Two unknowns remain, and they are different shapes:
 *   - which tilt corresponds to which quarter turn  (an OFFSET, 4 values)
 *   - whether the compensation turns the same way    (HANDEDNESS, 2 values)
 * An offset alone can never fix inverted handedness: flipping direction leaves
 * 0 and 180 correct while swapping 90 and 270, which shows up as "two of the
 * four orientations are right". So the knob covers all 8 combinations:
 * bit 2 = invert handedness, bits 1:0 = offset. Saved to NVS. */
static int s_rot_cfg;
#define ROT_CFG_COUNT 8
static int s_rot_votes;

/* Autorotate is switchable from CONTROL. The gate belongs at the commit and not
 * at the poll, for the same reason the FOCUS gate does: s_base_rot has a second
 * consumer that still needs live orientation when the panel is pinned. */
static volatile bool s_autorot = true;   /* volatile: LVGL task writes, main reads */
static volatile bool s_req_autorot_save;
/* The orientation to hold when autorotate is off. Without persisting this, a
 * reboot lands back at native 0 with the switch still reading OFF and — since
 * the calibration button fades out in that state — no way at all to get back. */
static int s_rot_held;
static int s_acc_x, s_acc_y, s_acc_z;

/* Rotation is quarter turns from the panel's NATIVE state.
 *
 * The CO5300 init sequence programs MADCTL = 0xA0, i.e. swap_xy + mirror_y,
 * and the BSP's touch flags match it. Feeding those native flags through the
 * rotation logic in Espressif's esp_lvgl_port (lvgl_port_disp_rotation_update)
 * gives the sequence below. Note it runs 0xA0 -> 0xC0 -> 0x60 -> 0x00; getting
 * that direction backwards leaves 0 and 180 looking right while 90 and 270
 * swap places, which is the classic "two of four orientations are wrong".
 *
 * Panel and touch take the SAME flags: both describe the mapping between LVGL
 * space and physical space, so they must move together.
 */
static const struct { bool swap, mx, my; } s_rot_tbl[4] = {
    { true,  false, true  },   /* r=0   native, MADCTL 0xA0 */
    { false, true,  true  },   /* r=90         MADCTL 0xC0 */
    { true,  true,  false },   /* r=180        MADCTL 0x60 */
    { false, false, false },   /* r=270        MADCTL 0x00 */
};

extern esp_lcd_panel_handle_t bsp_display_panel_handle(void);

static esp_err_t imu_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_imu, b, 2, 100);
}

/* Flip to 1 to make the probe report failure, so CONTROL's no-sensor path can be
 * checked without unsoldering anything. That branch never runs on a healthy
 * board, which is exactly what makes it the one most likely to be wrong. */
#define IMU_FORCE_ABSENT 0

static void imu_init(void) {
#if IMU_FORCE_ABSENT
    ESP_LOGW(TAG, "QMI8658 probe forced absent (IMU_FORCE_ABSENT)");
    return;
#endif
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return;

    const uint8_t addrs[2] = { 0x6B, 0x6A };      /* SA0 low / high */
    for (int i = 0; i < 2; i++) {
        i2c_device_config_t dev = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = addrs[i],
            .scl_speed_hz = 200000,
        };
        if (i2c_master_bus_add_device(bus, &dev, &s_imu) != ESP_OK) continue;

        uint8_t reg = QMI_REG_WHOAMI, who = 0;
        if (i2c_master_transmit_receive(s_imu, &reg, 1, &who, 1, 100) == ESP_OK &&
            who == 0x05) {
            imu_write(QMI_REG_CTRL1, 0x40);       /* auto-increment reads */
            imu_write(QMI_REG_CTRL2, 0x06);       /* accel +/-2 g, 125 Hz */
            imu_write(QMI_REG_CTRL7, 0x01);       /* accel on, gyro off */
            ESP_LOGI(TAG, "QMI8658 found at 0x%02X — autorotate enabled", addrs[i]);
            return;
        }
        i2c_master_bus_rm_device(s_imu);
        s_imu = NULL;
    }
    ESP_LOGW(TAG, "QMI8658 not found — autorotate disabled");
}

static void rotation_apply(int r) {
    r &= 3;
    if (!ui_lock()) return;

    esp_lcd_panel_handle_t panel = bsp_display_panel_handle();
    if (panel) {
        esp_lcd_panel_swap_xy(panel, s_rot_tbl[r].swap);
        esp_lcd_panel_mirror(panel, s_rot_tbl[r].mx, s_rot_tbl[r].my);
    }
    esp_lcd_touch_handle_t tp = bsp_touch_handle();
    if (tp) {
        esp_lcd_touch_set_swap_xy(tp,  s_rot_tbl[r].swap);
        esp_lcd_touch_set_mirror_x(tp, s_rot_tbl[r].mx);
        esp_lcd_touch_set_mirror_y(tp, s_rot_tbl[r].my);
    }
    lv_obj_invalidate(lv_screen_active());       /* old frame is now scrambled */
    bsp_display_unlock();

    s_rot = r;
    ESP_LOGI(TAG, "rotation -> %d deg (swap=%d mx=%d my=%d) ax=%d ay=%d az=%d",
             r * 90, s_rot_tbl[r].swap, s_rot_tbl[r].mx, s_rot_tbl[r].my,
             s_acc_x, s_acc_y, s_acc_z);
}

static void rot_off_save(void) {
    /* Called straight from a UI callback, so it needs its own guard: an NVS
     * commit is a flash erase and the BLE controller runs from flash. The
     * calibration stays in RAM and is written on the next change after the
     * session ends. */
    if (ble_prov_nvs_blocked()) return;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "rotcfg", s_rot_cfg);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Runs on the main task, so s_rot is read at commit time rather than at the
 * moment the switch was tapped — which is the more accurate answer anyway. */
static void autorot_save(void) {
    if (ble_prov_nvs_blocked()) return;
    if (!s_autorot) s_rot_held = s_rot;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "autorot", s_autorot ? 1 : 0);
        nvs_set_i32(h, "rothold", s_rot_held);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Both display-orientation settings, on one handle: this runs once at boot. */
static void rot_off_load(void) {
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "rotcfg", &v) == ESP_OK) s_rot_cfg = (int)(v & 7);
    if (nvs_get_i32(h, "autorot", &v) == ESP_OK) s_autorot = (v != 0);
    if (nvs_get_i32(h, "rothold", &v) == ESP_OK) s_rot_held = (int)(v & 3);
    nvs_close(h);
}

/* gravity quadrant -> quarter turns, through the current calibration */
static int rot_from_base(int base) {
    if (s_rot_cfg & 4) base = (4 - base) & 3;      /* invert handedness */
    return (base + (s_rot_cfg & 3)) & 3;
}

/* right button in the SYSTEM app walks all 8 calibrations */
static void rotation_bump(void) {
    s_rot_cfg = (s_rot_cfg + 1) % ROT_CFG_COUNT;
    rot_off_save();
    s_rot_votes = 0;
    ESP_LOGI(TAG, "rotation cfg -> %d/8 (offset %d, %s)",
             s_rot_cfg, s_rot_cfg & 3,
             (s_rot_cfg & 4) ? "reversed" : "normal");
    rotation_apply(rot_from_base(s_base_rot));
}

static void imu_poll(void) {
    if (!s_imu) return;

    uint8_t reg = QMI_REG_AX_L, d[6];
    if (i2c_master_transmit_receive(s_imu, &reg, 1, d, 6, 100) != ESP_OK) return;
    s_acc_x = (int16_t)((d[1] << 8) | d[0]);
    s_acc_y = (int16_t)((d[3] << 8) | d[2]);
    s_acc_z = (int16_t)((d[5] << 8) | d[4]);

    /* The dominant axis must lead the other by a clear margin. Held near 45
     * degrees the two axes trade places on noise alone, and the vote counter
     * cannot help — it happily counts eight samples of a wrong answer. */
    int ax = abs(s_acc_x), ay = abs(s_acc_y);
    if (ax > QMI_TILT_TH || ay > QMI_TILT_TH) {
        if (ax > ay + QMI_TILT_MARGIN) {
            s_base_rot = (s_acc_x > 0) ? 1 : 3;
        } else if (ay > ax + QMI_TILT_MARGIN) {
            s_base_rot = (s_acc_y > 0) ? 2 : 0;
        }
        /* ambiguous: keep the orientation we already have */
    }
    int want = rot_from_base(s_base_rot);

    if (want != s_rot_cand) { s_rot_cand = want; s_rot_votes = 0; }
    else if (s_rot_votes < QMI_VOTES_NEEDED) s_rot_votes++;

    /* The FOCUS app reads orientation as input and draws fixed-position labels;
     * counter-rotating the panel under it would cancel the whole gesture. Gate
     * the commit, not the poll, so s_base_rot stays live for it to read — and
     * the user's autorotate switch gates the same place for the same reason:
     * FOCUS must keep working as a dial with the panel pinned.
     *
     * Nothing is re-armed when the switch goes back on. The vote counter kept
     * running while it was off, so an already-settled orientation commits on the
     * next poll rather than 800 ms later. */
    if (s_autorot && s_rot_votes >= QMI_VOTES_NEEDED && s_rot_cand != s_rot &&
        s_app != APP_POMO) {
        rotation_apply(s_rot_cand);
    }
}

/* ---------------- GPIO keys with short/long press ---------------- */

/* BTN_REPEAT fires over and over while a key stays down, after the long press.
 * Only MUSIC's volume keys want it; every other consumer tests for BTN_SHORT or
 * BTN_LONG specifically and so ignores repeats without needing to know they
 * exist. */
typedef enum { BTN_NONE = 0, BTN_SHORT, BTN_LONG, BTN_REPEAT } btn_ev_t;

#define BTN_REPEAT_MS 130

typedef struct {
    gpio_num_t pin;
    int stable, last_raw;
    int64_t change_ms, press_ms, repeat_ms;
    bool long_fired;
} btn_t;

static btn_t s_key_left  = { .pin = KEY_LEFT_GPIO,  .stable = 1, .last_raw = 1 };
static btn_t s_key_right = { .pin = KEY_RIGHT_GPIO, .stable = 1, .last_raw = 1 };

static void btn_init(btn_t *b) {
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << b->pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
}

/* Adopt the pin's current level without emitting an event, so the release
 * after a wake-up press doesn't also trigger that button's action. */
static void btn_swallow(btn_t *b) {
    b->last_raw = b->stable = gpio_get_level(b->pin);
    b->long_fired = true;
    b->change_ms = 0;
}

static btn_ev_t btn_poll(btn_t *b, int64_t t) {
    int raw = gpio_get_level(b->pin);
    if (raw != b->last_raw) {
        b->last_raw = raw;
        b->change_ms = t;
        ESP_LOGI(TAG, "key gpio%d -> %d", (int)b->pin, raw);
        return BTN_NONE;
    }
    if (raw != b->stable && (t - b->change_ms) >= 50) {   /* debounced edge */
        b->stable = raw;
        if (raw == 0) {                                   /* pressed */
            b->press_ms = t;
            b->long_fired = false;
            b->repeat_ms = 0;
        } else if (!b->long_fired) {                      /* released, short */
            return BTN_SHORT;
        }
        return BTN_NONE;
    }
    if (b->stable == 0) {
        /* fire the long press while still held, so it feels immediate */
        if (!b->long_fired && (t - b->press_ms) >= LONG_PRESS_MS) {
            b->long_fired = true;
            b->repeat_ms = t;
            return BTN_LONG;
        }
        /* then keep firing while it stays down. repeat_ms stays zero after
         * btn_swallow(), so a key already held at wake-up does not auto-repeat
         * its way through a volume ramp the moment the screen lights up. */
        if (b->long_fired && b->repeat_ms && (t - b->repeat_ms) >= BTN_REPEAT_MS) {
            b->repeat_ms = t;
            return BTN_REPEAT;
        }
    }
    return BTN_NONE;
}

/* ---------------- display brightness ----------------
 *
 * The panel ran at 100% and nothing could change it: both
 * bsp_display_brightness_init() and bsp_display_backlight_on() hardcode it. That
 * is painful in a dark room and it is the largest power draw on the board — on an
 * AMOLED every lit pixel costs current, so this is a battery control as much as a
 * comfort one.
 *
 * BRIGHT_MIN is not cosmetic. bsp_display_brightness_set(0) blanks the panel
 * outright, and the only control that could undo it would be a slider you can no
 * longer see. There is no other way into this device, so the range stops at a
 * level that is dim but still legible.
 */
#define BRIGHT_MIN     10
#define BRIGHT_DEFAULT 100

/* volatile: published by the LVGL task from the slider callback, consumed by the
 * main task. Pitfall #19 — the compiler is free to sink a plain store past the
 * volatile flag that announces it. */
static volatile int  s_bright = BRIGHT_DEFAULT;
static volatile bool s_req_bright_apply;   /* the main loop owns the panel write */
static volatile bool s_req_bright_save;
static int s_bright_applied = -1;          /* what 0x51 actually holds; -1 = unknown */

static void bright_load(void) {
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "bright", &v) == ESP_OK)
        s_bright = clampi((int)v, BRIGHT_MIN, 100);
    nvs_close(h);
}

static void bright_save(void) {
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "bright", s_bright);
    nvs_commit(h);
    nvs_close(h);
}

/* Setting brightness is panel command 0x51 over the same QSPI device the LVGL
 * task flushes on — pitfall #13 IO, not a GPIO — so it takes the lock. Safe from
 * anywhere: the mutex is recursive, so a caller that already holds it just bumps
 * the count. What the lock buys is exclusion between TASKS; panel_io_spi_tx_param
 * drains any in-flight flush DMA before it writes, and that is only correct if
 * nothing else is issuing on the bus meanwhile.
 *
 * THE ONLY writer of 0x51 in this file, which is what makes s_bright_applied
 * trustworthy. Route the screen-off path through here too (bright_apply(0)) — a
 * bare bsp_display_backlight_off() would leave the cache reading 60 while the
 * panel sat at 0, and the next wake would then skip the write and stay black.
 *
 * Returns false only on a lock timeout, so callers can retry rather than strand
 * the panel at whatever it last wrote. */
static bool bright_apply(int pct) {
    pct = clampi(pct, 0, 100);
    if (pct == s_bright_applied) return true;
    if (!ui_lock()) return false;
    bsp_display_brightness_set(pct);
    bsp_display_unlock();
    s_bright_applied = pct;
    return true;
}

/* ---------------- screen switching ---------------- */

static void screen_toggle_power(void) {
    s_screen_on = !s_screen_on;

    /* The backlight calls are panel IO, not a GPIO: bsp_display_backlight_off()
     * writes panel command 0x51 over the same QSPI device the LVGL task flushes
     * on (§7b). Driving that from this task unlocked is pitfall #13, and it
     * deadlocked a board — the LVGL task blocked forever in
     * spi_device_acquire_bus() inside panel_io_spi_tx_param() while holding the
     * LVGL lock, so every other ui_lock() then timed out and the UI froze while
     * the main loop kept running and logging.
     *
     * power_set_doze() already took the lock for its own panel command; these
     * two were missed. Each takes it around its own call, which is a style
     * choice and not a constraint — the mutex IS recursive
     * (xSemaphoreCreateRecursiveMutex, esp_lv_adapter.c:601), as ui_lock()'s
     * comment says. An earlier version of this comment claimed the opposite and
     * was wrong; what mattered was never recursion, it was that two TASKS must
     * not drive the QSPI device at once. */
    if (s_screen_on) {
        /* The user's level, not bsp's hardcoded 100. If the lock times out the
         * panel stays dark while every flag says the screen is on, and nothing
         * would ever retry — so hand it to the main loop instead of losing it. */
        power_set_doze(false);
        if (!bright_apply(s_bright)) s_req_bright_apply = true;
    } else {
        bright_apply(0);          /* not backlight_off(): see bright_apply() */
        power_set_doze(true);
    }
    ESP_LOGI(TAG, "screen %s", s_screen_on ? "ON" : "OFF");
}

/* ---------------- pet: a tiny planet ----------------
 *
 * Full-screen scene rather than a centred blob. A little astronaut lives on a
 * small world at the bottom of the display, with deep space above it. It walks
 * around on its own, and every few seconds picks a new thing to do: stargaze,
 * dance, nap, wave, chase a snack that drops out of the sky, or watch a rocket
 * go up. Ambient events (twinkling stars, shooting stars, a passing UFO) run
 * independently so the scene is never still.
 *
 * Palette is flat retro — slate / teal / sand / orange / sienna on deep space,
 * no gradients on the character, no pixel art.
 *
 * One 25 ms timer drives it all through a frame counter; motion is
 * translate / size / opacity only, so the redraw stays cheap.
 */

#define PET_FPS_MS   25

/* the planet is a big circle whose top cap forms the horizon */
#define PLANET_CX    240
#define PLANET_CY    510
#define PLANET_R     270
#define WALK_MIN_X   120
#define WALK_MAX_X   360

#define CH_W         54
#define CH_H         62

typedef enum {
    ACT_IDLE = 0, ACT_WALK, ACT_EAT, ACT_DANCE, ACT_NAP,
    ACT_STARGAZE, ACT_WAVE, ACT_WATCH_ROCKET, ACT_JUMP
} act_t;

static act_t s_act;
static int s_aframe;          /* frames in the current activity */
static int s_adur;            /* how long it lasts */
static int s_fcount;          /* free-running frame counter */
static int s_next_pick;       /* frame at which to choose a new activity */

static int s_cx = 240;        /* character position along the surface */
static int s_tx = 240;        /* walk target */
static int s_face = 1;        /* +1 right, -1 left */
static bool s_walking_to_food;
static int  s_walk_speed = 2;

/* scene objects */
#define STAR_N 12
static lv_obj_t *s_star[STAR_N];
static uint8_t   s_star_ph[STAR_N];
static lv_obj_t *s_moon;
static lv_obj_t *s_shoot;
static int s_shoot_life;
static lv_obj_t *s_ufo;
static int s_ufo_x, s_ufo_life;
static lv_obj_t *s_rocket, *s_flame;
static int s_rocket_life;
static lv_obj_t *s_food_item;
static int s_food_x, s_food_fall;
static bool s_food_ready;

/* character parts */
static lv_obj_t *s_ch_wrap, *s_ch_body, *s_ch_pack, *s_ch_visor;
static lv_obj_t *s_ch_eye_l, *s_ch_eye_r, *s_ch_leg_l, *s_ch_leg_r;
static lv_obj_t *s_ch_arm_l, *s_ch_arm_r, *s_ch_ant, *s_ch_antdot;
static lv_obj_t *s_bubble;
static int s_bubble_life;

/* HUD */
static lv_obj_t *s_pet_name, *s_pet_mood;
static lv_obj_t *s_bar_food, *s_bar_fun, *s_bar_nrg;

static int isin(int deg, int amp) {
    while (deg < 0) deg += 360;
    return (int)((lv_trigo_sin((int16_t)(deg % 360)) * amp) / 32767);
}

static int rnd(int n) { return (int)(esp_random() % (uint32_t)n); }

/* surface height under a given x */
static int ground_y(int x) {
    int dx = x - PLANET_CX;
    int r2 = PLANET_R * PLANET_R - dx * dx;
    if (r2 < 0) r2 = 0;
    return PLANET_CY - (int)sqrtf((float)r2);
}

static const char *pet_mood_text(int *worst) {
    int m = s_food;
    const char *s = "hungry";
    if (s_fun < m) { m = s_fun; s = "bored"; }
    if (s_nrg < m) { m = s_nrg; s = "sleepy"; }
    *worst = m;
    if (m > 60) return "having a good day";
    if (m > 30) return s;
    return (m == s_food) ? "starving!" : (m == s_fun ? "lonely!" : "exhausted!");
}

static void say(const char *txt) {
    if (!s_bubble) return;
    lv_label_set_text(s_bubble, txt);
    lv_obj_remove_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    s_bubble_life = 50;
}

static void set_act(act_t a, int dur) {
    s_act = a;
    s_aframe = 0;
    s_adur = dur;
}

static void walk_to(int x) {
    s_walk_speed = 2;
    s_tx = x < WALK_MIN_X ? WALK_MIN_X : (x > WALK_MAX_X ? WALK_MAX_X : x);
    s_face = (s_tx >= s_cx) ? 1 : -1;
    set_act(ACT_WALK, 600);
}

static void drop_food(void) {
    s_food_x = WALK_MIN_X + rnd(WALK_MAX_X - WALK_MIN_X);
    s_food_fall = 0;
    s_food_ready = false;
    lv_obj_remove_flag(s_food_item, LV_OBJ_FLAG_HIDDEN);
    s_walking_to_food = true;
    walk_to(s_food_x);
    say("snack!");
}

static void launch_rocket(void) {
    s_rocket_life = 110;
    lv_obj_remove_flag(s_rocket, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_flame, LV_OBJ_FLAG_HIDDEN);
    set_act(ACT_WATCH_ROCKET, 110);
    say("whoa");
}

/* choose the next thing to do — this is what keeps the scene alive */
static void pick_activity(void) {
    int r = rnd(100);
    if (r < 26)      walk_to(WALK_MIN_X + rnd(WALK_MAX_X - WALK_MIN_X));
    else if (r < 40) { set_act(ACT_STARGAZE, 130); say("..."); }
    else if (r < 52) { set_act(ACT_WAVE, 70);      say("hi!"); }
    else if (r < 63) { set_act(ACT_DANCE, 120); }
    else if (r < 72) { set_act(ACT_NAP, 200);      say("zzz"); }
    else if (r < 85) drop_food();
    else if (r < 94) launch_rocket();
    else             set_act(ACT_IDLE, 80);
}

/* ---- actions from the keys / touch buttons ---- */
static void pet_feed(void) {
    s_food = clampi(s_food + 18, 0, 100);
    s_pet_dirty = true;
    drop_food();
    log_event("pet fed");
}
static void pet_play(void) {
    s_fun = clampi(s_fun + 20, 0, 100);
    s_nrg = clampi(s_nrg - 8, 0, 100);
    s_pet_dirty = true;
    set_act(ACT_DANCE, 140);
    say("yay!");
    log_event("pet danced");
}
static void pet_rest(void) {
    s_nrg = clampi(s_nrg + 22, 0, 100);
    s_pet_dirty = true;
    set_act(ACT_NAP, 200);
    say("zzz");
    log_event("pet napped");
}

/* decay + age, runs whichever screen is showing so the pet really lives */
static void pet_tick(void) {
    s_food = clampi(s_food - 1, 0, 100);
    s_fun  = clampi(s_fun  - 1, 0, 100);
    s_nrg  = clampi(s_nrg  - 1, 0, 100);
    static uint32_t sec_acc;
    sec_acc += PET_TICK_MS / 1000;
    while (sec_acc >= 60) { sec_acc -= 60; s_pet_age_min++; }
    s_pet_dirty = true;
}

/* tap empty space to send it there, tap the astronaut to pet it,
 * double-tap anywhere to make it dance */
static void scene_tap_cb(lv_event_t *e) {
    static int64_t last_tap;
    int64_t now = now_ms();
    bool dbl = (now - last_tap) < 400;
    last_tap = now;

    if (dbl) {
        s_fun = clampi(s_fun + 8, 0, 100);
        s_pet_dirty = true;
        set_act(ACT_DANCE, 140);
        say("wheee!");
        return;
    }

    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (abs(p.x - s_cx) < 46 && p.y > 180) {          /* on the astronaut */
        s_fun = clampi(s_fun + 4, 0, 100);
        s_pet_dirty = true;
        set_act(ACT_WAVE, 70);
        say("hehe");
    } else {
        walk_to(p.x);
    }
}

/* swipe: up = jump, left/right = dash, down = home */
static void pet_gesture_cb(lv_event_t *e) {
    switch (lv_indev_get_gesture_dir(lv_indev_active())) {
    case LV_DIR_BOTTOM:
        app_request(APP_DRAWER);
        break;
    case LV_DIR_TOP:
        set_act(ACT_JUMP, 34);
        say("hop!");
        break;
    case LV_DIR_LEFT:
        walk_to(WALK_MIN_X);
        s_walk_speed = 6;
        say("zoom");
        break;
    case LV_DIR_RIGHT:
        walk_to(WALK_MAX_X);
        s_walk_speed = 6;
        say("zoom");
        break;
    default:
        break;
    }
}

static void pet_timer_cb(lv_timer_t *t) {
    if (!s_ch_wrap) return;
    s_fcount++;
    s_aframe++;

    int worst;
    const char *mood = pet_mood_text(&worst);

    /* ---------- ambient sky ---------- */
    for (int i = 0; i < STAR_N; i++) {
        if ((s_fcount + s_star_ph[i]) % 5) continue;      /* stagger the work */
        int ph = (s_fcount * 2 + s_star_ph[i] * 11) % 360;
        lv_obj_set_style_opa(s_star[i], (lv_opa_t)(150 + isin(ph, 100)), 0);
    }

    if (s_shoot_life > 0) {
        s_shoot_life--;
        int k = 46 - s_shoot_life;
        lv_obj_set_style_translate_x(s_shoot, k * 9, 0);
        lv_obj_set_style_translate_y(s_shoot, k * 4, 0);
        lv_obj_set_style_opa(s_shoot, (lv_opa_t)(255 - (k * 255) / 46), 0);
        if (!s_shoot_life) lv_obj_add_flag(s_shoot, LV_OBJ_FLAG_HIDDEN);
    } else if (rnd(700) == 0) {
        s_shoot_life = 46;
        lv_obj_set_y(s_shoot, 40 + rnd(70));
        lv_obj_set_x(s_shoot, 30 + rnd(120));
        lv_obj_remove_flag(s_shoot, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_ufo_life > 0) {
        s_ufo_life--;
        s_ufo_x += 3;
        lv_obj_set_x(s_ufo, s_ufo_x);
        lv_obj_set_y(s_ufo, 96 + isin(s_ufo_life * 5, 12));
        if (!s_ufo_life) lv_obj_add_flag(s_ufo, LV_OBJ_FLAG_HIDDEN);
    } else if (rnd(1400) == 0) {
        s_ufo_life = 190;
        s_ufo_x = -70;
        lv_obj_remove_flag(s_ufo, LV_OBJ_FLAG_HIDDEN);
    }

    if (s_rocket_life > 0) {
        s_rocket_life--;
        int k = 110 - s_rocket_life;
        lv_obj_set_x(s_rocket, 340);
        lv_obj_set_y(s_rocket, 250 - k * 3);
        lv_obj_set_x(s_flame, 348);
        lv_obj_set_y(s_flame, 250 - k * 3 + 30);
        lv_obj_set_height(s_flame, 10 + rnd(12));
        if (!s_rocket_life) {
            lv_obj_add_flag(s_rocket, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_flame, LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* falling snack */
    if (!lv_obj_has_flag(s_food_item, LV_OBJ_FLAG_HIDDEN)) {
        int gy = ground_y(s_food_x) - 14;
        int fy = 60 + s_food_fall * 6;
        if (fy >= gy) { fy = gy; s_food_ready = true; }
        else          { s_food_fall++; }
        lv_obj_set_x(s_food_item, s_food_x - 9);
        lv_obj_set_y(s_food_item, fy);
    }

    /* ---------- character pose ---------- */
    int bob = 0, lean = 0, leg_l = 0, leg_r = 0, arm = 0;
    int eye_h = 12, squash = 0;

    switch (s_act) {
    case ACT_WALK: {
        int d = s_tx - s_cx;
        if (abs(d) <= s_walk_speed) {
            s_cx = s_tx;
            if (s_walking_to_food && s_food_ready) { s_walking_to_food = false; set_act(ACT_EAT, 90); }
            else                                    { set_act(ACT_IDLE, 40); }
        } else {
            s_cx += (d > 0) ? s_walk_speed : -s_walk_speed;
            s_face = (d > 0) ? 1 : -1;
        }
        int step = (s_fcount * 22) % 360;
        leg_l = isin(step, 7);
        leg_r = -leg_l;
        arm   = -leg_l;
        bob   = -abs(isin(step * 2, 3));
        break;
    }
    case ACT_EAT:
        if (s_aframe == 4) { say("yum!"); }
        if (s_aframe == 10) {
            lv_obj_add_flag(s_food_item, LV_OBJ_FLAG_HIDDEN);
            s_food = clampi(s_food + 10, 0, 100);
            s_pet_dirty = true;
        }
        squash = isin(s_aframe * 40, 5);
        eye_h  = 4;
        break;

    case ACT_DANCE:
        bob    = -abs(isin(s_aframe * 26, 14));
        lean   = isin(s_aframe * 13, 9);
        arm    = isin(s_aframe * 26, 12);
        leg_l  = isin(s_aframe * 26, 6);
        leg_r  = -leg_l;
        eye_h  = 4;
        squash = isin(s_aframe * 26, 4);
        if (s_aframe % 30 == 0) say("~*~");
        break;

    case ACT_NAP:
        eye_h  = 2;
        squash = isin(s_aframe * 4, 6);
        if (s_aframe % 60 == 20) say("z");
        break;

    case ACT_STARGAZE:
        eye_h = 14;
        bob   = isin(s_aframe * 3, 3);
        lean  = -6;
        break;

    case ACT_WAVE:
        arm  = 26 + isin(s_aframe * 34, 14);
        bob  = isin(s_aframe * 8, 3);
        eye_h = 4;
        break;

    case ACT_JUMP: {
        int u = s_aframe - 17;
        bob = -(48 - (u * u * 48) / 289);            /* parabola, peak -48 */
        squash = (s_aframe < 4 || s_aframe > 30) ? 10 : -8;
        eye_h = 22;
        break;
    }

    case ACT_WATCH_ROCKET:
        lean = 7;
        eye_h = 15;
        break;

    default:                                    /* ACT_IDLE */
        bob = isin(s_fcount * 4, 3);
        if (s_fcount % 130 < 4) eye_h = 3;      /* blink */
        if (worst <= 30) { eye_h = 6; lean = -4; }
        break;
    }

    if (s_act != ACT_WALK && s_act != ACT_IDLE && s_aframe >= s_adur) {
        set_act(ACT_IDLE, 40);
    }
    /* when nothing is going on, pick something to do */
    if (s_act == ACT_IDLE && s_fcount >= s_next_pick) {
        s_next_pick = s_fcount + 150 + rnd(220);
        pick_activity();
    }
    if (s_act != ACT_IDLE) s_next_pick = s_fcount + 150 + rnd(220);

    /* ---- place the character on the surface ---- */
    int gy = ground_y(s_cx);
    lv_obj_set_x(s_ch_wrap, s_cx - CH_W / 2 + lean);
    lv_obj_set_y(s_ch_wrap, gy - CH_H + bob);

    static int p_eye = -1, p_ll = 999, p_arm = 999, p_sq = 999, p_face = 0;
    if (eye_h != p_eye) {
        lv_obj_set_height(s_ch_eye_l, eye_h);
        lv_obj_set_height(s_ch_eye_r, eye_h);
        p_eye = eye_h;
    }
    if (leg_l != p_ll) {
        lv_obj_set_style_translate_y(s_ch_leg_l, leg_l, 0);
        lv_obj_set_style_translate_y(s_ch_leg_r, leg_r, 0);
        p_ll = leg_l;
    }
    if (arm != p_arm) {
        lv_obj_set_style_translate_y(s_ch_arm_l, -arm, 0);
        lv_obj_set_style_translate_y(s_ch_arm_r, arm, 0);
        p_arm = arm;
    }
    if (squash != p_sq) {
        lv_obj_set_size(s_ch_body, CH_W - 8 + squash, 40 - squash);
        p_sq = squash;
    }
    if (s_face != p_face) {                       /* shift the visor to "face" a way */
        lv_obj_align(s_ch_visor, LV_ALIGN_TOP_MID, s_face * 3, 6);
        p_face = s_face;
    }

    /* antenna light blinks slowly */
    if (s_fcount % 20 == 0) {
        lv_obj_set_style_bg_color(s_ch_antdot,
            lv_color_hex((s_fcount / 20) % 2 ? 0xE76F51 : 0x5A2A20), 0);
    }

    /* speech bubble rides above the head */
    if (s_bubble_life > 0) {
        s_bubble_life--;
        lv_obj_set_x(s_bubble, s_cx + 18);
        lv_obj_set_y(s_bubble, gy - CH_H - 26 - (50 - s_bubble_life) / 3);
        lv_obj_set_style_opa(s_bubble, (lv_opa_t)(s_bubble_life > 35 ? 255 : s_bubble_life * 7), 0);
        if (!s_bubble_life) lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    }

    /* HUD refresh is cheap but pointless every frame */
    if (s_fcount % 10 == 0) {
        lv_label_set_text(s_pet_mood, mood);
        lv_label_set_text_fmt(s_pet_name, "PIP  %lum", (unsigned long)s_pet_age_min);
        lv_bar_set_value(s_bar_food, s_food, LV_ANIM_OFF);
        lv_bar_set_value(s_bar_fun,  s_fun,  LV_ANIM_OFF);
        lv_bar_set_value(s_bar_nrg,  s_nrg,  LV_ANIM_OFF);
    }
}

/* ---------------- scene construction ---------------- */

static lv_obj_t *rect(lv_obj_t *par, int w, int h, int r, uint32_t color) {
    lv_obj_t *o = lv_obj_create(par);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);   /* let taps reach the scene */
    return o;
}

static lv_obj_t *make_stat_bar(lv_obj_t *parent, uint32_t color) {
    lv_obj_t *bar = lv_bar_create(parent);
    lv_obj_set_size(bar, 62, 6);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_radius(bar, 3, 0);
    lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x1E2A38), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(color), LV_PART_INDICATOR);
    return bar;
}

/* on-screen action buttons — usable without the side keys */
static void act_feed_cb(lv_event_t *e) { pet_feed(); }
static void act_play_cb(lv_event_t *e) { pet_play(); }
static void act_rest_cb(lv_event_t *e) { pet_rest(); }

/* swipe down on a sub-screen returns home */
/* Swipe UP from the bottom edge, like a phone. LV_DIR_TOP is the direction the
 * finger travelled, not the edge it started from. Shared by every app screen, so
 * home is one gesture everywhere — and in MUSIC, where all three keys are
 * rebound to volume, it is the only way home. */
static void gesture_home_cb(lv_event_t *e) {
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_TOP) {
        app_request(APP_DRAWER);
    }
}

static lv_obj_t *make_action_btn(lv_obj_t *parent, const char *txt,
                                 uint32_t color, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 104, 48);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 24, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_color(l, lv_color_hex(0x10162A), 0);
    lv_obj_center(l);
    return b;
}

static void build_pet_app(lv_obj_t *scr) {
    s_scr_pet = scr;
    lv_obj_remove_flag(s_scr_pet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr_pet, lv_color_hex(0x0A0F22), 0);
    lv_obj_set_style_bg_grad_color(s_scr_pet, lv_color_hex(0x241B3A), 0);
    lv_obj_set_style_bg_grad_dir(s_scr_pet, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(s_scr_pet, 0, 0);

    /* --- sky --- */
    /* uint16_t, not uint8_t: the five x positions past 255 used to wrap and
     * pile those stars up against the left edge */
    static const uint16_t sx[STAR_N] = { 34, 78, 132, 190, 250, 300, 352, 404, 60, 220, 330, 430 };
    static const uint16_t sy[STAR_N] = { 96, 52, 112, 40, 78, 34, 96, 130, 168, 150, 168, 74 };
    for (int i = 0; i < STAR_N; i++) {
        int sz = (i % 3 == 0) ? 4 : 3;
        s_star[i] = rect(s_scr_pet, sz, sz, 2, 0xFFF3D6);
        lv_obj_set_pos(s_star[i], sx[i] * 480 / 460, sy[i]);
        s_star_ph[i] = (uint8_t)(i * 7);
    }

    s_moon = rect(s_scr_pet, 46, 46, 23, 0xE9C46A);
    lv_obj_set_pos(s_moon, 76, 58);
    lv_obj_t *moon_dip = rect(s_moon, 14, 14, 7, 0xD9B45A);
    lv_obj_set_pos(moon_dip, 8, 12);

    s_shoot = rect(s_scr_pet, 26, 3, 2, 0xFFF3D6);
    lv_obj_add_flag(s_shoot, LV_OBJ_FLAG_HIDDEN);

    s_ufo = rect(s_scr_pet, 54, 14, 7, 0xB8C4D9);
    lv_obj_add_flag(s_ufo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *ufo_dome = rect(s_ufo, 24, 12, 6, 0x8DE0D2);
    lv_obj_align(ufo_dome, LV_ALIGN_TOP_MID, 0, -6);

    /* --- the planet: a big circle, only its cap is on screen --- */
    lv_obj_t *planet = rect(s_scr_pet, PLANET_R * 2, PLANET_R * 2, PLANET_R, 0x2A9D8F);
    lv_obj_set_pos(planet, PLANET_CX - PLANET_R, PLANET_CY - PLANET_R);
    lv_obj_set_style_border_width(planet, 4, 0);
    lv_obj_set_style_border_color(planet, lv_color_hex(0x6FD8C8), 0);
    lv_obj_set_style_border_opa(planet, 190, 0);

    lv_obj_t *c1 = rect(planet, 54, 20, 10, 0x21857A);
    lv_obj_set_pos(c1, PLANET_R - 130, PLANET_R - 232);
    lv_obj_t *c2 = rect(planet, 34, 14, 7, 0x21857A);
    lv_obj_set_pos(c2, PLANET_R + 74, PLANET_R - 224);
    lv_obj_t *c3 = rect(planet, 22, 10, 5, 0x21857A);
    lv_obj_set_pos(c3, PLANET_R - 16, PLANET_R - 200);

    /* rocket on the pad, hidden until it flies */
    s_rocket = rect(s_scr_pet, 18, 34, 8, 0xE76F51);
    lv_obj_add_flag(s_rocket, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *nose = rect(s_rocket, 10, 10, 5, 0xF4F1DE);
    lv_obj_align(nose, LV_ALIGN_TOP_MID, 0, 2);
    s_flame = rect(s_scr_pet, 8, 16, 4, 0xE9C46A);
    lv_obj_add_flag(s_flame, LV_OBJ_FLAG_HIDDEN);

    s_food_item = rect(s_scr_pet, 18, 18, 9, 0xF4A261);
    lv_obj_add_flag(s_food_item, LV_OBJ_FLAG_HIDDEN);

    /* --- the astronaut --- */
    s_ch_wrap = lv_obj_create(s_scr_pet);
    lv_obj_remove_style_all(s_ch_wrap);
    lv_obj_set_size(s_ch_wrap, CH_W, CH_H);
    lv_obj_remove_flag(s_ch_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ch_wrap, 240 - CH_W / 2, 200);

    s_ch_ant = rect(s_ch_wrap, 3, 10, 1, 0xB8C4D9);
    lv_obj_align(s_ch_ant, LV_ALIGN_TOP_MID, 10, -4);
    s_ch_antdot = rect(s_ch_wrap, 7, 7, 3, 0xE76F51);
    lv_obj_align(s_ch_antdot, LV_ALIGN_TOP_MID, 10, -10);

    s_ch_pack = rect(s_ch_wrap, 16, 26, 5, 0xE76F51);
    lv_obj_align(s_ch_pack, LV_ALIGN_TOP_LEFT, 0, 12);

    s_ch_leg_l = rect(s_ch_wrap, 11, 16, 4, 0x264653);
    lv_obj_align(s_ch_leg_l, LV_ALIGN_BOTTOM_MID, -10, 0);
    s_ch_leg_r = rect(s_ch_wrap, 11, 16, 4, 0x264653);
    lv_obj_align(s_ch_leg_r, LV_ALIGN_BOTTOM_MID, 10, 0);

    s_ch_arm_l = rect(s_ch_wrap, 9, 20, 4, 0xE0DCC8);
    lv_obj_align(s_ch_arm_l, LV_ALIGN_TOP_LEFT, 3, 22);
    s_ch_arm_r = rect(s_ch_wrap, 9, 20, 4, 0xE0DCC8);
    lv_obj_align(s_ch_arm_r, LV_ALIGN_TOP_RIGHT, -3, 22);

    /* body last so it sits over the pack and arm roots */
    s_ch_body = rect(s_ch_wrap, CH_W - 8, 40, 14, 0xF4F1DE);
    lv_obj_align(s_ch_body, LV_ALIGN_TOP_MID, 0, 6);

    s_ch_visor = rect(s_ch_body, 32, 18, 9, 0x264653);
    lv_obj_align(s_ch_visor, LV_ALIGN_TOP_MID, 0, 6);

    s_ch_eye_l = rect(s_ch_visor, 6, 12, 3, 0x8DE0D2);
    lv_obj_align(s_ch_eye_l, LV_ALIGN_CENTER, -7, 0);
    s_ch_eye_r = rect(s_ch_visor, 6, 12, 3, 0x8DE0D2);
    lv_obj_align(s_ch_eye_r, LV_ALIGN_CENTER, 7, 0);

    s_bubble = lv_label_create(s_scr_pet);
    lv_obj_set_style_text_color(s_bubble, lv_color_hex(0xE9C46A), 0);
    lv_label_set_text(s_bubble, "");
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);

    /* --- HUD: name + mood + three slim bars, kept out of the scene --- */
    s_pet_name = lv_label_create(s_scr_pet);
    lv_obj_set_style_text_color(s_pet_name, lv_color_hex(0xF4F1DE), 0);
    lv_label_set_text(s_pet_name, "PIP");
    lv_obj_align(s_pet_name, LV_ALIGN_TOP_MID, 0, TOP_MARGIN - 8);

    lv_obj_t *bars = lv_obj_create(s_scr_pet);
    lv_obj_remove_style_all(bars);
    lv_obj_set_size(bars, 220, 12);
    lv_obj_align(bars, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 16);
    lv_obj_set_flex_flow(bars, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bars, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(bars, LV_OBJ_FLAG_SCROLLABLE);
    s_bar_food = make_stat_bar(bars, 0xF4A261);
    s_bar_fun  = make_stat_bar(bars, 0xE76F51);
    s_bar_nrg  = make_stat_bar(bars, 0x8DE0D2);

    s_pet_mood = lv_label_create(s_scr_pet);
    lv_obj_set_width(s_pet_mood, CONTENT_W);
    lv_obj_set_style_text_align(s_pet_mood, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_pet_mood, lv_color_hex(0x9AA7B8), 0);
    lv_label_set_text(s_pet_mood, "having a good day");
    lv_obj_align(s_pet_mood, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 34);

    /* --- touch action bar --- */
    lv_obj_t *acts = lv_obj_create(s_scr_pet);
    lv_obj_remove_style_all(acts);
    lv_obj_set_size(acts, 330, 50);
    lv_obj_align(acts, LV_ALIGN_BOTTOM_MID, 0, -BOTTOM_MARGIN);
    lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(acts, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(acts, LV_OBJ_FLAG_SCROLLABLE);
    make_action_btn(acts, "FEED",  0xF4A261, act_feed_cb);
    make_action_btn(acts, "DANCE", 0xE76F51, act_play_cb);
    make_action_btn(acts, "NAP",   0x8DE0D2, act_rest_cb);

    /* tap the world to send it walking, tap the astronaut to pet it */
    lv_obj_add_flag(s_scr_pet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr_pet, scene_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_scr_pet, pet_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_app_timer = lv_timer_create(pet_timer_cb, PET_FPS_MS, NULL);
}

/* ---------------- sound ----------------
 *
 * ES8311 DAC into a small onboard power amp, enabled on GPIO46 by the codec's
 * own enable callback rather than by us. Two things shape this layer:
 *
 * 1. The I2S DMA rings are MALLOC_CAP_INTERNAL only and cannot be freed through
 *    the BSP, so the codec is brought up lazily on the first sound. A build that
 *    never plays anything pays nothing. bsp_audio_enable_rx(false) keeps the
 *    unused capture channel from costing another ~2.9 KB.
 * 2. esp_codec_dev_write() blocks until the DMA drains, so a one-second sound
 *    played inline would stall its caller for a second. Playback therefore runs
 *    on its own task, whose stack lives in PSRAM so it costs no internal SRAM.
 */

#define SFX_RATE     22050          /* matches the BSP default — no reconfigure */
#define SFX_CHUNK    512            /* samples per write */
/* The ES8311's volume curve reaches +32 dB, so 70 was leaving most of the
 * available gain unused — the first pass was barely audible on this driver.
 * Runtime-adjustable and saved, because the right level is an ear judgement and
 * reflashing to try a number is a waste of everyone's time. */
/* +12 dB on the curve below. The per-sound balance the user tuned by ear now
 * lives in the WAV levels, so this is a single master and 62 is where that
 * balance is correct. */
#define SFX_VOLUME_DEFAULT 62

static int s_vol = SFX_VOLUME_DEFAULT;
static volatile bool s_req_vol_save;

static void vol_load(void) {
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v = SFX_VOLUME_DEFAULT;
    if (nvs_get_i32(h, "vol2", &v) == ESP_OK) s_vol = clampi((int)v, 0, 100);
    nvs_close(h);
}

static void vol_save(void) {
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, "vol2", s_vol);
    nvs_commit(h);
    nvs_close(h);
}

typedef enum {
    SFX_TICK = 1,                       /* rotation detent          */
    SFX_START,                      /* session begins           */
    SFX_PAUSE,                      /* laid flat                */
    SFX_RESUME,                     /* stood back up            */
    SFX_DONE,                       /* countdown reached zero   */
} sfx_id_t;

/* Embedded by EMBED_FILES; authored by assets/sounds/render.py against this
 * board's measured speaker response (nothing below ~500 Hz, usable to 8 kHz). */
#define SFX_BLOB(sym)                                          \
    extern const uint8_t sym##_start[] asm("_binary_" #sym "_start"); \
    extern const uint8_t sym##_end[]   asm("_binary_" #sym "_end")

SFX_BLOB(tick_wav);
SFX_BLOB(start_wav);
SFX_BLOB(pause_wav);
SFX_BLOB(resume_wav);
SFX_BLOB(done_wav);

typedef struct {
    const char    *name;
    const uint8_t *begin, *end;
} sfx_clip_t;

static const sfx_clip_t s_clips[] = {
    { "tick",   tick_wav_start,   tick_wav_end   },
    { "start",  start_wav_start,  start_wav_end  },
    { "pause",  pause_wav_start,  pause_wav_end  },
    { "resume", resume_wav_start, resume_wav_end },
    { "done",   done_wav_start,   done_wav_end   },
};
#define CLIP_COUNT ((int)(sizeof(s_clips) / sizeof(s_clips[0])))
#define CLIP_OF(id) (&s_clips[(id) - SFX_TICK])

static esp_codec_dev_handle_t s_spk;
static QueueHandle_t s_sfx_q;
static volatile bool s_sfx_busy;

/* esp_codec_dev's default volume curve maps slider 100 to 0 dB — unity gain —
 * while the ES8311 itself reaches +32 dB. So the stock scale tops out at a
 * fortieth of the amplitude the hardware can actually produce, which is the real
 * reason everything sounded faint no matter how the files were mixed. Replace
 * the curve so 100 means the chip's genuine maximum.
 *
 * Three points rather than two: a straight line in dB spends most of the slider
 * in territory too quiet to be useful, so the bottom half covers the quiet range
 * coarsely and the top half gives fine control where it matters. */
static const esp_codec_dev_vol_map_t s_vol_curve[] = {
    {   0, -60.0f },
    {  50,   6.0f },
    { 100,  32.0f },        /* ES8311 hardware maximum */
};

/* Mirror of the curve above, so the UI can report the gain the slider actually
 * asks for. "It sounds right at 65" then means something specific. */
static float vol_db(int v) {
    const int n = (int)(sizeof(s_vol_curve) / sizeof(s_vol_curve[0]));
    if (v <= 0) return -96.0f;
    if (v >= s_vol_curve[n - 1].vol) return s_vol_curve[n - 1].db_value;
    for (int i = 0; i < n - 1; i++) {
        if (v < s_vol_curve[i + 1].vol) {
            float span = (float)(s_vol_curve[i + 1].vol - s_vol_curve[i].vol);
            float rise = s_vol_curve[i + 1].db_value - s_vol_curve[i].db_value;
            return s_vol_curve[i].db_value + (v - s_vol_curve[i].vol) * rise / span;
        }
    }
    return 0.0f;
}

static bool sfx_codec_ready(void) {
    if (s_spk) return true;
    bsp_audio_enable_rx(false);              /* playback only */
    uint32_t before = hp_free();
    s_spk = bsp_audio_codec_speaker_init();
    if (!s_spk) {
        ESP_LOGE(TAG, "speaker init failed — sound disabled");
        return false;
    }
    esp_codec_dev_vol_curve_t curve = {
        .vol_map = (esp_codec_dev_vol_map_t *)s_vol_curve,
        .count   = (int)(sizeof(s_vol_curve) / sizeof(s_vol_curve[0])),
    };
    if (esp_codec_dev_set_vol_curve(s_spk, &curve) != 0) {
        ESP_LOGW(TAG, "custom volume curve rejected — stuck at unity gain");
    }
    ESP_LOGI(TAG, "speaker up, internal heap %u -> %u (cost %d B)",
             (unsigned)before, (unsigned)hp_free(), (int)(before - hp_free()));
    return true;
}

/* Walk the RIFF chunks to the PCM payload rather than assuming a 44-byte
 * header — a WAV writer is free to insert LIST/fact chunks, and a wrong offset
 * here plays the header as audio, which is unmistakable but avoidable. */
static const uint8_t *wav_pcm(const uint8_t *p, const uint8_t *end, size_t *len) {
    if (end - p < 12 || memcmp(p, "RIFF", 4) || memcmp(p + 8, "WAVE", 4)) return NULL;
    const uint8_t *q = p + 12;
    while (q + 8 <= end) {
        uint32_t sz = (uint32_t)q[4] | ((uint32_t)q[5] << 8) |
                      ((uint32_t)q[6] << 16) | ((uint32_t)q[7] << 24);
        const uint8_t *body = q + 8;
        if (!memcmp(q, "data", 4)) {
            if (body + sz > end) sz = (uint32_t)(end - body);   /* be forgiving */
            *len = sz;
            return body;
        }
        q = body + sz + (sz & 1);        /* chunks are word-aligned */
    }
    return NULL;
}

/* Written in chunks so a long clip never needs a copy — the source is memory
 * mapped straight out of flash. */
static void sfx_render_clip(const sfx_clip_t *c) {
    size_t len = 0;
    const uint8_t *pcm = wav_pcm(c->begin, c->end, &len);
    if (!pcm || len < 2) {
        ESP_LOGW(TAG, "sfx %s: no PCM payload", c->name);
        return;
    }
    const size_t step = SFX_CHUNK * sizeof(int16_t);
    for (size_t off = 0; off < len; off += step) {
        size_t n = (len - off < step) ? (len - off) : step;
        esp_codec_dev_write(s_spk, (void *)(pcm + off), n);
    }
}

static void sfx_task(void *arg) {
    sfx_id_t id;
    bool open = false;

    while (1) {
        /* Stay open between sounds and only release the codec once things go
         * quiet. Closing straight after a write was the bug behind the crack at
         * the end of the bell: esp_codec_dev_write() returns once the data is
         * queued, not once it has been played, so the close was disabling the
         * I2S channel while the DMA ring was still draining. Holding it open
         * also stops the power amp clicking on every single sound. */
        if (xQueueReceive(s_sfx_q, &id, pdMS_TO_TICKS(4000)) != pdTRUE) {
            if (open) {
                vTaskDelay(pdMS_TO_TICKS(120));      /* let the ring drain */
                esp_codec_dev_close(s_spk);
                open = false;
            }
            continue;
        }
        if (!sfx_codec_ready()) continue;

        s_sfx_busy = true;
        esp_codec_dev_sample_info_t fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 1,
            .sample_rate = SFX_RATE,
        };
        if (open || esp_codec_dev_open(s_spk, &fs) == 0) {
            open = true;
            esp_codec_dev_set_out_vol(s_spk, s_vol);
            switch (id) {
            case SFX_TICK: case SFX_START: case SFX_PAUSE:
            case SFX_RESUME: case SFX_DONE:
                sfx_render_clip(CLIP_OF(id));
                break;
            default:
                break;
            }
        } else {
            ESP_LOGE(TAG, "codec open failed");
        }
        s_sfx_busy = false;
    }
}

static void sfx_init(void) {
    s_sfx_q = xQueueCreate(3, sizeof(sfx_id_t));
    if (!s_sfx_q) return;
    /* stack in PSRAM: this task does no ISR work and touches no DMA directly */
    if (xTaskCreateWithCaps(sfx_task, "sfx", 5120, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "!! sfx stack fell back to INTERNAL SRAM — costs ~5 KB of "
                      "the scarce pool; expect a lower floor");
        s_stack_fallback = true;
        xTaskCreate(sfx_task, "sfx", 4096, NULL, 4, NULL);
    }
}

static void sfx_play(sfx_id_t id) {
    if (!s_sfx_q) return;
    xQueueSend(s_sfx_q, &id, 0);                   /* drop if the queue is full */
}

/* ---------------- CONTROL: settings + diagnostics ----------------
 *
 * Replaces the old STATUS and SYSTEM screens, which between them showed a
 * decorative spinner, a wall of read-only text, and a "right button =
 * calibrate" hint for an action you could not see the result of.
 *
 * This is a scrolling column of cards, each pairing live readouts with the
 * control that acts on them: the wallpaper card owns the fetch button and shows
 * the actual download percentage, the display card owns rotation calibration
 * and shows what it just changed to. Nothing here is decorative.
 */

static void refr_ready_cb(lv_event_t *e) {
    s_refr_count++;
}

static lv_obj_t *s_cfg_wall_pool, *s_cfg_wall_state, *s_cfg_wall_bar, *s_cfg_wall_sub;
static lv_obj_t *s_cfg_rot_val;
static lv_obj_t *s_cfg_rot_sw, *s_cfg_rot_btn;
static lv_obj_t *s_cfg_bright_val;
static lv_obj_t *s_cfg_vol_val;
static lv_obj_t *s_cfg_batt_bar, *s_cfg_batt_val, *s_cfg_batt_sub;
static lv_obj_t *s_cfg_net_val;
static lv_obj_t *s_cfg_ble_val;
static lv_obj_t *s_cfg_ble_code;
static lv_obj_t *s_cfg_ble_btn;
static lv_obj_t *s_cfg_sys_val;
static lv_obj_t *s_cfg_log;

#define CFG_ACCENT_WALL 0x22D3EE
#define CFG_ACCENT_DISP 0xA78BFA
#define CFG_ACCENT_BATT 0x34D399
#define CFG_ACCENT_NET  0x60A5FA
#define CFG_ACCENT_SND  0xFB923C
#define CFG_ACCENT_SYS  0x94A3B8

/* one card in the scrolling column */
static lv_obj_t *cfg_card(lv_obj_t *parent, const char *title, uint32_t accent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x11161F), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    /* Controls here are sized for a fingertip (CFG_TOUCH_H), and 7 px of gutter
     * between 76 px controls reads as a pile rather than a list. */
    lv_obj_set_style_pad_row(card, 14, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(card, 80, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);

    lv_obj_t *t = lv_label_create(card);
    lv_obj_set_style_text_font(t, &hud_text_18, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(accent), 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_label_set_text(t, title);
    return card;
}

/* a readout line inside a card */
static lv_obj_t *cfg_text(lv_obj_t *card, uint32_t colour) {
    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_width(l, lv_pct(100));
    /* montserrat_20, not the 14 px default: 14 px readouts beside 76 px controls
     * looked like a desktop dialog scaled up wrong. Same glyph coverage as the
     * default (ASCII plus the LV_SYMBOL_* set), so nothing that renders today
     * stops rendering, and it is already compiled in — no extra flash. */
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

/* Touch targets on this panel have to be generous. The glass is curved, the
 * controller is noisy near the edges, and a 44 px control registered drags as
 * taps and taps as nothing — the same lesson the first MUSIC layout learned when
 * its 46 px transport buttons ghost-touched and were rebuilt at 76/88/76.
 * CONTROL is an unbounded scrolling column, so height is free here; there is no
 * reason to be frugal with it. Every control in the app is sized from these.
 *
 * The ext-click area matters as much as the drawn size: it widens the hit test
 * without changing the layout, which is what rescues a slider whose knob you are
 * chasing with a fingertip wider than the track. */
#define CFG_TOUCH_H   76     /* buttons and switch rows                      */
#define CFG_SLIDER_H  46     /* plus CFG_EXT_CLICK on each side              */
#define CFG_EXT_CLICK 18

static lv_obj_t *cfg_button(lv_obj_t *card, const char *text, uint32_t accent,
                            lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(card);
    lv_obj_set_size(b, lv_pct(100), CFG_TOUCH_H);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1B2432), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(b, 150, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

/* Fade a card button to inert and back — still visible, plainly not available,
 * which is how MUSIC renders a transport control the endpoint refuses. The
 * opacity cascades to the button's label, so the whole control dims together. */
static void cfg_button_live(lv_obj_t *b, bool live) {
    lv_obj_set_style_opa(b, live ? LV_OPA_COVER : LV_OPA_40, 0);
    if (live) lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    else      lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
}

/* A full-width slider sized for a finger rather than for a cursor. Both sliders
 * in this app go through here so they cannot drift apart. */
static lv_obj_t *cfg_slider(lv_obj_t *card, int lo, int hi, int val,
                            uint32_t accent, uint32_t knob, lv_event_cb_t cb) {
    lv_obj_t *s = lv_slider_create(card);
    lv_obj_set_size(s, lv_pct(100), CFG_SLIDER_H);
    lv_obj_set_ext_click_area(s, CFG_EXT_CLICK);
    lv_slider_set_range(s, lo, hi);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_radius(s, CFG_SLIDER_H / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s, CFG_SLIDER_H / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s, lv_color_hex(accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s, lv_color_hex(knob), LV_PART_KNOB);
    /* Knob flush with the track, NOT padded proud of it. Padding the knob was the
     * obvious way to make it easier to grab and it looked broken — a 46 px track
     * with a 66 px ball overhanging both edges reads as a rendering bug, not as a
     * control. It also bought nothing: an lv_slider in the default mode jumps to
     * wherever you press on the track, so the whole 46 px bar is already the
     * target and the knob never has to be hit at all. */
    lv_obj_set_style_pad_all(s, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
    return s;
}

/* a labelled toggle row inside a card */
static lv_obj_t *cfg_switch(lv_obj_t *card, const char *text, uint32_t accent,
                            bool on, lv_event_cb_t cb) {
    lv_obj_t *row = lv_obj_create(card);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, CFG_TOUCH_H);   /* fixed: a growing row would shift the card */
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    /* lv_obj_create() is clickable by default and would swallow taps aimed at
     * the switch sitting inside it (ARCHITECTURE.md, LVGL traps). */
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *l = lv_label_create(row);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xC7D2E0), 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 116, 58);
    lv_obj_set_ext_click_area(sw, CFG_EXT_CLICK);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, lv_color_hex(accent),
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_hex(0xE2E8F0), LV_PART_KNOB);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void cfg_fetch_cb(lv_event_t *e) {
    s_req_wallpaper = true;          /* the network task breaks its wait on this */
}

static void cfg_ble_cb(lv_event_t *e) {
    /* Flags only. Starting a session tears the Wi-Fi driver down, which is
     * far too much to do inside an LVGL callback. */
    if (ble_prov_active()) s_req_ble_off = true;
    else                   s_req_ble_on  = true;
}

static void cfg_rotate_cb(lv_event_t *e) {
    rotation_bump();
}

static void cfg_autorot_cb(lv_event_t *e) {
    s_autorot = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    s_req_autorot_save = true;              /* NVS is the main loop's business */
    ESP_LOGI(TAG, "autorotate %s", s_autorot ? "ON" : "OFF");
}

/* Same shape as cfg_vol_cb and for the same three reasons: the label is only
 * rewritten on release, because resizing it mid-drag re-lays out the card and
 * moves the slider under the finger; the panel write is left to the main loop,
 * because it is QSPI IO that needs the LVGL lock this callback already holds;
 * and NVS waits too, because committing per VALUE_CHANGED would erase flash on
 * every pixel of the drag. */
static void cfg_bright_cb(lv_event_t *e) {
    s_bright = clampi((int)lv_slider_get_value(lv_event_get_target(e)),
                      BRIGHT_MIN, 100);
    s_req_bright_apply = true;
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;

    if (s_cfg_bright_val) {
        lv_label_set_text_fmt(s_cfg_bright_val, "brightness  %d%%", s_bright);
    }
    s_req_bright_save = true;
}

/* Deliberately does almost nothing while the knob is moving.
 *
 * Rewriting the label on every LV_EVENT_VALUE_CHANGED was a crash: the text
 * length changes, the label sits in a LV_SIZE_CONTENT flex card, so the card and
 * the whole scrolling column re-laid out dozens of times a second — which moves
 * the slider itself while LVGL is midway through delivering an input event to
 * that very slider. The knob position is feedback enough during a drag; the
 * numbers land when you let go.
 *
 * NVS is written from the main loop rather than here, because a commit erases
 * flash and can block for tens of milliseconds with the LVGL lock held. */
static void cfg_vol_cb(lv_event_t *e) {
    s_vol = (int)lv_slider_get_value(lv_event_get_target(e));
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;

    if (s_cfg_vol_val) {
        /* integer dB, not %f: newlib's float formatting is stack-hungry and this
         * runs on the LVGL task, whose 8 KB is already carrying the renderer */
        lv_label_set_text_fmt(s_cfg_vol_val, "volume  %d%%   /   %+d dB",
                              s_vol, (int)vol_db(s_vol));
    }
    s_req_vol_save = true;
    sfx_play(SFX_DONE);      /* the loudest clip: the one to judge a level by */
}

static void cfg_timer_cb(lv_timer_t *t) {
    if (!s_cfg_sys_val) return;

    /* ---- wallpaper ---- */
    int have = __builtin_popcount(s_wall_have);
    lv_label_set_text_fmt(s_cfg_wall_pool, "pool  %d / %d   /   new one every 6 h",
                          have, WALL_SLOTS);

    /* clear a finished result after a few seconds so the card returns to rest */
    if ((s_dl_state == DL_OK || s_dl_state == DL_FAIL) &&
        now_ms() - s_dl_ended_ms > 6000) {
        s_dl_state = DL_IDLE;
    }

    switch (s_dl_state) {
    case DL_QUERY:
        lv_obj_remove_flag(s_cfg_wall_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_cfg_wall_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cfg_wall_bar, lv_color_hex(CFG_ACCENT_WALL),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_cfg_wall_state, lv_color_hex(CFG_ACCENT_WALL), 0);
        lv_label_set_text(s_cfg_wall_state, "asking unsplash...");
        break;
    case DL_IMAGE:
        lv_obj_remove_flag(s_cfg_wall_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_cfg_wall_bar, s_dl_pct, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cfg_wall_bar, lv_color_hex(CFG_ACCENT_WALL),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_cfg_wall_state, lv_color_hex(CFG_ACCENT_WALL), 0);
        if (s_dl_total > 0) {
            lv_label_set_text_fmt(s_cfg_wall_state, "downloading  %d%%   %d KB",
                                  s_dl_pct, s_dl_kb);
        } else {
            lv_label_set_text_fmt(s_cfg_wall_state, "downloading  %d KB", s_dl_kb);
        }
        break;
    case DL_OK:
        lv_obj_remove_flag(s_cfg_wall_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_cfg_wall_bar, 100, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cfg_wall_bar, lv_color_hex(0x35C759),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_cfg_wall_state, lv_color_hex(0x35C759), 0);
        lv_label_set_text_fmt(s_cfg_wall_state, LV_SYMBOL_OK "  saved  /  %d KB", s_dl_kb);
        break;
    case DL_FAIL:
        lv_obj_remove_flag(s_cfg_wall_bar, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(s_cfg_wall_bar, 100, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_cfg_wall_bar, lv_color_hex(0xFF453A),
                                  LV_PART_INDICATOR);
        lv_obj_set_style_text_color(s_cfg_wall_state, lv_color_hex(0xFF453A), 0);
        lv_label_set_text(s_cfg_wall_state, LV_SYMBOL_WARNING "  fetch failed");
        break;
    default:
        lv_obj_add_flag(s_cfg_wall_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(s_cfg_wall_state, lv_color_hex(0x64748B), 0);
        lv_label_set_text(s_cfg_wall_state,
                          s_req_wallpaper ? "queued..." : "idle");
        break;
    }

    if (s_dl_theme[0] && s_dl_state != DL_IDLE) {
        lv_label_set_text_fmt(s_cfg_wall_sub, "theme  %s", s_dl_theme);
    } else if (s_wall_credit[0]) {
        lv_label_set_text_fmt(s_cfg_wall_sub, "on screen  %s / Unsplash", s_wall_credit);
    } else {
        lv_label_set_text(s_cfg_wall_sub, "");
    }

    /* ---- display ---- */
    lv_label_set_text_fmt(s_cfg_rot_val,
                          "orientation  %d deg   %s\ncalibration  %d / %d  %s\naccel  %d  %d  %d",
                          s_rot * 90,
                          !s_imu     ? "no sensor" :
                          !s_autorot ? "held"      : "auto",
                          s_rot_cfg + 1, ROT_CFG_COUNT,
                          (s_rot_cfg & 4) ? "reversed" : "normal",
                          s_acc_x / 100, s_acc_y / 100, s_acc_z / 100);

    /* Calibrating something that is switched off makes no sense — the button
     * applies a rotation immediately, which would visibly contradict the switch
     * the user just turned off. Faded rather than hidden: a control that
     * disappears makes people think something broke and go looking for it, while
     * one that is plainly present and inert explains itself. The CLICKABLE flag
     * doubles as the state, so the restyle only runs on an actual change —
     * setting it every 400 ms would invalidate the card forever. */
    if (s_cfg_rot_btn &&
        s_autorot != lv_obj_has_flag(s_cfg_rot_btn, LV_OBJ_FLAG_CLICKABLE)) {
        cfg_button_live(s_cfg_rot_btn, s_autorot);
    }

    /* ---- battery ---- */
    int pct = s_batt_pct;
    lv_bar_set_value(s_cfg_batt_bar, pct < 0 ? 100 : pct, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_cfg_batt_bar,
        lv_color_hex(pct < 0 ? 0x4A9EFF
                    : pct >= 50 ? 0x35C759
                    : pct >= 20 ? 0xFFB020 : 0xFF453A), LV_PART_INDICATOR);
    if (pct < 0) {
        lv_label_set_text(s_cfg_batt_val, "external power");
    } else {
        lv_label_set_text_fmt(s_cfg_batt_val, "%d%%   %d mV%s", pct, s_batt_mv,
                              s_batt_charging ? "   " LV_SYMBOL_CHARGE " charging" : "");
    }
    int drain = battery_drain_mv_h();
    if (drain > 0) {
        lv_label_set_text_fmt(s_cfg_batt_sub, "drain  %d mV/h   /   %s", drain,
                              s_doze ? "dozing" : "active");
    } else {
        lv_label_set_text_fmt(s_cfg_batt_sub, "drain  measuring...   /   %s",
                              s_doze ? "dozing" : "active");
    }

    /* ---- pair ----
     * Written only when something actually changed. Rewriting a label inside an
     * LV_SIZE_CONTENT flex card re-lays out the whole scrolling column and
     * invalidates it; at 400 ms that is a continuous stream of flushes for a
     * card whose text is static almost all the time (§5). Continuous flushes
     * are also what widens the window on the panel-IO race in pitfall #13. */
    if (s_cfg_ble_val) {
        /* ASCII only — hud_text_18 carries 0x20-0x7F and nothing else, so a
         * typographic character would render as an empty box. */
        static const char *ble_st[] = {
            "idle", "waiting for phone", "phone connected", "ready",
            "credentials taken", "closing", "error",
        };
        static int  last_key = -1;
        bool act = ble_prov_active();
        int  key = act ? (100 + (int)ble_prov_state())
                       : (s_wifi_torn_down ? 1 : s_wifi_up ? 2 : 3);

        if (key != last_key) {
            last_key = key;
            if (act) {
                ble_prov_state_t st = ble_prov_state();
                lv_label_set_text(s_cfg_ble_val,
                                  st <= BLE_PROV_ERR ? ble_st[st] : "idle");
                lv_obj_remove_flag(s_cfg_ble_code, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(lv_obj_get_child(s_cfg_ble_btn, 0),
                                  LV_SYMBOL_CLOSE "  Stop pairing");
            } else {
                /* The join happens after the radio is down, so this card is
                 * where the result shows — the phone cannot be told. */
                lv_label_set_text(s_cfg_ble_val,
                                  s_wifi_torn_down ? "restoring wi-fi" :
                                  s_wifi_up        ? "wi-fi connected"
                                                   : "pair to set up wi-fi");
                lv_obj_add_flag(s_cfg_ble_code, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(lv_obj_get_child(s_cfg_ble_btn, 0),
                                  LV_SYMBOL_BLUETOOTH "  Pair with phone");
            }
        }
        /* The code itself changes only per session, but it is cheap to keep in
         * step and it must be right the moment the card is built. */
        if (act) lv_label_set_text_fmt(s_cfg_ble_code, "CODE  %s", ble_prov_code());
    }

    /* ---- network ---- */
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    lv_label_set_text_fmt(s_cfg_net_val,
                          "%s%s\nip  %s\nsignal  %d dBm",
                          s_wifi_up ? LV_SYMBOL_OK "  " : "",
                          s_wifi_up ? s_ssid : (s_wifi_disabled ? "wi-fi off" : "offline"),
                          s_ip[0] ? s_ip : "-",
                          rssi);

    /* ---- system ---- */
    lv_label_set_text_fmt(s_cfg_sys_val,
                          "uptime  %lu s\n"
                          "render  %u.%u fps\n"
                          "heap  %u KB free  /  %u KB min\n"
                          "psram  %lu KB free\n"
                          "sdcard  %s  /  %lu log rows\n"
                          "idf  %s",
                          (unsigned long)(now_ms() / 1000),
                          (unsigned)(s_last_fps_x10 / 10), (unsigned)(s_last_fps_x10 % 10),
                          (unsigned)(hp_free() / 1024),
                          (unsigned)(hp_min() / 1024),
                          (unsigned long)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
                          s_sd_ok ? "mounted" : "none", (unsigned long)s_tele_rows,
                          IDF_VER);

    if (s_log_mtx && xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        char buf[LOG_LINES * sizeof(s_log[0]) + 8];
        snprintf(buf, sizeof(buf), "%s\n%s\n%s", s_log[0], s_log[1], s_log[2]);
        xSemaphoreGive(s_log_mtx);
        lv_label_set_text(s_cfg_log, buf);
    }
}

static void build_control_app(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05070B), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &hud_text_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8FBFF), 0);
    lv_obj_set_style_text_letter_space(title, 6, 0);
    lv_label_set_text(title, "CONTROL");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TOP_MARGIN);

    /* The scroll column stays inside the corner radius: 364 wide is safe all
     * the way down to y=436, where the arc has only cut ~22 px off each side. */
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, CONTENT_W, 372);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_set_style_pad_all(col, 2, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_ACTIVE);

    /* ---- wallpaper ---- */
    lv_obj_t *c = cfg_card(col, "WALLPAPER", CFG_ACCENT_WALL);
    s_cfg_wall_pool  = cfg_text(c, 0xC7D2E0);
    s_cfg_wall_state = cfg_text(c, 0x64748B);

    s_cfg_wall_bar = lv_bar_create(c);
    lv_obj_set_size(s_cfg_wall_bar, lv_pct(100), 8);
    lv_bar_set_range(s_cfg_wall_bar, 0, 100);
    lv_bar_set_value(s_cfg_wall_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_cfg_wall_bar, 4, 0);
    lv_obj_set_style_radius(s_cfg_wall_bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_cfg_wall_bar, lv_color_hex(0x1E293B), 0);
    lv_obj_add_flag(s_cfg_wall_bar, LV_OBJ_FLAG_HIDDEN);

    s_cfg_wall_sub = cfg_text(c, 0x64748B);
    cfg_button(c, LV_SYMBOL_DOWNLOAD "  Fetch new wallpaper",
               CFG_ACCENT_WALL, cfg_fetch_cb);

    /* ---- display ---- */
    c = cfg_card(col, "DISPLAY", CFG_ACCENT_DISP);
    s_cfg_rot_val = cfg_text(c, 0xC7D2E0);

    /* fixed height, for the reason spelled out on the volume readout below */
    s_cfg_bright_val = cfg_text(c, 0xC7D2E0);
    lv_obj_set_height(s_cfg_bright_val, 26);   /* one montserrat_20 line, uncl'd */
    lv_label_set_text_fmt(s_cfg_bright_val, "brightness  %d%%", s_bright);

    /* never 0: see BRIGHT_MIN */
    cfg_slider(c, BRIGHT_MIN, 100, s_bright,
               CFG_ACCENT_DISP, 0xE9DDFF, cfg_bright_cb);

    s_cfg_rot_sw = cfg_switch(c, "Auto-rotate", CFG_ACCENT_DISP,
                              s_autorot, cfg_autorot_cb);
    /* No IMU, no autorotate — a switch that cannot do anything is worse than one
     * that plainly looks dead, which is how MUSIC renders an unavailable
     * transport control too. LV_STATE_DISABLED alone is not enough: the indev
     * does honour it, but the local styles set above beat the theme's grey, so
     * the switch would still be drawn as if live. Hence the explicit fade. */
    if (!s_imu) {
        lv_obj_add_state(s_cfg_rot_sw, LV_STATE_DISABLED);
        lv_obj_remove_flag(s_cfg_rot_sw, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(s_cfg_rot_sw, LV_OPA_40, 0);
    }

    /* Calibration only means something while autorotate is committing, so the
     * button follows the switch — see cfg_timer_cb. */
    s_cfg_rot_btn = cfg_button(c, LV_SYMBOL_REFRESH "  Step rotation calibration",
                               CFG_ACCENT_DISP, cfg_rotate_cb);

    /* ---- audio ---- */
    c = cfg_card(col, "AUDIO", CFG_ACCENT_SND);
    s_cfg_vol_val = cfg_text(c, 0xC7D2E0);
    /* fixed height: a growing label here would resize the card and shift the
     * slider under the finger mid-drag */
    lv_obj_set_height(s_cfg_vol_val, 26);      /* one montserrat_20 line, uncl'd */
    lv_label_set_text_fmt(s_cfg_vol_val, "volume  %d%%   /   %+d dB",
                          s_vol, (int)vol_db(s_vol));

    cfg_slider(c, 0, 100, s_vol, CFG_ACCENT_SND, 0xFFD9A8, cfg_vol_cb);

    /* ---- battery ---- */
    c = cfg_card(col, "BATTERY", CFG_ACCENT_BATT);
    s_cfg_batt_val = cfg_text(c, 0xC7D2E0);
    s_cfg_batt_bar = lv_bar_create(c);
    lv_obj_set_size(s_cfg_batt_bar, lv_pct(100), 12);
    lv_bar_set_range(s_cfg_batt_bar, 0, 100);
    lv_obj_set_style_radius(s_cfg_batt_bar, 6, 0);
    lv_obj_set_style_radius(s_cfg_batt_bar, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_cfg_batt_bar, lv_color_hex(0x1E293B), 0);
    s_cfg_batt_sub = cfg_text(c, 0x64748B);

    /* ---- pair ---- */
    /* Directly above NETWORK: pairing is what you reach for when the readout
     * below it says "offline". */
    c = cfg_card(col, "PAIR", CFG_ACCENT_NET);
    s_cfg_ble_val  = cfg_text(c, 0xC7D2E0);
    s_cfg_ble_code = cfg_text(c, 0x60A5FA);
    lv_obj_set_style_text_font(s_cfg_ble_code, &hud_text_18, 0);
    lv_obj_set_style_text_letter_space(s_cfg_ble_code, 4, 0);
    lv_obj_add_flag(s_cfg_ble_code, LV_OBJ_FLAG_HIDDEN);
    s_cfg_ble_btn = cfg_button(c, LV_SYMBOL_BLUETOOTH "  Pair with phone",
                               CFG_ACCENT_NET, cfg_ble_cb);

    /* ---- network ---- */
    c = cfg_card(col, "NETWORK", CFG_ACCENT_NET);
    s_cfg_net_val = cfg_text(c, 0xC7D2E0);

    /* ---- system ---- */
    c = cfg_card(col, "SYSTEM", CFG_ACCENT_SYS);
    s_cfg_sys_val = cfg_text(c, 0x94A3B8);
    s_cfg_log = cfg_text(c, 0x475569);

    lv_obj_add_event_cb(scr, gesture_home_cb, LV_EVENT_GESTURE, NULL);
    s_app_timer = lv_timer_create(cfg_timer_cb, 400, NULL);
    cfg_timer_cb(NULL);
}

/* ---------------- POMODORO: the cube is the interface ----------------
 *
 * Stand the cube upright on the desk facing you. Four durations sit at fixed
 * positions on the glass, each pre-rotated so it reads upright when its own edge
 * is the top one — the bottom label is genuinely printed upside down. Turning
 * the cube brings a different number to the top, and that instantly becomes the
 * running timer. Lay it flat to pause; stand it up to resume.
 *
 * Two things make this work, and both are the opposite of what the rest of the
 * firmware does:
 *
 *  - Autorotate is suppressed while this app owns the screen. If the panel
 *    counter-rotated, the labels would stay put relative to your eye and turning
 *    the cube would change nothing. Orientation is input here, not layout.
 *  - The dial is fixed to the DEVICE, like a bezel. The countdown in the middle
 *    is fixed to YOU, counter-rotated so it always reads upright.
 */

#define POMO_SLOTS       4
#define POMO_FLAT_TH     12000     /* ~0.73 g on Z: lying down either way up   */
#define POMO_FLAT_MS     600       /* setting it down should not flicker       */
#define POMO_DIM_MS      120000    /* untouched and still -> dim the panel     */
#define POMO_DIM_PCT     12        /* of the user's brightness, not of full     */
#define POMO_DIM_FLOOR   3         /* still legible across the room             */
#define POMO_MOTION_TH   2600      /* ~0.16 g of movement counts as "handled"  */
#define POMO_DONE_MS     7000      /* how long the finish screen lingers       */

/* Clockwise from the top edge, matching the layout on screen. */
static const uint8_t s_pomo_min[POMO_SLOTS] = { 60, 10, 5, 30 };

typedef enum { POMO_IDLE = 0, POMO_RUN, POMO_PAUSE, POMO_DONE } pomo_state_t;

/* Session state is file-scope, not owned by the screen: a countdown keeps
 * running if you navigate away, and the app is only ever a view onto it. */
static pomo_state_t s_pomo_state;
static int      s_pomo_sel = 0;
static int      s_pomo_total_s, s_pomo_left_s;
static int64_t  s_pomo_tick_ms;
static int64_t  s_pomo_done_at;
static uint32_t s_pomo_sessions;

static bool     s_pomo_flat;
static int64_t  s_pomo_flat_since;
static int      s_pomo_last_rot = -1;
static int64_t  s_pomo_active_ms;      /* last touch or movement */
static bool     s_pomo_dimmed;
static int      s_acc_ref_x, s_acc_ref_y, s_acc_ref_z;

/* widgets */
static lv_obj_t *s_pomo_dial[POMO_SLOTS];
static lv_obj_t *s_pomo_clock, *s_pomo_word, *s_pomo_ring, *s_pomo_arc, *s_pomo_fill;
static int s_pomo_drawn_rot = -1;

typedef struct {
    uint16_t ver;
    uint16_t sel;
    uint32_t sessions;
} pomo_blob_t;
#define POMO_BLOB_VER 1

static void pomo_load(void) {
    pomo_blob_t b;
    if (store_load("pomodoro", &b, sizeof(b)) && b.ver == POMO_BLOB_VER) {
        s_pomo_sel = clampi(b.sel, 0, POMO_SLOTS - 1);
        s_pomo_sessions = b.sessions;
    }
}

static void pomo_save(void) {
    pomo_blob_t b = { .ver = POMO_BLOB_VER, .sel = (uint16_t)s_pomo_sel,
                      .sessions = s_pomo_sessions };
    store_save("pomodoro", &b, sizeof(b));
}

#define POMO_LOG_PATH BSP_SD_MOUNT_POINT "/logs/pomo.csv"

static void pomo_log(const char *how) {
    if (!s_sd_ok) return;
    struct stat st;
    bool fresh = (stat(POMO_LOG_PATH, &st) != 0);
    FILE *f = fopen(POMO_LOG_PATH, "a");
    if (!f) return;
    if (fresh) fprintf(f, "clock,minutes,outcome,sessions\n");

    char clock[16] = "";
    time_t now; struct tm ti;
    time(&now); localtime_r(&now, &ti);
    if (ti.tm_year >= (2024 - 1900)) strftime(clock, sizeof(clock), "%H:%M", &ti);

    fprintf(f, "%s,%d,%s,%lu\n", clock, s_pomo_total_s / 60, how,
            (unsigned long)s_pomo_sessions);
    fclose(f);
}

static void pomo_begin(int slot, bool announce) {
    s_pomo_sel = clampi(slot, 0, POMO_SLOTS - 1);
    s_pomo_total_s = s_pomo_min[s_pomo_sel] * 60;
    s_pomo_left_s = s_pomo_total_s;
    s_pomo_tick_ms = now_ms();
    s_pomo_state = POMO_RUN;
    if (announce) sfx_play(SFX_START);
    ESP_LOGI(TAG, "pomodoro: %d min started", s_pomo_min[s_pomo_sel]);
}

static void pomo_finish(void) {
    s_pomo_state = POMO_DONE;
    s_pomo_left_s = 0;
    s_pomo_done_at = now_ms();
    s_pomo_sessions++;
    sfx_play(SFX_DONE);
    pomo_log("done");
    pomo_save();
    ESP_LOGI(TAG, "pomodoro: complete (%lu total)", (unsigned long)s_pomo_sessions);
}

/* ---- the screen ---- */

/* Which edge is physically at the top. Reads the same calibration autorotate
 * uses, so a correctly calibrated device gives a correctly oriented dial.
 *
 * There is deliberately no fudge factor here. One was added after misreading two
 * photographs — a photo has no gravity reference, so "the top label looks upright
 * in the picture" says nothing about which way the cube was actually facing. */
static int pomo_top_edge(void) {
    return rot_from_base(s_base_rot) & 3;
}

/* 0.1-degree units, clockwise. Edge i must be pre-rotated by -90*i so that
 * turning the cube by +90*i cancels it out and the label reads upright. */
static int32_t pomo_edge_angle(int i) {
    return (3600 - 900 * i) % 3600;
}

static void pomo_build_dial(lv_obj_t *scr) {
    static const lv_coord_t dx[POMO_SLOTS] = {   0,  176,    0, -176 };
    static const lv_coord_t dy[POMO_SLOTS] = { -176,   0,  176,    0 };

    for (int i = 0; i < POMO_SLOTS; i++) {
        lv_obj_t *l = lv_label_create(scr);
        lv_obj_set_size(l, 130, 28);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        /* rotate about the label's middle, not its corner */
        lv_obj_set_style_transform_pivot_x(l, 65, 0);
        lv_obj_set_style_transform_pivot_y(l, 14, 0);
        lv_obj_set_style_transform_rotation(l, pomo_edge_angle(i), 0);
        lv_label_set_text_fmt(l, "%d MIN", s_pomo_min[i]);
        lv_obj_align(l, LV_ALIGN_CENTER, dx[i], dy[i]);
        lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
        s_pomo_dial[i] = l;
    }
}

static void pomo_refresh(void) {
    if (!s_pomo_clock) return;

    int wr = pomo_top_edge();
    int32_t counter = (3600 - 900 * wr) % 3600;

    /* Highlight whichever label is physically at the top. */
    for (int i = 0; i < POMO_SLOTS; i++) {
        bool active = (i == wr);
        lv_obj_set_style_text_color(s_pomo_dial[i],
            lv_color_hex(active ? 0xFFB454 : 0x33465C), 0);
        lv_obj_set_style_text_letter_space(s_pomo_dial[i], active ? 3 : 1, 0);
    }

    if (wr != s_pomo_drawn_rot) {
        s_pomo_drawn_rot = wr;

        /* Rotating each label only spins it about its own centre — its offset
         * from the middle of the screen stays put. So the word, pinned 52 px
         * "below" the clock in screen space, swung round into the digits as soon
         * as the cube was turned. The offsets have to rotate with the content,
         * so the stack keeps reading clock-then-word whichever way is up. */
        static const lv_coord_t cx[4] = {   0, -14,   0,  14 };
        static const lv_coord_t cy[4] = { -14,   0,  14,   0 };
        static const lv_coord_t wx[4] = {   0,  52,   0, -52 };
        static const lv_coord_t wy[4] = {  52,   0, -52,   0 };

        lv_obj_set_style_transform_rotation(s_pomo_clock, counter, 0);
        lv_obj_set_style_transform_rotation(s_pomo_word, counter, 0);
        lv_obj_align(s_pomo_clock, LV_ALIGN_CENTER, cx[wr], cy[wr]);
        lv_obj_align(s_pomo_word,  LV_ALIGN_CENTER, wx[wr], wy[wr]);

        /* keep the depleting ring starting from world-up, not screen-up */
        lv_arc_set_rotation(s_pomo_arc,  (270 - 90 * wr + 360) % 360);
        lv_arc_set_rotation(s_pomo_fill, (270 - 90 * wr + 360) % 360);
    }

    int left = s_pomo_left_s < 0 ? 0 : s_pomo_left_s;
    lv_label_set_text_fmt(s_pomo_clock, "%02d:%02d", left / 60, left % 60);

    int pct = (s_pomo_total_s > 0)
            ? (int)(((int64_t)left * 360) / s_pomo_total_s) : 360;
    int elapsed = 360 - pct;
    int done_mille = (s_pomo_total_s > 0)
            ? (int)((((int64_t)s_pomo_total_s - left) * 1000) / s_pomo_total_s) : 0;

    uint32_t col;
    const char *word;
    switch (s_pomo_state) {
    case POMO_RUN:   col = 0xFFB454; word = "FOCUS";  break;
    case POMO_PAUSE: col = 0x60A5FA; word = "PAUSED"; break;
    case POMO_DONE:  col = 0x35C759; word = "DONE";   break;
    default:         col = 0x64748B; word = "TAP TO START";  break;
    }
    lv_label_set_text(s_pomo_word, word);
    lv_obj_set_style_text_color(s_pomo_word, lv_color_hex(col), 0);
    lv_obj_set_style_text_color(s_pomo_clock,
        lv_color_hex(s_pomo_state == POMO_PAUSE ? 0x7A8BA5 : 0xF2E9DC), 0);

    if (s_pomo_state == POMO_DONE) {
        /* a slow pulse rather than a static screen, so a finished session
         * catches the eye from across a room */
        int64_t age = now_ms() - s_pomo_done_at;
        int k = (int)((age / 40) % 50);
        int w = 8 + (k < 25 ? k : 50 - k) / 2;
        lv_obj_add_flag(s_pomo_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_width(s_pomo_arc, w, LV_PART_MAIN);
        lv_arc_set_bg_angles(s_pomo_arc, 0, 360);
        lv_obj_set_style_arc_color(s_pomo_arc, lv_color_hex(0x35C759), LV_PART_MAIN);
    } else {
        /* Elapsed time fills clockwise from the top in green, deepening from
         * near-black to vivid as the session runs out. Green is the AMOLED's
         * strongest primary and it has 6 bits in RGB565 against 5 for the
         * others, so this ramps smoothly and genuinely glows at the end —
         * arriving at the same green the completion pulse uses. */
        int g_r = 0x0C + (0x3A - 0x0C) * done_mille / 1000;
        int g_g = 0x30 + (0xFF - 0x30) * done_mille / 1000;
        int g_b = 0x1C + (0x78 - 0x1C) * done_mille / 1000;

        if (elapsed > 0) {
            lv_obj_remove_flag(s_pomo_fill, LV_OBJ_FLAG_HIDDEN);
            lv_arc_set_bg_angles(s_pomo_fill, 0, elapsed);
            lv_obj_set_style_arc_color(s_pomo_fill,
                lv_color_make(g_r, g_g, g_b), LV_PART_MAIN);
            /* swells very slightly as it completes */
            lv_obj_set_style_arc_width(s_pomo_fill,
                10 + 3 * done_mille / 1000, LV_PART_MAIN);
        } else {
            lv_obj_add_flag(s_pomo_fill, LV_OBJ_FLAG_HIDDEN);
        }

        /* what is left, running ahead of the green */
        lv_obj_set_style_arc_width(s_pomo_arc, 10, LV_PART_MAIN);
        lv_arc_set_bg_angles(s_pomo_arc, elapsed, 360);
        lv_obj_set_style_arc_color(s_pomo_arc, lv_color_hex(col), LV_PART_MAIN);
    }
}

/* The only thing a tap does. Not pause, not cancel, not skip — those are the
 * cube's job (lay it flat, or hold the right key). */
/* Pause and resume in one place, because there are now two ways to ask for it —
 * a tap and laying the cube flat — and two copies would drift.
 *
 * Resuming MUST re-stamp s_pomo_tick_ms. The countdown is driven by elapsed wall
 * time rather than by counting ticks, so without it the entire pause is charged to
 * the session the instant it resumes. Both callers get that for free here.
 *
 * Guarded on the current state, so asking for a transition that does not apply is
 * a no-op: that is what lets the tap and the gesture coexist without fighting. */
static void pomo_set_running(bool run, const char *why) {
    if (run && s_pomo_state == POMO_PAUSE) {
        s_pomo_state = POMO_RUN;
        s_pomo_tick_ms = now_ms();
        sfx_play(SFX_RESUME);
        ESP_LOGI(TAG, "pomodoro: resumed (%s)", why);
    } else if (!run && s_pomo_state == POMO_RUN) {
        s_pomo_state = POMO_PAUSE;
        sfx_play(SFX_PAUSE);
        ESP_LOGI(TAG, "pomodoro: paused (%s)", why);
    }
}

static void pomo_tap_cb(lv_event_t *e) {
    /* Tap starts an idle timer and toggles a live one. DONE deliberately ignores
     * taps: the finish screen retires itself after POMO_DONE_MS, and a stray tap
     * on it would otherwise start a whole new session by accident. */
    switch (s_pomo_state) {
    case POMO_IDLE:  pomo_begin(pomo_top_edge(), true); break;
    case POMO_RUN:   pomo_set_running(false, "tap");     break;
    case POMO_PAUSE: pomo_set_running(true,  "tap");     break;
    default: break;
    }
}

static void pomo_timer_cb(lv_timer_t *t) {
    if (s_screen_on) pomo_refresh();
}

static void build_pomo_app(lv_obj_t *scr) {
    /* near-black: on an AMOLED those pixels are simply off, which is what lets
     * this sit lit on a desk for an hour without eating the battery */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_pomo_ring = lv_arc_create(scr);
    lv_obj_set_size(s_pomo_ring, 404, 404);
    lv_obj_center(s_pomo_ring);
    lv_obj_remove_style(s_pomo_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_pomo_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_pomo_ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_pomo_ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_pomo_ring, lv_color_hex(0x1A2230), LV_PART_MAIN);
    lv_arc_set_bg_angles(s_pomo_ring, 0, 360);

    s_pomo_fill = lv_arc_create(scr);
    lv_obj_set_size(s_pomo_fill, 404, 404);
    lv_obj_center(s_pomo_fill);
    lv_obj_remove_style(s_pomo_fill, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_pomo_fill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_pomo_fill, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_pomo_fill, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_pomo_fill, lv_color_hex(0x0C301C), LV_PART_MAIN);
    lv_arc_set_bg_angles(s_pomo_fill, 0, 1);
    lv_obj_add_flag(s_pomo_fill, LV_OBJ_FLAG_HIDDEN);

    s_pomo_arc = lv_arc_create(scr);
    lv_obj_set_size(s_pomo_arc, 404, 404);
    lv_obj_center(s_pomo_arc);
    lv_obj_remove_style(s_pomo_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_pomo_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_pomo_arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_pomo_arc, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_pomo_arc, lv_color_hex(0xFFB454), LV_PART_MAIN);
    lv_arc_set_bg_angles(s_pomo_arc, 0, 360);

    pomo_build_dial(scr);

    s_pomo_clock = lv_label_create(scr);
    lv_obj_set_size(s_pomo_clock, 300, 90);
    lv_obj_set_style_text_font(s_pomo_clock, &hud_clock_76, 0);
    lv_obj_set_style_text_align(s_pomo_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_pivot_x(s_pomo_clock, 150, 0);
    lv_obj_set_style_transform_pivot_y(s_pomo_clock, 45, 0);
    lv_label_set_text(s_pomo_clock, "00:00");
    lv_obj_align(s_pomo_clock, LV_ALIGN_CENTER, 0, -14);

    s_pomo_word = lv_label_create(scr);
    lv_obj_set_size(s_pomo_word, 300, 26);
    lv_obj_set_style_text_font(s_pomo_word, &hud_text_18, 0);
    lv_obj_set_style_text_align(s_pomo_word, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_pivot_x(s_pomo_word, 150, 0);
    lv_obj_set_style_transform_pivot_y(s_pomo_word, 13, 0);
    lv_label_set_text(s_pomo_word, "");
    lv_obj_align(s_pomo_word, LV_ALIGN_CENTER, 0, 52);

    lv_obj_add_event_cb(scr, gesture_home_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, pomo_tap_cb, LV_EVENT_CLICKED, NULL);

    s_pomo_drawn_rot = -1;
    s_pomo_active_ms = now_ms();
    if (s_pomo_state == POMO_IDLE) {
        s_pomo_sel = pomo_top_edge();
        s_pomo_total_s = s_pomo_min[s_pomo_sel] * 60;
        s_pomo_left_s = s_pomo_total_s;
    }
    pomo_refresh();
    s_app_timer = lv_timer_create(pomo_timer_cb, 250, NULL);
}

/* ---- the physical side, driven from the main loop ---- */

/* Lying down, either face up. Testing |z| rather than a signed value keeps this
 * independent of how the IMU is mounted relative to the panel, which is not
 * documented anywhere and had to be calibrated empirically for autorotate. */
static bool pomo_is_flat(void) {
    return abs(s_acc_z) > POMO_FLAT_TH;
}

static void pomo_poll(int64_t t) {
    if (s_app != APP_POMO) {
        if (s_pomo_dimmed) {                 /* never leave the panel dimmed */
            bright_apply(s_bright);
            s_pomo_dimmed = false;
        }
        return;
    }

    /* --- flat means pause --- */
    bool flat = pomo_is_flat();
    if (flat != s_pomo_flat) {
        if (!s_pomo_flat_since) s_pomo_flat_since = t;
        if (t - s_pomo_flat_since >= POMO_FLAT_MS) {
            s_pomo_flat = flat;
            s_pomo_flat_since = 0;
            /* Flat pauses, upright resumes. The helper ignores whichever of the
             * two does not apply, so a session already paused by tap is not
             * disturbed by the cube being set down. */
            pomo_set_running(!flat, flat ? "laid flat" : "upright");
        }
    } else {
        s_pomo_flat_since = 0;
    }

    /* --- turning the cube picks a duration and starts it --- */
    int wr = pomo_top_edge();
    if (s_pomo_last_rot < 0) s_pomo_last_rot = wr;
    if (wr != s_pomo_last_rot && !flat) {
        s_pomo_last_rot = wr;
        sfx_play(SFX_TICK);                       /* the detent */
        s_pomo_active_ms = t;
        if (s_pomo_state == POMO_RUN || s_pomo_state == POMO_PAUSE) {
            pomo_begin(wr, false);                /* mid-session: switch and restart */
        } else {
            /* Idle turning only previews. Requiring a turn to start meant that
             * if the cube already sat on the duration you wanted, you had to go
             * all the way around to get back to it. */
            s_pomo_sel = wr;
            s_pomo_total_s = s_pomo_min[wr] * 60;
            s_pomo_left_s = s_pomo_total_s;
        }
    }

    /* --- the countdown itself --- */
    if (s_pomo_state == POMO_RUN) {
        while (t - s_pomo_tick_ms >= 1000) {
            s_pomo_tick_ms += 1000;
            if (--s_pomo_left_s <= 0) { pomo_finish(); break; }
        }
        /* Keep the device awake. idle is min(LVGL inactivity, time since a key),
         * and a running countdown touches neither, so without this the auto-lock
         * would tear the app down 60 s in. */
        lv_display_trigger_activity(NULL);
    } else if (s_pomo_state == POMO_PAUSE) {
        lv_display_trigger_activity(NULL);
    } else if (s_pomo_state == POMO_DONE) {
        lv_display_trigger_activity(NULL);
        if (t - s_pomo_done_at > POMO_DONE_MS) {
            s_pomo_state = POMO_IDLE;
            app_request(APP_LOCK);                /* settle into the clock */
        }
    }

    /* --- dim when nothing is happening, wake on touch or movement --- */
    int dx = abs(s_acc_x - s_acc_ref_x) + abs(s_acc_y - s_acc_ref_y) +
             abs(s_acc_z - s_acc_ref_z);
    s_acc_ref_x = s_acc_x; s_acc_ref_y = s_acc_y; s_acc_ref_z = s_acc_z;
    bool touched = lv_display_get_inactive_time(NULL) < 1000;
    if (dx > POMO_MOTION_TH || touched) s_pomo_active_ms = t;

    bool want_dim = (t - s_pomo_active_ms > POMO_DIM_MS);
    if (want_dim != s_pomo_dimmed) {
        s_pomo_dimmed = want_dim;
        /* Proportional, not the flat constant, and not a min() either. Flat 12
         * would make "dimming" brighter for anyone running the panel below it;
         * min(user, 12) fixes that but then dims by nothing at all at
         * BRIGHT_MIN, silently retiring the feature for exactly the people who
         * chose a dark panel on purpose. A percentage of the user's level is
         * identical to today at 100 (100 * 12 / 100 = 12) and still a real
         * three-of-ten dim at the floor. */
        int lvl = s_bright;          /* one read: it is volatile and used thrice */
        int dim = clampi(lvl * POMO_DIM_PCT / 100, POMO_DIM_FLOOR, lvl);
        /* bright_apply(), not a bare bsp_ call: this runs on the main task while
         * pomo_timer_cb is repainting at 4 Hz, so the panel IO is contended
         * every single time this fires — pitfall #13's worst case, not its
         * rarest. It was unlocked here for a long time and got away with it. */
        bright_apply(want_dim ? dim : lvl);
        ESP_LOGI(TAG, "pomodoro: %s (panel %d%%)",
                 want_dim ? "dimmed" : "undimmed", want_dim ? dim : lvl);
    }
}

/* ---------------- MUSIC: a Spotify remote ----------------
 *
 * Controls whatever is already playing — phone, laptop, speaker — and never
 * plays audio itself.
 *
 * Three measured facts shape all of this:
 *
 *  - One reused connection turns a 390 ms call into 6 ms, and it survives idle
 *    gaps (HARDWARE.md 7f). So a single long-lived esp_http_client handle serves
 *    every endpoint, and polling costs almost nothing.
 *  - That handle is NOT thread-safe and mutates state per request, so it lives on
 *    exactly one task and touch callbacks reach it through a queue.
 *  - Album art arrives from the broker pre-decoded as an LVGL RGB565 .bin, so the
 *    device does no image decoding at all and LVGL streams rows off the card.
 */

#define SP_API        "https://api.spotify.com/v1"
#define SP_TOKEN_URL  "https://accounts.spotify.com/api/token"
#define SP_POLL_MS    3000
/* The cover has to end above the track title at y262, and it starts at y110, so
 * 148 is the whole budget. It was 240 for three commits — the Apple Watch
 * relayout moved the labels up and never shrank the art, which drew the title,
 * the artist and the top of the play button straight over the artwork. The fetch
 * uses the same number: asking the broker for exactly the drawn size means no
 * scaling and a 43 KB file instead of 115 KB. */
#define SP_ART_PX     148
/* The cover lives in PSRAM, not on the card. LVGL's bin decoder can stream rows
 * off FATFS per draw chunk, which is what made a file cheap — but that is ~15 card
 * reads per frame, and it collapses the moment the image has to be scaled (the
 * lock screen draws this 148 px cover into a 100 px slot). 43,824 bytes against
 * 8 MB of free PSRAM removes the card from both the write and the render path, and
 * an in-memory descriptor needs no decoder at all: LVGL blits RGB565 straight out
 * of the buffer. */
#define SP_ART_HDR    12
#define SP_ART_BYTES  (SP_ART_HDR + SP_ART_PX * SP_ART_PX * 2)

typedef enum {
    SP_CMD_POLL = 1,
    SP_CMD_PLAY, SP_CMD_PAUSE, SP_CMD_NEXT, SP_CMD_PREV, SP_CMD_SHUFFLE,
    SP_CMD_DEVICES,
    SP_CMD_TRANSFER,
    SP_CMD_VOLUME,
    SP_CMD_LIKE,
} sp_cmd_t;

#define SP_MAX_DEV 8
typedef struct {
    char id[42];
    char name[34];
    bool active;
} sp_dev_t;

/* Player state. Written only by the spotify task, read by the UI timer — all
 * scalars or fixed buffers, so a torn read shows a stale field, never a crash. */
static char s_sp_track[64];
static char s_sp_artist[64];
static char s_sp_devname[34];
static char s_sp_art_url[160];
static char s_sp_art_have[160];        /* url currently on the card */
static volatile bool s_sp_playing;
static volatile bool s_sp_shuffle;
static volatile bool s_sp_have_state;  /* a device is active */
static volatile bool s_sp_authfail;
static volatile bool s_sp_art_ready;
/* Spotify reports which transport actions are currently impossible via
 * actions.disallows — shuffle is genuinely unavailable while a radio/autoplay
 * context is running, and skipping can be blocked too. Only disallowed actions
 * appear in that object, so absent means allowed. */
static volatile bool s_sp_no_shuffle, s_sp_no_next, s_sp_no_prev;
/* Liked state. Spotify deprecated /me/tracks/contains and /me/tracks in the
 * February 2026 Dev Mode changes: both now answer 403 even with
 * user-library-read granted, while /me/tracks?limit=1 on the same token still
 * returns 200 — which reads exactly like a scope problem and is not one. The
 * replacement is /me/library, keyed on Spotify **URIs** rather than bare ids. */
static char s_sp_track_id[26];
static char s_sp_liked_id[26];          /* which id s_sp_liked refers to */
static volatile bool s_sp_liked;
static volatile bool s_sp_liked_known;

/* Volume. s_sp_vol is what the UI shows and what the keys move: stepped locally
 * so the bar tracks the finger. s_sp_vol_sent is the last value Spotify accepted,
 * so the two being unequal *is* the "one PUT owed" flag — no separate request
 * slot to lose. A held key therefore costs a trickle of requests rather than one
 * per step, and the pair can only ever converge. */
/* Accent tint, from the broker's X-Art-Accent header on the art fetch. Spotify
 * green is the fallback and it is what a monochrome cover keeps: the broker
 * declines to guess rather than handing back a grey that would be
 * indistinguishable from the UI's own chrome. */
/* 0 means "no accent known yet". The backdrop stays black until a cover has
 * actually supplied one — a default-tinted screen while the app is still waiting
 * for /me/player reads as a colour chosen on purpose, which is worse than black. */
static volatile uint32_t s_sp_accent;

/* The accent is a full-strength colour, fit for a glyph but not for 480x480 of
 * backdrop: at full value it fights white text and, on an AMOLED, every lit pixel
 * costs power. A quarter strength reads clearly as "this album is copper" while
 * leaving the controls and the title legible on top. Flat, never a gradient —
 * RGB565 bands visibly on a dark ramp (HARDWARE.md 5). */
static uint32_t accent_bg(uint32_t c) {
    uint32_t r = ((c >> 16) & 0xFF) * 26 / 100;
    uint32_t g = ((c >> 8) & 0xFF) * 26 / 100;
    uint32_t b = (c & 0xFF) * 26 / 100;
    return (r << 16) | (g << 8) | b;
}

static volatile int  s_sp_vol = -1;     /* 0..100, -1 = not read yet */
static volatile int  s_sp_vol_sent = -1;
static volatile bool s_sp_vol_ok;       /* device reports supports_volume */
static int  s_sp_vol_premute = 35;      /* restore point for unmute */
static int64_t s_sp_vol_shown;          /* when the HUD last had something to say */
static sp_dev_t s_sp_dev[SP_MAX_DEV];
static volatile int s_sp_devcount;
static volatile int s_sp_transfer_idx = -1;

static char s_sp_access[320];
static int64_t s_sp_expires_ms;         /* when the access token dies */

static QueueHandle_t s_sp_q;
static esp_http_client_handle_t s_sp_http;
static char *s_sp_body;                 /* PSRAM response buffer */
static int s_sp_len;
#define SP_BODY_MAX 8192

/* Spotify text is real-world Unicode: typographic apostrophes, accented artist
 * names, em dashes. Our fonts carry ASCII plus LVGL's own symbols and nothing
 * else, so any of that renders as an empty box — "Jainal's MacBook Pro" came
 * back with U+2019 and showed as "Jainal[]s MacBook Pro".
 *
 * Folding to ASCII beats shipping a bigger font: a full Latin-1 range costs flash
 * for glyphs chosen by guessing which languages turn up, and would still box on
 * the first Cyrillic or CJK track title. A missing accent is legible; a box is
 * not. */
static void ascii_fold(const char *in, char *out, size_t n) {
    size_t o = 0;
    while (*in && o + 1 < n) {
        unsigned char c = (unsigned char)*in;
        uint32_t cp;
        int len;
        if (c < 0x80)        { cp = c;             len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { in++; continue; }                  /* stray continuation byte */
        for (int k = 1; k < len; k++) {
            if ((in[k] & 0xC0) != 0x80) { cp = 0xFFFD; len = k; break; }
            cp = (cp << 6) | (in[k] & 0x3F);
        }
        in += len;

        const char *rep = NULL;
        if (cp < 0x80) { out[o++] = (char)cp; continue; }
        switch (cp) {
        case 0x2018: case 0x2019: case 0x02BC: rep = "'";   break;  /* curly quotes */
        case 0x201C: case 0x201D: rep = "\"";               break;
        case 0x2013: case 0x2014: case 0x2212: rep = "-";   break;  /* dashes */
        case 0x2026: rep = "...";                           break;
        case 0x00A0: rep = " ";                             break;
        case 0x00E6: rep = "ae"; break;  case 0x00C6: rep = "AE"; break;
        case 0x00DF: rep = "ss"; break;
        case 0x0153: rep = "oe"; break;  case 0x0152: rep = "OE"; break;
        default:
            /* Latin-1 / Latin Extended-A: keep the base letter. */
            if (cp >= 0x00C0 && cp <= 0x00FF) {
                static const char *lat =
                    "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPs"
                    "aaaaaaaceeeeiiiidnooooo/ouuuuypy";
                rep = NULL;
                out[o++] = lat[cp - 0x00C0];
                continue;
            }
            if (cp >= 0x0100 && cp <= 0x017F) {
                /* Latin Extended-A alternates upper/lower around a base letter;
                 * close enough for a name, and far better than a box. */
                static const char *ext = "AaAaAaCcCcCcCcCcDdDdEeEeEeEeEeGgGgGgGg"
                                         "HhHhIiIiIiIiIiJjKkkLlLlLlLlLlNnNnNnnNn"
                                         "OoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUu"
                                         "WwYyYZzZzZzs";
                size_t idx = cp - 0x0100;
                out[o++] = (idx < strlen(ext)) ? ext[idx] : '?';
                continue;
            }
            rep = "?";
            break;
        }
        if (rep) {
            while (*rep && o + 1 < n) out[o++] = *rep++;
        }
    }
    out[o] = '\0';
}

/* ---- HTTP plumbing, all on the spotify task ---- */

static esp_err_t sp_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_DATA && s_sp_body) {
        int n = e->data_len;
        if (s_sp_len + n > SP_BODY_MAX - 1) n = SP_BODY_MAX - 1 - s_sp_len;
        if (n > 0) {
            memcpy(s_sp_body + s_sp_len, e->data, n);
            s_sp_len += n;
            s_sp_body[s_sp_len] = '\0';
        }
    }
    return ESP_OK;
}

/* Refresh the access token. No client secret: this is a PKCE public client, and
 * Spotify returns no new refresh token for this grant, so the stored one keeps
 * working and never has to be re-persisted. */
static bool sp_refresh_token(void) {
    if (SPOTIFY_REFRESH_TOKEN[0] == '\0' || SPOTIFY_CLIENT_ID[0] == '\0') return false;

    char *body = heap_caps_malloc(1024, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) return false;
    snprintf(body, 1024, "grant_type=refresh_token&refresh_token=%s&client_id=%s",
             SPOTIFY_REFRESH_TOKEN, SPOTIFY_CLIENT_ID);

    s_sp_len = 0;
    if (s_sp_body) s_sp_body[0] = '\0';

    esp_http_client_config_t cfg = {
        .url = SP_TOKEN_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 12000,
        .event_handler = sp_evt,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(c, body, strlen(body));
        esp_http_client_perform(c);
        int code = esp_http_client_get_status_code(c);
        if (code == 200 && s_sp_len > 0) {
            cJSON *j = cJSON_Parse(s_sp_body);
            if (j) {
                cJSON *at = cJSON_GetObjectItem(j, "access_token");
                cJSON *ex = cJSON_GetObjectItem(j, "expires_in");
                if (cJSON_IsString(at)) {
                    snprintf(s_sp_access, sizeof(s_sp_access), "%s", at->valuestring);
                    int secs = cJSON_IsNumber(ex) ? ex->valueint : 3600;
                    s_sp_expires_ms = now_ms() + (int64_t)secs * 1000;
                    ok = true;
                    ESP_LOGI(TAG, "spotify: token refreshed, valid %d s", secs);
                }
                cJSON_Delete(j);
            }
        } else {
            ESP_LOGE(TAG, "spotify: token refresh failed, HTTP %d", code);
        }
        esp_http_client_cleanup(c);
    }
    free(body);
    s_sp_authfail = !ok;
    return ok;
}

static bool sp_token_ok(void) {
    /* Refresh early rather than discovering expiry via a 401: that path leaves
     * the socket undrained (HARDWARE.md 7f) and costs a reconnect. */
    if (s_sp_access[0] && now_ms() < s_sp_expires_ms - 120000) return true;
    return sp_refresh_token();
}

/* One handle for every endpoint. Only the path changes, and esp_http_client only
 * drops the connection when host or port change, so this keeps the 6 ms path. */
static int sp_call(esp_http_client_method_t method, const char *path, const char *json)
{
    if (!sp_token_ok()) return -1;

    char url[224];
    snprintf(url, sizeof(url), "%s%s", SP_API, path);

    if (!s_sp_http) {
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 12000,
            .event_handler = sp_evt,
            .keep_alive_enable = true,
        };
        s_sp_http = esp_http_client_init(&cfg);
        if (!s_sp_http) return -1;
    } else {
        esp_http_client_set_url(s_sp_http, url);
    }

    char auth[352];
    snprintf(auth, sizeof(auth), "Bearer %s", s_sp_access);
    esp_http_client_set_header(s_sp_http, "Authorization", auth);
    esp_http_client_set_method(s_sp_http, method);

    /* A body set on a previous request persists on a reused handle, so a bodiless
     * POST would resend the last JSON. Clearing it also drops Content-Type. */
    if (json) {
        esp_http_client_set_header(s_sp_http, "Content-Type", "application/json");
        esp_http_client_set_post_field(s_sp_http, json, strlen(json));
    } else {
        esp_http_client_set_post_field(s_sp_http, NULL, 0);
    }

    s_sp_len = 0;
    if (s_sp_body) s_sp_body[0] = '\0';
    esp_err_t err = esp_http_client_perform(s_sp_http);
    int code = esp_http_client_get_status_code(s_sp_http);

    if (code == 401) {
        /* perform() bails on the Bearer challenge before draining the body, so
         * the socket has unread bytes. Flush before reusing, then retry once. */
        esp_http_client_flush_response(s_sp_http, NULL);
        s_sp_access[0] = '\0';
        if (sp_refresh_token()) {
            snprintf(auth, sizeof(auth), "Bearer %s", s_sp_access);
            esp_http_client_set_header(s_sp_http, "Authorization", auth);
            s_sp_len = 0;
            if (s_sp_body) s_sp_body[0] = '\0';
            err = esp_http_client_perform(s_sp_http);
            code = esp_http_client_get_status_code(s_sp_http);
        }
    }

    /* A transport failure leaves the socket unusable, and perform() will not
     * redial on its own — it retries the dead fd forever. Observed live: the
     * server reset a connection mid-transfer and every 3 s poll after it logged
     * "esp_tls_conn_read error / Socket is not connected" while the app sat
     * frozen, with no error path anywhere near the UI. Closing here costs one
     * 390 ms handshake on the next call and is the only way back.
     *
     * ESP_ERR_NOT_SUPPORTED is the Bearer-challenge return handled above, not a
     * transport fault, so it must not trigger a redial. */
    if (err != ESP_OK && !(err == ESP_ERR_NOT_SUPPORTED && code > 0)) {
        ESP_LOGW(TAG, "spotify: transport error %s (HTTP %d) — redialling",
                 esp_err_to_name(err), code);
        esp_http_client_close(s_sp_http);
        code = 0;
    }
    return code;
}

/* ---- state ---- */

static void sp_poll_state(void) {
    int code = sp_call(HTTP_METHOD_GET, "/me/player", NULL);

    if (code == 204 || (code == 200 && s_sp_len < 4)) {
        s_sp_have_state = false;        /* nothing playing anywhere */
        return;
    }
    if (code != 200 || s_sp_len == 0) return;

    cJSON *j = cJSON_Parse(s_sp_body);
    if (!j) return;

    cJSON *item = cJSON_GetObjectItem(j, "item");
    cJSON *dev  = cJSON_GetObjectItem(j, "device");
    cJSON *pl   = cJSON_GetObjectItem(j, "is_playing");
    cJSON *sh   = cJSON_GetObjectItem(j, "shuffle_state");

    cJSON *act = cJSON_GetObjectItem(j, "actions");
    cJSON *dis = act ? cJSON_GetObjectItem(act, "disallows") : NULL;
    s_sp_no_shuffle = dis && cJSON_IsTrue(cJSON_GetObjectItem(dis, "toggling_shuffle"));
    s_sp_no_next    = dis && cJSON_IsTrue(cJSON_GetObjectItem(dis, "skipping_next"));
    s_sp_no_prev    = dis && cJSON_IsTrue(cJSON_GetObjectItem(dis, "skipping_prev"));

    s_sp_playing = cJSON_IsTrue(pl);
    s_sp_shuffle = cJSON_IsTrue(sh);
    s_sp_have_state = true;

    if (dev) {
        cJSON *n = cJSON_GetObjectItem(dev, "name");
        if (cJSON_IsString(n)) ascii_fold(n->valuestring, s_sp_devname, sizeof(s_sp_devname));

        /* Plenty of endpoints cannot be set remotely — Echos and most Connect
         * speakers report false — so the keys have to be able to say "no". */
        s_sp_vol_ok = cJSON_IsTrue(cJSON_GetObjectItem(dev, "supports_volume"));

        /* Adopt the server's level only when nothing local is owed. Spotify keeps
         * reporting the pre-PUT value for a moment, so trusting it while a press
         * is in flight makes the bar jump backwards under the finger. */
        cJSON *vp = cJSON_GetObjectItem(dev, "volume_percent");
        if (cJSON_IsNumber(vp) && s_sp_vol == s_sp_vol_sent) {
            s_sp_vol = s_sp_vol_sent = vp->valueint;
        }
    }
    if (item) {
        cJSON *n = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(n)) ascii_fold(n->valuestring, s_sp_track, sizeof(s_sp_track));

        cJSON *tid = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(tid)) {
            snprintf(s_sp_track_id, sizeof(s_sp_track_id), "%s", tid->valuestring);
            /* A stale heart is worse than no heart: it invites a tap that
             * un-likes something. Drop it until this track is checked. */
            if (strcmp(s_sp_track_id, s_sp_liked_id) != 0) s_sp_liked_known = false;
        }

        cJSON *arts = cJSON_GetObjectItem(item, "artists");
        if (cJSON_IsArray(arts) && cJSON_GetArraySize(arts) > 0) {
            cJSON *a0 = cJSON_GetArrayItem(arts, 0);
            cJSON *an = a0 ? cJSON_GetObjectItem(a0, "name") : NULL;
            if (cJSON_IsString(an)) ascii_fold(an->valuestring, s_sp_artist, sizeof(s_sp_artist));
        }

        /* Pick the image nearest the tile size; the broker rescales anyway, but
         * asking for a smaller source keeps its fetch cheap. */
        cJSON *alb = cJSON_GetObjectItem(item, "album");
        cJSON *imgs = alb ? cJSON_GetObjectItem(alb, "images") : NULL;
        if (cJSON_IsArray(imgs)) {
            const char *best = NULL;
            int bestd = 1 << 30;
            for (int i = 0; i < cJSON_GetArraySize(imgs); i++) {
                cJSON *im = cJSON_GetArrayItem(imgs, i);
                cJSON *u = cJSON_GetObjectItem(im, "url");
                cJSON *w = cJSON_GetObjectItem(im, "width");
                if (!cJSON_IsString(u) || !cJSON_IsNumber(w)) continue;
                int d = abs(w->valueint - SP_ART_PX);
                if (d < bestd) { bestd = d; best = u->valuestring; }
            }
            if (best) snprintf(s_sp_art_url, sizeof(s_sp_art_url), "%s", best);
        }
    }
    cJSON_Delete(j);
}

static void sp_poll_devices(void) {
    int code = sp_call(HTTP_METHOD_GET, "/me/player/devices", NULL);
    if (code != 200 || s_sp_len == 0) return;

    cJSON *j = cJSON_Parse(s_sp_body);
    if (!j) return;
    cJSON *devs = cJSON_GetObjectItem(j, "devices");
    int n = 0;
    if (cJSON_IsArray(devs)) {
        for (int i = 0; i < cJSON_GetArraySize(devs) && n < SP_MAX_DEV; i++) {
            cJSON *d = cJSON_GetArrayItem(devs, i);
            cJSON *id = cJSON_GetObjectItem(d, "id");
            cJSON *nm = cJSON_GetObjectItem(d, "name");
            if (!cJSON_IsString(id) || !cJSON_IsString(nm)) continue;
            snprintf(s_sp_dev[n].id, sizeof(s_sp_dev[0].id), "%s", id->valuestring);
            ascii_fold(nm->valuestring, s_sp_dev[n].name, sizeof(s_sp_dev[0].name));
            s_sp_dev[n].active = cJSON_IsTrue(cJSON_GetObjectItem(d, "is_active"));
            n++;
        }
    }
    s_sp_devcount = n;
    cJSON_Delete(j);
    ESP_LOGI(TAG, "spotify: %d device(s)", n);
}

/* Is the current track in Liked Songs? Needs user-library-read. Checked once per
 * track rather than per poll — the answer only changes when we change it. */
/* Push the level if one is owed. Nothing is cleared up front, so a press landing
 * mid-flight cannot be dropped by a read-then-clear race: the trailing check
 * simply notices s_sp_vol moved again and queues another round. Called on every
 * poll too, which makes any missed command self-healing within one cycle. */
static void sp_send(sp_cmd_t c);        /* defined with the task, below */

/* The colon in a Spotify URI is legal in a query string, but %3A is what was
 * verified against the live API, so that is what goes on the wire. */
#define SP_URI_FMT "spotify%%3Atrack%%3A%s"

static void sp_check_liked(void) {
    if (!s_sp_track_id[0]) return;
    if (s_sp_liked_known && strcmp(s_sp_track_id, s_sp_liked_id) == 0) return;

    char path[96];
    snprintf(path, sizeof(path), "/me/library/contains?uris=" SP_URI_FMT, s_sp_track_id);
    int code = sp_call(HTTP_METHOD_GET, path, NULL);
    if (code == 200 && s_sp_len > 0) {
        s_sp_liked = (strstr(s_sp_body, "true") != NULL);   /* body is [true]/[false] */
        snprintf(s_sp_liked_id, sizeof(s_sp_liked_id), "%s", s_sp_track_id);
        s_sp_liked_known = true;
    } else {
        /* leave the heart blank rather than showing a state we cannot verify */
        s_sp_liked_known = false;
        ESP_LOGW(TAG, "spotify: library check failed (HTTP %d)", code);
    }
}

static void sp_toggle_like(void) {
    if (!s_sp_track_id[0]) return;
    char path[96];
    snprintf(path, sizeof(path), "/me/library?uris=" SP_URI_FMT, s_sp_track_id);
    /* s_sp_liked already holds the optimistic target set by the tap. */
    int code = sp_call(s_sp_liked ? HTTP_METHOD_PUT : HTTP_METHOD_DELETE, path, NULL);
    ESP_LOGI(TAG, "spotify: %s -> HTTP %d", s_sp_liked ? "like" : "unlike", code);
    if (code == 200 || code == 204) {
        snprintf(s_sp_liked_id, sizeof(s_sp_liked_id), "%s", s_sp_track_id);
        s_sp_liked_known = true;
    } else {
        s_sp_liked = !s_sp_liked;      /* it did not take — put the heart back */
        s_sp_liked_known = false;
    }
}

static void sp_push_volume(void) {
    int v = s_sp_vol;
    if (v < 0 || v == s_sp_vol_sent) return;      /* nothing owed */

    char path[56];
    snprintf(path, sizeof(path), "/me/player/volume?volume_percent=%d", v);
    int code = sp_call(HTTP_METHOD_PUT, path, NULL);
    if (code == 200 || code == 204) {
        s_sp_vol_sent = v;
        ESP_LOGI(TAG, "spotify: volume -> %d", v);
    } else if (code == 403 || code == 404) {
        /* the endpoint refuses remote volume after all — stop pretending */
        s_sp_vol_ok = false;
        s_sp_vol_sent = v;                        /* don't retry forever */
        ESP_LOGW(TAG, "spotify: volume rejected (HTTP %d)", code);
    }

    /* moved again while that was in flight — chase it rather than settle short */
    if (s_sp_vol != s_sp_vol_sent) sp_send(SP_CMD_VOLUME);
}

/* Album art, pre-decoded by the broker into LVGL's RGB565 binary format. The
 * device never decodes an image: LVGL's bin decoder reads rows straight off the
 * card per draw chunk. Reuses the wallpaper download path wholesale. */
/* A failed art fetch used to retry on every poll, because only success recorded
 * the URL. A truncated download therefore became a 43 KB request every 3 s —
 * hammering the broker and holding internal SRAM near its floor for as long as
 * the track played. Back off instead: art is decoration, and the next track
 * clears the block anyway. */
#define SP_ART_RETRY_MS 30000
static int64_t s_sp_art_failed_at;
static char s_sp_art_failed[160];

static void sp_fetch_art(void) {
    if (!s_sd_ok || !s_sp_art_url[0] || BROKER_URL[0] == '\0') return;

    /* Same art as the file already on the card — the next track off the same
     * album, usually. Re-assert ready rather than returning silently: sp_art_clear()
     * lowers the flag on every track change, and without this the placeholder
     * would stay up forever for anything that does not need a new download. */
    if (strcmp(s_sp_art_url, s_sp_art_have) == 0) {
        s_sp_art_ready = true;
        return;
    }

    if (strcmp(s_sp_art_url, s_sp_art_failed) == 0 &&
        (now_ms() - s_sp_art_failed_at) < SP_ART_RETRY_MS) return;

    char url[512];
    int n = snprintf(url, sizeof(url), "%s/art.bin?s=%d&u=", BROKER_URL, SP_ART_PX);
    /* percent-encode the Spotify URL into the query */
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s_sp_art_url;
         *p && n < (int)sizeof(url) - 4; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            url[n++] = (char)*p;
        } else {
            url[n++] = '%'; url[n++] = hex[*p >> 4]; url[n++] = hex[*p & 0xF];
        }
    }
    url[n] = '\0';

    if (!s_sp_art_buf) {
        s_sp_art_buf = heap_caps_malloc(SP_ART_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sp_art_buf) { ESP_LOGE(TAG, "spotify: no PSRAM for art"); return; }
    }

    size_t got = 0;
    bool ok = asset_fetch_mem(url, BROKER_TOKEN, s_sp_art_buf, SP_ART_BYTES, &got);

    /* Trust the bytes only if they are the shape that was asked for. A short body
     * or a broker that answered with something else would otherwise be blitted
     * straight to the panel as garbage — there is no decoder in this path to
     * reject it on the way through. */
    if (ok && got == SP_ART_BYTES) {
        uint16_t w = (uint16_t)(s_sp_art_buf[4] | (s_sp_art_buf[5] << 8));
        uint16_t h = (uint16_t)(s_sp_art_buf[6] | (s_sp_art_buf[7] << 8));
        if (s_sp_art_buf[0] != 0x19 || s_sp_art_buf[1] != 0x12 ||
            w != SP_ART_PX || h != SP_ART_PX) {
            ESP_LOGW(TAG, "spotify: art header wrong (%02x %02x %ux%u)",
                     s_sp_art_buf[0], s_sp_art_buf[1], w, h);
            ok = false;
        }
    } else if (ok) {
        ESP_LOGW(TAG, "spotify: art short (%u of %u)", (unsigned)got, SP_ART_BYTES);
        ok = false;
    }

    if (ok) {
        snprintf(s_sp_art_have, sizeof(s_sp_art_have), "%s", s_sp_art_url);
        s_sp_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        s_sp_art_dsc.header.cf    = LV_COLOR_FORMAT_RGB565;
        s_sp_art_dsc.header.w     = SP_ART_PX;
        s_sp_art_dsc.header.h     = SP_ART_PX;
        s_sp_art_dsc.header.stride = SP_ART_PX * 2;
        s_sp_art_dsc.data      = s_sp_art_buf + SP_ART_HDR;
        s_sp_art_dsc.data_size = SP_ART_PX * SP_ART_PX * 2;

        /* Swap it in here. This function already holds the lock to drop the cache,
         * and the alternative was leaving the image ready on this task while the
         * 400 ms UI timer got round to it — up to 400 ms of dead wait for nothing. */
        if (ui_lock()) {
            lv_image_cache_drop(&s_sp_art_dsc);   /* same pointer, new bytes */
            if (s_sp_art) {
                lv_image_set_src(s_sp_art, &s_sp_art_dsc);
                lv_obj_remove_flag(s_sp_art, LV_OBJ_FLAG_HIDDEN);
                if (s_sp_art_ph) lv_obj_add_flag(s_sp_art_ph, LV_OBJ_FLAG_HIDDEN);
            }
            if (s_lock_np_art) {
                lv_image_set_src(s_lock_np_art, &s_sp_art_dsc);
                lv_obj_remove_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN);
            }
            bsp_display_unlock();
        }
        snprintf(s_sp_art_shown, sizeof(s_sp_art_shown), "%s", s_sp_art_have);
        s_sp_art_ready = true;
        s_sp_art_failed[0] = '\0';
        if (s_dl_accent) s_sp_accent = s_dl_accent;
        ESP_LOGI(TAG, "spotify: art updated (%u B, psram)", (unsigned)got);
    } else {
        snprintf(s_sp_art_failed, sizeof(s_sp_art_failed), "%s", s_sp_art_url);
        s_sp_art_failed_at = now_ms();
        ESP_LOGW(TAG, "spotify: art fetch failed, backing off %d s",
                 SP_ART_RETRY_MS / 1000);
    }
}

static void sp_task(void *arg) {
    s_sp_body = heap_caps_malloc(SP_BODY_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_sp_body) {
        ESP_LOGE(TAG, "spotify: no PSRAM for the response buffer");
        vTaskDelete(NULL);
        return;
    }

    sp_cmd_t cmd;
    while (1) {
        if (xQueueReceive(s_sp_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (!s_wifi_up) continue;

        switch (cmd) {
        /* sp_push_volume() is a no-op unless a level is owed, so putting it on the
         * poll costs nothing and guarantees a dropped command is picked up. */
        case SP_CMD_POLL:    sp_poll_state(); sp_push_volume();
                             sp_check_liked(); sp_fetch_art(); break;
        case SP_CMD_DEVICES: sp_poll_devices(); break;
        case SP_CMD_LIKE:    sp_toggle_like(); break;
        /* No confirm poll: the bar is already showing the value we just sent,
         * and a read-back would only fight the next press. */
        case SP_CMD_VOLUME:  sp_push_volume(); break;

        /* Optimistic UI: the icon already flipped, so a poll follows to confirm
         * rather than to discover. */
        case SP_CMD_PLAY:    sp_call(HTTP_METHOD_PUT,  "/me/player/play", NULL);  goto confirm;
        case SP_CMD_PAUSE:   sp_call(HTTP_METHOD_PUT,  "/me/player/pause", NULL); goto confirm;
        case SP_CMD_NEXT:    sp_call(HTTP_METHOD_POST, "/me/player/next", NULL);  goto confirm;
        case SP_CMD_PREV:    sp_call(HTTP_METHOD_POST, "/me/player/previous", NULL); goto confirm;
        case SP_CMD_SHUFFLE: {
            char p[64];
            snprintf(p, sizeof(p), "/me/player/shuffle?state=%s",
                     s_sp_shuffle ? "true" : "false");
            sp_call(HTTP_METHOD_PUT, p, NULL);
            goto confirm;
        }
        case SP_CMD_TRANSFER: {
            int idx = s_sp_transfer_idx;
            if (idx >= 0 && idx < s_sp_devcount) {
                char body[96];
                snprintf(body, sizeof(body), "{\"device_ids\":[\"%s\"],\"play\":true}",
                         s_sp_dev[idx].id);
                int code = sp_call(HTTP_METHOD_PUT, "/me/player", body);
                ESP_LOGI(TAG, "spotify: transfer to %s -> HTTP %d",
                         s_sp_dev[idx].name, code);
                /* is_active moved, so the cached list is now wrong */
                vTaskDelay(pdMS_TO_TICKS(400));
                sp_poll_devices();
            }
            goto confirm;
        }
        default: break;
        }
        continue;

    confirm:
        vTaskDelay(pdMS_TO_TICKS(350));   /* let Spotify settle before reading back */
        sp_poll_state();
        sp_check_liked();
        sp_fetch_art();
    }
}

static void sp_send(sp_cmd_t c) {
    if (s_sp_q) xQueueSend(s_sp_q, &c, 0);
}

static void sp_init(void) {
    s_sp_q = xQueueCreate(6, sizeof(sp_cmd_t));
    if (!s_sp_q) return;
    if (xTaskCreateWithCaps(sp_task, "spotify", 8192, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "!! spotify stack fell back to INTERNAL SRAM — costs ~8 KB "
                      "of the scarce pool; expect a lower floor");
        s_stack_fallback = true;
        xTaskCreate(sp_task, "spotify", 8192, NULL, 4, NULL);
    }
}

/* ---- the screen ---- */

static lv_obj_t *s_sp_lbl_track, *s_sp_lbl_artist;
static lv_obj_t *s_sp_btn_play_lbl, *s_sp_btn_shuf, *s_sp_btn_dev;
static lv_obj_t *s_sp_btn_prev, *s_sp_btn_next, *s_sp_btn_like;
/* The volume HUD: a vertical fill beside the cover plus a glyph that goes to
 * mute at zero. Hidden until a key says otherwise, so the cover keeps the space
 * and nothing permanent is spent on it. */
static lv_obj_t *s_sp_vol_bar, *s_sp_vol_icon;
static int s_sp_devdrawn = -1;   /* signature of the drawn device list */
static int s_sp_devlit = -1;     /* last device-button tint, -1 = unset */
static lv_obj_t *s_sp_devpanel, *s_sp_devlist;
static lv_obj_t *s_sp_scr;      /* the MUSIC screen, for the accent backdrop */
static uint32_t s_sp_bg_drawn;  /* last backdrop colour, 0 = unset */

/* Circular button placed by centre, in absolute screen coordinates.
 *
 * The first pass used 58 px targets in a row of four. On a 2.16" panel held in
 * one hand that is genuinely hard to hit, so the transport controls are now
 * 92-112 px and there are three of them, Apple Watch style. Positioning by
 * centre keeps the corner-radius arithmetic checkable: the panel's corners are
 * r=110, so a button is safe when its distance from the arc centre plus its own
 * radius stays under 110. */
static lv_obj_t *sp_round_btn(lv_obj_t *par, const char *glyph, const lv_font_t *font,
                              lv_coord_t cx, lv_coord_t cy, lv_coord_t d,
                              lv_event_cb_t cb, uint32_t accent, uint32_t bg)
{
    lv_obj_t *b = lv_button_create(par);
    lv_obj_set_size(b, d, d);
    lv_obj_set_pos(b, cx - d / 2, cy - d / 2);
    lv_obj_set_style_radius(b, d / 2, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(b, 140, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(b, 255, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    if (font) lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE8FBFF), 0);
    lv_label_set_text(l, glyph);
    lv_obj_center(l);
    return l;
}

/* Every control flips the UI first and enqueues second. At 6 ms the round trip is
 * nearly instant anyway, but this makes a dropped call invisible rather than
 * making every tap feel laggy. */
/* Drop the cover the instant a track change is asked for. The new art is a poll
 * plus a fetch away, and leaving the previous album up makes the gesture look
 * like it did nothing — or worse, like it changed to the wrong track. The
 * placeholder is honest about not knowing yet. */
static void sp_art_clear(void) {
    if (s_sp_art)    lv_obj_add_flag(s_sp_art, LV_OBJ_FLAG_HIDDEN);
    if (s_sp_art_ph) lv_obj_remove_flag(s_sp_art_ph, LV_OBJ_FLAG_HIDDEN);
    s_sp_art_ready = false;
    s_sp_art_shown[0] = '\0';
}

static void sp_play_cb(lv_event_t *e) {
    s_sp_playing = !s_sp_playing;
    if (s_sp_btn_play_lbl) {
        lv_label_set_text(s_sp_btn_play_lbl, s_sp_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    sp_send(s_sp_playing ? SP_CMD_PLAY : SP_CMD_PAUSE);
}
static void sp_next_cb(lv_event_t *e) {
    if (s_sp_no_next) return;
    sp_art_clear();
    sp_send(SP_CMD_NEXT);
}
static void sp_prev_cb(lv_event_t *e) {
    if (s_sp_no_prev) return;
    sp_art_clear();
    sp_send(SP_CMD_PREV);
}
static void sp_shuf_cb(lv_event_t *e) {
    if (s_sp_no_shuffle) return;
    s_sp_shuffle = !s_sp_shuffle;
    sp_send(SP_CMD_SHUFFLE);   /* the timer restyles the button */
}

/* On/off has to be unmistakable at a glance, so it is carried by a filled
 * background rather than a tint on a small glyph — that was too subtle to read.
 * Unavailable is a third state, drawn dimmer than off and not clickable, so a tap
 * cannot fire a request Spotify would reject. */
/* lv_label_set_text() has no equality short-circuit — set_text_internal() always
 * reallocates, re-measures and invalidates, and on a SCROLL_CIRCULAR label it
 * re-runs the scroll setup. Rewriting unchanged text on a 400 ms tick therefore
 * buys a needless flush every tick, which costs frames and, because continuous
 * flushes are exactly what widens the panel-IO lock race in pitfall #13, makes a
 * freeze more likely rather than merely wasting cycles. */
static void label_set_changed(lv_obj_t *lbl, const char *s) {
    if (!lbl) return;
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, s) == 0) return;
    lv_label_set_text(lbl, s);
}

static void sp_style_toggle(lv_obj_t *lbl, bool on, bool avail,
                            uint32_t accent, const char *glyph)
{
    if (!lbl) return;
    lv_obj_t *b = lv_obj_get_parent(lbl);
    if (glyph) label_set_changed(lbl, glyph);

    /* Same reasoning for the style properties: each setter invalidates whether or
     * not the value moved, and these five buttons are re-styled every tick. The
     * signature lives in the label's user_data — free on these, since sp_round_btn
     * does not use it and only the device cards do. Bit 26 marks "set", so a
     * legitimately all-zero state is distinguishable from never-styled. */
    uintptr_t sig = (uintptr_t)(accent & 0xFFFFFF) |
                    ((uintptr_t)on << 24) | ((uintptr_t)avail << 25) | (1u << 26);
    if ((uintptr_t)lv_obj_get_user_data(lbl) == sig) return;
    lv_obj_set_user_data(lbl, (void *)sig);

    if (!avail) {
        lv_obj_set_style_bg_color(b, lv_color_hex(0x0A0E14), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(0x1A222D), 0);
        lv_obj_set_style_border_opa(b, 255, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x2A3441), 0);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
    } else if (on) {
        lv_obj_set_style_bg_color(b, lv_color_hex(accent), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
        lv_obj_set_style_border_opa(b, 255, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x05070B), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_set_style_bg_color(b, lv_color_hex(0x10161F), 0);
        lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
        lv_obj_set_style_border_opa(b, 140, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x8FA3B8), 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void sp_show_devices(bool on) {
    if (!s_sp_devpanel) return;
    if (on) {
        s_sp_devdrawn = -1;              /* force a redraw, never trust the cache */
        lv_obj_remove_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN);
        sp_send(SP_CMD_DEVICES);
    } else {
        lv_obj_add_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void sp_devtap_cb(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    if (idx < 0 || idx >= s_sp_devcount) return;
    s_sp_transfer_idx = idx;
    snprintf(s_sp_devname, sizeof(s_sp_devname), "%s", s_sp_dev[idx].name);  /* already folded */
    for (int k = 0; k < s_sp_devcount; k++) s_sp_dev[k].active = (k == idx);
    s_sp_have_state = true;
    sp_send(SP_CMD_TRANSFER);
    sp_show_devices(false);
}

static void sp_devbtn_cb(lv_event_t *e) { sp_show_devices(true); }

/* A swipe is invisible until Spotify answers, and that is a poll plus a 350 ms
 * settle away — long enough to read as "nothing happened" and swipe again. So the
 * card nudges in the direction of the flick and springs back, which says
 * "received" without claiming the track already changed. Translate, not position:
 * it is purely visual, composes with the existing alignment, and unlike a
 * transform it allocates no transient layer.
 *
 * A refused flick gets a much smaller bounce — the iOS rubber-band idea. Silence
 * would be ambiguous between "not detected" and "not allowed". */
static void sp_nudge_exec(void *obj, int32_t v) {
    lv_obj_set_style_translate_x((lv_obj_t *)obj, v, 0);
}

static void sp_nudge(int dir, int32_t dist) {
    lv_obj_t *card[] = { s_sp_art, s_sp_art_ph, s_sp_lbl_track, s_sp_lbl_artist };
    for (unsigned i = 0; i < sizeof(card) / sizeof(card[0]); i++) {
        if (!card[i]) continue;
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, card[i]);
        lv_anim_set_exec_cb(&a, sp_nudge_exec);
        lv_anim_set_values(&a, 0, dir * dist);
        lv_anim_set_duration(&a, 100);
        lv_anim_set_playback_duration(&a, 200);   /* slower return reads as spring */
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

/* MUSIC's own gesture handler, replacing gesture_home_cb on this screen: swipe
 * up still goes home, and a horizontal flick changes track. Left means the
 * finger travelled left, which flicks the current card away and brings the next
 * one in — the direction every carousel uses. Skipped while the device picker is
 * up, where a flick belongs to the list, and honours the same actions.disallows
 * that grey out the transport buttons.
 *
 * Runs in the LVGL task as a touch event, so the lock is already held. */
static void sp_gesture_cb(lv_event_t *e) {
    lv_dir_t d = lv_indev_get_gesture_dir(lv_indev_active());

    if (d == LV_DIR_TOP) { app_request(APP_DRAWER); return; }
    if (!s_sp_devpanel || !lv_obj_has_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN)) return;
    if (d != LV_DIR_LEFT && d != LV_DIR_RIGHT) return;

    int dir = (d == LV_DIR_LEFT) ? -1 : 1;      /* the card follows the finger */
    bool ok = s_sp_have_state &&
              (d == LV_DIR_LEFT ? !s_sp_no_next : !s_sp_no_prev);

    sp_nudge(dir, ok ? 36 : 10);
    if (ok) {
        sp_art_clear();
        sp_send(d == LV_DIR_LEFT ? SP_CMD_NEXT : SP_CMD_PREV);
    }
}

static void sp_like_cb(lv_event_t *e) {
    if (!s_sp_track_id[0]) return;
    s_sp_liked = !s_sp_liked;          /* optimistic; sp_toggle_like reverts on failure */
    s_sp_liked_known = true;
    sp_send(SP_CMD_LIKE);
}

/* ---- volume ----
 *
 * MUSIC takes all three keys: left down, right up, middle mute. That overrides
 * the global contract (left = lock, middle = home), so the way out is the
 * swipe-down gesture the screen already carries — see gesture_home_cb. Lock is
 * one swipe plus one key away rather than being unreachable.
 *
 * Held keys repeat, so the HUD has to be cheap to update and the network side
 * has to coalesce; sp_push_volume() handles the latter.
 */
#define SP_VOL_STEP     5
#define SP_VOL_HUD_MS   2600      /* how long the bar lingers after the last press */

/* The numeric readout borrows the track-title line rather than claiming space of
 * its own: while you are holding a volume key the level matters more than the
 * song name, and the title is the only place on this layout with room for a
 * figure big enough to read at arm's length. The title comes back on its own,
 * because sp_timer_cb rewrites it every tick once the HUD retires. */
static void sp_vol_hud_paint(void) {
    if (!s_sp_vol_bar) return;

    int v = s_sp_vol < 0 ? 0 : s_sp_vol;
    lv_bar_set_value(s_sp_vol_bar, v, LV_ANIM_OFF);

    uint32_t c;
    if (!s_sp_vol_ok) {
        c = 0x64748B;
        label_set_changed(s_sp_vol_icon, ICON_VOL_MUTE);
        if (s_sp_lbl_track) label_set_changed(s_sp_lbl_track, "no volume control");
    } else if (v == 0) {
        c = 0xF43F5E;                               /* muted reads as a warning */
        label_set_changed(s_sp_vol_icon, ICON_VOL_MUTE);
        if (s_sp_lbl_track) label_set_changed(s_sp_lbl_track, "MUTED");
    } else {
        c = 0x1DB954;
        label_set_changed(s_sp_vol_icon, ICON_VOL_UP);
        if (s_sp_lbl_track) {
            char t[20];
            snprintf(t, sizeof(t), "VOLUME %d", v);
            label_set_changed(s_sp_lbl_track, t);   /* the HUD repaints every tick */
        }
    }
    lv_obj_set_style_text_color(s_sp_vol_icon, lv_color_hex(c), 0);
    lv_obj_set_style_bg_color(s_sp_vol_bar, lv_color_hex(c), LV_PART_INDICATOR);
}

static void sp_vol_hud_show(void) {
    s_sp_vol_shown = now_ms();
    if (!s_sp_vol_bar) return;
    sp_vol_hud_paint();
    lv_obj_remove_flag(s_sp_vol_bar,  LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_sp_vol_icon, LV_OBJ_FLAG_HIDDEN);
}

/* Step the level. Local first so the bar answers the key immediately, then the
 * task pushes it — at 6 ms warm the round trip is invisible anyway, but this also
 * keeps a held key from stalling on the network. */
static void sp_vol_step(int delta) {
    if (s_sp_vol < 0) s_sp_vol = 50;      /* no reading yet; assume mid-scale */
    int v = s_sp_vol + delta;
    if (v < 0)   v = 0;
    if (v > 100) v = 100;

    if (v != s_sp_vol && s_sp_vol_ok) {
        s_sp_vol = v;
        sp_send(SP_CMD_VOLUME);
    }
    sp_vol_hud_show();                    /* show even when refused, to say why */
}

static void sp_vol_mute_toggle(void) {
    if (!s_sp_vol_ok) { sp_vol_hud_show(); return; }

    int v;
    if (s_sp_vol > 0) {
        s_sp_vol_premute = s_sp_vol;      /* remember where to come back to */
        v = 0;
    } else {
        v = s_sp_vol_premute > 0 ? s_sp_vol_premute : 35;
    }
    s_sp_vol = v;
    sp_send(SP_CMD_VOLUME);
    sp_vol_hud_show();
}

/* Called from the main loop with the LVGL lock held. Returns true when MUSIC has
 * consumed the key, so the global lock/home/back bindings are skipped. */
static bool sp_keys(btn_ev_t kleft, btn_ev_t kright, bool pwr) {
    /* The device picker keeps the global bindings. Volume there would leave the
     * sub-scene with no way to pop, and the only exit would be leaving the app. */
    if (!s_sp_devpanel || !lv_obj_has_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN)) return false;

    /* Left raises, right lowers. This deliberately contradicts the silkscreen —
     * the board labels the leftmost key "minus" and the rightmost "plus" — because
     * it matches how the cube is actually held. Requested explicitly; do not
     * "fix" it back to the labels. */
    bool used = false;
    if (kleft  == BTN_SHORT || kleft  == BTN_LONG || kleft  == BTN_REPEAT) {
        sp_vol_step(+SP_VOL_STEP); used = true;
    }
    if (kright == BTN_SHORT || kright == BTN_LONG || kright == BTN_REPEAT) {
        sp_vol_step(-SP_VOL_STEP); used = true;
    }
    if (pwr) { sp_vol_mute_toggle(); used = true; }
    return used;
}

static void sp_timer_cb(lv_timer_t *t) {
    if (!s_sp_lbl_track) return;

    if (s_sp_authfail) {
        label_set_changed(s_sp_lbl_track, "not authorised");
        label_set_changed(s_sp_lbl_artist, "check SPOTIFY_REFRESH_TOKEN");
    } else if (!s_sp_have_state) {
        label_set_changed(s_sp_lbl_track, "nothing playing");
        label_set_changed(s_sp_lbl_artist, "pick a device to start");
    } else {
        label_set_changed(s_sp_lbl_track, s_sp_track);
        label_set_changed(s_sp_lbl_artist, s_sp_artist);
    }

    sp_style_toggle(s_sp_btn_play_lbl, s_sp_playing, s_sp_have_state, 0x1DB954,
                    s_sp_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    sp_style_toggle(s_sp_btn_shuf, s_sp_shuffle, s_sp_have_state && !s_sp_no_shuffle,
                    0x1DB954, NULL);
    sp_style_toggle(s_sp_btn_prev, false, s_sp_have_state && !s_sp_no_prev, 0x334155, NULL);
    /* Unknown is its own state: an unverified heart invites a tap that would
     * silently un-like something. */
    sp_style_toggle(s_sp_btn_like, s_sp_liked_known && s_sp_liked,
                    s_sp_have_state && s_sp_liked_known && s_sp_track_id[0],
                    0xF43F5E,
                    (s_sp_liked_known && s_sp_liked) ? ICON_HEART : ICON_HEART_OPEN);
    sp_style_toggle(s_sp_btn_next, false, s_sp_have_state && !s_sp_no_next, 0x334155, NULL);

    /* Backdrop follows the cover. Change-gated because setting a screen's
     * background invalidates all 480x480 of it, which at a 400 ms tick would be
     * a full-frame flush forever. */
    if (s_sp_scr) {
        /* No state or no accent -> the original near-black. */
        uint32_t bg = (s_sp_have_state && s_sp_accent)
                      ? accent_bg(s_sp_accent) : 0x05070B;
        if (bg != s_sp_bg_drawn) {
            s_sp_bg_drawn = bg;
            lv_obj_set_style_bg_color(s_sp_scr, lv_color_hex(bg), 0);
            if (s_sp_art_ph) lv_obj_set_style_bg_color(s_sp_art_ph, lv_color_hex(bg), 0);
        }
    }

    /* The volume HUD is on demand: it appears when a key moves the level and
     * retires itself, so the cover keeps the space the rest of the time. */
    if (s_sp_vol_bar) {
        bool up = s_sp_vol_shown && (now_ms() - s_sp_vol_shown) < SP_VOL_HUD_MS;
        if (up) {
            sp_vol_hud_paint();           /* after the title write above, so it wins */
        } else {
            lv_obj_add_flag(s_sp_vol_bar,  LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_sp_vol_icon, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* Which device is playing lives on the corner button's colour, not on a
     * permanent caption — the name itself is in the picker. */
    if (s_sp_btn_dev) {
        /* Holds the drawn colour, not a flag, so a new accent repaints it too. */
        uint32_t c = s_sp_have_state ? 0x1DB954 : 0x475569;
        if ((uint32_t)s_sp_devlit != c) {
            s_sp_devlit = (int)c;
            lv_obj_set_style_text_color(s_sp_btn_dev, lv_color_hex(c), 0);
        }
    }

    /* Show the cover only while the file on the card is the one this track wants.
     * Asking "is what we have what is wanted" rather than reacting to whatever
     * caused the change covers every path with one rule — swipe, button, and a
     * track ending on its own — and needs no flag from the Spotify task, which
     * cannot touch widgets anyway. A stale cover under a new title is worse than
     * no cover: it reads as the wrong track rather than as a pending one.
     *
     * The explicit sp_art_clear() on swipe and button still earns its place: it
     * drops the cover on the touch, where this rule would wait for the poll that
     * moves s_sp_art_url. */
    bool art_current = s_sp_art_ready && s_sp_art_url[0] &&
                       strcmp(s_sp_art_have, s_sp_art_url) == 0;

    if (art_current && strcmp(s_sp_art_shown, s_sp_art_have) != 0) {
        /* sp_fetch_art() swaps the image in itself now, so this only catches a
         * cover that arrived while MUSIC was closed and the widgets did not exist. */
        snprintf(s_sp_art_shown, sizeof(s_sp_art_shown), "%s", s_sp_art_have);
        lv_image_set_src(s_sp_art, &s_sp_art_dsc);
        lv_obj_remove_flag(s_sp_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_sp_art_ph, LV_OBJ_FLAG_HIDDEN);
    } else if (!art_current && !lv_obj_has_flag(s_sp_art, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_sp_art, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_sp_art_ph, LV_OBJ_FLAG_HIDDEN);
        s_sp_art_shown[0] = '\0';
    }

    /* Rebuild the device list only when the set changes, not every tick — it is
     * a list of buttons and rebuilding it under a finger would fight the touch. */
    /* Keyed on count AND which one is active. Keying on count alone meant
     * switching between the same three devices never redrew, so the tick stayed
     * on the old one — the transfer worked and the UI silently lied. */
    int devsig = s_sp_devcount << 8;
    for (int i = 0; i < s_sp_devcount; i++) {
        if (s_sp_dev[i].active) devsig |= (i + 1);
    }
    if (s_sp_devlist && !lv_obj_has_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN) &&
        s_sp_devdrawn != devsig) {
        s_sp_devdrawn = devsig;
        lv_obj_clean(s_sp_devlist);
        /* Cards, not list rows. There are only ever a handful of devices, so the
         * space is better spent on targets big enough to hit on a small panel
         * than on fitting a long list nobody has. */
        for (int i = 0; i < s_sp_devcount; i++) {
            bool on = s_sp_dev[i].active;
            lv_obj_t *b = lv_button_create(s_sp_devlist);
            lv_obj_set_size(b, lv_pct(100), 76);
            lv_obj_set_style_radius(b, 16, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(on ? 0x11331F : 0x141B26), 0);
            lv_obj_set_style_border_width(b, 2, 0);
            lv_obj_set_style_border_color(b, lv_color_hex(on ? 0x1DB954 : 0x27313F), 0);
            lv_obj_set_style_border_opa(b, on ? 255 : 160, 0);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x1DB954), LV_STATE_PRESSED);
            lv_obj_set_style_pad_left(b, 16, 0);
            lv_obj_set_user_data(b, (void *)(intptr_t)i);
            lv_obj_add_event_cb(b, sp_devtap_cb, LV_EVENT_CLICKED, NULL);

            lv_obj_t *ic = lv_label_create(b);
            lv_obj_set_style_text_font(ic, on ? &lv_font_montserrat_20 : &hud_icons_30, 0);
            lv_obj_set_style_text_color(ic, lv_color_hex(on ? 0x1DB954 : 0x64748B), 0);
            lv_label_set_text(ic, on ? LV_SYMBOL_OK : ICON_DEVICES);
            lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

            lv_obj_t *nm = lv_label_create(b);
            lv_obj_set_style_text_font(nm, &lv_font_montserrat_20, 0);
            lv_obj_set_style_text_color(nm, lv_color_hex(on ? 0xE8FBFF : 0xC7D2E0), 0);
            lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
            lv_obj_set_width(nm, CONTENT_W - 92);
            lv_label_set_text(nm, s_sp_dev[i].name);
            lv_obj_align(nm, LV_ALIGN_LEFT_MID, 38, 0);
        }
        if (s_sp_devcount == 0) {
            lv_obj_t *t = lv_label_create(s_sp_devlist);
            lv_obj_set_width(t, CONTENT_W - 24);
            lv_obj_set_style_text_font(t, &hud_text_18, 0);
            lv_obj_set_style_text_color(t, lv_color_hex(0x64748B), 0);
            lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
            lv_label_set_text(t, "no devices\nopen Spotify somewhere first");
        }
    }
}

static void build_music_app(lv_obj_t *scr) {
    s_sp_scr = scr;
    s_sp_bg_drawn = 0;                       /* force a repaint on this build */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05070B), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Layout borrowed from Apple Watch Now Playing, for the reason it exists
     * there: on a screen this size the transport controls matter more than the
     * artwork. The first pass had four 58 px buttons in a row and maximised the
     * cover; that is backwards for something you poke with a thumb. Transport is
     * now 96-116 px, and the device picker moved to a corner button so nothing
     * has to be spent on a permanent device caption.
     *
     * Positions are absolute centres so the corner radius stays checkable: the
     * panel's corners are r=110 arcs, and a circle is safe when its distance from
     * the arc centre plus its own radius stays under 110. */

    /* top corners: shuffle left, device picker right */
    s_sp_btn_shuf = sp_round_btn(scr, LV_SYMBOL_SHUFFLE, NULL, 80, 68, 64,
                                 sp_shuf_cb, 0x1DB954, 0x10161F);
    /* Matches the glyph Spotify itself uses for Connect — see ICON_DEVICES. */
    s_sp_btn_dev = sp_round_btn(scr, ICON_DEVICES, &hud_icons_30, 400, 68, 64,
                                sp_devbtn_cb, 0x1DB954, 0x10161F);
    /* Top middle, between shuffle and the device picker. Smaller than the
     * transport on purpose — liking is a deliberate act, not something you want
     * to hit by accident while reaching for pause. */
    s_sp_btn_like = sp_round_btn(scr, ICON_HEART_OPEN, &hud_icons_30, 240, 58, 56,
                                 sp_like_cb, 0xF43F5E, 0x10161F);

    /* cover, smaller than before on purpose */
    s_sp_art_ph = lv_obj_create(scr);
    lv_obj_remove_style_all(s_sp_art_ph);
    lv_obj_set_size(s_sp_art_ph, SP_ART_PX, SP_ART_PX);
    lv_obj_align(s_sp_art_ph, LV_ALIGN_TOP_MID, 0, 110);
    lv_obj_set_style_bg_color(s_sp_art_ph, lv_color_hex(0x11161F), 0);
    lv_obj_set_style_bg_opa(s_sp_art_ph, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_sp_art_ph, 14, 0);
    lv_obj_remove_flag(s_sp_art_ph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ph = lv_label_create(s_sp_art_ph);
    lv_obj_set_style_text_font(ph, &app_icons_64, 0);
    lv_obj_set_style_text_color(ph, lv_color_hex(0x1E293B), 0);
    lv_label_set_text(ph, ICON_MUSIC);
    lv_obj_center(ph);

    s_sp_art = lv_image_create(scr);
    lv_obj_set_size(s_sp_art, SP_ART_PX, SP_ART_PX);
    lv_obj_align(s_sp_art, LV_ALIGN_TOP_MID, 0, 110);
    lv_image_set_inner_align(s_sp_art, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(s_sp_art, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_sp_art, LV_OBJ_FLAG_CLICKABLE);

    /* Volume HUD, in the gap to the left of the cover. The cover is 148 wide and
     * centred, so it occupies x166..314; an 18 px fill at x128 sits in clear
     * space and still clears the r=110 corner arcs, which only cut x<110. The
     * readout goes top-middle, in the slot the like button vacated, where it is
     * big enough to read at a glance without crowding the corner buttons. */
    s_sp_vol_bar = lv_bar_create(scr);
    lv_obj_set_size(s_sp_vol_bar, 18, SP_ART_PX);
    lv_obj_set_pos(s_sp_vol_bar, 128, 110);
    lv_bar_set_range(s_sp_vol_bar, 0, 100);
    lv_obj_set_style_radius(s_sp_vol_bar, 9, 0);
    lv_obj_set_style_radius(s_sp_vol_bar, 9, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sp_vol_bar, lv_color_hex(0x18202C), 0);
    lv_obj_set_style_bg_opa(s_sp_vol_bar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_sp_vol_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_sp_vol_bar, LV_OBJ_FLAG_HIDDEN);

    s_sp_vol_icon = lv_label_create(scr);
    lv_obj_set_style_text_font(s_sp_vol_icon, &hud_icons_30, 0);
    lv_obj_set_style_text_color(s_sp_vol_icon, lv_color_hex(0x1DB954), 0);
    label_set_changed(s_sp_vol_icon, ICON_VOL_UP);
    lv_obj_set_pos(s_sp_vol_icon, 122, 74);
    lv_obj_add_flag(s_sp_vol_icon, LV_OBJ_FLAG_HIDDEN);

    s_sp_lbl_track = lv_label_create(scr);
    lv_obj_set_width(s_sp_lbl_track, CONTENT_W);
    lv_obj_set_style_text_font(s_sp_lbl_track, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_sp_lbl_track, lv_color_hex(0xF2E9DC), 0);
    lv_obj_set_style_text_align(s_sp_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_sp_lbl_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_sp_lbl_track, "connecting...");
    lv_obj_align(s_sp_lbl_track, LV_ALIGN_TOP_MID, 0, 262);

    s_sp_lbl_artist = lv_label_create(scr);
    lv_obj_set_width(s_sp_lbl_artist, CONTENT_W);
    lv_obj_set_style_text_font(s_sp_lbl_artist, &hud_text_18, 0);
    lv_obj_set_style_text_color(s_sp_lbl_artist, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_text_align(s_sp_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_sp_lbl_artist, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_sp_lbl_artist, "");
    lv_obj_align(s_sp_lbl_artist, LV_ALIGN_TOP_MID, 0, 292);

    /* transport: the things you actually press, sized accordingly */
    /* Glyph sizes are 36 for the skips and 48 for play/pause, against the 14 px
     * default and 20 px they started at — icons sized for a button a third as
     * wide, which read as timid rather than as small. The two sizes are the
     * hierarchy: play is the primary verb and gets the bigger circle *and* the
     * bigger glyph, while a matched 48 made all three compete. 36 is enabled in
     * sdkconfig.defaults purely for this; nothing sits between 20 and 48. */
    s_sp_btn_prev = sp_round_btn(scr, LV_SYMBOL_PREV, &lv_font_montserrat_36,
                                 118, 376, 96, sp_prev_cb, 0x334155, 0x141B26);
    s_sp_btn_play_lbl = sp_round_btn(scr, LV_SYMBOL_PLAY, &lv_font_montserrat_48,
                                     240, 376, 116, sp_play_cb, 0x1DB954, 0x16241C);
    s_sp_btn_next = sp_round_btn(scr, LV_SYMBOL_NEXT, &lv_font_montserrat_36,
                                 362, 376, 96, sp_next_cb, 0x334155, 0x141B26);

    /* Device picker, full-screen over the top. Same shape as the Wi-Fi app's
     * sub-scene so the right key pops it the same way. */
    s_sp_devpanel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_sp_devpanel);
    lv_obj_set_size(s_sp_devpanel, 480, 480);
    lv_obj_center(s_sp_devpanel);
    lv_obj_set_style_bg_color(s_sp_devpanel, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_sp_devpanel, 250, 0);
    lv_obj_remove_flag(s_sp_devpanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *dt = lv_label_create(s_sp_devpanel);
    lv_obj_set_style_text_font(dt, &hud_text_18, 0);
    lv_obj_set_style_text_color(dt, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_text_letter_space(dt, 4, 0);
    lv_label_set_text(dt, "PLAY ON");
    lv_obj_align(dt, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 18);

    s_sp_devlist = lv_obj_create(s_sp_devpanel);
    lv_obj_remove_style_all(s_sp_devlist);
    lv_obj_set_size(s_sp_devlist, CONTENT_W, 312);
    lv_obj_align(s_sp_devlist, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_flex_flow(s_sp_devlist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sp_devlist, 12, 0);
    lv_obj_set_scroll_dir(s_sp_devlist, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_sp_devlist, LV_SCROLLBAR_MODE_OFF);

    lv_obj_add_event_cb(scr, sp_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_sp_devdrawn = -1;
    s_sp_devlit = -1;
    s_sp_art_shown[0] = '\0';
    sp_send(SP_CMD_POLL);
    sp_send(SP_CMD_DEVICES);
    s_app_timer = lv_timer_create(sp_timer_cb, 400, NULL);
}

/* ---------------- app drawer ---------------- */

typedef struct {
    const char *name;                 /* shown in the drawer */
    const char *id;                   /* stable storage key   */
    const char *icon;                 /* glyph from app_icons_64 */
    uint32_t    color;
    void      (*build)(lv_obj_t *scr);
    void      (*save)(void);          /* flushed when the app closes */
} app_def_t;

static const app_def_t s_apps[APP_COUNT] = {
    [APP_CONTROL] = { "CONTROL", "control", ICON_DASHBOARD, 0x22D3EE, build_control_app, NULL     },
    [APP_PET]    = { "PIP",    "pet",    ICON_PETS,      0xF59E0B, build_pet_app,    pet_save },
    [APP_MUSIC]  = { "MUSIC",  "music",  ICON_MUSIC,     0x1DB954, build_music_app,  NULL      },
    /* Red, and not the amber the app itself still uses for a running session:
     * PIP directly above it is 0xF59E0B, and two ambers side by side in a 2x2
     * made the tiles hard to tell apart at a glance. */
    [APP_POMO]   = { "FOCUS",  "pomo",   ICON_TARGET,    0xFF453A, build_pomo_app,   pomo_save },
};

static void tile_cb(lv_event_t *e) {
    app_request((int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

static void build_drawer(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* 2x2 of 196 px tiles with a 16 px gap fills 408 px of the 480 px panel.
     * The outer tile corners land at (36,36), still inside the display's
     * corner radius, so nothing is clipped. */
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 412, 412);
    lv_obj_center(grid);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < APP_COUNT; i++) {
        lv_color_t accent = lv_color_hex(s_apps[i].color);

        lv_obj_t *tile = lv_button_create(grid);
        lv_obj_set_size(tile, 196, 196);
        lv_obj_set_style_radius(tile, 34, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x0C1018), 0);
        lv_obj_set_style_bg_grad_color(tile, lv_color_hex(0x141B26), 0);
        lv_obj_set_style_bg_grad_dir(tile, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_set_style_border_color(tile, accent, 0);
        lv_obj_set_style_border_opa(tile, 130, 0);
        lv_obj_set_style_shadow_width(tile, 18, 0);
        lv_obj_set_style_shadow_color(tile, accent, 0);
        lv_obj_set_style_shadow_opa(tile, 45, 0);
        /* pressed state gives the tap somewhere to land visually */
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x1B2432), LV_STATE_PRESSED);
        lv_obj_set_style_border_opa(tile, 255, LV_STATE_PRESSED);
        lv_obj_set_user_data(tile, (void *)(intptr_t)i);
        lv_obj_add_event_cb(tile, tile_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *ic = lv_label_create(tile);
        lv_obj_set_style_text_font(ic, &app_icons_64, 0);
        lv_obj_set_style_text_color(ic, accent, 0);
        lv_label_set_text(ic, s_apps[i].icon);
        lv_obj_align(ic, LV_ALIGN_CENTER, 0, -22);

        lv_obj_t *l = lv_label_create(tile);
        lv_obj_set_style_text_font(l, &hud_text_18, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(0xC7D2E0), 0);
        lv_obj_set_style_text_letter_space(l, 2, 0);
        lv_label_set_text(l, s_apps[i].name);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 46);
    }
}

/* ---------------- PCF85063 RTC ----------------
 *
 * Without this the clock reads "--:--" for the several seconds SNTP takes, on
 * every boot. Seed the system clock from the battery-backed RTC immediately
 * and write back once SNTP lands. Stored in UTC so DST is purely a TZ concern.
 * BCD registers from 0x04: sec (bit7 = oscillator stopped), min, hour, day,
 * weekday, month, year(00-99).
 */

#define PCF85063_ADDR 0x51

static i2c_master_dev_handle_t s_rtc;
static bool s_rtc_written;

static int bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t dec2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* newlib here has no timegm(), and mktime() would apply the local timezone to
 * a value we deliberately keep in UTC — so convert explicitly */
static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static time_t tm_to_utc(const struct tm *t) {
    static const int cum[12] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int year = t->tm_year + 1900;
    long days = 0;
    for (int y = 1970; y < year; y++) days += is_leap(y) ? 366 : 365;
    days += cum[t->tm_mon];
    if (t->tm_mon > 1 && is_leap(year)) days++;
    days += t->tm_mday - 1;
    return (time_t)days * 86400L + t->tm_hour * 3600 + t->tm_min * 60 + t->tm_sec;
}

static void rtc_init(void) {
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) return;
    i2c_device_config_t dev = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dev, &s_rtc) != ESP_OK) {
        s_rtc = NULL;
        return;
    }

    uint8_t reg = 0x04, d[7];
    if (i2c_master_transmit_receive(s_rtc, &reg, 1, d, 7, 100) != ESP_OK) return;
    if (d[0] & 0x80) {
        ESP_LOGI(TAG, "RTC present but unset — waiting for SNTP");
        return;
    }

    struct tm t = {
        .tm_sec  = bcd2dec(d[0] & 0x7F),
        .tm_min  = bcd2dec(d[1] & 0x7F),
        .tm_hour = bcd2dec(d[2] & 0x3F),
        .tm_mday = bcd2dec(d[3] & 0x3F),
        .tm_mon  = bcd2dec(d[5] & 0x1F) - 1,
        .tm_year = bcd2dec(d[6]) + 100,
    };
    time_t utc = tm_to_utc(&t);
    if (utc > 1735689600) {                  /* sanity: after 2025-01-01 */
        struct timeval tv = { .tv_sec = utc, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "clock seeded from RTC: %04d-%02d-%02d %02d:%02d UTC",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
    }
}

static void rtc_store(void) {
    if (!s_rtc) return;
    time_t now;
    time(&now);
    struct tm t;
    gmtime_r(&now, &t);

    uint8_t buf[8] = {
        0x04,
        dec2bcd(t.tm_sec), dec2bcd(t.tm_min), dec2bcd(t.tm_hour),
        dec2bcd(t.tm_mday), (uint8_t)t.tm_wday,
        dec2bcd(t.tm_mon + 1), dec2bcd(t.tm_year - 100),
    };
    if (i2c_master_transmit(s_rtc, buf, sizeof(buf), 100) == ESP_OK) {
        s_rtc_written = true;
        ESP_LOGI(TAG, "RTC updated from SNTP");
    }
}

/* ---------------- asset fetch ----------------
 *
 * Artwork is streamed straight to the card through the HTTP event callback, so
 * the image never exists whole in RAM — only the client's receive buffer.
 * Written to .part and renamed, so a failed download cannot leave a corrupt
 * file. Requested at exactly panel resolution so we never pull pixels we
 * cannot show.
 */

/* A pool, not a single file.
 *
 * One wallpaper meant every unlock showed the same picture until the next
 * download — four images a day, and the device looked static. The card has
 * 32 GB, so keep WALL_SLOTS of them (~400 KB each) and pick a random one every
 * time the lock screen is built. Downloads then only control how fast the pool
 * turns over, not how much variety you see.
 */
#define UNSPLASH_JSON   WALL_DIR "/rnd.json"

/* Legacy single-wallpaper path, deleted on boot if it is still lying around. */
#define WALLPAPER_OLD   WALL_DIR "/lock.png"

static FILE *s_dl_file;

/* Album art goes to PSRAM rather than onto the card, so the download sink is
 * switchable. Only one download is ever in flight — the wallpaper fetch and the
 * art fetch are each serialised behind their own task and neither reenters — so a
 * pair of file-scope sinks is safe and keeps one event handler and one set of
 * progress counters serving both. */
static uint8_t *s_dl_mem;
static size_t   s_dl_mem_cap, s_dl_mem_len;

static void wall_png(int slot, char *out, size_t n) {
    snprintf(out, n, WALL_DIR "/w%d.png", slot);
}
static void wall_lv(int slot, char *out, size_t n) {
    snprintf(out, n, "S:" WALL_DIR "/w%d.png", slot);
}
static void wall_txt(int slot, char *out, size_t n) {
    snprintf(out, n, WALL_DIR "/w%d.txt", slot);
}

/* A cached wallpaper is only usable if it really is a PNG. Earlier builds
 * saved a progressive JPEG under a .png name, and a stale bad file would
 * otherwise never be replaced — the fetch only runs when the file is missing. */
static bool wallpaper_valid(const char *path) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[8] = {0};
    size_t n = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    return n == sizeof(hdr) && memcmp(hdr, sig, sizeof(sig)) == 0;
}

static void wall_scan(void) {
    s_wall_have = 0;
    if (!s_sd_ok) return;
    for (int i = 0; i < WALL_SLOTS; i++) {
        char p[64];
        wall_png(i, p, sizeof(p));
        if (wallpaper_valid(p)) s_wall_have |= (uint16_t)(1u << i);
        else remove(p);                  /* truncated or wrong format */
    }
    ESP_LOGI(TAG, "wallpaper pool: %d/%d slots", __builtin_popcount(s_wall_have),
             WALL_SLOTS);
}

static int wall_first_empty(void) {
    for (int i = 0; i < WALL_SLOTS; i++) {
        if (!(s_wall_have & (1u << i))) return i;
    }
    return -1;
}

/* A slot to download into: fill the gaps first so a fresh card reaches full
 * variety quickly, then recycle at random once the pool is complete. */
static int wall_target_slot(void) {
    int empty = wall_first_empty();
    return empty >= 0 ? empty : (int)(esp_random() % WALL_SLOTS);
}

/* A slot to display, avoiding an immediate repeat of the one already up. */
static int wall_display_slot(void) {
    int have = __builtin_popcount(s_wall_have);
    if (have == 0) return -1;
    for (int tries = 0; tries < 8; tries++) {
        int pick = (int)(esp_random() % WALL_SLOTS);
        if (!(s_wall_have & (1u << pick))) continue;
        if (pick == s_wall_slot && have > 1) continue;
        return pick;
    }
    return wall_first_empty() == 0 ? -1 : __builtin_ctz(s_wall_have);
}

static esp_err_t dl_evt(esp_http_client_event_t *e) {
    if (e->event_id == HTTP_EVENT_ON_HEADER) {
        if (e->header_key && strcasecmp(e->header_key, "Content-Length") == 0) {
            s_dl_total = atoll(e->header_value);
        } else if (e->header_key && strcasecmp(e->header_key, "X-Art-Accent") == 0) {
            /* Rides the art fetch, so the tint costs no request and no bytes. */
            s_dl_accent = (uint32_t)strtoul(e->header_value, NULL, 16);
        }
    } else if (e->event_id == HTTP_EVENT_ON_DATA && e->data_len > 0 &&
               (s_dl_file || s_dl_mem)) {
        if (s_dl_mem) {
            size_t n = (size_t)e->data_len;
            if (s_dl_mem_len + n > s_dl_mem_cap) n = s_dl_mem_cap - s_dl_mem_len;
            if (n) {
                memcpy(s_dl_mem + s_dl_mem_len, e->data, n);
                s_dl_mem_len += n;
            }
        } else {
            fwrite(e->data, 1, e->data_len, s_dl_file);
        }
        s_dl_got += e->data_len;
        s_dl_kb = (int)(s_dl_got / 1024);
        if (s_dl_total > 0) {
            int pct = (int)((s_dl_got * 100) / s_dl_total);
            s_dl_pct = pct > 100 ? 100 : pct;
        }
    }
    return ESP_OK;
}

/* Fetch into a caller-owned buffer. Same client and event handler as the file
 * path, minus the .part-and-rename dance: a partial body cannot leave anything
 * behind when the destination is memory, and the caller checks the length. */
static bool asset_fetch_mem(const char *url, const char *bearer,
                            uint8_t *buf, size_t cap, size_t *out_len) {
    s_dl_total = 0; s_dl_got = 0; s_dl_pct = 0; s_dl_kb = 0; s_dl_accent = 0;
    s_dl_mem = buf; s_dl_mem_cap = cap; s_dl_mem_len = 0;

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,
        .event_handler = dl_evt,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c) {
        if (bearer && bearer[0]) {
            char auth[96];
            snprintf(auth, sizeof(auth), "Bearer %.72s", bearer);
            esp_http_client_set_header(c, "Authorization", auth);
        }
        esp_err_t err = esp_http_client_perform(c);
        int status = esp_http_client_get_status_code(c);
        ok = (err == ESP_OK && status == 200);
        esp_http_client_cleanup(c);
    }
    *out_len = s_dl_mem_len;
    s_dl_mem = NULL; s_dl_mem_cap = s_dl_mem_len = 0;
    return ok;
}

static bool asset_fetch_auth(const char *url, const char *path, const char *bearer) {
    if (!s_sd_ok) return false;

    s_dl_total = 0;
    s_dl_got = 0;
    s_dl_pct = 0;
    s_dl_kb = 0;
    s_dl_accent = 0;

    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    s_dl_file = fopen(tmp, "wb");
    if (!s_dl_file) {
        ESP_LOGW(TAG, "asset: cannot open %s (%s)", tmp, strerror(errno));
        return false;
    }

    esp_http_client_config_t cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 12000,     /* per socket read, not total */
        .event_handler = dl_evt,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    bool ok = false;
    if (c) {
        if (bearer && bearer[0]) {
            char auth[96];
            snprintf(auth, sizeof(auth), "Bearer %.72s", bearer);
            esp_http_client_set_header(c, "Authorization", auth);
        }
        esp_err_t err = esp_http_client_perform(c);
        int status = esp_http_client_get_status_code(c);
        ok = (err == ESP_OK && status == 200);
        ESP_LOGI(TAG, "asset: HTTP %d, heap %u", status,
                 (unsigned)hp_free());
        esp_http_client_cleanup(c);
    }
    fclose(s_dl_file);
    s_dl_file = NULL;

    if (ok) {
        remove(path);
        ok = (rename(tmp, path) == 0);
    } else {
        remove(tmp);
    }
    return ok;
}

static bool asset_fetch(const char *url, const char *path) {
    return asset_fetch_auth(url, path, NULL);
}

/* Pick one theme at random out of the ';'-separated UNSPLASH_QUERY list.
 * A single query would fill the whole pool with one subject, which is the
 * opposite of the point. */
static void wall_pick_theme(char *out, size_t n) {
    const char *all = UNSPLASH_QUERY;
    int count = 1;
    for (const char *p = all; *p; p++) if (*p == ';') count++;

    const char *s = all;
    for (uint32_t i = esp_random() % (uint32_t)count; i > 0; i--) {
        s = strchr(s, ';');
        if (!s) { s = all; break; }
        s++;
    }
    const char *e = strchr(s, ';');
    size_t len = e ? (size_t)(e - s) : strlen(s);

    while (len && *s == ' ') { s++; len--; }              /* trim */
    while (len && s[len - 1] == ' ') len--;
    if (len >= n) len = n - 1;
    memcpy(out, s, len);
    out[len] = '\0';
}

/* Themes are human-written and contain spaces, so they cannot go into a URL
 * raw. Anything not unreserved gets percent-encoded. */
static void url_escape(const char *in, char *out, size_t n) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < n; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = hex[*p >> 4];
            out[o++] = hex[*p & 0x0F];
        }
    }
    out[o] = '\0';
}

/* One API call for a random photo, then the image itself at panel resolution,
 * into the given pool slot. The JSON is parked on the card and parsed from
 * PSRAM so the ~10 KB response never competes for internal heap. The
 * photographer's name is saved beside the image — Unsplash's API terms require
 * attribution, and STATUS shows it for whichever wallpaper is on screen. */
static bool unsplash_wallpaper(int slot) {
    if (UNSPLASH_KEY[0] == '\0') return false;

    char theme[80], q[240], api[420];
    wall_pick_theme(theme, sizeof(theme));
    url_escape(theme, q, sizeof(q));
    snprintf(s_dl_theme, sizeof(s_dl_theme), "%s", theme);
    s_dl_state = DL_QUERY;
    ESP_LOGI(TAG, "wallpaper slot %d, theme \"%s\"", slot, theme);

    snprintf(api, sizeof(api),
             "https://api.unsplash.com/photos/random"
             "?orientation=squarish&content_filter=high&query=%s&client_id=%s",
             q, UNSPLASH_KEY);
    if (!asset_fetch(api, UNSPLASH_JSON)) return false;

    FILE *f = fopen(UNSPLASH_JSON, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 64 * 1024) { fclose(f); return false; }

    char *buf = heap_caps_malloc(sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) { fclose(f); return false; }
    size_t got = fread(buf, 1, sz, f);
    fclose(f);
    buf[got] = '\0';

    bool ok = false;
    cJSON *root = cJSON_Parse(buf);
    if (root) {
        cJSON *urls = cJSON_GetObjectItem(root, "urls");
        cJSON *raw  = urls ? cJSON_GetObjectItem(urls, "raw") : NULL;
        cJSON *user = cJSON_GetObjectItem(root, "user");
        cJSON *name = user ? cJSON_GetObjectItem(user, "name") : NULL;

        if (cJSON_IsString(raw)) {
            /* urls.raw already carries imgix params, including an auto=format
             * that silently overrides ours — cut the query and set our own, or
             * you get a progressive JPEG named .png that nothing can decode. */
            char img[512];
            snprintf(img, sizeof(img), "%s", raw->valuestring);
            char *q = strchr(img, '?');
            if (q) *q = '\0';
            size_t used = strlen(img);
            snprintf(img + used, sizeof(img) - used, "?w=480&h=480&fit=crop&fm=png");
            char dest[64];
            wall_png(slot, dest, sizeof(dest));
            s_dl_state = DL_IMAGE;
            ok = asset_fetch(img, dest);
        }
        if (ok) {
            s_wall_have |= (uint16_t)(1u << slot);
            char cpath[64];
            wall_txt(slot, cpath, sizeof(cpath));
            FILE *cf = fopen(cpath, "w");
            if (cf) {
                fputs(cJSON_IsString(name) ? name->valuestring : "", cf);
                fclose(cf);
            }
            if (cJSON_IsString(name)) {
                ESP_LOGI(TAG, "slot %d by %s", slot, name->valuestring);
            }
        }
        cJSON_Delete(root);
    }
    free(buf);
    remove(UNSPLASH_JSON);
    return ok;
}

/* Called once per network-task cycle (~45 s awake, 10 min dozing).
 *
 * While the pool has gaps it downloads every cycle, so a fresh card reaches
 * full variety in about ten minutes; once complete it drops to one replacement
 * every WALLPAPER_PERIOD_MS. The timestamp is stamped whether or not the fetch
 * worked, so a failing network backs off to the cycle rate instead of spinning. */
static void wall_service(void) {
    /* Deliberately runs while dozing too. The device spends nearly all of its
     * life asleep, so skipping downloads there meant the pool only grew while
     * someone was actively using it — it would never fill. Throttling comes
     * free from this task's own cadence, which is already 10 min when dozing
     * against 45 s awake, so an idle device quietly finishes the pool in about
     * two hours and then drops to one replacement every WALLPAPER_PERIOD_MS. */
    if (!s_sd_ok || !s_wifi_up) return;
    if (s_pomo_state == POMO_RUN) return;   /* not during a session */
    if (ble_prov_active()) return;          /* nor during pairing        */
    /* nor while the lock screen is showing now-playing, which fetches album art
     * on the same internal heap this download already drives to its low point */
    if (s_lock_np_up) return;

    bool filling = (wall_first_empty() >= 0);
    int64_t due = filling ? 0 : WALLPAPER_PERIOD_MS;
    if (!s_req_wallpaper && (now_ms() - s_wall_last) < due) return;

    s_req_wallpaper = false;
    s_wall_last = now_ms();

    int slot = wall_target_slot();
    if (!unsplash_wallpaper(slot)) {
        s_dl_state = DL_FAIL;
        s_dl_ended_ms = now_ms();
        return;
    }
    s_dl_state = DL_OK;
    s_dl_ended_ms = now_ms();

    /* Same path, new bytes: without this LVGL keeps serving the previously
     * decoded bitmap for that slot out of its image cache. */
    char lvpath[72];
    wall_lv(slot, lvpath, sizeof(lvpath));
    if (ui_lock()) {
        lv_image_cache_drop(lvpath);
        bsp_display_unlock();
    }
    log_event("wallpaper %d/%d", __builtin_popcount(s_wall_have), WALL_SLOTS);
}

/* ---------------- app state store ----------------
 *
 * The backbone for treating this as a small OS: an app keeps nothing resident.
 * It is built when opened, its state flushed to the card when it closes, and
 * its widgets freed. RAM cost is one app, not N.
 *
 *   /sdcard/apps/<id>.bin    opaque per-app state blob
 *   /sdcard/logs/pwrlog.csv  power telemetry
 *   /sdcard/assets/...       fonts, images
 */

#define STORE_APPS   BSP_SD_MOUNT_POINT "/apps"
#define STORE_LOGS   BSP_SD_MOUNT_POINT "/logs"
#define STORE_ASSETS BSP_SD_MOUNT_POINT "/assets"

static void store_init_dirs(void) {
    if (!s_sd_ok) return;
    mkdir(STORE_APPS, 0777);
    mkdir(STORE_LOGS, 0777);
    mkdir(STORE_ASSETS, 0777);
}

static bool store_save(const char *id, const void *data, size_t len) {
    if (s_sd_ok) {
        char path[64];
        snprintf(path, sizeof(path), STORE_APPS "/%.16s.bin", id);
        FILE *f = fopen(path, "wb");
        if (f) {
            size_t n = fwrite(data, 1, len, f);
            fclose(f);
            if (n == len) return true;
        }
        ESP_LOGW(TAG, "store: SD write failed for %s (%s)", id, strerror(errno));
    }
    nvs_handle_t h;                                  /* fall back to NVS */
    /* Only the NVS fallback is flash; the SD path above is the card. */
    if (ble_prov_nvs_blocked()) return false;
    if (nvs_open("appstate", NVS_READWRITE, &h) != ESP_OK) return false;
    esp_err_t e = nvs_set_blob(h, id, data, len);
    nvs_commit(h);
    nvs_close(h);
    return e == ESP_OK;
}

static bool store_load(const char *id, void *data, size_t len) {
    if (s_sd_ok) {
        char path[64];
        snprintf(path, sizeof(path), STORE_APPS "/%.16s.bin", id);
        FILE *f = fopen(path, "rb");
        if (f) {
            size_t n = fread(data, 1, len, f);
            fclose(f);
            if (n == len) return true;
        }
    }
    nvs_handle_t h;
    if (nvs_open("appstate", NVS_READONLY, &h) != ESP_OK) return false;
    size_t sz = len;
    esp_err_t e = nvs_get_blob(h, id, data, &sz);
    nvs_close(h);
    return e == ESP_OK && sz == len;
}

/* ---------------- SD telemetry ----------------
 *
 * The device cannot log to USB while running on battery, which is exactly when
 * the power numbers matter — so it keeps its own history on the card.
 */

#define TELEMETRY_PATH   BSP_SD_MOUNT_POINT "/logs/pwrlog2.csv"
#define TELEMETRY_OLD    BSP_SD_MOUNT_POINT "/logs/pwrlog.csv"
#define TELEMETRY_PERIOD 60000

static void sd_init(void) {
    esp_err_t err = bsp_sdcard_mount();
    if (err == ESP_OK) {
        s_sd_ok = true;
        ESP_LOGI(TAG, "microSD mounted at %s", BSP_SD_MOUNT_POINT);
        store_init_dirs();
        remove(WALLPAPER_OLD);          /* pre-pool single wallpaper */
        wall_scan();
    } else {
        ESP_LOGW(TAG, "no microSD (%s) — telemetry disabled", esp_err_to_name(err));
    }
}

/* One-shot dump of the power log to serial. The card cannot be read without
 * pulling it out of the device, and the whole point of this log is that it
 * accumulates while running on battery with no console attached. */
#define PWRLOG_DUMP 0

#if PWRLOG_DUMP
static void telemetry_dump(void) {
    FILE *f = fopen(TELEMETRY_PATH, "r");
    if (!f) {
        ESP_LOGW(TAG, "no power log at %s", TELEMETRY_PATH);
        return;
    }
    ESP_LOGW(TAG, "=== BEGIN pwrlog.csv ===");
    char line[224];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        printf("CSV %s\n", line);
        n++;
        if ((n % 40) == 0) vTaskDelay(pdMS_TO_TICKS(20));  /* let USB-CDC drain */
    }
    fclose(f);
    ESP_LOGW(TAG, "=== END pwrlog.csv (%d lines) ===", n);
}
#endif

static void telemetry_row(const char *event) {
    if (!s_sd_ok) return;

    struct stat st;
    bool fresh = (stat(TELEMETRY_PATH, &st) != 0);

    FILE *f = fopen(TELEMETRY_PATH, "a");
    if (!f) {
        s_sd_ok = false;
        ESP_LOGW(TAG, "telemetry write failed (%s), disabling", strerror(errno));
        return;
    }
    if (fresh) {
        fprintf(f, "build,clock,uptime_s,event,batt_mv,batt_pct,charging,doze,screen,"
                   "wifi,app,heap_free,heap_min,heap_largest,fps\n");
    }

    char clock[16] = "";
    time_t tnow;
    struct tm ti;
    time(&tnow);
    localtime_r(&tnow, &ti);
    if (ti.tm_year >= (2024 - 1900)) strftime(clock, sizeof(clock), "%H:%M:%S", &ti);

    fprintf(f, "%s,%s,%lld,%s,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u.%u\n",
            build_id(), clock, (long long)(now_ms() / 1000), event,
            s_batt_mv, s_batt_pct, s_batt_charging ? 1 : 0,
            s_doze ? 1 : 0, s_screen_on ? 1 : 0, s_wifi_up ? 1 : 0, s_app,
            (unsigned)hp_free(), (unsigned)hp_min(), (unsigned)hp_largest(),
            (unsigned)(s_last_fps_x10 / 10), (unsigned)(s_last_fps_x10 % 10));
    fclose(f);
    s_tele_rows++;
}

/* ---------------- power management ----------------
 *
 * ACTIVE is normal running. DOZE is everything-off-but-still-connected: panel
 * off at the driver IC (brightness 0 alone leaves it scanning), Wi-Fi in max
 * modem sleep, CPU capped at 80 MHz, sensors and network polling stretched out.
 */

static int  s_batt_mv_first;
static int64_t s_batt_t_first;

static void power_set_doze(bool doze) {
    if (doze == s_doze) return;
    s_doze = doze;

    esp_lcd_panel_handle_t panel = bsp_display_panel_handle();

    /* Under the LVGL lock: this runs on the main task, and the panel IO is not
     * safe to drive while the LVGL task is mid-flush on the same QSPI device.
     * Auto-sleep firing on the lock screen is the worst case for that race —
     * its 40 ms sweep timer keeps flushes almost continuous. */
    if (doze) {
        if (panel && ui_lock()) {
            esp_lcd_panel_disp_on_off(panel, false);          /* 0x28 */
            bsp_display_unlock();
        }
        esp_wifi_set_ps(WIFI_PS_MAX_MODEM);
        esp_pm_config_t pm = { .max_freq_mhz = 80, .min_freq_mhz = 80,
                               .light_sleep_enable = false };
        esp_pm_configure(&pm);
    } else {
        esp_pm_config_t pm = { .max_freq_mhz = 240, .min_freq_mhz = 80,
                               .light_sleep_enable = false };
        esp_pm_configure(&pm);
        esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        if (panel && ui_lock()) {
            esp_lcd_panel_disp_on_off(panel, true);           /* 0x29 */
            bsp_display_unlock();
        }
    }
    ESP_LOGI(TAG, "power: %s", doze ? "DOZE (panel off, wifi max sleep, 80 MHz)"
                                    : "ACTIVE");
    telemetry_row(doze ? "doze" : "wake");
}

/* mV lost per hour since the first reading — the number to watch overnight */
static int battery_drain_mv_h(void) {
    if (!s_batt_t_first || s_batt_mv <= 0) return 0;
    int64_t dt = now_ms() - s_batt_t_first;
    if (dt < 120000) return 0;
    return (int)((int64_t)(s_batt_mv_first - s_batt_mv) * 3600000 / dt);
}

/* ---------------- lock screen ----------------
 *
 * Left key locks; a tap unlocks. Idle timers do the rest: untouched for a
 * minute locks, and once locked the backlight drops after fifteen seconds —
 * unless desk-clock mode is on, which suppresses the sleep entirely.
 *
 * Deliberately text-light: clock, date, battery, nothing else. Desk-clock mode
 * is signalled by the sweep ring turning amber rather than by a status line.
 */

static bool s_always_on;

static void time_sync_start(void) {
    setenv("TZ", TIMEZONE, 1);
    tzset();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started (%s, TZ=%s)", NTP_SERVER, TIMEZONE);
}

static int s_lock_slow;

/* Everything that is not the sweep. Called directly when the screen is built
 * so the readouts are correct on the very first frame — a persistent "slow"
 * counter used to make a freshly rebuilt lock screen skip its first update,
 * which is how the placeholder text kept reappearing. */
/* Driven from lock_refresh() at 1 Hz rather than from a timer of its own. The
 * sweep already invalidates this screen every 40 ms, so the cost that matters is
 * not how often this runs but whether it touches widgets when nothing changed —
 * hence label_set_changed() and the visibility check. */
static void lock_np_refresh(void) {
    if (!s_lock_np) return;

    bool show = s_sp_have_state && s_sp_track[0];
    if (show == s_lock_np_up) {
        if (!show) return;                      /* nothing to do while hidden */
    } else {
        s_lock_np_up = show;
        if (show) lv_obj_remove_flag(s_lock_np, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(s_lock_np, LV_OBJ_FLAG_HIDDEN);

        /* Move the clock stack up rather than shrinking the player to fit under
         * it. The first attempt kept the clock centred and squeezed the transport
         * into what was left, which produced 46 px targets — small enough to
         * ghost-touch, which is the same mistake the first MUSIC layout made. The
         * clock has nothing below it worth protecting, so it yields. */
        lv_obj_set_style_text_font(s_lock_time,
                                   show ? &hud_clock_48 : &hud_clock_76, 0);
        lv_obj_align(s_lock_time, LV_ALIGN_CENTER, 0, show ? -140 : -18);
        lv_obj_align(s_lock_date, LV_ALIGN_CENTER, 0, show ? -104 :  46);
        if (s_lock_rule) {
            if (show) lv_obj_add_flag(s_lock_rule, LV_OBJ_FLAG_HIDDEN);
            else      lv_obj_remove_flag(s_lock_rule, LV_OBJ_FLAG_HIDDEN);
        }

        if (!show) return;
    }

    label_set_changed(s_lock_np_track, s_sp_track);
    sp_style_toggle(s_lock_np_play, s_sp_playing, true, 0x1DB954,
                    s_sp_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    sp_style_toggle(s_lock_np_prev, false, !s_sp_no_prev, 0x334155, NULL);
    sp_style_toggle(s_lock_np_next, false, !s_sp_no_next, 0x334155, NULL);

    /* Same rule as MUSIC: show the cover only while the file on the card is the
     * one this track wants, so a stale album never sits under a new title. */
    bool art_ok = s_sp_art_ready && s_sp_art_url[0] &&
                  strcmp(s_sp_art_have, s_sp_art_url) == 0;
    if (art_ok && lv_obj_has_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN)) {
        lv_image_set_src(s_lock_np_art, &s_sp_art_dsc);
        lv_obj_remove_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN);
    } else if (!art_ok && !lv_obj_has_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lock_refresh(void) {
    if (!s_lock_time) return;
    lock_np_refresh();

    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    if (ti.tm_year < (2024 - 1900)) {
        lv_label_set_text(s_lock_time, "00:00");
        lv_label_set_text(s_lock_date, "");
    } else {
        char hm[8], date[40];
        strftime(hm, sizeof(hm), "%H:%M", &ti);
        strftime(date, sizeof(date), "%a %d %b", &ti);
        for (char *p = date; *p; p++) *p = toupper((unsigned char)*p);
        lv_label_set_text(s_lock_time, hm);
        lv_label_set_text(s_lock_date, date);
        s_time_synced = true;
    }

    int pct = s_batt_pct;
    if (pct < 0) {
        lv_label_set_text(s_lock_batt, "EXT PWR");
        lv_arc_set_bg_angles(s_lock_batt_arc, 130, 380);
        lv_obj_set_style_arc_color(s_lock_batt_arc, lv_color_hex(0x22D3EE), LV_PART_MAIN);
    } else {
        lv_label_set_text_fmt(s_lock_batt, "%s%d%%", s_batt_charging ? "+" : "", pct);
        lv_arc_set_bg_angles(s_lock_batt_arc, 130, 130 + (250 * pct) / 100);
        lv_obj_set_style_arc_color(s_lock_batt_arc,
            lv_color_hex(pct >= 50 ? 0x22D3EE : (pct >= 20 ? 0xF59E0B : 0xEF4444)),
            LV_PART_MAIN);
    }

    /* the only desk-clock indicator: amber sweep instead of cyan */
    if (s_lock_sweep) {
        lv_obj_set_style_arc_color(s_lock_sweep,
            lv_color_hex(s_always_on ? 0xF59E0B : 0x22D3EE), LV_PART_MAIN);
    }
}

static void lock_timer_cb(lv_timer_t *t) {
    if (!s_lock_time || !s_screen_on) return;      /* no flushes while dozing */

    s_sweep_deg = (s_sweep_deg + 3) % 360;
    lv_arc_set_rotation(s_lock_sweep, s_sweep_deg);

    if (s_lock_slow++ % 25) return;                /* the rest at ~1 Hz */
    lock_refresh();
}

static void lock_tap_cb(lv_event_t *e) {
    /* Asleep? The first touch only wakes — it must not unlock, the same way a
     * phone shows you the lock screen before letting you in. */
    if (!s_screen_on) {
        s_req_wake = true;
        return;
    }
    ESP_LOGI(TAG, "lock: screen tapped -> home");
    app_request(APP_DRAWER);      /* always home, not "wherever you locked from" */
}

/* The cover is a shortcut straight into MUSIC. Everywhere else on this screen
 * goes home, so the art has to consume its own click — which it does simply by
 * being clickable: LVGL delivers to the topmost clickable object and does not
 * bubble without LV_OBJ_FLAG_EVENT_BUBBLE, so lock_tap_cb never sees it.
 *
 * The asleep check is repeated rather than shared, because it is the whole reason
 * the panel is reachable at all: with the panel dark, the first touch must only
 * wake. Without this, tapping a cube on the desk to see the time would drop you
 * into an app. */
static void lock_np_tap_cb(lv_event_t *e) {
    if (!s_screen_on) {
        s_req_wake = true;
        return;
    }
    ESP_LOGI(TAG, "lock: cover tapped -> MUSIC");
    app_request(APP_MUSIC);
}

static void build_lock_screen(lv_obj_t *scr) {
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    /* Flat black, no gradient: RGB565 cannot render a smooth dark ramp, so a
     * gradient shows as hard bands — and on an AMOLED black pixels are off. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_NONE, 0);

    /* A different one from the pool on every build, so locking the device
     * twice in a row does not show the same picture twice. */
    int slot = s_sd_ok ? wall_display_slot() : -1;
    if (slot >= 0) {
        char lvpath[72];
        wall_lv(slot, lvpath, sizeof(lvpath));
        lv_obj_t *wall = lv_image_create(scr);
        lv_image_set_src(wall, lvpath);
        /* Fill the panel, whatever size the file turned out to be.
         *
         * The request asks imgix for exactly 480x480, but what comes back is not
         * always that — a source photo smaller than 480 on one side comes back
         * short, and the image was then drawn at its natural size, leaving a
         * bare strip along one edge where the wallpaper simply stopped. Pinning
         * the widget to the full screen and letting COVER scale into it keeps
         * the aspect ratio and guarantees no gap. */
        lv_obj_set_size(wall, BSP_LCD_H_RES, BSP_LCD_V_RES);
        lv_obj_center(wall);
        lv_image_set_inner_align(wall, LV_IMAGE_ALIGN_COVER);
        lv_obj_set_style_image_opa(wall, 110, 0);
        lv_obj_remove_flag(wall, LV_OBJ_FLAG_CLICKABLE);
        s_wall_slot = slot;

        /* attribution for whatever is actually on screen, shown in STATUS */
        char cpath[64];
        wall_txt(slot, cpath, sizeof(cpath));
        s_wall_credit[0] = '\0';
        FILE *cf = fopen(cpath, "r");
        if (cf) {
            if (fgets(s_wall_credit, sizeof(s_wall_credit), cf)) {
                s_wall_credit[strcspn(s_wall_credit, "\r\n")] = '\0';
            }
            fclose(cf);
        }
    }

    /* HUD rings. A 430 px circle on a 480 px square clears the corner radius. */
    lv_obj_t *ring = lv_arc_create(scr);
    lv_obj_set_size(ring, 430, 430);
    lv_obj_center(ring);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ring, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, lv_color_hex(0x123A52), LV_PART_MAIN);
    lv_arc_set_bg_angles(ring, 0, 360);

    lv_obj_t *ring2 = lv_arc_create(scr);
    lv_obj_set_size(ring2, 372, 372);
    lv_obj_center(ring2);
    lv_obj_remove_style(ring2, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring2, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(ring2, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring2, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring2, lv_color_hex(0x0C2438), LV_PART_MAIN);
    lv_arc_set_bg_angles(ring2, 0, 360);

    s_lock_batt_arc = lv_arc_create(scr);
    lv_obj_set_size(s_lock_batt_arc, 430, 430);
    lv_obj_center(s_lock_batt_arc);
    lv_obj_remove_style(s_lock_batt_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_lock_batt_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_lock_batt_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_lock_batt_arc, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_lock_batt_arc, lv_color_hex(0x22D3EE), LV_PART_MAIN);
    lv_arc_set_bg_angles(s_lock_batt_arc, 130, 180);

    s_lock_sweep = lv_arc_create(scr);
    lv_obj_set_size(s_lock_sweep, 402, 402);
    lv_obj_center(s_lock_sweep);
    lv_obj_remove_style(s_lock_sweep, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_lock_sweep, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_lock_sweep, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_lock_sweep, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_lock_sweep, lv_color_hex(0x22D3EE), LV_PART_MAIN);
    lv_arc_set_bg_angles(s_lock_sweep, 0, 42);

    s_lock_time = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lock_time, &hud_clock_76, 0);
    lv_obj_set_style_text_color(s_lock_time, lv_color_hex(0xE8FBFF), 0);
    lv_label_set_text(s_lock_time, "--:--");
    lv_obj_align(s_lock_time, LV_ALIGN_CENTER, 0, -18);

    s_lock_rule = lv_obj_create(scr);
    lv_obj_remove_style_all(s_lock_rule);
    lv_obj_set_size(s_lock_rule, 190, 1);
    lv_obj_set_style_bg_color(s_lock_rule, lv_color_hex(0x1B4E68), 0);
    lv_obj_set_style_bg_opa(s_lock_rule, LV_OPA_COVER, 0);
    lv_obj_align(s_lock_rule, LV_ALIGN_CENTER, 0, 26);
    lv_obj_remove_flag(s_lock_rule, LV_OBJ_FLAG_CLICKABLE);

    s_lock_date = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lock_date, &hud_text_18, 0);
    lv_obj_set_style_text_color(s_lock_date, lv_color_hex(0x5FD3EC), 0);
    lv_obj_set_style_text_letter_space(s_lock_date, 3, 0);
    lv_obj_set_width(s_lock_date, CONTENT_W);
    lv_obj_set_style_text_align(s_lock_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lock_date, "");
    lv_obj_align(s_lock_date, LV_ALIGN_CENTER, 0, 46);

    s_lock_batt = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lock_batt, &hud_text_18, 0);
    lv_obj_set_style_text_color(s_lock_batt, lv_color_hex(0x22D3EE), 0);
    lv_obj_set_style_text_letter_space(s_lock_batt, 2, 0);
    lv_label_set_text(s_lock_batt, "");
    lv_obj_align(s_lock_batt, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 12);

    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, lock_tap_cb, LV_EVENT_CLICKED, NULL);

    s_lock_slow = 0;
    lock_refresh();                       /* correct on the first frame */
    /* Now-playing panel, in the band between the date and the inner ring —
     * y 326..436, the only clear space on this screen. It sits on a scrim because
     * it has to stay legible over an arbitrary photograph, and it starts hidden:
     * lock_np_refresh() reveals it only when Spotify reports an active device, so
     * a cube with nothing playing looks exactly as sparse as it did before.
     *
     * Reuses sp_round_btn and the MUSIC transport callbacks wholesale. Those
     * callbacks null-check every widget they touch, which is what makes them safe
     * to fire from a screen where the MUSIC widgets do not exist. */
    s_lock_np = lv_obj_create(scr);
    lv_obj_remove_style_all(s_lock_np);
    lv_obj_set_size(s_lock_np, 400, 246);
    lv_obj_align(s_lock_np, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_obj_set_style_bg_color(s_lock_np, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_lock_np, 190, 0);       /* scrim, not opaque */
    lv_obj_set_style_radius(s_lock_np, 20, 0);
    lv_obj_remove_flag(s_lock_np, LV_OBJ_FLAG_SCROLLABLE);
    /* Not clickable: a tap on the panel background must still unlock, the same as
     * a tap anywhere else. Only the three buttons swallow their own clicks. */
    lv_obj_remove_flag(s_lock_np, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lock_np, LV_OBJ_FLAG_HIDDEN);

    s_lock_np_ph = lv_obj_create(s_lock_np);
    lv_obj_remove_style_all(s_lock_np_ph);
    lv_obj_set_size(s_lock_np_ph, 100, 100);
    lv_obj_set_pos(s_lock_np_ph, 16, 18);
    lv_obj_set_style_bg_color(s_lock_np_ph, lv_color_hex(0x11161F), 0);
    lv_obj_set_style_bg_opa(s_lock_np_ph, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_lock_np_ph, 10, 0);
    lv_obj_add_flag(s_lock_np_ph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lock_np_ph, lock_np_tap_cb, LV_EVENT_CLICKED, NULL);

    s_lock_np_art = lv_image_create(s_lock_np);
    lv_obj_set_size(s_lock_np_art, 100, 100);
    lv_obj_set_pos(s_lock_np_art, 16, 18);
    lv_image_set_inner_align(s_lock_np_art, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(s_lock_np_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_lock_np_art, lock_np_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN);

    s_lock_np_track = lv_label_create(s_lock_np);
    lv_obj_set_width(s_lock_np_track, 254);
    lv_obj_set_pos(s_lock_np_track, 130, 58);
    lv_obj_set_style_text_font(s_lock_np_track, &hud_text_18, 0);
    lv_obj_set_style_text_color(s_lock_np_track, lv_color_hex(0xF2E9DC), 0);
    lv_label_set_long_mode(s_lock_np_track, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_lock_np_track, "");

    s_lock_np_prev = sp_round_btn(s_lock_np, LV_SYMBOL_PREV, &lv_font_montserrat_36,
                                  100, 172, 76, sp_prev_cb, 0x334155, 0x141B26);
    s_lock_np_play = sp_round_btn(s_lock_np, LV_SYMBOL_PLAY, &lv_font_montserrat_48,
                                  200, 172, 88, sp_play_cb, 0x1DB954, 0x16241C);
    s_lock_np_next = sp_round_btn(s_lock_np, LV_SYMBOL_NEXT, &lv_font_montserrat_36,
                                  300, 172, 76, sp_next_cb, 0x334155, 0x141B26);

    s_app_timer = lv_timer_create(lock_timer_cb, 40, NULL);   /* drives the sweep */
}

static void lock_engage(void) {
    if (s_app == APP_LOCK) {                 /* already locked: sleep now */
        if (s_screen_on) screen_toggle_power();
        return;
    }
    app_request(APP_LOCK);
}

/* Desk-clock mode: hold the right key on the lock screen to keep the panel lit
 * indefinitely. A banner confirms which way it just went, then fades itself
 * out and deletes itself — no timer to own or tear down. */
static void always_on_toggle(void) {
    s_always_on = !s_always_on;
    ESP_LOGI(TAG, "always-on %s", s_always_on ? "ON" : "OFF");

    if (!ui_lock()) return;
    lock_refresh();                          /* recolours the sweep ring */

    lv_obj_t *toast = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(toast, &hud_text_18, 0);
    lv_obj_set_style_text_letter_space(toast, 3, 0);
    lv_obj_set_style_text_color(toast,
        lv_color_hex(s_always_on ? 0xF59E0B : 0x7C8AA5), 0);
    lv_obj_set_style_bg_color(toast, lv_color_hex(0x08131C), 0);
    lv_obj_set_style_bg_opa(toast, 235, 0);
    lv_obj_set_style_pad_all(toast, 14, 0);
    lv_obj_set_style_radius(toast, 14, 0);
    lv_obj_set_style_border_width(toast, 1, 0);
    lv_obj_set_style_border_color(toast,
        lv_color_hex(s_always_on ? 0xF59E0B : 0x33465C), 0);
    lv_label_set_text(toast, s_always_on ? "ALWAYS ON SCREEN  ON"
                                         : "ALWAYS ON SCREEN  OFF");
    lv_obj_align(toast, LV_ALIGN_CENTER, 0, 118);
    /* fade_out only animates opacity — it does NOT delete, so without the
     * delayed delete every toggle would leave an invisible label behind.
     * Deleting an object cancels animations bound to it, so this is still safe
     * if the lock screen is torn down before the timer elapses. */
    lv_obj_fade_out(toast, 900, 900);
    lv_obj_delete_delayed(toast, 1900);
    bsp_display_unlock();
}

/* ---------------- the global button contract ----------------
 *
 *   middle (PWR)   short : HOME — always, from anywhere
 *   right  (BOOT)  tap   : BACK — one scene inside the app, or HOME at its root
 *                  hold  : ACTION — the running app's primary verb
 *   left   (IO18)  short : LOCK — always
 */

/* true if the app consumed the Back itself (it had a sub-scene to pop) */
static bool app_back(void) {
    if (s_app == APP_MUSIC) {
        if (s_sp_devpanel && !lv_obj_has_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN)) {
            if (ui_lock()) { sp_show_devices(false); bsp_display_unlock(); }
            return true;
        }
        return false;
    }
    return false;
}

static void app_action(void) {
    static int drawer_pick;
    switch (s_app) {
    case APP_LOCK:
        always_on_toggle();                         /* desk-clock mode */
        break;
    case APP_PET:
        pet_play();
        break;
    case APP_CONTROL:
        s_req_wallpaper = true;      /* the card shows real progress for this */
        break;
    case APP_MUSIC:
        s_sp_playing = !s_sp_playing;
        sp_send(s_sp_playing ? SP_CMD_PLAY : SP_CMD_PAUSE);
        break;
    case APP_POMO:
        if (s_pomo_state == POMO_RUN || s_pomo_state == POMO_PAUSE) {
            pomo_log("cancelled");
            s_pomo_state = POMO_IDLE;
            s_pomo_left_s = s_pomo_total_s;
            sfx_play(SFX_PAUSE);
            ESP_LOGI(TAG, "pomodoro: cancelled");
        }
        break;
    default:                                        /* drawer: step through apps */
        drawer_pick = (drawer_pick + 1) % APP_COUNT;
        app_request(drawer_pick);
        break;
    }
}

/* ---------------- app switching ---------------- */

/* Runs on the main task with the LVGL lock held. The outgoing screen is freed
 * by lv_screen_load_anim(auto_del), so only one app's widgets exist at a time
 * (plus the incoming one during the 120 ms cross-fade). */
static void app_open(int idx) {
    static int64_t last_switch;
    int64_t t0 = esp_timer_get_time();
    if (t0 - last_switch < 250000) return;      /* debounce double taps */
    last_switch = t0;

    /* Leaving CONTROL ends any pairing session. The code is only shown on that
     * card, so a session outliving it would advertise with no way to read the
     * code — and would hold the Wi-Fi driver down the whole time. The main
     * loop's restore path brings Wi-Fi back. Deliberately before ui_lock():
     * teardown does not touch LVGL and can take a moment. */
    if (s_app == APP_CONTROL && idx != APP_CONTROL && ble_prov_active()) {
        s_req_ble_off = true;
    }

    if (!ui_lock()) return;

    /* let the outgoing app flush its state before its widgets disappear */
    if (s_app >= 0 && s_app < APP_COUNT && s_apps[s_app].save) {
        s_apps[s_app].save();
    }
    if (s_app_timer) { lv_timer_delete(s_app_timer); s_app_timer = NULL; }
    s_scr_home = s_scr_setup = s_scr_pet = NULL;
    s_status_label = NULL; s_batt_bar = NULL; s_batt_label = NULL;
    s_bolt_label = NULL; s_fps_label = NULL; s_events_label = NULL;
    s_ch_wrap = NULL;
    s_cfg_wall_pool = NULL; s_cfg_wall_state = NULL;
    s_cfg_wall_bar = NULL; s_cfg_wall_sub = NULL;
    s_cfg_rot_val = NULL; s_cfg_vol_val = NULL;
    s_cfg_rot_sw = NULL; s_cfg_rot_btn = NULL; s_cfg_bright_val = NULL;
    s_cfg_batt_bar = NULL;
    s_cfg_batt_val = NULL; s_cfg_batt_sub = NULL;
    s_cfg_net_val = NULL; s_cfg_sys_val = NULL; s_cfg_log = NULL;
    s_cfg_ble_val = NULL; s_cfg_ble_code = NULL; s_cfg_ble_btn = NULL;
    s_sp_art = NULL; s_sp_art_ph = NULL; s_sp_lbl_track = NULL;
    s_sp_lbl_artist = NULL; s_sp_btn_play_lbl = NULL; s_sp_btn_shuf = NULL;
    s_sp_btn_dev = NULL; s_sp_devpanel = NULL; s_sp_devlist = NULL;
    s_sp_btn_prev = NULL; s_sp_btn_next = NULL; s_sp_btn_like = NULL;
    s_sp_vol_bar = NULL; s_sp_vol_icon = NULL;
    s_sp_scr = NULL; s_sp_bg_drawn = 0;
    s_pomo_clock = NULL; s_pomo_word = NULL;
    s_pomo_ring = NULL; s_pomo_arc = NULL; s_pomo_fill = NULL;
    for (int i = 0; i < POMO_SLOTS; i++) s_pomo_dial[i] = NULL;
    s_lock_time = NULL; s_lock_date = NULL; s_lock_batt = NULL;
    s_lock_sweep = NULL; s_lock_batt_arc = NULL;
    s_lock_np = NULL; s_lock_np_art = NULL; s_lock_np_ph = NULL;
    s_lock_np_track = NULL; s_lock_np_prev = NULL; s_lock_np_play = NULL;
    s_lock_np_next = NULL;
    /* Must clear, not just null: wall_service() runs on the network task and would
     * otherwise keep suppressing wallpaper downloads forever after the lock screen
     * is torn down, with nothing left on screen to explain why. */
    s_lock_np_up = false;
    s_lock_np_bg = 0;
    s_lock_rule = NULL;

    /* Free the outgoing app BEFORE building the next one. A cross-fade with
     * auto_del keeps both alive at once, and that peak drove internal heap to
     * 16 bytes, which wedged the flush path and starved the idle task. */
    lv_obj_t *old_scr = lv_screen_active();
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    if (old_scr && old_scr != scr) lv_obj_delete(old_scr);

    if (idx >= 0 && idx < APP_COUNT)  s_apps[idx].build(scr);
    else if (idx == APP_LOCK)         build_lock_screen(scr);
    else                              build_drawer(scr);
    s_app = idx;

    bsp_display_unlock();

    ESP_LOGI(TAG, "opened %s in %lld ms (internal free %u)",
             (idx >= 0 && idx < APP_COUNT) ? s_apps[idx].name
                 : (idx == APP_LOCK ? "LOCK" : "DRAWER"),
             (long long)((esp_timer_get_time() - t0) / 1000),
             (unsigned)hp_free());

}

/* One-shot per-app memory bench.
 *
 * The power log showed heap_free reaching 1,223 bytes with an app open, awake,
 * and no download in flight — so the cost is in the screens themselves, not in
 * a transient. This opens each app in turn, lets it render, and reports what it
 * actually costs, because guessing which of five cards is expensive is how you
 * optimise the wrong thing. */
#define APP_MEM_BENCH 0

#if APP_MEM_BENCH
static void mem_line(const char *what) {
    ESP_LOGW(TAG, "  %-10s free=%6u  min=%6u  largest=%6u  psram=%lu",
             what, (unsigned)hp_free(), (unsigned)hp_min(), (unsigned)hp_largest(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void app_mem_bench(void) {
    /* Wait for the network first: measuring an app that cannot reach its API
     * measures the wrong thing, and MUSIC is the one that cares. */
    ESP_LOGW(TAG, "bench: waiting for wifi...");
    xEventGroupWaitBits(s_evt, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    ESP_LOGW(TAG, "=== per-app internal SRAM cost ===");
    app_open(APP_DRAWER);
    vTaskDelay(pdMS_TO_TICKS(1200));
    mem_line("DRAWER");

    for (int i = 0; i < APP_COUNT; i++) {
        app_open(i);
        vTaskDelay(pdMS_TO_TICKS(1800));    /* render a few frames */
        mem_line(s_apps[i].name);
    }

    app_open(APP_LOCK);
    vTaskDelay(pdMS_TO_TICKS(1800));
    mem_line("LOCK");

    /* Leave it on MUSIC so the poll loop actually runs and can be observed. */
    app_open(APP_MUSIC);
    vTaskDelay(pdMS_TO_TICKS(1200));
    mem_line("rest-on-MUSIC");
    ESP_LOGW(TAG, "=== end bench ===");
}
#endif

/* ---------------- Main ---------------- */

void app_main(void) {
    ESP_LOGI(TAG, "Funnel-profile firmware start, reset_reason=%d", (int)esp_reset_reason());

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_log_mtx = xSemaphoreCreateMutex();
    for (int i = 0; i < LOG_LINES; i++) {
        s_log[i][0] = ' ';
        s_log[i][1] = '\0';
    }
    log_event("boot");
    log_mem("boot");

    creds_load();
    known_load();
    /* Seed from the boot credential. Without this a device that has been on the
     * same network since before the table existed reports nothing as saved, and
     * the phone asks for a password the cube is holding. Self-dedupes. */
    if (s_ssid[0] && s_pass[0]) known_remember(s_ssid, s_pass);
    pet_load();
    pomo_load();
    /* Ahead of the display, not with the other settings further down, and handed
     * to the BSP rather than applied here: the panel is already lit and holding a
     * 600 ms delay partway through bsp_display_start_with_config(), so anything
     * this side of that call is far too late to stop a full-brightness flash. The
     * BSP fork takes the level up front instead (its fork change #6). */
    bright_load();
    bsp_display_brightness_set_boot(s_bright);

    /* Wi-Fi driver first: its DMA pool needs one contiguous chunk of pristine
     * internal heap. Association proceeds async while the display comes up. */
    wifi_init();

    bsp_display_cfg_t disp_cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = { .swap_xy = 1, .mirror_x = 0, .mirror_y = 1 },
    };
    disp_cfg.lv_adapter_cfg.stack_in_psram = true;

    lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "DISPLAY INIT FAILED — continuing headless (board stays flashable)");
        log_mem("display-fail");
    } else {
        bright_apply(s_bright);
        log_mem("display-up");
        if (ui_lock()) {
            lv_display_add_event_cb(disp, refr_ready_cb, LV_EVENT_REFR_READY, NULL);
            bsp_display_unlock();
        }
        app_open(APP_DRAWER);
        log_mem("ui-built");


    }

    sd_init();
    rtc_init();
    pmu_init();
    battery_poll();
    rot_off_load();
    imu_init();
    /* Autorotating: native, which aligns the driver's state with no visual change
     * and is corrected by the IMU within a few polls. Held: the orientation the
     * user pinned, which is the whole point of having switched it off. */
    rotation_apply(s_autorot ? 0 : s_rot_held);
    btn_init(&s_key_left);
    btn_init(&s_key_right);

#if PWRLOG_DUMP
    telemetry_dump();          /* before this boot adds its own rows */
#endif
    telemetry_row("boot");

    vol_load();
    sfx_init();
    sp_init();
    /* PSRAM stack: 8 KB of internal SRAM is far too expensive for a task that
     * polls and downloads. Same trick as the sfx task. */
    if (xTaskCreateWithCaps(net_task, "net", 8192, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "!! net stack fell back to INTERNAL SRAM — costs ~8 KB of "
                      "the scarce pool; expect a lower floor");
        s_stack_fallback = true;
        xTaskCreate(net_task, "net", 8192, NULL, 5, NULL);
    }

    int64_t last_stats = now_ms();
    int64_t last_batt = 0, last_imu = 0, last_pet = now_ms(), last_pet_save = now_ms();
    int64_t last_tele = now_ms(), last_rejoin = now_ms(), last_sp_poll = 0;
    uint32_t last_refr = 0;
    int64_t s_last_btn = now_ms();

    /* Watch the main loop itself.
     *
     * The task WDT only watches the idle tasks by default, so it cannot see the
     * failure mode that actually bit us: a deadlock in which every task blocks
     * and both cores go idle. The idle tasks were perfectly healthy, so nothing
     * fired and the device simply sat frozen until it was power-cycled.
     * Subscribing the main task means a stalled loop panics after 30 s and
     * reboots with a backtrace — self-healing, and diagnosable next time. */
#if APP_MEM_BENCH
    app_mem_bench();
#endif

    esp_task_wdt_add(NULL);

    while (1) {
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(s_doze ? 120 : 20));
        int64_t t = now_ms();

        btn_ev_t kleft  = btn_poll(&s_key_left,  t);   /* BOOT / minus */
        btn_ev_t kright = btn_poll(&s_key_right, t);   /* plus         */
        bool pwr = pmu_pwrkey_pressed();               /* PWR          */

        /* left = lock (always) | middle = home | right: tap = back, hold = action */
        if (kleft || kright || pwr) s_last_btn = t;

        /* Waking has to test the pin LEVEL, not a debounced edge: while dozing
         * the loop runs at 120 ms, and a ~160 ms tap cannot produce two
         * consistent samples, so no edge is ever detected. PWR was unaffected
         * only because the PMU latches it in hardware. */
        if (!s_screen_on) {
            bool held = (gpio_get_level(KEY_LEFT_GPIO) == 0) ||
                        (gpio_get_level(KEY_RIGHT_GPIO) == 0);
            if (held || pwr || kleft || kright) {
                screen_toggle_power();                 /* first press only wakes */
                btn_swallow(&s_key_left);
                btn_swallow(&s_key_right);
                kleft = BTN_NONE; kright = BTN_NONE; pwr = false;
                s_last_btn = t;
            }
        }

        /* MUSIC rebinds all three keys to volume, so it is offered them first.
         * It declines on the device-picker scene, which leaves the right key free
         * to pop that sub-scene the usual way. Home there is the swipe-down
         * gesture the music screen already carries. */
        bool key_used = false;
        if (s_app == APP_MUSIC && (kleft || kright || pwr)) {
            if (ui_lock()) {
                key_used = sp_keys(kleft, kright, pwr);
                bsp_display_unlock();
            }
        }

        if (key_used) {
            /* consumed by MUSIC; the global bindings sit this one out */
        } else if (kleft == BTN_SHORT || kleft == BTN_LONG) {
            ESP_LOGI(TAG, "LEFT -> lock");
            lock_engage();
        } else if (s_app == APP_LOCK) {
            /* Locked. Home and tap-back stay disabled — the touchscreen is how
             * you unlock, and a key press must not bypass that. The action key
             * is the one exception: it toggles desk-clock mode, which is only
             * reachable from here. Gating the whole block on "not locked" is
             * what made that toggle dead code. */
            if (kright == BTN_LONG) {
                ESP_LOGI(TAG, "RIGHT hold on lock -> desk clock toggle");
                app_action();
            }
        } else {
            if (pwr) {
                ESP_LOGI(TAG, "MID -> home");
                app_request(APP_DRAWER);
            }
            if (kright == BTN_SHORT) {
                if (app_back()) ESP_LOGI(TAG, "RIGHT tap -> back within app");
                else          { ESP_LOGI(TAG, "RIGHT tap -> home"); app_request(APP_DRAWER); }
            }
            if (kright == BTN_LONG) {
                ESP_LOGI(TAG, "RIGHT hold -> action (app %d)", s_app);
                app_action();
            }
        }

        /* idle handling: auto-lock, then auto-sleep once locked */
        {
            int64_t idle = (int64_t)lv_display_get_inactive_time(NULL);
            if (t - s_last_btn < idle) idle = t - s_last_btn;

            if (s_app == APP_LOCK) {
                /* desk-clock mode deliberately never sleeps */
                if (s_screen_on && !s_always_on && idle > LOCK_SLEEP_MS) {
                    ESP_LOGI(TAG, "auto-sleep");
                    screen_toggle_power();
                }
            } else if (s_screen_on && !s_always_on && idle > AUTO_LOCK_MS &&
                       s_req_app != APP_LOCK) {
                /* !s_always_on: always-on is a promise that the display stays as
                 * you left it, and it was only honouring half of that. The sleep
                 * branch above checked the flag, this one did not, so a cube left
                 * showing MUSIC as a desk display reverted to the clock after
                 * AUTO_LOCK_MS — the panel stayed lit, which is why it read as a
                 * navigation bug rather than as a power one. */
                ESP_LOGI(TAG, "auto-lock after %llds idle", (long long)(idle / 1000));
                s_last_btn = t;               /* reset the clock, don't re-fire */
                lock_engage();
            }
        }

        if (s_req_wake) {                 /* touch-to-wake, still locked */
            s_req_wake = false;
            s_last_btn = t;
            if (!s_screen_on) {
                ESP_LOGI(TAG, "touch wake (stays locked)");
                screen_toggle_power();
            }
        }

        if (s_req_sntp) {
            s_req_sntp = false;
            time_sync_start();
        }

        /* wallpapers are fetched on the network task — see https_task() */

        if (s_req_app != APP_NONE) {
            int want = s_req_app;
            s_req_app = APP_NONE;
            if (want != s_app) app_open(want);
        }

        /* Poll while MUSIC is open, and while the lock screen is actually being
         * looked at so its now-playing panel is live. Never while dozing: §7b's
         * whole design is stopping periodic work nobody can see, and at 80 MHz
         * with WIFI_PS_MAX_MODEM an HTTPS call costs ~4 s, so a 3 s cadence would
         * keep the radio permanently awake and undo the battery projection.
         *
         * The lock screen polls at half the rate. It only needs to be right at a
         * glance, and desk-clock mode never sleeps — an ungated 3 s poll there
         * would run for as long as the cube sat on the desk. */
        bool sp_poll_lock = (s_app == APP_LOCK && s_screen_on && !s_doze);
        int64_t sp_due = sp_poll_lock ? (SP_POLL_MS * 2) : SP_POLL_MS;
        if ((s_app == APP_MUSIC || sp_poll_lock) && s_wifi_up &&
            t - last_sp_poll >= sp_due) {
            last_sp_poll = t;
            sp_send(SP_CMD_POLL);
        }

        if (s_req_vol_save && !ble_prov_nvs_blocked()) {
            s_req_vol_save = false;
            vol_save();
        }

        /* The slider only records a number; the panel write happens here, one per
         * pass at most, which is also what keeps a drag from issuing a 0x51 per
         * touch sample. The flag is cleared only when the write actually lands —
         * clearing it up front would drop the request on a lock timeout, or
         * whenever it arrived while the panel was off or dimmed. */
        if (s_req_bright_apply && s_screen_on && !s_pomo_dimmed) {
            if (bright_apply(s_bright)) s_req_bright_apply = false;
        }

        if (s_req_bright_save && !ble_prov_nvs_blocked()) {
            s_req_bright_save = false;
            bright_save();
        }

        if (s_req_autorot_save && !ble_prov_nvs_blocked()) {
            s_req_autorot_save = false;
            autorot_save();
        }

        if (s_req_ble_on) {
            s_req_ble_on = false;

            /* Scan while there is still a driver to scan with. This snapshot is
             * the entire list the phone will ever see — Wi-Fi is gone for the
             * whole session, so there is no rescan. */
            if (!s_wifi_disabled) {
                /* A scan is refused while a connect is in flight and
                 * wifi_scan_now() has already zeroed the list by then, so the
                 * phone gets an empty one. The reconnect diagnostic elsewhere
                 * in this loop works around it the same way. */
                esp_wifi_disconnect();
                vTaskDelay(pdMS_TO_TICKS(200));
                wifi_scan_now();
            }

            ble_prov_ap_t snap[16];
            int n = (s_ap_count < 16) ? s_ap_count : 16;
            for (int i = 0; i < n; i++) {
                /* %.32s: the truncation is intentional and -Werror=format-
                 * truncation fires without an explicit bound (pitfall #7). */
                snprintf(snap[i].ssid, sizeof(snap[i].ssid), "%.32s", s_aps[i].ssid);
                snap[i].rssi   = s_aps[i].rssi;
                snap[i].secure = s_aps[i].secure;
                snap[i].saved  = known_pass(snap[i].ssid) != NULL;
            }
            char cur[33];
            snprintf(cur, sizeof(cur), "%.32s", s_wifi_up ? s_ssid : "");
            /* The network we hold a password for, whether or not we are on it.
             * s_ssid is the configured SSID and s_pass its key; being offline
             * does not mean we have forgotten them. */
            int nsaved = 0;
            for (int i = 0; i < n; i++) if (snap[i].saved) nsaved++;
            ESP_LOGI(TAG, "ble pairing: %d networks (%d known), current=\"%s\"",
                     n, nsaved, cur);
            log_event("ble pairing (%d networks)", n);
            wifi_driver_down();

            if (!ble_prov_start(snap, n, cur)) {
                ESP_LOGE(TAG, "BLE failed to start; restoring wifi");
                log_event("ble start failed");
                /* wifi_driver_up() happens in the restore block below, which
                 * every exit route funnels through. */
            }
        }

        if (s_req_ble_off) {
            s_req_ble_off = false;
            ble_prov_stop();
        }

        ble_prov_poll(t);

        /* A pairing session is neither touch nor key activity, so without this
         * the 60 s auto-lock tears down the very screen showing the code.
         * Same reason pomo_poll() does it. */
        if (ble_prov_active()) lv_display_trigger_activity(NULL);

        /* The single restore path. Every way a session can end funnels through
         * here — handed off, timed out, Stop pressed, phone walked away, or the
         * stack refused to start. Restoring only after a successful hand-off
         * would strand the cube offline until reboot whenever someone opened
         * pairing and wandered off, which is the likeliest way this gets used
         * wrong. */
        if (s_wifi_torn_down && !ble_prov_active()) {
            /* Before any join, so a forget-then-join in one session ends up
             * with the new credentials rather than nothing. */
            if (s_ble_forget) {
                s_ble_forget = false;
                s_ssid[0] = '\0';
                s_pass[0] = '\0';
                nvs_handle_t h;
                if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
                    nvs_erase_key(h, "ssid");
                    nvs_erase_key(h, "pass");
                    nvs_commit(h);
                    nvs_close(h);
                }
                ESP_LOGI(TAG, "stored wifi credentials forgotten");
                log_event("wifi forgotten");
            }
            if (s_ble_handoff) {
                s_ble_handoff = false;
                if (s_ble_ssid[0]) {
                    snprintf(s_ssid, sizeof(s_ssid), "%.32s", s_ble_ssid);
                    snprintf(s_pass, sizeof(s_pass), "%.63s", s_ble_pass);
                    s_wifi_disabled = false;
                    /* Pending, not saved. NVS wins over the compiled-in values,
                     * so committing a password that turns out to be wrong would
                     * permanently override a working config and present as
                     * reason=201 forever. The GOT_IP handler commits it once it
                     * actually joins — which also puts the flash erase well
                     * after the BLE controller is down. */
                    s_creds_pending = true;
                    s_wifi_tries = 0;
                    log_event("ble join %s", s_ssid);
                } else {
                    s_wifi_disabled = true;   /* the "stay off" hand-off */
                    log_event("ble wifi off");
                }
                memset(s_ble_pass, 0, sizeof(s_ble_pass));
            }
            wifi_driver_up();
            if (!s_wifi_disabled) esp_wifi_connect();
        }

        /* Deferred credential writes land here, on the main task, once the
         * radio is down. */
        if (s_req_known_remember && !ble_prov_nvs_blocked()) {
            s_req_known_remember = false;
            known_remember(s_ssid, s_pass);
        }
        if (s_req_creds_save && !ble_prov_nvs_blocked()) {
            s_req_creds_save = false;
            creds_save(s_ssid, s_pass);
            ESP_LOGI(TAG, "credentials for \"%s\" confirmed and saved", s_ssid);
        }
        known_flush();

        /* The single owner of reconnection. The event handler only records the
         * reason; everything else happens here, so there is exactly one caller
         * of esp_wifi_connect() and no chance of the two racing.
         *
         * Backoff matters: reason 201 (NO_AP_FOUND) means the network is simply
         * not there, and it can stay that way for hours. Hammering it every
         * 10 s achieves nothing, keeps the radio busy and floods the log. Ramp
         * 5 s -> 60 s and sit there. */
        if (s_wifi_up || s_wifi_disabled || s_wifi_torn_down || s_wifi_scan_only) {
            /* s_wifi_torn_down: the driver is deinitialised for a pairing
             * session. Reconnecting into a dead driver would return
             * ESP_ERR_WIFI_NOT_INIT on the backoff schedule for the whole
             * session, burying real failures in the log.
             *
             * s_wifi_scan_only: a look-only bring-up for the phone's rescan. The
             * event handler already declines to associate on STA_START, but that
             * is not sufficient — ble_prov_rescan() runs on the NimBLE host task
             * while this loop keeps its own schedule, and wifi_driver_up() clears
             * s_wifi_torn_down, so for the second or two the scan takes this
             * branch would otherwise be eligible to call esp_wifi_connect() and
             * associate underneath it. A scan cannot run during a connect, so the
             * result would be an empty list reported as "no networks in range".
             * It would depend on where the backoff timer happened to land, which
             * makes it the intermittent version of the bug the event-handler
             * guard fixes deterministically. */
            s_wifi_tries = 0;
        } else {
            int64_t wait = 5000LL << (s_wifi_tries < 4 ? s_wifi_tries : 4);
            if (wait > 60000) wait = 60000;
            if (t - last_rejoin >= wait) {
                last_rejoin = t;
                if (s_wifi_tries < 12) s_wifi_tries++;
                /* Diagnose before reconnecting, not after. A scan cannot start
                 * while a connect is in flight, so doing this straight after
                 * esp_wifi_connect() just fails and reports "nothing visible",
                 * which reads as a dead radio. Spend one round on the scan
                 * instead of a connect attempt. */
                if (s_wifi_tries >= 3 && !s_wifi_diag_done) {
                    s_wifi_diag_done = true;
                    ESP_LOGW(TAG, "cannot join \"%s\" — scanning to see what is here", s_ssid);
                    esp_wifi_disconnect();
                    vTaskDelay(pdMS_TO_TICKS(200));
                    wifi_scan_now();
                    if (s_ap_count == 0) {
                        ESP_LOGW(TAG, "  scan returned nothing — either genuinely no "
                                      "2.4 GHz networks in range, or the scan was refused");
                    }
                    for (int k = 0; k < s_ap_count; k++) {
                        ESP_LOGW(TAG, "  visible: \"%s\" %d dBm%s%s",
                                 s_aps[k].ssid, s_aps[k].rssi,
                                 s_aps[k].secure ? "" : " (open)",
                                 strcmp(s_aps[k].ssid, s_ssid) == 0 ? "  <-- CONFIGURED" : "");
                    }
                } else {
                    ESP_LOGI(TAG, "wifi reconnect attempt %d (last reason=%d, next in %llds)",
                             s_wifi_tries, s_wifi_reason, (long long)(wait / 1000));
                    esp_wifi_connect();
                }
            }
        }

        if (t - last_pet >= PET_TICK_MS) {
            last_pet = t;
            pet_tick();
        }
        /* NVS commits erase flash, which stalls a BLE controller running
         * from flash. The dirty flag stays set, so the write lands on the
         * first pass after teardown rather than being dropped. */
        if (s_pet_dirty && !ble_prov_nvs_blocked() && (t - last_pet_save) >= 60000) {
            last_pet_save = t;
            pet_save();
        }

        if (!s_doze && t - last_imu >= 100) {     /* rotation is meaningless dozing */
            last_imu = t;
            imu_poll();
            pomo_poll(t);                         /* needs fresh accelerometer */
        }

        if (!s_rtc_written && s_time_synced) {
            rtc_store();
        }

        if (t - last_tele >= TELEMETRY_PERIOD) {
            last_tele = t;
            telemetry_row("tick");
        }

        if (t - last_batt >= (s_doze ? 30000 : BATTERY_PERIOD_MS)) {
            last_batt = t;
            battery_poll();
            if (s_batt_mv > 0 && !s_batt_t_first) {
                s_batt_mv_first = s_batt_mv;
                s_batt_t_first = now_ms();
            }
        }

        if (t - last_stats >= STATS_PERIOD_MS) {
            uint32_t rc = s_refr_count;
            uint32_t frames = rc - last_refr;
            last_refr = rc;
            s_last_fps_x10 = (uint32_t)((uint64_t)frames * 10000 / (uint64_t)(t - last_stats));
            last_stats = t;

            char clock[16] = "--:--:--";
            {
                time_t tnow; struct tm tinfo;
                time(&tnow); localtime_r(&tnow, &tinfo);
                if (tinfo.tm_year >= (2024 - 1900)) {
                    strftime(clock, sizeof(clock), "%H:%M:%S", &tinfo);
                }
            }
            ESP_LOGI(TAG, "uptime=%llds clock=%s scr=%d idle=%lu/%llds wifi=%s screen=%s batt=%d%% %dmV%s fps=%u.%u "
                          "rot=%d sd=%lu pet[%d/%d/%d]%s",
                     (long long)(t / 1000), clock, (int)s_app,
                     (unsigned long)lv_display_get_inactive_time(NULL),
                     (long long)((t - s_last_btn) / 1000),
                     s_wifi_up ? "up" : "down",
                     s_screen_on ? "on" : "off",
                     (int)s_batt_pct, (int)s_batt_mv,
                     s_batt_charging ? " CHG" : "",
                     (unsigned)(s_last_fps_x10 / 10), (unsigned)(s_last_fps_x10 % 10),
                     s_rot * 90, (unsigned long)s_tele_rows, s_food, s_fun, s_nrg,
                     s_stack_fallback ? " STACK-FALLBACK!" : "");
            log_mem("periodic");
        }
    }
}
