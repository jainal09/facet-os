/*
 * Funnel-profile firmware — Waveshare ESP32-S3-Touch-AMOLED-2.16
 *
 * No VPN stack. Wi-Fi STA + periodic HTTPS request (bearer token) to the STT
 * endpoint — TLS cost is transient per call instead of continuous.
 *
 * Screens
 *   HOME   : battery bar, STT/wifi status, spinning arc, rolling event log
 *   SETUP  : Wi-Fi scan list -> LVGL on-screen keyboard, creds saved to NVS
 *   PET    : a small virtual pet, interactive through the side keys
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
enum { APP_CONTROL = 0, APP_POMO, APP_PET, APP_WIFI, APP_COUNT };
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
static lv_obj_t *s_setup_title;
static lv_obj_t *s_ap_list;
static lv_obj_t *s_rescan_btn;
static lv_obj_t *s_pw_panel;
static lv_obj_t *s_pw_ssid_label;
static lv_obj_t *s_pw_ta;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_wifi_hdr;        /* "connected to X" banner above the list */
static lv_obj_t *s_conn_panel;      /* details + disconnect for the joined AP */
static lv_obj_t *s_conn_ssid;
static lv_obj_t *s_conn_ip;
static lv_obj_t *s_conn_btn;
static lv_obj_t *s_conn_btn_label;

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
static char s_http_status[40] = "starting";

/* pet state (persisted) */
static int s_food = 80, s_fun = 75, s_nrg = 90;
static uint32_t s_pet_age_min;
static volatile bool s_pet_dirty;

/* Wi-Fi credentials in use, and a pending change from the setup screen */
static char s_ssid[33];
static char s_pass[65];
static char s_new_ssid[33];
static char s_new_pass[65];
static volatile bool s_req_scan;
static volatile bool s_req_apply;
static volatile bool s_req_wifi_off;   /* user tapped DISCONNECT  */
static volatile bool s_req_wifi_on;    /* user tapped RECONNECT   */
/* Set only by an explicit disconnect. Auto-reconnect must respect it, or the
 * station is back on the network a second after the user asked it not to be. */
static bool s_wifi_disabled;
static char s_ip[16];
static volatile bool s_req_http;
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
static lv_obj_t *s_lock_time, *s_lock_date, *s_lock_batt;
static lv_obj_t *s_lock_sweep, *s_lock_batt_arc;
static int s_sweep_deg;

/* App state store + power state — defined further down, used from above */
static void store_init_dirs(void);
static bool store_save(const char *id, const void *data, size_t len);
static bool store_load(const char *id, void *data, size_t len);

static bool s_sd_ok;
static uint32_t s_tele_rows;
static bool s_doze;
static char s_wall_credit[48];      /* Unsplash photographer, shown in CONTROL */

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
static void power_set_doze(bool doze);
static int  battery_drain_mv_h(void);

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
        "MEM[%s] internal_free=%u internal_min=%u internal_largest_block=%u psram_free=%u",
        tag2,
        (unsigned)esp_get_free_internal_heap_size(),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
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

static void creds_load(void) {
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
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        s_wifi_up = false;
        s_ip[0] = '\0';
        xEventGroupClearBits(s_evt, WIFI_CONNECTED_BIT);
        /* a scan drops the association on purpose — don't fight it; and never
         * undo an explicit disconnect */
        if (s_app != APP_WIFI && !s_wifi_disabled) {
            ESP_LOGW(TAG, "WiFi disconnected (reason=%d), retrying...", d->reason);
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "WiFi got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_wifi_up = true;
        log_event("wifi " IPSTR, IP2STR(&e->ip_info.ip));
        if (!s_time_synced) s_req_sntp = true;
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

static void wifi_init(void) {
    s_evt = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    wifi_apply_config(s_ssid, s_pass);
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi connecting to \"%s\"...", s_ssid);
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

static void https_task(void *arg) {
    while (1) {
        xEventGroupWaitBits(s_evt, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        int64_t t0 = esp_timer_get_time();
        uint32_t heap_before = esp_get_free_internal_heap_size();

        esp_http_client_config_t cfg = {
            .url = STT_ENDPOINT_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 15000,
            .method = HTTP_METHOD_GET,
        };
        esp_http_client_handle_t client = esp_http_client_init(&cfg);
        if (client) {
            if (STT_BEARER_TOKEN[0] != '\0') {
                char auth[160];
                snprintf(auth, sizeof(auth), "Bearer %s", STT_BEARER_TOKEN);
                esp_http_client_set_header(client, "Authorization", auth);
            }
            esp_err_t err = esp_http_client_perform(client);
            int code = esp_http_client_get_status_code(client);
            int64_t dur_ms = (esp_timer_get_time() - t0) / 1000;

            if (err == ESP_OK) {
                snprintf(s_http_status, sizeof(s_http_status),
                         "HTTP %d  %lldms", code, (long long)dur_ms);
                log_event("STT %d in %lldms", code, (long long)dur_ms);
            } else {
                snprintf(s_http_status, sizeof(s_http_status), "err %s", esp_err_to_name(err));
                log_event("STT err %s", esp_err_to_name(err));
            }
            ESP_LOGI(TAG, "STT: %s | heap before=%u after=%u min_ever=%u",
                     s_http_status, (unsigned)heap_before,
                     (unsigned)esp_get_free_internal_heap_size(),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
            esp_http_client_cleanup(client);
        }
        /* Wallpapers ride along on this task rather than the main loop. It
         * already owns an 8 KB stack and a TLS path, so the pool costs no
         * additional internal SRAM — and a slow download can no longer stall
         * button handling or the app switcher, which it did when the fetch ran
         * inline. */
        wall_service();

        /* A wallpaper request breaks the wait too, so pressing Fetch acts now
         * instead of sitting there until the next cycle — up to 45 s awake, and
         * ten minutes dozing, which read as "stuck". */
        int period = s_doze ? (10 * 60 * 1000) : HTTPS_PERIOD_MS;
        for (int w = 0; w < period / 100 && !s_req_http && !s_req_wallpaper; w++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        s_req_http = false;
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

static void imu_init(void) {
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
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "rotcfg", s_rot_cfg);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void rot_off_load(void) {
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "rotcfg", &v) == ESP_OK) s_rot_cfg = (int)(v & 7);
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
     * the commit, not the poll, so s_base_rot stays live for it to read. */
    if (s_rot_votes >= QMI_VOTES_NEEDED && s_rot_cand != s_rot && s_app != APP_POMO) {
        rotation_apply(s_rot_cand);
    }
}

/* ---------------- GPIO keys with short/long press ---------------- */

typedef enum { BTN_NONE = 0, BTN_SHORT, BTN_LONG } btn_ev_t;

typedef struct {
    gpio_num_t pin;
    int stable, last_raw;
    int64_t change_ms, press_ms;
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
        } else if (!b->long_fired) {                      /* released, short */
            return BTN_SHORT;
        }
        return BTN_NONE;
    }
    /* fire the long press while still held, so it feels immediate */
    if (b->stable == 0 && !b->long_fired && (t - b->press_ms) >= LONG_PRESS_MS) {
        b->long_fired = true;
        return BTN_LONG;
    }
    return BTN_NONE;
}

/* ---------------- screen switching ---------------- */

static void screen_toggle_power(void) {
    s_screen_on = !s_screen_on;
    if (s_screen_on) {
        power_set_doze(false);
        bsp_display_backlight_on();
    } else {
        bsp_display_backlight_off();
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
static void gesture_home_cb(lv_event_t *e) {
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM) {
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
    uint32_t before = esp_get_free_internal_heap_size();
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
             (unsigned)before, (unsigned)esp_get_free_internal_heap_size(),
             (int)(before - esp_get_free_internal_heap_size()));
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
        ESP_LOGW(TAG, "sfx task (PSRAM stack) failed — falling back to internal");
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
static lv_obj_t *s_cfg_vol_val;
static lv_obj_t *s_cfg_batt_bar, *s_cfg_batt_val, *s_cfg_batt_sub;
static lv_obj_t *s_cfg_net_val;
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
    lv_obj_set_style_pad_all(card, 14, 0);
    lv_obj_set_style_pad_row(card, 7, 0);
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
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, "");
    return l;
}

static lv_obj_t *cfg_button(lv_obj_t *card, const char *text, uint32_t accent,
                            lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(card);
    lv_obj_set_size(b, lv_pct(100), 44);
    lv_obj_set_style_radius(b, 12, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1B2432), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(accent), 0);
    lv_obj_set_style_border_opa(b, 150, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(accent), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return b;
}

static void cfg_fetch_cb(lv_event_t *e) {
    s_req_wallpaper = true;          /* the network task breaks its wait on this */
}

static void cfg_rotate_cb(lv_event_t *e) {
    rotation_bump();
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
                          "orientation  %d deg\ncalibration  %d / %d  %s\naccel  %d  %d  %d",
                          s_rot * 90, s_rot_cfg + 1, ROT_CFG_COUNT,
                          (s_rot_cfg & 4) ? "reversed" : "normal",
                          s_acc_x / 100, s_acc_y / 100, s_acc_z / 100);

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

    /* ---- network ---- */
    wifi_ap_record_t ap;
    int rssi = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
    lv_label_set_text_fmt(s_cfg_net_val,
                          "%s%s\nip  %s\nsignal  %d dBm\nendpoint  %s",
                          s_wifi_up ? LV_SYMBOL_OK "  " : "",
                          s_wifi_up ? s_ssid : (s_wifi_disabled ? "wi-fi off" : "offline"),
                          s_ip[0] ? s_ip : "-",
                          rssi, s_http_status);

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
                          (unsigned)(esp_get_free_internal_heap_size() / 1024),
                          (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
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
    cfg_button(c, LV_SYMBOL_REFRESH "  Step rotation calibration",
               CFG_ACCENT_DISP, cfg_rotate_cb);

    /* ---- audio ---- */
    c = cfg_card(col, "AUDIO", CFG_ACCENT_SND);
    s_cfg_vol_val = cfg_text(c, 0xC7D2E0);
    /* fixed height: a growing label here would resize the card and shift the
     * slider under the finger mid-drag */
    lv_obj_set_height(s_cfg_vol_val, 20);
    lv_label_set_text_fmt(s_cfg_vol_val, "volume  %d%%   /   %+d dB",
                          s_vol, (int)vol_db(s_vol));

    lv_obj_t *vs = lv_slider_create(c);
    lv_obj_set_size(vs, lv_pct(100), 16);
    lv_slider_set_range(vs, 0, 100);
    lv_slider_set_value(vs, s_vol, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(vs, lv_color_hex(0x1E293B), LV_PART_MAIN);
    lv_obj_set_style_bg_color(vs, lv_color_hex(CFG_ACCENT_SND), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(vs, lv_color_hex(0xFFD9A8), LV_PART_KNOB);
    lv_obj_add_event_cb(vs, cfg_vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(vs, cfg_vol_cb, LV_EVENT_RELEASED, NULL);

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

/* ---------------- setup screen ---------------- */

/* true when this SSID is the one the station is actually associated with */
static bool wifi_is_current(const char *ssid) {
    return s_wifi_up && s_ssid[0] && strcmp(ssid, s_ssid) == 0;
}

/* The banner above the list. Which network you are on is the first thing this
 * screen should answer, and it used to not answer it at all. */
static void wifi_hdr_refresh(void) {
    if (!s_wifi_hdr) return;
    if (s_wifi_up) {
        lv_obj_set_style_text_color(s_wifi_hdr, lv_color_hex(0x35C759), 0);
        lv_label_set_text_fmt(s_wifi_hdr, LV_SYMBOL_OK " %.24s", s_ssid);
    } else if (s_wifi_disabled) {
        lv_obj_set_style_text_color(s_wifi_hdr, lv_color_hex(0xFF453A), 0);
        lv_label_set_text(s_wifi_hdr, "Wi-Fi off");
    } else {
        lv_obj_set_style_text_color(s_wifi_hdr, lv_color_hex(0x8A8AA0), 0);
        lv_label_set_text(s_wifi_hdr, "Not connected");
    }
}

static void show_list_mode(void) {
    lv_label_set_text(s_setup_title, "Select Wi-Fi");
    wifi_hdr_refresh();
    lv_obj_remove_flag(s_wifi_hdr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_conn_panel, LV_OBJ_FLAG_HIDDEN);
}

static void show_password_mode(const char *ssid) {
    lv_label_set_text(s_setup_title, "Password");
    lv_label_set_text(s_pw_ssid_label, ssid);
    lv_textarea_set_text(s_pw_ta, "");
    lv_obj_add_flag(s_wifi_hdr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_conn_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(s_keyboard, s_pw_ta);
}

/* Tapping the network you are already on has no business opening a keyboard —
 * you are not entering a password for it. Show what it is and offer the one
 * action that makes sense. */
static void show_conn_mode(const char *ssid) {
    lv_label_set_text(s_setup_title, "Connected");
    lv_label_set_text_fmt(s_conn_ssid, "%.28s", ssid);
    lv_label_set_text_fmt(s_conn_ip, "%s", s_ip[0] ? s_ip : "no address");

    bool on = s_wifi_up;
    lv_label_set_text(s_conn_btn_label, on ? LV_SYMBOL_CLOSE "  Disconnect"
                                           : LV_SYMBOL_WIFI  "  Reconnect");
    lv_obj_set_style_bg_color(s_conn_btn,
        lv_color_hex(on ? 0x7A1F26 : 0x1F5A46), 0);

    lv_obj_add_flag(s_wifi_hdr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ap_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_rescan_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_conn_panel, LV_OBJ_FLAG_HIDDEN);
}

static void conn_btn_cb(lv_event_t *e) {
    /* deferred to the main task, like every other side effect here */
    if (s_wifi_up) s_req_wifi_off = true;
    else           s_req_wifi_on  = true;
    show_list_mode();
}

static void ap_click_cb(lv_event_t *e) {
    lv_obj_t *btn = lv_event_get_target(e);
    int idx = (int)(intptr_t)lv_obj_get_user_data(btn);
    if (idx < 0 || idx >= s_ap_count) return;

    if (wifi_is_current(s_aps[idx].ssid)) {
        show_conn_mode(s_aps[idx].ssid);
        return;
    }

    snprintf(s_new_ssid, sizeof(s_new_ssid), "%s", s_aps[idx].ssid);
    if (!s_aps[idx].secure) {
        s_new_pass[0] = '\0';
        s_req_apply = true;
        app_request(APP_DRAWER);
        return;
    }
    show_password_mode(s_new_ssid);
}

static void kb_event_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {
        snprintf(s_new_pass, sizeof(s_new_pass), "%s", lv_textarea_get_text(s_pw_ta));
        s_req_apply = true;
        app_request(APP_DRAWER);
    } else if (code == LV_EVENT_CANCEL) {
        show_list_mode();
    }
}

/* The disconnect is handled by the main task, so the UI only learns it worked a
 * moment later. Poll so the banner and the button follow reality. */
static void wifi_timer_cb(lv_timer_t *t) {
    if (!s_wifi_hdr || !s_conn_btn_label) return;
    if (!lv_obj_has_flag(s_wifi_hdr, LV_OBJ_FLAG_HIDDEN)) {
        wifi_hdr_refresh();
    } else if (!lv_obj_has_flag(s_conn_panel, LV_OBJ_FLAG_HIDDEN)) {
        bool on = s_wifi_up;
        lv_label_set_text(s_conn_btn_label, on ? LV_SYMBOL_CLOSE "  Disconnect"
                                               : LV_SYMBOL_WIFI  "  Reconnect");
        lv_obj_set_style_bg_color(s_conn_btn,
            lv_color_hex(on ? 0x7A1F26 : 0x1F5A46), 0);
        lv_label_set_text(s_conn_ip, s_ip[0] ? s_ip : "no address");
    }
}

static void rescan_cb(lv_event_t *e) {
    lv_obj_clean(s_ap_list);
    lv_label_set_text(s_setup_title, "Scanning...");
    s_req_scan = true;
}

static void build_wifi_app(lv_obj_t *scr) {
    s_scr_setup = scr;
    lv_obj_set_style_bg_color(s_scr_setup, lv_color_hex(0x0B0B14), 0);
    lv_obj_remove_flag(s_scr_setup, LV_OBJ_FLAG_SCROLLABLE);

    s_setup_title = lv_label_create(s_scr_setup);
    lv_obj_set_style_text_color(s_setup_title, lv_color_hex(0xE6E6EE), 0);
    lv_label_set_text(s_setup_title, "Scanning...");
    lv_obj_align(s_setup_title, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 4);

    s_wifi_hdr = lv_label_create(s_scr_setup);
    lv_obj_set_width(s_wifi_hdr, CONTENT_W);
    lv_obj_set_style_text_align(s_wifi_hdr, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_wifi_hdr, LV_LABEL_LONG_DOT);
    lv_obj_align(s_wifi_hdr, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 34);
    lv_label_set_text(s_wifi_hdr, "");

    s_ap_list = lv_list_create(s_scr_setup);
    lv_obj_set_size(s_ap_list, CONTENT_W, 264);
    lv_obj_align(s_ap_list, LV_ALIGN_TOP_MID, 0, 96);
    lv_obj_set_style_bg_color(s_ap_list, lv_color_hex(0x14141F), 0);
    lv_obj_set_style_border_width(s_ap_list, 0, 0);
    lv_obj_set_style_radius(s_ap_list, 16, 0);
    lv_obj_set_style_pad_all(s_ap_list, 6, 0);

    s_rescan_btn = lv_button_create(s_scr_setup);
    lv_obj_set_size(s_rescan_btn, 150, 42);
    lv_obj_align(s_rescan_btn, LV_ALIGN_BOTTOM_MID, 0, -BOTTOM_MARGIN);
    lv_obj_set_style_bg_color(s_rescan_btn, lv_color_hex(0x24243A), 0);
    lv_obj_add_event_cb(s_rescan_btn, rescan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rl = lv_label_create(s_rescan_btn);
    lv_label_set_text(rl, LV_SYMBOL_REFRESH " Rescan");
    lv_obj_center(rl);

    s_pw_panel = lv_obj_create(s_scr_setup);
    lv_obj_remove_style_all(s_pw_panel);
    lv_obj_set_size(s_pw_panel, CONTENT_W, 110);
    lv_obj_align(s_pw_panel, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_remove_flag(s_pw_panel, LV_OBJ_FLAG_SCROLLABLE);

    s_pw_ssid_label = lv_label_create(s_pw_panel);
    lv_obj_set_width(s_pw_ssid_label, CONTENT_W);
    lv_obj_set_style_text_align(s_pw_ssid_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_pw_ssid_label, lv_color_hex(0x8A8AA0), 0);
    lv_label_set_long_mode(s_pw_ssid_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_pw_ssid_label, "");
    lv_obj_align(s_pw_ssid_label, LV_ALIGN_TOP_MID, 0, 0);

    s_pw_ta = lv_textarea_create(s_pw_panel);
    lv_obj_set_size(s_pw_ta, 320, 48);
    lv_obj_align(s_pw_ta, LV_ALIGN_TOP_MID, 0, 30);
    lv_textarea_set_one_line(s_pw_ta, true);
    lv_textarea_set_password_mode(s_pw_ta, true);
    lv_textarea_set_placeholder_text(s_pw_ta, "password");
    lv_obj_set_style_bg_color(s_pw_ta, lv_color_hex(0x14141F), 0);
    lv_obj_set_style_border_color(s_pw_ta, lv_color_hex(0x38B2AC), LV_PART_MAIN);

    s_conn_panel = lv_obj_create(s_scr_setup);
    lv_obj_remove_style_all(s_conn_panel);
    lv_obj_set_size(s_conn_panel, CONTENT_W, 250);
    lv_obj_align(s_conn_panel, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_remove_flag(s_conn_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_conn_panel, lv_color_hex(0x14141F), 0);
    lv_obj_set_style_bg_opa(s_conn_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_conn_panel, 16, 0);
    lv_obj_set_style_pad_all(s_conn_panel, 14, 0);

    lv_obj_t *ct = lv_label_create(s_conn_panel);
    lv_obj_set_style_text_color(ct, lv_color_hex(0x35C759), 0);
    lv_label_set_text(ct, LV_SYMBOL_OK "  CONNECTED TO");
    lv_obj_align(ct, LV_ALIGN_TOP_MID, 0, 4);

    s_conn_ssid = lv_label_create(s_conn_panel);
    lv_obj_set_width(s_conn_ssid, CONTENT_W - 36);
    lv_obj_set_style_text_align(s_conn_ssid, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_conn_ssid, lv_color_hex(0xE6E6EE), 0);
    lv_obj_set_style_text_font(s_conn_ssid, &lv_font_montserrat_20, 0);
    lv_label_set_long_mode(s_conn_ssid, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_conn_ssid, "");
    lv_obj_align(s_conn_ssid, LV_ALIGN_TOP_MID, 0, 36);

    s_conn_ip = lv_label_create(s_conn_panel);
    lv_obj_set_width(s_conn_ip, CONTENT_W - 36);
    lv_obj_set_style_text_align(s_conn_ip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_conn_ip, lv_color_hex(0x8A8AA0), 0);
    lv_label_set_text(s_conn_ip, "");
    lv_obj_align(s_conn_ip, LV_ALIGN_TOP_MID, 0, 74);

    s_conn_btn = lv_button_create(s_conn_panel);
    lv_obj_set_size(s_conn_btn, 210, 50);
    lv_obj_align(s_conn_btn, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_radius(s_conn_btn, 14, 0);
    lv_obj_add_event_cb(s_conn_btn, conn_btn_cb, LV_EVENT_CLICKED, NULL);
    s_conn_btn_label = lv_label_create(s_conn_btn);
    lv_obj_center(s_conn_btn_label);
    lv_label_set_text(s_conn_btn_label, "");

    s_keyboard = lv_keyboard_create(s_scr_setup);
    lv_obj_set_size(s_keyboard, KB_W, KB_H);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, -BOTTOM_MARGIN);
    lv_obj_set_style_bg_color(s_keyboard, lv_color_hex(0x12121C), 0);
    lv_obj_set_style_radius(s_keyboard, 14, 0);
    lv_obj_set_style_pad_all(s_keyboard, 4, 0);
    lv_keyboard_set_textarea(s_keyboard, s_pw_ta);
    lv_obj_add_event_cb(s_keyboard, kb_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(s_keyboard, kb_event_cb, LV_EVENT_CANCEL, NULL);
    lv_obj_add_event_cb(s_scr_setup, gesture_home_cb, LV_EVENT_GESTURE, NULL);

    show_list_mode();
    s_app_timer = lv_timer_create(wifi_timer_cb, 500, NULL);
}

/* caller holds the LVGL lock */
static void setup_fill_list(void) {
    lv_obj_clean(s_ap_list);
    if (s_ap_count == 0) {
        lv_label_set_text(s_setup_title, "No networks");
        return;
    }
    lv_label_set_text(s_setup_title, "Select Wi-Fi");
    wifi_hdr_refresh();

    /* The joined network goes first and is marked, so it is obvious at a glance
     * which one you are on without reading signal strengths. */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < s_ap_count; i++) {
            bool cur = wifi_is_current(s_aps[i].ssid);
            if (cur != (pass == 0)) continue;

            char row[64];
            if (cur) {
                snprintf(row, sizeof(row), "%.32s  connected", s_aps[i].ssid);
            } else {
                snprintf(row, sizeof(row), "%.32s  %ddBm%s", s_aps[i].ssid,
                         s_aps[i].rssi, s_aps[i].secure ? "" : "  open");
            }
            lv_obj_t *btn = lv_list_add_button(s_ap_list,
                                               cur ? LV_SYMBOL_OK : LV_SYMBOL_WIFI,
                                               row);
            lv_obj_set_user_data(btn, (void *)(intptr_t)i);
            lv_obj_add_event_cb(btn, ap_click_cb, LV_EVENT_CLICKED, NULL);
            lv_obj_set_style_bg_color(btn, lv_color_hex(cur ? 0x14332A : 0x1C1C2B), 0);
            lv_obj_set_style_text_color(btn,
                lv_color_hex(cur ? 0x35C759 : 0xD8D8E4), 0);
        }
    }
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
#define POMO_DIM_PCT     12
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
static void pomo_tap_cb(lv_event_t *e) {
    if (s_pomo_state != POMO_IDLE) return;
    pomo_begin(pomo_top_edge(), true);
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
            bsp_display_brightness_set(100);
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
            if (flat && s_pomo_state == POMO_RUN) {
                s_pomo_state = POMO_PAUSE;
                sfx_play(SFX_PAUSE);
                ESP_LOGI(TAG, "pomodoro: paused (laid flat)");
            } else if (!flat && s_pomo_state == POMO_PAUSE) {
                s_pomo_state = POMO_RUN;
                s_pomo_tick_ms = t;
                sfx_play(SFX_RESUME);
                ESP_LOGI(TAG, "pomodoro: resumed");
            }
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
        bsp_display_brightness_set(want_dim ? POMO_DIM_PCT : 100);
        ESP_LOGI(TAG, "pomodoro: %s", want_dim ? "dimmed" : "full brightness");
    }
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
    [APP_POMO]   = { "FOCUS",  "pomo",   ICON_TARGET,    0xFFB454, build_pomo_app,   pomo_save },
    [APP_WIFI]   = { "WI-FI",  "wifi",   ICON_WIFI,      0x34D399, build_wifi_app,   NULL     },
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
        }
    } else if (e->event_id == HTTP_EVENT_ON_DATA && s_dl_file && e->data_len > 0) {
        fwrite(e->data, 1, e->data_len, s_dl_file);
        s_dl_got += e->data_len;
        s_dl_kb = (int)(s_dl_got / 1024);
        if (s_dl_total > 0) {
            int pct = (int)((s_dl_got * 100) / s_dl_total);
            s_dl_pct = pct > 100 ? 100 : pct;
        }
    }
    return ESP_OK;
}

static bool asset_fetch(const char *url, const char *path) {
    if (!s_sd_ok) return false;

    s_dl_total = 0;
    s_dl_got = 0;
    s_dl_pct = 0;
    s_dl_kb = 0;

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
        esp_err_t err = esp_http_client_perform(c);
        int status = esp_http_client_get_status_code(c);
        ok = (err == ESP_OK && status == 200);
        ESP_LOGI(TAG, "asset: HTTP %d, heap %u", status,
                 (unsigned)esp_get_free_internal_heap_size());
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

#define TELEMETRY_PATH   BSP_SD_MOUNT_POINT "/logs/pwrlog.csv"
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
        fprintf(f, "clock,uptime_s,event,batt_mv,batt_pct,charging,doze,screen,"
                   "wifi,app,heap_free,heap_min,fps\n");
    }

    char clock[16] = "";
    time_t tnow;
    struct tm ti;
    time(&tnow);
    localtime_r(&tnow, &ti);
    if (ti.tm_year >= (2024 - 1900)) strftime(clock, sizeof(clock), "%H:%M:%S", &ti);

    fprintf(f, "%s,%lld,%s,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u.%u\n",
            clock, (long long)(now_ms() / 1000), event,
            s_batt_mv, s_batt_pct, s_batt_charging ? 1 : 0,
            s_doze ? 1 : 0, s_screen_on ? 1 : 0, s_wifi_up ? 1 : 0, s_app,
            (unsigned)esp_get_free_internal_heap_size(),
            (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
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
static void lock_refresh(void) {
    if (!s_lock_time) return;

    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    if (ti.tm_year < (2024 - 1900)) {
        lv_label_set_text(s_lock_time, "--:--");
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
    app_request(APP_DRAWER);      /* always home, not "wherever you locked from" */
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

    lv_obj_t *rule = lv_obj_create(scr);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, 190, 1);
    lv_obj_set_style_bg_color(rule, lv_color_hex(0x1B4E68), 0);
    lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
    lv_obj_align(rule, LV_ALIGN_CENTER, 0, 26);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);

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
    if (s_app != APP_WIFI) return false;
    bool on_pw   = s_pw_panel   && !lv_obj_has_flag(s_pw_panel, LV_OBJ_FLAG_HIDDEN);
    bool on_conn = s_conn_panel && !lv_obj_has_flag(s_conn_panel, LV_OBJ_FLAG_HIDDEN);
    if (!on_pw && !on_conn) return false;
    if (ui_lock()) {
        show_list_mode();          /* password or details -> network list */
        bsp_display_unlock();
    }
    return true;
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
    case APP_WIFI:
        if (ui_lock()) {
            if (s_ap_list)     lv_obj_clean(s_ap_list);
            if (s_setup_title) lv_label_set_text(s_setup_title, "Scanning...");
            bsp_display_unlock();
        }
        s_req_scan = true;
        break;
    case APP_CONTROL:
        s_req_wallpaper = true;      /* the card shows real progress for this */
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

    if (!ui_lock()) return;

    /* let the outgoing app flush its state before its widgets disappear */
    if (s_app >= 0 && s_app < APP_COUNT && s_apps[s_app].save) {
        s_apps[s_app].save();
    }
    if (s_app_timer) { lv_timer_delete(s_app_timer); s_app_timer = NULL; }
    s_scr_home = s_scr_setup = s_scr_pet = NULL;
    s_status_label = NULL; s_batt_bar = NULL; s_batt_label = NULL;
    s_bolt_label = NULL; s_fps_label = NULL; s_events_label = NULL;
    s_ap_list = NULL; s_keyboard = NULL; s_pw_ta = NULL;
    s_pw_panel = NULL; s_setup_title = NULL;
    s_wifi_hdr = NULL; s_conn_panel = NULL; s_conn_ssid = NULL;
    s_conn_ip = NULL; s_conn_btn = NULL; s_conn_btn_label = NULL;
    s_ch_wrap = NULL;
    s_cfg_wall_pool = NULL; s_cfg_wall_state = NULL;
    s_cfg_wall_bar = NULL; s_cfg_wall_sub = NULL;
    s_cfg_rot_val = NULL; s_cfg_vol_val = NULL;
    s_cfg_batt_bar = NULL;
    s_cfg_batt_val = NULL; s_cfg_batt_sub = NULL;
    s_cfg_net_val = NULL; s_cfg_sys_val = NULL; s_cfg_log = NULL;
    s_pomo_clock = NULL; s_pomo_word = NULL;
    s_pomo_ring = NULL; s_pomo_arc = NULL; s_pomo_fill = NULL;
    for (int i = 0; i < POMO_SLOTS; i++) s_pomo_dial[i] = NULL;
    s_lock_time = NULL; s_lock_date = NULL; s_lock_batt = NULL;
    s_lock_sweep = NULL; s_lock_batt_arc = NULL;

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
             (unsigned)esp_get_free_internal_heap_size());

    if (idx == APP_WIFI) s_req_scan = true;     /* scan as soon as it opens */
}

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
    pet_load();
    pomo_load();

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
        bsp_display_backlight_on();
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
    rotation_apply(0);   /* native: aligns the driver's state, no visual change */
    btn_init(&s_key_left);
    btn_init(&s_key_right);

    telemetry_row("boot");

    vol_load();
    sfx_init();
    xTaskCreate(https_task, "https", 8192, NULL, 5, NULL);

    int64_t last_stats = now_ms();
    int64_t last_batt = 0, last_imu = 0, last_pet = now_ms(), last_pet_save = now_ms();
    int64_t last_tele = now_ms(), last_rejoin = now_ms();
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

        if (kleft == BTN_SHORT || kleft == BTN_LONG) {
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
            } else if (s_screen_on && idle > AUTO_LOCK_MS && s_req_app != APP_LOCK) {
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

        if (s_req_scan) {
            s_req_scan = false;
            wifi_scan_now();
            if (ui_lock()) {
                if (s_ap_list) setup_fill_list();
                bsp_display_unlock();
            }
        }

        if (s_req_apply) {
            s_req_apply = false;
            s_wifi_disabled = false;       /* joining is an explicit "on" */
            snprintf(s_ssid, sizeof(s_ssid), "%s", s_new_ssid);
            snprintf(s_pass, sizeof(s_pass), "%s", s_new_pass);
            creds_save(s_ssid, s_pass);
            ESP_LOGI(TAG, "applying new credentials for \"%s\"", s_ssid);
            log_event("join %s", s_ssid);
            esp_wifi_disconnect();
            wifi_apply_config(s_ssid, s_pass);
            esp_wifi_connect();
        }

        if (s_req_vol_save) {
            s_req_vol_save = false;
            vol_save();
        }

        if (s_req_wifi_off) {
            s_req_wifi_off = false;
            s_wifi_disabled = true;
            ESP_LOGI(TAG, "wifi disconnect requested");
            log_event("wifi off");
            esp_wifi_disconnect();
        }
        if (s_req_wifi_on) {
            s_req_wifi_on = false;
            s_wifi_disabled = false;
            ESP_LOGI(TAG, "wifi reconnect requested");
            log_event("wifi on");
            esp_wifi_connect();
        }

        /* Keep-alive for the association. The event handler deliberately skips
         * its retry while the WI-FI app is open (a scan drops the link on
         * purpose), which left no one to reconnect after leaving the app. A
         * periodic nudge is more robust than relying on the event alone. */
        if (!s_wifi_up && !s_wifi_disabled && s_app != APP_WIFI &&
            t - last_rejoin >= 10000) {
            last_rejoin = t;
            esp_wifi_connect();
        }

        if (t - last_pet >= PET_TICK_MS) {
            last_pet = t;
            pet_tick();
        }
        if (s_pet_dirty && (t - last_pet_save) >= 60000) {
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
                          "rot=%d sd=%lu pet[%d/%d/%d] | STT %s",
                     (long long)(t / 1000), clock, (int)s_app,
                     (unsigned long)lv_display_get_inactive_time(NULL),
                     (long long)((t - s_last_btn) / 1000),
                     s_wifi_up ? "up" : "down",
                     s_screen_on ? "on" : "off",
                     (int)s_batt_pct, (int)s_batt_mv,
                     s_batt_charging ? " CHG" : "",
                     (unsigned)(s_last_fps_x10 / 10), (unsigned)(s_last_fps_x10 % 10),
                     s_rot * 90, (unsigned long)s_tele_rows, s_food, s_fun, s_nrg,
                     s_http_status);
            log_mem("periodic");
        }
    }
}
