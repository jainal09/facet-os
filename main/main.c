/*
 * Funnel-profile firmware — Waveshare ESP32-S3-Touch-AMOLED-2.16
 *
 * No VPN stack. Wi-Fi STA and ordinary outbound HTTPS, so TLS cost is transient
 * per call instead of a permanent set of task stacks.
 *
 * Apps are built on open and freed on close; see docs/ARCHITECTURE.md.
 *
 * Keys use one global contract: left (GPIO0) locks, middle (PWRKEY through the
 * AXP2101) goes home, and right (GPIO18) goes back / invokes the app action.
 * MUSIC deliberately consumes all three for volume. See ARCHITECTURE.md.
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
#include "esp_private/esp_clk.h"
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
#include "src/draw/lv_image_decoder_private.h"
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
#define PERF_PERIOD_MS    (3 * 1000)
#define BATTERY_PERIOD_MS (2 * 1000)
#define HTTPS_PERIOD_MS   (45 * 1000)

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
#define AXP_REG_STATUS1       0x00      /* b5 VBUS good, b4 BATFET, b3 battery present */
#define AXP_REG_STATUS2       0x01      /* b[6:5] 00 idle/01 charge/10 discharge, b[2:0] charge state */
#define AXP_REG_GAUGE_CTRL    0x18      /* bit3: fuel-gauge module enable */
#define AXP_REG_ADC_CH_CTRL   0x30      /* bit0: battery voltage ADC enable */
#define AXP_REG_ADC_DATA_H    0x34
#define AXP_REG_ADC_DATA_L    0x35
#define AXP_REG_INTEN2        0x41      /* bit3: PWRKEY short-press IRQ enable */
#define AXP_REG_INTSTS2       0x49      /* bit3: PWRKEY short press, write 1 to clear */
#define AXP_REG_CHG_ICC       0x62      /* bits[4:0]: constant-current charge step */
#define AXP_REG_CHG_CV        0x64      /* bits[2:0]: charge target (CV) voltage */
#define AXP_REG_BAT_DET_CTRL  0x68      /* bit0: battery detection enable */
#define AXP_REG_GAUGE_RESET   0x17      /* bit3: reset the fuel gauge (RWAC) */
#define AXP_REG_BAT_PERCENT   0xA4

/* Terminal voltage is not open-circuit voltage, and the gap is this cell's
 * internal resistance times the charge current. MEASURED on this board rather
 * than assumed: at a 400 mA charge target, unplugging dropped the reading from
 * 3768 mV to ~3678 mV at the same state of charge — 90 mV, or ten points on the
 * 900 mV scale, which is exactly the jump that made the percentage look random
 * every time the cable moved. Subtracting it while charging is what makes the
 * voltage fallback stop lying about a cell that has not changed. */
#define BATT_IR_CHG_MV        90
#define AXP_PKEY_SHORT_BIT    3
/* PWRKEY is not short-press-only. The PMU distinguishes four states, and the
 * two edge IRQs give the middle key a real press-down and release — they are
 * simply disabled at power-up (reg 0x41 default 0b for both), which is why it
 * looked like the completed press was all the hardware could report. */
#define AXP_PKEY_NEG_BIT      1      /* key went down */
#define AXP_PKEY_POS_BIT      0      /* key came up   */

/* Charge target voltage, reg 0x64[2:0]. The cell is never held above this, and
 * the PMU restarts charging on its own once it falls 100 mV below (VRECHG is
 * fixed at CV-100mV), so the whole charge-limit feature is these three codes —
 * no polling loop, no charge-inhibit bit. */
#define AXP_CV_4V00           0x01
#define AXP_CV_4V10           0x02
#define AXP_CV_4V20           0x03
#define AXP_CV_MASK           0x07

/* reg 0x01[2:0] charge state. Only "done" matters to us: it is the point where
 * the PMU stops charging and opens BATFET, which is the bypass we are after. */
#define AXP_CHG_DONE          0x04

static EventGroupHandle_t s_evt;
#define WIFI_CONNECTED_BIT BIT0

/* Apps are built when opened and destroyed when closed, so only the running
 * app costs RAM. That removes the widget ceiling entirely — the cost of a
 * switch is one rebuild, not a reboot. */
enum { APP_CONTROL = 0, APP_MUSIC, APP_POMO, APP_DAYS, APP_PET, APP_COUNT };
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

/* 0 when not associated. Worth having on the status line: a bulk transfer that
 * stalls for 12 s is a link symptom, and this board can sit on a range extender
 * where sustained downloads behave very differently from short API calls. */
static int wifi_rssi(void) {
    wifi_ap_record_t ap;
    return (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}

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

/* Short-window renderer telemetry.  The 15-second UI number above is useful
 * for a quiet status screen, but it hides frame-time spikes and used to count
 * refresh checks that drew nothing.  These counters are updated only on actual
 * LVGL render/flush events and reported in one aggregated log line, so the
 * profiler itself does not turn a smooth gesture into a stream of UART stalls. */
typedef struct {
    uint32_t redraws;
    uint32_t render_us_sum;
    uint32_t render_us_max;
    uint32_t interval_count;
    uint32_t interval_us_sum;
    uint32_t interval_us_max;
    uint32_t flushes;
    uint32_t pixels;
    uint32_t submit_us_sum;
    uint32_t submit_us_max;
    uint32_t wait_count;
    uint32_t wait_us_sum;
    uint32_t wait_us_max;
    uint16_t cpu_mhz_min;
    uint16_t cpu_mhz_max;
} render_perf_t;

static portMUX_TYPE s_perf_mux = portMUX_INITIALIZER_UNLOCKED;
static render_perf_t s_render_perf;
static int64_t s_perf_render_started_us;
static int64_t s_perf_last_render_us;
static int64_t s_perf_flush_started_us;
static int64_t s_perf_wait_started_us;

/* shared state */
static volatile int  s_batt_pct = -1;       /* -1 = no battery present */
static volatile int  s_batt_mv;
static volatile bool s_batt_charging;
static volatile bool s_wifi_up;
static volatile bool s_screen_on = true;

/* Battery care. A device that lives on a desk sits pinned at 4.2 V, which is
 * the worst thing you can do to a lithium cell — roughly 300-500 cycles there
 * against 1200-2000 at 4.0 V. Capping the charge target trades a little
 * runtime for that, the same bargain every laptop vendor now ships. */
typedef enum { CHG_FULL = 0, CHG_BALANCED = 1, CHG_LIFESPAN = 2 } chg_mode_t;
static volatile int  s_chg_mode = CHG_BALANCED;
static volatile bool s_chg_once;            /* one-shot: charge to full this once */
static volatile bool s_chg_once_seen;       /* current flowed since it was armed */
static volatile int  s_chg_once_idle;       /* polls at "done" without charging */
static volatile bool s_req_chg_save;

/* Read from the PMU status bytes battery_poll() already fetches. s_vbus is
 * what "plugged in" means now: once the cap engages the charger stops while
 * still on USB, so s_batt_charging alone would report a docked cube as running
 * on battery. */
static volatile bool    s_vbus;
static volatile bool    s_bypass;           /* on USB, charge finished, cell idle */
static volatile uint8_t s_chg_state;        /* reg 0x01[2:0] */

/* ---- pet: persisted life (the engine lives near pet_load, below) ----
 *
 * The pet follows the FOCUS pattern taken further: the ENGINE is file-scope
 * state ticked from the main loop on every screen, and survives power-off by
 * being anchored to wall-clock time; the pet APP is only a view onto it. The
 * broker is authoritative for what the owner DESIGNS (species, world, theme,
 * name, sleep window — s_pet.cfg_ver tracks what has been applied); the cube
 * is authoritative for the life actually lived (stage, meters, mistakes).
 *
 * Field order is largest-first so natural alignment needs no packing — a
 * packed struct would put int64s at odd offsets, and this blob is accessed as
 * ordinary struct members, not memcpy'd fields. */
typedef struct {
    int64_t  hatched_utc;        /* epoch seconds; RTC makes these trustworthy */
    int64_t  stage_utc;          /* when the current stage began */
    int64_t  tick_utc;           /* last minute credited — the offline anchor */
    int64_t  seen_utc;           /* last owner interaction — departure timer */
    int64_t  wx_utc;             /* weather fetch time; stale > 6 h = not drawn */
    uint32_t seed;               /* personality, rolled once at egg creation */
    uint32_t cfg_ver;            /* last broker config version applied */
    uint32_t stardust;           /* step/mini-game currency */
    uint32_t steps_today;
    uint32_t day_stamp;          /* civil day steps_today belongs to */
    uint16_t ver;                /* PET_BLOB_VER */
    uint16_t cue_left_s;         /* awake-seconds before the cue is a mistake */
    uint16_t mistakes;           /* lifetime care mistakes — pick the adult */
    uint16_t sleep_start;        /* minutes from local midnight */
    uint16_t sleep_end;
    uint16_t bday;               /* month<<8|day, 0 = unset */
    uint8_t  stage;              /* pet_stage_t */
    uint8_t  form;               /* branch within the stage */
    uint8_t  species, world, theme, hat;
    uint8_t  hunger, happy;      /* the two coupled meters, 0..100 */
    uint8_t  sick, poop;
    uint8_t  cue;                /* active attention cue, PET_CUE_NONE = 0 */
    uint8_t  snack_run;          /* consecutive snacks — the meal tradeoff */
    uint8_t  happy_hist;         /* EWMA of happy, 0..100 — picks the teen */
    uint8_t  wx;                 /* cached weather kind */
    uint8_t  flags;
    int8_t   wx_temp;
    char     name[12];           /* ASCII, broker-set, default "PIP" */
    /* v3 appends — append-only from here on, so a version bump is a prefix
     * read plus zeroed tail, never a field-by-field migration. The spare
     * bytes are the down payment on v4. */
    uint32_t odo_m;              /* lifetime metres walked (10 px = 1 m) */
    uint16_t laps;               /* full trips around the world */
    uint16_t best_alt;           /* highest jetpack flight, metres */
    uint8_t  spare[8];
} pet_blob2_t;

/* The v2 file is the struct without the appended tail: 100 payload bytes
 * padded to 104. Frozen by assert so an innocent field reorder cannot
 * silently orphan every saved pet. */
#define PET_BLOB_V2_SIZE 104
_Static_assert(offsetof(pet_blob2_t, odo_m) == 100, "v2 prefix moved");
_Static_assert(sizeof(pet_blob2_t) == 120, "blob layout changed — bump the version");

enum { PET_EGG = 0, PET_BABY, PET_CHILD, PET_TEEN, PET_ADULT, PET_AWAY };
enum { PET_CUE_NONE = 0, PET_CUE_HUNGRY, PET_CUE_LONELY };

static pet_blob2_t s_pet;
static volatile bool s_pet_dirty;
/* Sleep pressure, presentation-only: recomputable from the clock, so it is
 * deliberately not part of the persisted blob. Resets mid-range on boot. */
static int s_nrg = 75;

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
static lv_obj_t *s_sp_art, *s_sp_art_ph, *s_sp_spin;
static char s_sp_art_shown[160];  /* which art the widgets are showing */
static char s_sp_art_id[26];      /* the TRACK the loaded cover belongs to */
static uint8_t *s_sp_art_buf;            /* PSRAM, SP_ART_MAX */
static size_t   s_sp_art_len;            /* bytes of JPEG actually held */
static lv_image_dsc_t s_sp_art_dsc;      /* points into s_sp_art_buf + header */
static volatile int s_brk_status;        /* last broker_fetch HTTP status */
static bool broker_fetch(const char *path, const char *xname, const char *xval,
                         uint8_t *buf, size_t cap, size_t *out_len);

static lv_obj_t *s_lock_time, *s_lock_date, *s_lock_meridiem;
static lv_obj_t *s_lock_batt, *s_lock_charge, *s_lock_eta;
static lv_obj_t *s_lock_ring, *s_lock_inner_ring, *s_lock_batt_arc;
static lv_obj_t *s_lock_ao_ring;      /* the desk-clock cue, amber */
/* The wallpaper, kept as a file static only so the desk-clock dim can take it
 * off the glass. On this panel a hidden full-screen photo is not "the photo
 * drawn darker" — the 0x000000 screen behind it emits nothing at all, which is
 * the half of the dim that lowering 0x51 cannot buy. NULL on every screen but
 * the lock screen, and that is precisely what scopes the blackout to it. */
static lv_obj_t *s_lock_wall;
static uint8_t s_lock_charge_phase, s_lock_charge_div;

/* Now-playing panel on the lock screen. Hidden unless Spotify actually has an
 * active device, so the screen stays as sparse as it was whenever there is
 * nothing to say. s_lock_np_up is read by wall_service() on the network task: a
 * wallpaper download and an album-art download must not overlap, because the
 * wallpaper fetch is already the largest transient consumer of internal SRAM on
 * this board and the margin is thinner than it has ever been. */
static lv_obj_t *s_lock_np;
static lv_obj_t *s_lock_np_art, *s_lock_np_ph, *s_lock_np_track;
static lv_obj_t *s_lock_np_prev, *s_lock_np_play, *s_lock_np_next;
static lv_obj_t *s_lock_np_drag_img;
static lv_draw_buf_t *s_lock_np_snapshot;
static char s_lock_np_snapshot_track[26];
static uint8_t s_lock_np_snapshot_state;
static volatile bool s_lock_np_up;
static uint32_t s_lock_np_bg;    /* last scrim colour, 0 = unset */
static bool s_lock_np_off;       /* stays dismissed until MUSIC is opened */
static int64_t s_lock_swipe_at;  /* lock_tap_cb ignores a click in a swipe's shadow */
static bool s_lock_pointer_down, s_lock_dragging, s_lock_drag_active;
static bool s_lock_drag_moved;
static int16_t s_lock_drag_start_x, s_lock_drag_start_y;
static int16_t s_lock_drag_origin_x, s_lock_drag_x;
static lv_obj_t *s_lock_rule;    /* divider under the clock; moves with it */

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
/* Effective on-glass brightness the lock wallpaper is dimmed toward (0-255
 * mean). The dim keeps the clock legible over bright photos; a photo already
 * at or below this draws at full opacity — see build_lock_screen(). */
#define WALL_DIM_TARGET 55
#define WALL_DIR        BSP_SD_MOUNT_POINT "/assets"

static volatile bool s_req_wallpaper;
static uint16_t s_wall_have;         /* bitmask of slots holding a usable PNG */
static int      s_wall_slot = -1;    /* slot currently on screen, -1 = none */
static int      s_wall_primed = -1;  /* next slot already decoded in LVGL cache */
static volatile bool s_req_wall_prime;
static int64_t  s_wall_prime_after;
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

/* Defined with the CONTROL card that presents it, but the charge-ETA tracker
 * next to battery_poll() needs the same number to know what it is counting to.
 * One definition of "the limit is 85%", not two. */
static int  chg_mode_pct(int mode);

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

typedef struct {                     /* legacy v1 blob, read once to migrate */
    uint16_t ver;
    int16_t  food, fun, nrg;
    uint32_t age_min;
} pet_blob_v1_t;

#define PET_BLOB_VER 3

/* 1 for real life. 60 compresses the egg-to-adult arc into ~3.5 hours for a
 * bench soak: durations divide by it, decay rates multiply by it. Keep it 1
 * in commits, like the self-test defines. */
#define PET_TIME_SCALE 1

/* Nothing below trusts the clock until it reads as real. The RTC seeds it in
 * early boot, so this only bites on a cold board with a drained RTC cell —
 * where the honest answer is that no time passed that we can vouch for. */
#define PET_EPOCH_MIN 1750000000LL

typedef struct { uint32_t dur_min; uint8_t hunger_per_h, happy_per_h; } pet_stage_row_t;
static const pet_stage_row_t s_pet_stages[] = {
    [PET_EGG]   = { 30,          0, 0 },
    [PET_BABY]  = { 24 * 60,     6, 5 },
    [PET_CHILD] = { 2 * 24 * 60, 4, 4 },
    [PET_TEEN]  = { 3 * 24 * 60, 4, 3 },
    [PET_ADULT] = { 0,           3, 3 },   /* dur 0 = terminal */
    [PET_AWAY]  = { 0,           0, 0 },
};

#define PET_CUE_WINDOW_S   (900 / PET_TIME_SCALE)     /* 15 awake-visible min */
#define PET_CUE_COOLDOWN_S (1800 / PET_TIME_SCALE)
#define PET_AWAY_AFTER_S   (3 * 86400 / PET_TIME_SCALE)

/* Consumed by the main loop: an engine event (evolution, departure) wants the
 * scene rebuilt if PET is on screen. The engine cannot call app_request()
 * directly — it is defined much later in the file. */
static volatile bool s_req_pet_rebuild;

static const char *pet_stage_word(void) {
    static const char *w[] = { "EGG", "BABY", "CHILD", "TEEN", "GROWN", "AWAY" };
    return w[s_pet.stage <= PET_AWAY ? s_pet.stage : PET_ADULT];
}

static void pet_save(void);

static void pet_fresh_egg(void) {
    memset(&s_pet, 0, sizeof(s_pet));
    s_pet.ver = PET_BLOB_VER;
    s_pet.stage = PET_EGG;
    s_pet.hunger = s_pet.happy = 70;
    s_pet.happy_hist = 70;
    s_pet.seed = esp_random();          /* personality, decided in the shell */
    s_pet.sleep_start = 22 * 60;
    s_pet.sleep_end = 7 * 60;
    snprintf(s_pet.name, sizeof(s_pet.name), "PIP");
    /* Timestamps stay 0 until the clock is real; pet_credit() stamps them. */
}

static void pet_load(void) {
    pet_blob2_t b;
    memset(&b, 0, sizeof(b));
    bool have = store_load("pet", &b, sizeof(b)) && b.ver == PET_BLOB_VER;
    if (!have && store_load("pet", &b, PET_BLOB_V2_SIZE) && b.ver == 2) {
        /* v3 is v2 plus appended fields: prefix-read the old file, zero the
         * tail, stamp the version. This is what append-only buys. */
        memset((uint8_t *)&b + PET_BLOB_V2_SIZE, 0,
               sizeof(b) - PET_BLOB_V2_SIZE);
        b.ver = PET_BLOB_VER;
        have = true;
    }
    if (have) {
        s_pet = b;
        s_pet.name[sizeof(s_pet.name) - 1] = '\0';
        ESP_LOGI(TAG, "pet restored: %s %s hunger=%d happy=%d mistakes=%d odo=%lum",
                 s_pet.name, pet_stage_word(), s_pet.hunger, s_pet.happy,
                 (int)s_pet.mistakes, (unsigned long)s_pet.odo_m);
        return;
    }
    pet_blob_v1_t v1;
    if (store_load("pet", &v1, sizeof(v1)) && v1.ver == 1) {
        /* The old astronaut lived through v1; keep the life, not just the
         * numbers. Age places it on the stage table, three meters fold into
         * two, and the stamps are back-dated so growth continues mid-arc. */
        pet_fresh_egg();
        s_pet.hunger = clampi(v1.food, 0, 100);
        s_pet.happy  = clampi((v1.fun + v1.nrg) / 2, 0, 100);
        s_pet.happy_hist = s_pet.happy;
        uint32_t acc = 0, into = v1.age_min;
        s_pet.stage = PET_ADULT;
        for (int st = PET_EGG; st < PET_ADULT; st++) {
            if (v1.age_min < acc + s_pet_stages[st].dur_min) {
                s_pet.stage = st;
                into = v1.age_min - acc;
                break;
            }
            acc += s_pet_stages[st].dur_min;
        }
        time_t now = time(NULL);
        if (now >= PET_EPOCH_MIN) {
            s_pet.hatched_utc = now - (int64_t)v1.age_min * 60;
            s_pet.stage_utc   = now - (int64_t)into * 60;
            s_pet.tick_utc    = now;
            s_pet.seen_utc    = now;
        }
        ESP_LOGI(TAG, "pet migrated from v1: %lumin old -> %s",
                 (unsigned long)v1.age_min, pet_stage_word());
        /* Persist v2 NOW: the card is mounted (load order guarantees it),
         * and a migration that waits for the 60 s flush re-runs on any
         * power cut in between. */
        pet_save();
        return;
    }
    pet_fresh_egg();
    s_pet_dirty = true;
    ESP_LOGI(TAG, "pet: a new egg appears");
}

static void pet_save(void) {
    store_save("pet", &s_pet, sizeof(s_pet));
    s_pet_dirty = false;
}

/* ---- broker handoff ----
 *
 * The net task and the engine never touch each other's state directly: the
 * net task publishes a parsed config into a staging struct the engine
 * consumes on its own tick, and the engine publishes a report snapshot the
 * net task serialises — the DAYS publish/snapshot discipline, because both
 * of tonight's owners live on different tasks. */
typedef struct {
    uint32_t ver;
    uint16_t ss, se, bday;
    uint8_t  species, world, theme, hat;
    uint8_t  wx, coax, has_wx;
    int8_t   wx_temp;
    char     name[12];
    char     sheet[8];
} pet_cfg_msg_t;

typedef struct {
    uint32_t stardust, steps, seed;
    int64_t  hatched;
    uint16_t mistakes;
    uint8_t  stage, form, hunger, happy, sick, poop, away;
} pet_report_t;

static pet_cfg_msg_t s_pet_cfg_in;
static volatile bool s_pet_cfg_ready;
static pet_report_t  s_pet_report;
static volatile bool s_req_pet_push;
static volatile bool s_req_pet_cfg = true;   /* first fetch rides the boot */
/* Outcome of the most recent config fetch, for the QR panel's sync button:
 * 0 none/pending, 1 landed, 2 failed. Written by the net task, consumed by
 * the view; a single byte, so no lock. */
static volatile uint8_t s_pet_cfg_result;
static portMUX_TYPE  s_pet_net_lock = portMUX_INITIALIZER_UNLOCKED;

static void pet_report_publish(void) {
    pet_report_t r = {
        .stardust = s_pet.stardust, .steps = s_pet.steps_today,
        .seed = s_pet.seed, .hatched = s_pet.hatched_utc,
        .mistakes = s_pet.mistakes, .stage = s_pet.stage, .form = s_pet.form,
        .hunger = s_pet.hunger, .happy = s_pet.happy,
        .sick = s_pet.sick, .poop = s_pet.poop,
        .away = s_pet.stage == PET_AWAY,
    };
    portENTER_CRITICAL(&s_pet_net_lock);
    s_pet_report = r;
    portEXIT_CRITICAL(&s_pet_net_lock);
    __atomic_store_n(&s_req_pet_push, true, __ATOMIC_RELEASE);
}

/* Runs on the engine's tick. Weather applies on every response (it changes
 * under a constant version); the design applies only when the version moved,
 * because applying it rebuilds the scene and cfg_ver is the whole gate. */
static void pet_cfg_consume(void) {
    if (!__atomic_load_n(&s_pet_cfg_ready, __ATOMIC_ACQUIRE)) return;
    pet_cfg_msg_t m;
    portENTER_CRITICAL(&s_pet_net_lock);
    m = s_pet_cfg_in;
    portEXIT_CRITICAL(&s_pet_net_lock);
    __atomic_store_n(&s_pet_cfg_ready, false, __ATOMIC_RELEASE);

    time_t now = time(NULL);
    if (m.has_wx && now >= PET_EPOCH_MIN) {
        s_pet.wx = m.wx;
        s_pet.wx_temp = m.wx_temp;
        s_pet.wx_utc = now;
        s_pet_dirty = true;
    }
    if (m.coax && s_pet.stage == PET_AWAY) {
        /* The doorstep meal worked. Meters restart mid-range: a returning
         * pet is glad to be home, not fixed. */
        s_pet.stage = PET_ADULT;
        s_pet.hunger = s_pet.happy = 60;
        if (now >= PET_EPOCH_MIN) { s_pet.seen_utc = now; s_pet.stage_utc = now; }
        log_event("pet came home");
        ESP_LOGI(TAG, "pet %s came home", s_pet.name);
        s_pet_dirty = true;
        s_req_pet_rebuild = true;
        pet_report_publish();      /* the broker clears its coax flag on this */
    }
    if (m.ver == s_pet.cfg_ver) return;

    if (m.name[0]) {
        snprintf(s_pet.name, sizeof(s_pet.name), "%s", m.name);
        /* The broker folds to ASCII; keep a hostile or stale deployment from
         * feeding boxes to the subset fonts anyway. */
        for (char *p = s_pet.name; *p; p++)
            if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) *p = '?';
    }
    s_pet.species     = m.species;
    s_pet.world       = m.world;
    s_pet.theme       = m.theme;
    s_pet.hat         = m.hat;
    s_pet.sleep_start = m.ss;
    s_pet.sleep_end   = m.se;
    s_pet.bday        = m.bday;
    s_pet.cfg_ver     = m.ver;
    s_pet_dirty = true;
    s_req_pet_rebuild = true;
    ESP_LOGI(TAG, "pet cfg v%lu applied: %s species=%d world=%d theme=%d hat=%d",
             (unsigned long)m.ver, s_pet.name, s_pet.species, s_pet.world,
             s_pet.theme, s_pet.hat);
}

/* ---- the life itself ---- */

static int pet_minute_of_day(time_t t) {
    struct tm ti;
    localtime_r(&t, &ti);
    return ti.tm_hour * 60 + ti.tm_min;
}

static bool pet_asleep_at(int mod) {
    int ss = s_pet.sleep_start, se = s_pet.sleep_end;
    if (ss == se) return false;
    return ss < se ? (mod >= ss && mod < se) : (mod >= ss || mod < se);
}

static void pet_seen(void) {
    time_t now = time(NULL);
    if (now >= PET_EPOCH_MIN) s_pet.seen_utc = now;
}

/* Promotion runs on wall time, so a pet hatches, grows and evolves while the
 * cube sits in a drawer — exactly like the 1996 toy. The teen is picked by
 * how happy the childhood was; the adult by how few cues went unanswered. */
static void pet_promote_check(time_t now) {
    while (s_pet.stage < PET_ADULT) {
        uint32_t dur = s_pet_stages[s_pet.stage].dur_min;
        if (!dur || !s_pet.stage_utc) break;
        int64_t dur_s = (int64_t)dur * 60 / PET_TIME_SCALE;
        if (now - s_pet.stage_utc < dur_s) break;
        s_pet.stage_utc += dur_s;
        s_pet.stage++;
        if (s_pet.stage == PET_TEEN)
            s_pet.form = s_pet.happy_hist >= 67 ? 0 : (s_pet.happy_hist >= 34 ? 1 : 2);
        else if (s_pet.stage == PET_ADULT)
            s_pet.form = s_pet.mistakes <= 2 ? 0 : (s_pet.mistakes <= 6 ? 1 : 2);
        else
            s_pet.form = 0;
        log_event(s_pet.stage == PET_BABY ? "pet hatched" : "pet evolved");
        ESP_LOGI(TAG, "pet %s is now %s (form %d, happy_hist=%d, mistakes=%d)",
                 s_pet.name, pet_stage_word(), s_pet.form,
                 s_pet.happy_hist, (int)s_pet.mistakes);
        s_pet_dirty = true;
        s_req_pet_rebuild = true;
        pet_report_publish();          /* evolutions reach the dashboard */
    }
}

static void pet_credit(time_t now) {
    if (s_pet.tick_utc == 0 || s_pet.tick_utc > now) {
        /* First sighting of a real clock, or the clock moved backwards.
         * Either way, credit nothing we cannot vouch for. */
        s_pet.tick_utc = now;
        if (!s_pet.hatched_utc) {
            s_pet.hatched_utc = now;
            s_pet.stage_utc   = now;
            s_pet.seen_utc    = now;
        }
        s_pet_dirty = true;
        return;
    }
    int64_t mins = (int64_t)(now - s_pet.tick_utc) / 60;
    if (mins <= 0) return;

    /* Decay is capped at 4 days of it: a cube found in a drawer resumes
     * hungry-but-alive rather than instantly departed. Promotion and the
     * departure check use wall time directly, so the cap never delays
     * growing up — only starving. Decay pauses through the sleep window:
     * the pet sleeps, it does not starve overnight. */
    int64_t decay_mins = mins > 4 * 1440 ? 4 * 1440 : mins;
    if (s_pet.stage >= PET_BABY && s_pet.stage <= PET_ADULT) {
        static uint32_t hunger_acc, happy_acc, nrg_acc;
        int mod = pet_minute_of_day(now - decay_mins * 60);
        const pet_stage_row_t *row = &s_pet_stages[s_pet.stage];
        for (int64_t i = 0; i < decay_mins; i++, mod = (mod + 1) % 1440) {
            if (pet_asleep_at(mod)) {
                if (++nrg_acc >= 3) { nrg_acc = 0; s_nrg = clampi(s_nrg + 1, 5, 100); }
                continue;
            }
            hunger_acc += (uint32_t)row->hunger_per_h * PET_TIME_SCALE;
            happy_acc  += (uint32_t)row->happy_per_h * PET_TIME_SCALE;
            while (hunger_acc >= 60) { hunger_acc -= 60; s_pet.hunger = clampi(s_pet.hunger - 1, 0, 100); }
            while (happy_acc >= 60)  { happy_acc -= 60;  s_pet.happy  = clampi(s_pet.happy - 1, 0, 100); }
            if (++nrg_acc >= 8) { nrg_acc = 0; s_nrg = clampi(s_nrg - 1, 5, 100); }
            s_pet.happy_hist = (uint8_t)((s_pet.happy_hist * 15 + s_pet.happy + 8) / 16);
        }
        s_pet_dirty = true;
    }
    s_pet.tick_utc = now;
    pet_promote_check(now);

    if (s_pet.stage == PET_ADULT && s_pet.hunger == 0 && s_pet.happy == 0 &&
        s_pet.seen_utc && now - s_pet.seen_utc > PET_AWAY_AFTER_S) {
        s_pet.stage = PET_AWAY;
        log_event("pet left");
        ESP_LOGW(TAG, "pet %s packed a tiny suitcase and left "
                      "(unseen %llds, both meters empty)",
                 s_pet.name, (long long)(now - s_pet.seen_utc));
        s_pet_dirty = true;
        s_req_pet_rebuild = true;
        pet_report_publish();          /* the dashboard offers the coax-back */
    }
}

/* The Uni-style care loop: a need raises a cue; a cue answered in time is
 * bonding, a cue ignored for its whole window is one care mistake, and the
 * mistakes pick which adult arrives. The window only counts down while the
 * pet is awake AND the screen is on — mistakes never accrue where nobody
 * could have seen the ask. That asymmetry is the no-guilt rule from the
 * research, and it is also what makes offline time safe to credit. */
static void pet_cue_service(time_t now) {
    static time_t s_next_cue;
    if (s_pet.stage < PET_BABY || s_pet.stage > PET_ADULT) return;
    if (!s_screen_on || s_doze || pet_asleep_at(pet_minute_of_day(now))) return;

    if (s_pet.cue == PET_CUE_NONE) {
        if (now < s_next_cue) return;
        if (s_pet.hunger < 25)     s_pet.cue = PET_CUE_HUNGRY;
        else if (s_pet.happy < 25) s_pet.cue = PET_CUE_LONELY;
        else return;
        s_pet.cue_left_s = PET_CUE_WINDOW_S;
        s_pet_dirty = true;
        return;
    }
    /* Met by any route — feeding, playing, or decay reversing — it resolves
     * silently; there is no wrong way to answer a need. */
    if ((s_pet.cue == PET_CUE_HUNGRY && s_pet.hunger >= 40) ||
        (s_pet.cue == PET_CUE_LONELY && s_pet.happy >= 40)) {
        s_pet.cue = PET_CUE_NONE;
        s_pet_dirty = true;
        return;
    }
    if (s_pet.cue_left_s > 0) { s_pet.cue_left_s--; return; }
    s_pet.mistakes++;
    s_pet.cue = PET_CUE_NONE;
    s_next_cue = now + PET_CUE_COOLDOWN_S;
    s_pet_dirty = true;
    ESP_LOGI(TAG, "pet: care mistake #%d", (int)s_pet.mistakes);
}

/* ~1 Hz from the main loop, on every screen, awake or dozing. */
static void pet_engine_service(void) {
    static int64_t s_last_ms;
    int64_t t = now_ms();
    if (t - s_last_ms < 1000) return;
    s_last_ms = t;
    /* Config applies even before the clock is trusted — a renamed pet does
     * not need to know what time it is. */
    pet_cfg_consume();
    time_t now = time(NULL);
    if (now < PET_EPOCH_MIN) return;
    pet_credit(now);
    pet_cue_service(now);
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

/* ---------------- DAYS: cached countdown data ----------------
 *
 * The web page writes one target to the broker. The cube keeps the last good
 * answer locally so opening the app never waits on DNS or TLS, then asks again
 * in the background on every open and once a day. The network task is the only
 * countdown writer. A small cross-core critical section publishes fixed-size
 * countdown and one-time edit-link snapshots to the LVGL task; it never covers
 * a render callback or network operation. */

#define DAYS_BLOB_VER        1
#define DAYS_FETCH_MS        (24 * 60 * 60 * 1000LL)
#define DAYS_RETRY_MS        (15 * 60 * 1000LL)
#define DAYS_RESPONSE_MAX    512
#define DAYS_LINK_URL_MAX    256

typedef struct {
    uint8_t ver;
    char target[11];                    /* YYYY-MM-DD */
    char set_on[11];                    /* YYYY-MM-DD */
    char text[49];                      /* broker caps it at 48 ASCII bytes */
} days_blob_t;

static days_blob_t s_days;
static uint32_t s_days_version;
static bool s_req_days_fetch = true;
static bool s_req_days_link;
static bool s_days_fetching;
static bool s_days_link_fetching;
static bool s_req_days_save;
static portMUX_TYPE s_days_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_days_last_ok_ms;
static int64_t s_days_last_attempt_ms;
static char s_days_link_url[DAYS_LINK_URL_MAX];
static uint32_t s_days_link_version;

static void days_publish(const days_blob_t *next) {
    portENTER_CRITICAL(&s_days_lock);
    s_days = *next;
    s_days_version++;
    portEXIT_CRITICAL(&s_days_lock);
}

static void days_snapshot(days_blob_t *out, uint32_t *version) {
    portENTER_CRITICAL(&s_days_lock);
    *out = s_days;
    if (version) *version = s_days_version;
    portEXIT_CRITICAL(&s_days_lock);
}

static void days_link_publish(const char *url) {
    portENTER_CRITICAL(&s_days_lock);
    snprintf(s_days_link_url, sizeof(s_days_link_url), "%s", url ? url : "");
    s_days_link_version++;
    portEXIT_CRITICAL(&s_days_lock);
}

static void days_link_snapshot(char *out, size_t cap, uint32_t *version) {
    portENTER_CRITICAL(&s_days_lock);
    snprintf(out, cap, "%s", s_days_link_url);
    if (version) *version = s_days_link_version;
    portEXIT_CRITICAL(&s_days_lock);
}

static void days_load(void) {
    days_blob_t saved = {0};
    if (store_load("days", &saved, sizeof(saved)) && saved.ver == DAYS_BLOB_VER) {
        saved.target[sizeof(saved.target) - 1] = '\0';
        saved.set_on[sizeof(saved.set_on) - 1] = '\0';
        saved.text[sizeof(saved.text) - 1] = '\0';
        days_publish(&saved);
        ESP_LOGI(TAG, "days: cached target %s", saved.target[0] ? saved.target : "unset");
    }
}

typedef struct {
    char data[DAYS_RESPONSE_MAX];
    size_t len;
    bool overflow;
} days_rx_t;

static days_rx_t s_days_rx;
static esp_http_client_handle_t s_days_http;

static esp_err_t days_http_evt(esp_http_client_event_t *e) {
    days_rx_t *rx = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_DATA || !rx || e->data_len <= 0) return ESP_OK;
    size_t room = sizeof(rx->data) - 1 - rx->len;
    size_t n = (size_t)e->data_len;
    if (n > room) { n = room; rx->overflow = true; }
    if (n) {
        memcpy(rx->data + rx->len, e->data, n);
        rx->len += n;
        rx->data[rx->len] = '\0';
    }
    return ESP_OK;
}

static bool days_http_get(const char *path) {
    s_days_rx.len = 0;
    s_days_rx.overflow = false;
    s_days_rx.data[0] = '\0';

    if (!s_days_http) {
        char url[224];
        size_t base_len = strlen(BROKER_URL);
        snprintf(url, sizeof(url), "%s%s%s", BROKER_URL,
                 (base_len && BROKER_URL[base_len - 1] == '/') ? "" : "/",
                 path[0] == '/' ? path + 1 : path);
        esp_http_client_config_t cfg = {
            .url = url,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 12000,
            .event_handler = days_http_evt,
            .user_data = &s_days_rx,
            .keep_alive_enable = true,
            .max_redirection_count = 5,
        };
        s_days_http = esp_http_client_init(&cfg);
        if (!s_days_http) return false;
    } else {
        esp_http_client_set_url(s_days_http, path);
    }

    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %.72s", BROKER_TOKEN);
    esp_http_client_set_header(s_days_http, "Authorization", auth);
    esp_http_client_set_method(s_days_http, HTTP_METHOD_GET);
    /* The pet sync POSTs on this same handle, and esp_http_client resends a
     * stale body silently — post_data is never cleared automatically
     * (HARDWARE.md 7f). Clear it before every GET. */
    esp_http_client_set_post_field(s_days_http, NULL, 0);

    esp_err_t err = esp_http_client_perform(s_days_http);
    int status = esp_http_client_get_status_code(s_days_http);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "days %.24s: %s (HTTP %d) - redial next time",
                 path, esp_err_to_name(err), status);
        esp_http_client_close(s_days_http);
        return false;
    }
    if (status != 200 || s_days_rx.overflow) {
        ESP_LOGW(TAG, "days %.24s: HTTP %d, response %s", path, status,
                 s_days_rx.overflow ? "too large" : "rejected");
        return false;
    }
    return true;
}

static bool days_fetch(void) {
    if (!days_http_get("/countdown")) return false;

    cJSON *root = cJSON_Parse(s_days_rx.data);
    cJSON *date = root ? cJSON_GetObjectItem(root, "d") : NULL;
    cJSON *text = root ? cJSON_GetObjectItem(root, "t") : NULL;
    cJSON *set  = root ? cJSON_GetObjectItem(root, "s") : NULL;
    if (!root || !cJSON_IsString(date) || !cJSON_IsString(text) ||
        !cJSON_IsString(set)) {
        ESP_LOGW(TAG, "days: malformed broker response");
        if (root) cJSON_Delete(root);
        return false;
    }

    days_blob_t next = { .ver = DAYS_BLOB_VER };
    snprintf(next.target, sizeof(next.target), "%.10s", date->valuestring);
    snprintf(next.set_on, sizeof(next.set_on), "%.10s", set->valuestring);
    snprintf(next.text, sizeof(next.text), "%.48s", text->valuestring);
    /* The broker already folds to ASCII; keep a hostile or stale deployment
     * from putting unsupported glyphs into the cube's subset fonts anyway. */
    for (char *p = next.text; *p; p++) {
        if ((unsigned char)*p < 0x20 || (unsigned char)*p > 0x7E) *p = '?';
    }
    cJSON_Delete(root);

    days_publish(&next);
    /* Saving here would write NVS on this task, whose stack is in PSRAM and so is
     * unreachable the moment a flash write disables the cache. Hand it to the
     * main loop, which runs on an internal stack. */
    __atomic_store_n(&s_req_days_save, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "days: synced %s, %u B", next.target[0] ? next.target : "unset",
             (unsigned)s_days_rx.len);
    return true;
}

static bool days_link_fetch(void) {
    if (!days_http_get("/days/link")) return false;

    cJSON *root = cJSON_Parse(s_days_rx.data);
    cJSON *url = root ? cJSON_GetObjectItem(root, "authorization_url") : NULL;
    cJSON *expires = root ? cJSON_GetObjectItem(root, "expires_in") : NULL;
    const char *value = cJSON_IsString(url) ? url->valuestring : NULL;
    size_t n = value ? strlen(value) : 0;
    bool valid = value && strncmp(value, "https://", 8) == 0 &&
                 n > 8 && n < DAYS_LINK_URL_MAX &&
                 cJSON_IsNumber(expires) && expires->valueint > 0 &&
                 expires->valueint <= 300;
    if (valid) {
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)value[i];
            if (c < 0x21 || c > 0x7E) { valid = false; break; }
        }
    }
    if (!valid) {
        ESP_LOGW(TAG, "days: malformed secure-link response");
        if (root) cJSON_Delete(root);
        return false;
    }

    days_link_publish(value);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "days: secure edit QR ready (%u B)", (unsigned)n);
    return true;
}

static void days_service(void) {
    if (!s_wifi_up || ble_prov_active() || BROKER_URL[0] == '\0' ||
        BROKER_TOKEN[0] == '\0') return;

    bool link = __atomic_exchange_n(&s_req_days_link, false, __ATOMIC_ACQ_REL);
    if (link) {
        __atomic_store_n(&s_days_link_fetching, true, __ATOMIC_RELEASE);
        if (!days_link_fetch()) days_link_publish("");
        __atomic_store_n(&s_days_link_fetching, false, __ATOMIC_RELEASE);
    }

    int64_t t = now_ms();
    bool forced = __atomic_exchange_n(&s_req_days_fetch, false, __ATOMIC_ACQ_REL);
    bool daily = !s_days_last_ok_ms || t - s_days_last_ok_ms >= DAYS_FETCH_MS;
    if (!forced && !daily) return;
    if (!forced && s_days_last_attempt_ms &&
        t - s_days_last_attempt_ms < DAYS_RETRY_MS) return;

    s_days_last_attempt_ms = t;
    __atomic_store_n(&s_days_fetching, true, __ATOMIC_RELEASE);
    bool ok = days_fetch();
    __atomic_store_n(&s_days_fetching, false, __ATOMIC_RELEASE);
    if (ok) s_days_last_ok_ms = t;
}

/* ---------------- PET broker sync ----------------
 *
 * Rides the DAYS client: same broker host, same task, same keep-alive handle,
 * so a pet fetch after a countdown fetch costs 6 ms instead of a 390 ms
 * handshake. The engine's side of this lives beside the pet blob
 * (pet_cfg_consume / pet_report_publish). */

#define PET_CFG_FETCH_MS (2 * 60 * 60 * 1000LL)
#define PET_CFG_RETRY_MS (15 * 60 * 1000LL)
#define PET_LINK_URL_MAX 256

static bool s_req_pet_link;
static bool s_pet_cfg_fetching, s_pet_link_fetching;
static int64_t s_pet_cfg_ok_ms, s_pet_cfg_attempt_ms;
static char s_pet_link_url[PET_LINK_URL_MAX];
static uint32_t s_pet_link_version;

static void pet_link_publish(const char *url) {
    portENTER_CRITICAL(&s_pet_net_lock);
    snprintf(s_pet_link_url, sizeof(s_pet_link_url), "%s", url ? url : "");
    s_pet_link_version++;
    portEXIT_CRITICAL(&s_pet_net_lock);
}

static void pet_link_snapshot(char *out, size_t cap, uint32_t *version) {
    portENTER_CRITICAL(&s_pet_net_lock);
    snprintf(out, cap, "%s", s_pet_link_url);
    if (version) *version = s_pet_link_version;
    portEXIT_CRITICAL(&s_pet_net_lock);
}

static bool days_http_post_json(const char *path, const char *body) {
    /* Prime the handle exactly like days_http_get, then flip it to POST. */
    if (!s_days_http) {
        if (!days_http_get("/healthz")) return false;    /* builds the handle */
    }
    s_days_rx.len = 0;
    s_days_rx.overflow = false;
    s_days_rx.data[0] = '\0';
    esp_http_client_set_url(s_days_http, path);
    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %.72s", BROKER_TOKEN);
    esp_http_client_set_header(s_days_http, "Authorization", auth);
    esp_http_client_set_header(s_days_http, "Content-Type", "application/json");
    esp_http_client_set_method(s_days_http, HTTP_METHOD_POST);
    esp_http_client_set_post_field(s_days_http, body, (int)strlen(body));

    esp_err_t err = esp_http_client_perform(s_days_http);
    int status = esp_http_client_get_status_code(s_days_http);
    esp_http_client_set_post_field(s_days_http, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pet %.24s: %s (HTTP %d) - redial next time",
                 path, esp_err_to_name(err), status);
        esp_http_client_close(s_days_http);
        return false;
    }
    if (status != 200 || s_days_rx.overflow) {
        ESP_LOGW(TAG, "pet %.24s: HTTP %d", path, status);
        return false;
    }
    return true;
}

static bool pet_cfg_fetch(void) {
    if (!days_http_get("/pet/cfg")) return false;

    cJSON *root = cJSON_Parse(s_days_rx.data);
    cJSON *v  = root ? cJSON_GetObjectItem(root, "v") : NULL;
    cJSON *n  = root ? cJSON_GetObjectItem(root, "n") : NULL;
    if (!root || !cJSON_IsNumber(v) || !cJSON_IsString(n)) {
        ESP_LOGW(TAG, "pet: malformed cfg response");
        if (root) cJSON_Delete(root);
        return false;
    }
    pet_cfg_msg_t m = { .ver = (uint32_t)v->valuedouble };
    snprintf(m.name, sizeof(m.name), "%.11s", n->valuestring);
    cJSON *it;
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "s")))  m.species = (uint8_t)clampi(it->valueint, 0, 8);
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "w")))  m.world   = (uint8_t)clampi(it->valueint, 0, 8);
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "t")))  m.theme   = (uint8_t)clampi(it->valueint, 0, 8);
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "h")))  m.hat     = (uint8_t)clampi(it->valueint, 0, 8);
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "ss"))) m.ss      = (uint16_t)clampi(it->valueint, 0, 1439);
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "se"))) m.se      = (uint16_t)clampi(it->valueint, 0, 1439);
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "cb"))) m.coax    = it->valueint ? 1 : 0;
    if (cJSON_IsString(it = cJSON_GetObjectItem(root, "b")) && strlen(it->valuestring) == 5) {
        int mo = (it->valuestring[0] - '0') * 10 + (it->valuestring[1] - '0');
        int dy = (it->valuestring[3] - '0') * 10 + (it->valuestring[4] - '0');
        if (mo >= 1 && mo <= 12 && dy >= 1 && dy <= 31) m.bday = (uint16_t)(mo << 8 | dy);
    }
    if (cJSON_IsNumber(it = cJSON_GetObjectItem(root, "wx"))) {
        m.wx = (uint8_t)clampi(it->valueint, 0, 7);
        m.has_wx = 1;
        cJSON *wt = cJSON_GetObjectItem(root, "wt");
        if (cJSON_IsNumber(wt)) m.wx_temp = (int8_t)clampi(wt->valueint, -99, 99);
    }
    if (cJSON_IsString(it = cJSON_GetObjectItem(root, "sn")))
        snprintf(m.sheet, sizeof(m.sheet), "%.7s", it->valuestring);
    cJSON_Delete(root);

    portENTER_CRITICAL(&s_pet_net_lock);
    s_pet_cfg_in = m;
    portEXIT_CRITICAL(&s_pet_net_lock);
    __atomic_store_n(&s_pet_cfg_ready, true, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "pet: cfg v%lu fetched, %u B", (unsigned long)m.ver,
             (unsigned)s_days_rx.len);
    return true;
}

static bool pet_link_fetch(void) {
    if (!days_http_get("/pet/link")) return false;

    cJSON *root = cJSON_Parse(s_days_rx.data);
    cJSON *url = root ? cJSON_GetObjectItem(root, "authorization_url") : NULL;
    const char *value = cJSON_IsString(url) ? url->valuestring : NULL;
    size_t n = value ? strlen(value) : 0;
    bool valid = value && strncmp(value, "https://", 8) == 0 &&
                 n > 8 && n < PET_LINK_URL_MAX;
    if (valid) {
        for (size_t i = 0; i < n; i++) {
            unsigned char c = (unsigned char)value[i];
            if (c < 0x21 || c > 0x7E) { valid = false; break; }
        }
    }
    if (!valid) {
        ESP_LOGW(TAG, "pet: malformed designer-link response");
        if (root) cJSON_Delete(root);
        return false;
    }
    pet_link_publish(value);
    cJSON_Delete(root);
    ESP_LOGI(TAG, "pet: designer QR ready (%u B)", (unsigned)n);
    return true;
}

static bool pet_push(void) {
    pet_report_t r;
    portENTER_CRITICAL(&s_pet_net_lock);
    r = s_pet_report;
    portEXIT_CRITICAL(&s_pet_net_lock);
    char body[224];
    snprintf(body, sizeof(body),
             "{\"g\":%u,\"f\":%u,\"hu\":%u,\"ha\":%u,\"m\":%u,\"sd\":%lu,"
             "\"st\":%lu,\"sk\":%u,\"pp\":%u,\"aw\":%u,\"hz\":%lld,\"e\":%lu}",
             r.stage, r.form, r.hunger, r.happy, (unsigned)r.mistakes,
             (unsigned long)r.stardust, (unsigned long)r.steps,
             r.sick, r.poop, r.away, (long long)r.hatched,
             (unsigned long)r.seed);
    if (!days_http_post_json("/pet/st", body)) return false;
    /* The response carries the current config version, so a state push
     * doubles as a drift check — a design saved on the phone minutes ago is
     * noticed here instead of waiting out the 2 h cycle. */
    cJSON *root = cJSON_Parse(s_days_rx.data);
    cJSON *v = root ? cJSON_GetObjectItem(root, "v") : NULL;
    if (cJSON_IsNumber(v) && (uint32_t)v->valuedouble != s_pet.cfg_ver)
        __atomic_store_n(&s_req_pet_cfg, true, __ATOMIC_RELEASE);
    if (root) cJSON_Delete(root);
    ESP_LOGI(TAG, "pet: state reported (%s)", body);
    return true;
}

static void pet_net_service(void) {
    if (!s_wifi_up || ble_prov_active() || BROKER_URL[0] == '\0' ||
        BROKER_TOKEN[0] == '\0') return;

    if (__atomic_exchange_n(&s_req_pet_link, false, __ATOMIC_ACQ_REL)) {
        __atomic_store_n(&s_pet_link_fetching, true, __ATOMIC_RELEASE);
        if (!pet_link_fetch()) pet_link_publish("");
        __atomic_store_n(&s_pet_link_fetching, false, __ATOMIC_RELEASE);
    }

    if (__atomic_exchange_n(&s_req_pet_push, false, __ATOMIC_ACQ_REL)) {
        if (!pet_push()) {
            /* keep the report; retry rides the next service pass */
            __atomic_store_n(&s_req_pet_push, true, __ATOMIC_RELEASE);
        }
    }

    int64_t t = now_ms();
    bool forced = __atomic_exchange_n(&s_req_pet_cfg, false, __ATOMIC_ACQ_REL);
    bool due = !s_pet_cfg_ok_ms || t - s_pet_cfg_ok_ms >= PET_CFG_FETCH_MS;
    if (!forced && !due) return;
    if (!forced && s_pet_cfg_attempt_ms &&
        t - s_pet_cfg_attempt_ms < PET_CFG_RETRY_MS) return;

    s_pet_cfg_attempt_ms = t;
    __atomic_store_n(&s_pet_cfg_fetching, true, __ATOMIC_RELEASE);
    bool ok = pet_cfg_fetch();
    __atomic_store_n(&s_pet_cfg_fetching, false, __ATOMIC_RELEASE);
    s_pet_cfg_result = ok ? 1 : 2;
    if (ok) s_pet_cfg_ok_ms = t;
}

/* The network task.
 *
 * Once polled a stand-in "speech to text" endpoint every 45 s to characterise
 * TLS cost. That measurement is long done and recorded in HARDWARE.md 7f, so the
 * poll was pure waste: a TLS handshake, a log line and a UI string, several
 * times a minute, for information nobody read.
 *
 * Its stack lives in PSRAM. As plain xTaskCreate it took 8 KB of internal SRAM —
 * the scarcest pool on the board — for a task that does nothing time-critical.
 *
 * That stack is unreachable while the flash cache is disabled, so nothing on this
 * task may write NVS. Persistence is deferred to the main loop; the card is fine,
 * being SDMMC rather than SPI flash.
 */
static void net_task(void *arg) {
    while (1) {
        xEventGroupWaitBits(s_evt, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

        /* Small broker state first so opening DAYS does not wait behind a
         * several-hundred-KB wallpaper. Both stay off the main loop, so a slow
         * request cannot stall button handling or the app switcher. */
        days_service();
        pet_net_service();
        wall_service();

        /* A wallpaper request breaks the wait, so pressing Fetch acts now
         * instead of sitting until the next cycle. */
        int period = s_doze ? (10 * 60 * 1000) : HTTPS_PERIOD_MS;
        for (int w = 0; w < period / 100 && !s_req_wallpaper &&
                        !__atomic_load_n(&s_req_days_fetch, __ATOMIC_ACQUIRE) &&
                        !__atomic_load_n(&s_req_days_link, __ATOMIC_ACQUIRE) &&
                        !__atomic_load_n(&s_req_pet_link, __ATOMIC_ACQUIRE) &&
                        !__atomic_load_n(&s_req_pet_cfg, __ATOMIC_ACQUIRE) &&
                        !__atomic_load_n(&s_req_pet_push, __ATOMIC_ACQUIRE); w++) {
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

/* The mode -> CV mapping, split out so the percentage the UI shows can be
 * DERIVED from the volts we actually write rather than kept as a second copy
 * beside them. Two hand-maintained tables of the same fact is how the labels
 * drifted four points off the hardware in the first place. */
static uint8_t chg_mode_cv_code(int mode) {
    switch (mode) {
        case CHG_LIFESPAN: return AXP_CV_4V00;
        case CHG_BALANCED: return AXP_CV_4V10;
        default:           return AXP_CV_4V20;
    }
}

static uint8_t chg_cv_code(void) {
    if (s_chg_once) return AXP_CV_4V20;
    return chg_mode_cv_code(s_chg_mode);
}

static int chg_cv_mv(uint8_t code) {
    switch (code & AXP_CV_MASK) {
        case AXP_CV_4V00: return 4000;
        case AXP_CV_4V10: return 4100;
        case AXP_CV_4V20: return 4200;
        default:          return 0;        /* 4.35/4.4 V, or the reserved 000 */
    }
}

/* What the charger is actually aiming at, as a percentage. The one-shot beats
 * the cap while it is armed, exactly as chg_cv_code() resolves the same
 * question in volts. */
static int chg_target_pct(void) {
    return s_chg_once ? 100 : chg_mode_pct(s_chg_mode);
}

/* ---- time to the charge limit ----
 *
 * The AXP2101 reports no time-to-full, so it is derived from the only signal
 * that moves: the same percentage the ring draws. Each 1-point step is one
 * rate sample — one point per N ms — and N is smoothed rather than averaged
 * over the whole session, because a session average cannot see the CV taper
 * and would still be promising "6 min" half an hour after current started
 * falling.
 *
 * It counts to the LIMIT, not to 100%. A cube capped at 85% stops there, so a
 * countdown to full would name a moment that never arrives — which is the
 * whole point of the feature.
 *
 * Computed here, on the main task, and published as one plain int: the UI
 * reads it from the LVGL task, and a 64-bit timestamp shared across the two
 * can tear on a 32-bit core and produce a garbage frame. */
#define CHG_ETA_MIN_MS_PP   (10 * 1000)        /* faster is a gauge jump, not charge */
#define CHG_ETA_MAX_MS_PP   (90 * 60 * 1000)   /* slower is a stall, not a rate      */

/* Opt-in, default OFF. A countdown on the lock screen is a taste question —
 * some people want the number, some want the clock and nothing else — and the
 * screen's standing rule is sparseness, so the default has to be the sparse
 * one. The switch gates only the two READOUTS; the estimator keeps running
 * either way, because it costs nothing measurable and switching the feature on
 * mid-charge should show an answer immediately rather than start a fresh
 * two-point wait. */
static volatile bool s_chg_eta_on;

static volatile int s_chg_eta_mins = -1;   /* -1 = no honest answer to give */
static int64_t s_chg_eta_mark;             /* when the gauge last stepped up */
static int     s_chg_eta_pct;              /* the value it stepped to */
static int     s_chg_eta_ms_pp;            /* smoothed ms per point, 0 = not learned */
static int     s_chg_eta_steps;            /* rate samples taken this session */

static void chg_eta_track(void) {
    int pct = s_batt_pct;
    /* Not charging covers the two ends as well as the middle: unplugged, and
     * sitting at "done" with the cap reached. Both mean there is nothing to
     * count down, so the session is dropped rather than frozen. */
    if (!s_batt_charging || pct < 0) {
        s_chg_eta_mark = 0;
        s_chg_eta_ms_pp = 0;
        s_chg_eta_steps = 0;
        s_chg_eta_mins = -1;
        return;
    }

    int64_t t = now_ms();
    if (!s_chg_eta_mark || pct < s_chg_eta_pct) {
        /* First sample of the session, or the gauge went backwards — a load
         * spike, or one of battery_poll()'s voltage corrections overruling a
         * latched coulomb counter. Re-anchor; never feed a negative interval
         * into the rate. */
        s_chg_eta_mark = t;
        s_chg_eta_pct  = pct;
    } else if (pct > s_chg_eta_pct) {
        int step = pct - s_chg_eta_pct;
        int ms_pp = (int)(t - s_chg_eta_mark);
        s_chg_eta_mark = t;
        s_chg_eta_pct  = pct;
        /* Only a single-point step is charge. Polling is every 2 s and no real
         * charge rate covers two points in that, so a bigger jump is the gauge
         * correcting itself — dividing it out would fabricate a rate several
         * times the true one. Re-anchor and keep the rate already learned.
         *
         * The first interval also does not count: it started wherever the cube
         * happened to be inside a point, so it is short by an unknown amount
         * and would seed the whole estimate optimistic. Measure from the first
         * step to the second. */
        if (step == 1 && ++s_chg_eta_steps > 1 &&
            ms_pp >= CHG_ETA_MIN_MS_PP && ms_pp <= CHG_ETA_MAX_MS_PP) {
            /* Three-quarters old: one fast point (the gauge catching up to a
             * voltage it already reached) must not halve the estimate, while a
             * real slowdown still lands within a few points. */
            s_chg_eta_ms_pp = s_chg_eta_ms_pp
                            ? (s_chg_eta_ms_pp * 3 + ms_pp) / 4 : ms_pp;
        }
    }

    /* How far there is to go is measured in VOLTS, never in gauge points.
     *
     * The limit is a CV target in reg 0x64 — 4100 mV for the setting CONTROL
     * calls "85%" — and the PMU terminates on that voltage and the taper
     * current. It never reads the fuel gauge. Those two scales do not line up:
     * on the voltage cross-check this firmware already uses (3300 mV = 0%,
     * 4200 mV = 100%) a 4100 mV cap is really ~89%, so the gauge sails through
     * 85, 86, 87 with the charger still working. Counting down to the LABEL
     * therefore hit `remain <= 0` and withdrew the caption several minutes
     * early, while the cube was visibly still charging — observed at 86% and
     * 4077 mV against a 4100 mV cap, 23 mV short.
     *
     * It also made one of this feature's own tests unobservable: "the caption
     * disappears when the charge terminates" could never be seen, because the
     * caption was already gone by then.
     *
     * The RATE stays ms-per-gauge-point, which is smoothed, monotonic and
     * already proven; only the distance changes units. 9 mV per point is the
     * same 900 mV / 100 scale, so the two agree. */
    int cv = chg_cv_mv(chg_cv_code());
    int remain = (cv > 0 && s_batt_mv > 0) ? (cv - s_batt_mv + 8) / 9
                                           : chg_target_pct() - pct;
    int64_t open = t - s_chg_eta_mark;
    /* A point that has taken longer than any rate we would have learned is not
     * a slow charge, it is a charge that has stopped moving — a cell holding
     * just under a target it will not reach, or a supply that cannot deliver.
     * Withdraw the estimate instead of letting the taper term below inflate it
     * into an ever-growing number nobody should plan around. */
    if (!s_chg_eta_ms_pp || remain <= 0 || open > CHG_ETA_MAX_MS_PP) {
        s_chg_eta_mins = -1;
        return;
    }
    int64_t ms = (int64_t)remain * s_chg_eta_ms_pp;
    /* Charge the point in progress for the time it has ALREADY taken once that
     * exceeds the learned rate. This is the CV taper, where every remaining
     * point costs more than the ones behind it; without it the estimate parks
     * on its last value and visibly counts nothing down. */
    if (open > s_chg_eta_ms_pp) ms += open - s_chg_eta_ms_pp;
    s_chg_eta_mins = (int)((ms + 59999) / 60000);   /* round up: "0 min" is not a wait */
}

/* "1h 12m to 85%". The limit is in the string because the number is only
 * honest with it — a bare "1h 12m" under a 62% ring reads as time to full.
 * Rounded to five minutes past a quarter hour: the input is a 1%-granular
 * gauge, and a minute-precise countdown from it twitches by more than it
 * resolves. Lower case, like every other CONTROL string; the lock screen
 * upper-cases it on the way out the same way it does the date. */
static void chg_eta_text(char *buf, size_t n, int mins) {
    if (mins > 15) mins = (mins + 2) / 5 * 5;
    int target = chg_target_pct();
    if (mins >= 60) snprintf(buf, n, "%dh %02dm to %d%%",
                             mins / 60, mins % 60, target);
    else            snprintf(buf, n, "%dm to %d%%", mins, target);
}

/* Write the charge cap if the PMU is not already holding it.
 *
 * This is called from battery_poll() rather than once at boot, and that is the
 * point: reg 0x64 survives a system reset but not a POR, and the datasheet is
 * explicit that "the charger is enabled when an adapter is inserted" — so the
 * cap has to be something we keep asserting, not something we set and trust.
 * The read is one byte on a bus we are already talking to. */
static void chg_apply(void) {
    if (!s_axp) return;
    uint8_t want = chg_cv_code(), cv = 0;
    if (axp_read(AXP_REG_CHG_CV, &cv) != ESP_OK) return;
    if ((cv & AXP_CV_MASK) == want) return;
    if (axp_write(AXP_REG_CHG_CV, (uint8_t)((cv & ~AXP_CV_MASK) | want)) != ESP_OK) return;
    ESP_LOGI(TAG, "charge cap -> %d mV (CV 0x%02x -> 0x%02x)",
             chg_cv_mv(want), cv & AXP_CV_MASK, want);
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
    axp_set_bit(AXP_REG_INTEN2, AXP_PKEY_NEG_BIT);
    axp_set_bit(AXP_REG_INTEN2, AXP_PKEY_POS_BIT);
    axp_write(AXP_REG_INTSTS2, (1u << AXP_PKEY_SHORT_BIT) |
                               (1u << AXP_PKEY_NEG_BIT) |
                               (1u << AXP_PKEY_POS_BIT));
    /* The PMU powers up at 25 mA, which barely charges the cell at all.
     * 400 mA is what Waveshare's own AXP2101 example programs. */
    uint8_t icc = 0, cv = 0, ctrl = 0;
    axp_read(AXP_REG_CHG_ICC, &icc);
    axp_write(AXP_REG_CHG_ICC, (uint8_t)((icc & 0xE0) | 10));   /* step 10 = 400 mA */
    chg_apply();
    axp_read(AXP_REG_CHG_ICC, &icc);
    axp_read(AXP_REG_CHG_CV, &cv);
    axp_read(AXP_REG_GAUGE_CTRL, &ctrl);
    int step = icc & 0x1F;
    int ma = (step <= 8) ? step * 25 : 200 + (step - 8) * 100;
    ESP_LOGI(TAG, "AXP2101 ready — charge current %d mA (ICC=0x%02x) cap %d mV "
                  "(CV=0x%02x) CTRL=0x%02x", ma, icc, chg_cv_mv(cv), cv, ctrl);
}

static void battery_poll(void) {
    if (!s_axp) return;

    uint8_t st1 = 0, st2 = 0, pct = 0, hi = 0, lo = 0;
    if (axp_read(AXP_REG_STATUS1, &st1) != ESP_OK) return;
    axp_read(AXP_REG_STATUS2, &st2);
    s_batt_charging = ((st2 >> 5) & 0x03) == 0x01;

    /* Presence is reg 0x00 bit 5 alone. XPowersLib's isVbusIn() also demands
     * "not in VINDPM", which reads as unplugged whenever the supply droops —
     * exactly the case a charge cap makes more likely, not less. */
    s_vbus      = (st1 >> 5) & 0x01;
    s_chg_state = st2 & 0x07;
    s_bypass    = s_vbus && s_chg_state == AXP_CHG_DONE;

    /* Above the no-cell return: the cap is a PMU setting, and both it and VBUS
     * stay meaningful on a board running with the battery unplugged. */
    chg_apply();

    if (!((st1 >> 3) & 0x01)) {          /* no battery on the connector */
        s_batt_pct = -1;
        s_batt_mv = 0;
        chg_eta_track();                 /* drops the session, board runs on USB */
        return;
    }
    if (axp_read(AXP_REG_ADC_DATA_H, &hi) == ESP_OK &&
        axp_read(AXP_REG_ADC_DATA_L, &lo) == ESP_OK) {
        s_batt_mv = ((hi & 0x1F) << 8) | lo;
    }
    /* Voltage first, because it is the number that can be checked. 3.30 V ~ 0%,
     * 4.20 V ~ 100%. */
    int ocv = s_batt_mv - (s_batt_charging ? BATT_IR_CHG_MV : 0);
    int vpct = (s_batt_mv > 2500)
             ? clampi((ocv - 3300) * 100 / 900, 0, 100) : -1;

    if (axp_read(AXP_REG_BAT_PERCENT, &pct) == ESP_OK && pct > 0 && pct <= 100) {
        s_batt_pct = pct;

        /* The AXP2101's gauge is a coulomb counter and it can latch — observed
         * reading a confident 100% during a 5 h soak while the ADC said 3964 mV,
         * which is nearer 60%. It only self-corrects across a full charge and
         * discharge, so believing it unconditionally means the reading is wrong for
         * as long as the device stays plugged in.
         *
         * Charge raises terminal voltage and load sags it, so small disagreements
         * are expected and the gauge is the better number for those. A gap this
         * wide is not a load offset, it is a broken integrator — so past 25 points
         * the direct measurement wins. Logged once per crossing rather than every
         * minute; this is a condition, not an event. */
        /* Two independent tests, because the wide-gap one alone missed the case
         * that prompted this: a gauge reading 100% at 4013 mV is only 22 points out
         * and slips through, yet it is still plainly wrong. A lithium cell at full
         * charge sits near 4.20 V and does not read below ~4.10 V even *while*
         * charging, when terminal voltage is at its most flattering. So "claims
         * full, measures under 4.10 V" is a contradiction no load condition
         * explains. */
        bool implausible_full = (s_batt_pct >= 95 && s_batt_mv > 0 && s_batt_mv < 4100);
        if (vpct >= 0 && (implausible_full || abs(s_batt_pct - vpct) > 25)) {
            static bool warned;
            if (!warned) {
                warned = true;
                ESP_LOGW(TAG, "battery gauge says %d%% but %d mV is ~%d%% — "
                              "using voltage", pct, s_batt_mv, vpct);
            }
            s_batt_pct = vpct;

            /* Ask the chip to fix itself before working around it. The gauge is
             * an OCV + coulomb-counter engine with its own calibration module
             * (datasheet 7.11), and reg 0x17 bit 3 exists precisely to reset a
             * counter that has drifted — which is what "100% at 3719 mV" is.
             * Nothing in this firmware had ever written it, so a latched gauge
             * stayed latched for the life of the board and the voltage fallback
             * became permanent rather than transitional.
             *
             * ONCE per boot, and only after the reading has been contradicted:
             * a reset discards the learned capacity and Rdc, so doing it on a
             * timer would keep destroying the thing the chip is trying to
             * learn. 60 s of uptime first, because the ADC and the gauge both
             * need to settle before their disagreement means anything. */
            static bool gauge_reset_done;
            if (!gauge_reset_done && now_ms() > 60000) {
                gauge_reset_done = true;
                uint8_t r = 0;
                if (axp_read(AXP_REG_GAUGE_RESET, &r) == ESP_OK &&
                    axp_write(AXP_REG_GAUGE_RESET, (uint8_t)(r | (1u << 3))) == ESP_OK) {
                    ESP_LOGW(TAG, "fuel gauge reset issued (0x17 b3) — "
                                  "was %d%% at %d mV", pct, s_batt_mv);
                } else {
                    ESP_LOGW(TAG, "fuel gauge reset FAILED to write 0x17");
                }
            }
        }
    } else if (vpct >= 0) {
        s_batt_pct = vpct;
    } else {
        s_batt_pct = 0;
    }

    /* Last, and deliberately: it estimates from the settled s_batt_pct — the
     * one the ring draws — so the countdown and the gauge can never disagree. */
    chg_eta_track();
}

/* bit0 = went down, bit1 = came up. Both can land in one poll on a tap
 * shorter than the 20 ms loop, which the caller has to handle rather than
 * pick one. Write-1-to-clear, like every other IRQ status bit here. */
static uint8_t pmu_pwrkey_edges(void) {
    if (!s_axp) return 0;
    uint8_t sts = 0, out = 0, clr = 0;
    if (axp_read(AXP_REG_INTSTS2, &sts) != ESP_OK) return 0;
    if (sts & (1u << AXP_PKEY_NEG_BIT)) { out |= 1; clr |= 1u << AXP_PKEY_NEG_BIT; }
    if (sts & (1u << AXP_PKEY_POS_BIT)) { out |= 2; clr |= 1u << AXP_PKEY_POS_BIT; }
    if (clr) axp_write(AXP_REG_INTSTS2, clr);
    return out;
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
static volatile bool s_clock_24 = true;
static volatile bool s_req_clock_save;

/* Lock-screen quick action for the middle key, which does nothing there
 * otherwise. Held as an app index rather than a menu position so the setting
 * keeps its meaning if an app is compiled in or out; LOCK_KEY_OFF is the
 * default, so the documented "a key press must not bypass the touchscreen
 * unlock" behaviour is unchanged unless someone opts in. */
#define LOCK_KEY_OFF (-1)
static volatile int s_lock_key_app = LOCK_KEY_OFF;

static volatile bool s_always_on;
static volatile bool s_lock_rings = true;
/* Cosmetic only: whether desk-clock mode announces itself with the amber
 * ring. The mode itself belongs to the lock screen's right-key hold. */
static volatile bool s_ao_ring_pref = true;
static volatile bool s_req_lock_pref_save;

/* ---- desk-clock dim ----
 *
 * Always-on is the only mode in this firmware that can hold the panel lit for
 * hours, and on an AMOLED that is paid for twice: once in emission per lit
 * pixel, and once in how MANY pixels are lit. So the dim does both. It lowers
 * 0x51, and on the lock screen it also takes the wallpaper off the glass,
 * leaving a black field that costs nothing at all and a clock that costs a few
 * thousand pixels. Brightness alone would keep paying for 230,400 lit pixels.
 *
 * The depth is a PERCENTAGE of the user's brightness, never a flat constant and
 * never a min() — that reasoning is written out at the FOCUS dim in pomo_poll()
 * and it applies here unchanged. The numbers are deliberately the same pair
 * rather than a fresh guess: 12 / 3 is what was actually checked through this
 * cover glass, and nothing measured says a desk clock wants a different one.
 * They are separate symbols so a future measurement can move one alone.
 *
 * 60 s by default because that is already this firmware's idea of "nobody is
 * here" — it is AUTO_LOCK_MS. FOCUS waits 120 s because you are sitting in
 * front of it doing something; a desk clock is finished with you the moment
 * you look away.
 *
 * ON by default. The mode's only switch is a right-key hold on the lock screen,
 * so the person who turns always-on on has no reason ever to open CONTROL, and
 * a battery feature nobody finds is not a feature. One touch undoes it. */
static volatile bool s_ao_dim_on = true;
static volatile int  s_ao_dim_s  = 60;   /* idle seconds before the dim engages */
static bool s_ao_dimmed;                 /* main task only, like s_pomo_dimmed  */
#define AO_DIM_PCT   12     /* of the user's brightness, not of full        */
#define AO_DIM_FLOOR 3      /* on a black field, still readable across a room */

/* The dim FADES rather than steps, phone-style, and the reason is interaction
 * rather than looks. A panel that drops in one frame has already happened by
 * the time you notice it, so the only thing left to do is wonder whether the
 * cube is broken; a fade is an announcement with a window inside it, and a
 * touch anywhere in that window calls the whole thing off.
 *
 * Quantised into steps rather than run off the 20 ms loop tick, because every
 * one of them is a 0x51 write taking the LVGL lock from the main task while the
 * lock screen's own 16 ms timer is running — pitfall #13's contended case. A
 * free-running ramp over the ~88 levels between full and dim would be ~88 lock
 * acquisitions in 2.4 s; 24 is enough to look continuous and is one write per
 * 100 ms, which is slower than the FOCUS dim already does on this same path.
 *
 * The ramp is linear in the panel's own units, and perception of brightness is
 * closer to logarithmic, so the visible change front-loads — most of the drop
 * reads in the first second. That is the right way round here: the point is to
 * be noticed early enough to be stopped, not to be inconspicuous.
 *
 * Restoring is deliberately NOT faded. A fade out is the device announcing
 * something; a fade in would be the device answering your finger slowly. */
#define AO_DIM_FADE_MS   2400
#define AO_DIM_FADE_STEPS 24

static int     s_ao_dim_lvl  = -1;   /* level the fade has reached; -1 = idle */
static int     s_ao_dim_from;        /* level it started from                 */
static int64_t s_ao_dim_began;
static bool    s_ao_wall_hidden;     /* the blackout half, taken at the bottom */

/* ---- the two things that are about THIS panel rather than about power ----
 *
 * Burn-in is the AMOLED-specific RISK, and the blackout above made this cube's
 * exposure to it worse rather than better. A desk clock holds the same digits
 * in the same pixels for hours, which is the textbook OLED case; until now the
 * wallpaper at least reshuffled everything underneath on each lock build, and
 * hiding it leaves nothing on the glass but static elements. So the content
 * walks a slow eight-point ring while dimmed — the standard mitigation, and it
 * costs nothing on a screen nobody is reading closely.
 *
 * The shift is applied to the SCREEN, not to nine widgets. One translate moves
 * the clock, the date, the battery text, the rings and the now-playing card
 * together, so nothing can drift out of alignment with anything else, and no
 * future widget has to remember to join in. Under the shift the screen's own
 * black shows at the trailing edge, which on this panel is unlit rather than
 * grey — the one place the drift costs literally nothing to hide.
 *
 * 4 px is enough: burn-in is driven by the EDGES of a static luminance step, and
 * moving a glyph by a third of its stroke width is what stops one column of
 * pixels carrying the whole duty cycle. Larger would be visible as a jump.
 *
 * The amber is the other half. 0xE8FBFF drives all three subpixels near full;
 * AMOLED power is per-subpixel, blue is both the least efficient primary and
 * the fastest to age, and 0xF59E0B is already what desk-clock mode means on
 * this device (s_lock_ao_ring). Total subpixel drive falls from 738 to 414 for
 * the same legibility. Note that is drive, not measured power — see §7b. */
#define AO_DRIFT_MS 60000    /* one step a minute: slower than anyone watching */
#define AO_DRIFT_PX 4
#define AO_DIM_CLOCK 0xF59E0B

static const int8_t s_ao_drift[8][2] = {
    { 0, -1 }, { 1, -1 }, { 1, 0 }, { 1, 1 },
    { 0,  1 }, { -1, 1 }, { -1, 0 }, { -1, -1 },
};
static int     s_ao_drift_i;
static int64_t s_ao_drift_at;
static uint32_t s_lock_time_col = 0xE8FBFF;   /* what build_lock_screen set */

/* Stops, not a free-running seconds range: "dim after 37 s" is a number nobody
 * chose, and a control that can produce one is worse than seven detents.
 *
 * SECONDS are what gets persisted and the slider index is derived from them at
 * the UI boundary — the same direction chg_mode_to_slider() converts in, for
 * the same reason. Storing the index instead would silently redefine everyone's
 * saved setting on the day a stop is added to this table. */
static const uint16_t s_ao_dim_stops[] = { 10, 20, 30, 60, 120, 300, 600 };
#define AO_DIM_STOPS ((int)(sizeof s_ao_dim_stops / sizeof s_ao_dim_stops[0]))

static int ao_dim_to_slider(int secs) {
    int best = 0;
    for (int i = 1; i < AO_DIM_STOPS; i++) {
        if (abs((int)s_ao_dim_stops[i] - secs) <
            abs((int)s_ao_dim_stops[best] - secs)) best = i;
    }
    return best;
}

/* Pocket lock: the panel stops answering touch while it is asleep, so a cube in
 * a bag or a coat pocket cannot be woken by whatever it is pressed against.
 * Keys still wake it — they are the way back in, and a key is not something a
 * pocket presses for ten minutes at a time.
 *
 * Deliberately NOT persisted, unlike always-on beside it. The lock screen is
 * kept sparse, so this mode has no indicator while the panel is dark and none
 * is possible; a mode you cannot see that survived a reboot is indistinguishable
 * from a broken touchscreen. A power cycle always gives the glass back. */
static volatile bool s_pocket_lock;

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

/* ---------------- bezel pop-out ----------------
 *
 * Press a side key and the black bezel beside it swells into the screen, the
 * way iOS 18 deforms the iPhone's edge. On an AMOLED the lobe is unlit pixels,
 * so it reads as the bezel moving rather than as drawn UI — which also means it
 * is invisible over an already-black screen, exactly as it is on a phone with a
 * black wallpaper. There is nothing to deform there.
 *
 * The whole design is a budget: LVGL renders and ships one strip at a time with
 * no tear gate, and `max_row` comes from the invalidated area's *width*, so a
 * region is a single flush — and structurally incapable of the band-by-band
 * wipe — exactly when `width * height <= 15360` (one 30,720 B draw buffer). A
 * 26x120 lobe is 3,120 px: one render pass, ~0.3 ms on the wire.
 *
 * Flat black, and translated rather than resized or faded. A gradient would
 * band regardless of flush count (RGB565 cannot ramp a dark colour smoothly,
 * HARDWARE.md §5) and a shadow would be re-blurred every frame.
 *
 * The shape is a circle parked tangent to the edge and mostly off-panel, so
 * what shows is a shallow circular segment: a wide, gently curved swell that
 * tapers away at both ends, which is what the bezel actually looks like when it
 * deforms. A rounded rectangle read as a drawn widget; an arc reads as the edge
 * itself moving. Cost stays low because LVGL clips the invalidation to the
 * panel — only the segment's bounding strip is dirty, never the whole circle.
 *
 * Chord width at depth d is 2*sqrt(2Rd - d^2): R=150, d=26 gives a 168 px wide
 * swell in a 300x26 dirty strip = 7,800 px, comfortably one flush. */
#define BEZEL_ARC_R       150      /* circle radius; larger = flatter, wider swell */
#define BEZEL_LOBE_DEEP    26      /* how far it intrudes  */
#define BEZEL_IN_MS       110      /* out fast, so the finger feels like the cause */
#define BEZEL_OUT_MS      190      /* back slower, so it reads elastic */

/* Which LVGL edge the key strip occupies at s_rot == 0: 0 top, 1 right,
 * 2 bottom, 3 left. The three side keys are on the TOP of the cube. Nothing in
 * the repo recorded this; HARDWARE.md §1 now does. */
#define BTN_EDGE_NATIVE     0

/* Where each key sits along that edge, percent of 480, in the silkscreen's
 * leftmost/middle/rightmost order. */
static const uint8_t s_bezel_at[3] = { 32, 50, 68 };

static lv_obj_t *s_bezel[3];
/* The middle lobe can carry a glyph, because unlike the other two that key
 * does something configurable — on the lock screen it opens whichever app
 * CONTROL points it at. Showing that app's symbol as the arc dips is what
 * says the special key fired, and which shortcut it took. A child of the
 * lobe, so it rides the same translate and costs no extra dirty area. */
static lv_obj_t *s_bezel_icon;
static int32_t   s_bezel_v[3];     /* current intrusion in px, 0 = parked */

/* Quarter turns to carry a device-fixed point through for the current panel
 * rotation. Both the edge and the along-edge position go through this, so the
 * two can never disagree.
 *
 * It is a plain s_rot, NOT the inverse. The inverse is the intuitive guess —
 * the panel turns the content, so a feature bolted to the case ought to travel
 * the opposite way — and it is wrong here, because s_rot already counts turns
 * of the panel relative to the case rather than of the case relative to the
 * world. Measured, not reasoned: with the inverse, 0 and 180 were correct while
 * 90 and 270 emerged from the opposite side, which is precisely the failure the
 * MADCTL table warns about. Test all FOUR orientations; two will lie to you. */
static int bezel_rot(void) { return s_rot & 3; }

static int bezel_edge(void) { return (BTN_EDGE_NATIVE + bezel_rot()) & 3; }

static void bezel_apply(int i) {
    lv_obj_t *o = s_bezel[i];
    if (!o) return;
    int32_t v = s_bezel_v[i], tx = 0, ty = 0;
    switch (bezel_edge()) {
        case 0:  ty =  v; break;               /* top    -> push down  */
        case 1:  tx = -v; break;               /* right  -> push left  */
        case 2:  ty = -v; break;               /* bottom -> push up    */
        default: tx =  v; break;               /* left   -> push right */
    }
    /* Change-gated: LVGL style setters invalidate whether or not the value
     * actually moved (HARDWARE.md §5), and this runs every animation step. */
    if (lv_obj_get_style_translate_x(o, 0) != tx) lv_obj_set_style_translate_x(o, tx, 0);
    if (lv_obj_get_style_translate_y(o, 0) != ty) lv_obj_set_style_translate_y(o, ty, 0);
}

/* Native panel point -> current screen point. The panel rotates the content via
 * MADCTL and LVGL's origin never moves, so a device-fixed feature like a side
 * key has to be carried through the same turn by hand. */
static void bezel_rotate(int r, int32_t nx, int32_t ny, int32_t *sx, int32_t *sy) {
    switch (r & 3) {
        case 1:  *sx = 479 - ny; *sy = nx;       break;
        case 2:  *sx = 479 - nx; *sy = 479 - ny; break;
        case 3:  *sx = ny;       *sy = 479 - nx; break;
        default: *sx = nx;       *sy = ny;       break;
    }
}

static void bezel_place(int i) {
    lv_obj_t *o = s_bezel[i];
    if (!o) return;
    int edge = bezel_edge();

    int32_t f = 480 * s_bezel_at[i] / 100, nx, ny;
    switch (BTN_EDGE_NATIVE) {
        case 0:  nx = f;   ny = 0;   break;
        case 1:  nx = 479; ny = f;   break;
        case 2:  nx = f;   ny = 479; break;
        default: nx = 0;   ny = f;   break;
    }
    int32_t cx, cy;
    bezel_rotate(bezel_rot(), nx, ny, &cx, &cy);

    const int32_t r = BEZEL_ARC_R, d = r * 2;
    lv_obj_set_size(o, d, d);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);

    /* Parked exactly tangent to its edge and centred on its key, so at rest not
     * one pixel of it is on the panel. The translate is what dips the arc in. */
    switch (edge) {
        case 0:  lv_obj_set_pos(o, cx - r, -d);    break;   /* tangent to y=0   */
        case 1:  lv_obj_set_pos(o, 480,    cy - r); break;   /* tangent to x=480 */
        case 2:  lv_obj_set_pos(o, cx - r, 480);   break;   /* tangent to y=480 */
        default: lv_obj_set_pos(o, -d,     cy - r); break;   /* tangent to x=0   */
    }
    /* Park the glyph at the arc's deepest point, which is whichever side of
     * the circle faces the panel. */
    if (i == 1 && s_bezel_icon) {
        switch (edge) {
            case 0:  lv_obj_align(s_bezel_icon, LV_ALIGN_BOTTOM_MID, 0, -5); break;
            case 1:  lv_obj_align(s_bezel_icon, LV_ALIGN_LEFT_MID,   5,  0); break;
            case 2:  lv_obj_align(s_bezel_icon, LV_ALIGN_TOP_MID,    0,  5); break;
            default: lv_obj_align(s_bezel_icon, LV_ALIGN_RIGHT_MID, -5,  0); break;
        }
    }
    bezel_apply(i);
}

/* NULL hides it. Set just before the middle lobe pops, from the one place
 * that knows both the current screen and the configured shortcut. */
static void bezel_icon_set(const char *sym) {
    if (!s_bezel_icon) return;
    if (!sym) {
        if (!lv_obj_has_flag(s_bezel_icon, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_bezel_icon, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (strcmp(lv_label_get_text(s_bezel_icon), sym) != 0) {
        lv_label_set_text(s_bezel_icon, sym);
    }
    if (lv_obj_has_flag(s_bezel_icon, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(s_bezel_icon, LV_OBJ_FLAG_HIDDEN);
    }
}

static void bezel_layout(void) { for (int i = 0; i < 3; i++) bezel_place(i); }

static void bezel_anim_exec(void *var, int32_t v) {
    int32_t *slot = (int32_t *)var;
    *slot = v;
    bezel_apply((int)(slot - s_bezel_v));
}

static void bezel_press(int i, bool down) {
    if (i < 0 || i > 2 || !s_bezel[i]) return;
    int32_t to = down ? BEZEL_LOBE_DEEP : 0;
    if (s_bezel_v[i] == to) return;
    lv_anim_delete(&s_bezel_v[i], bezel_anim_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_bezel_v[i]);
    lv_anim_set_exec_cb(&a, bezel_anim_exec);
    lv_anim_set_values(&a, s_bezel_v[i], to);
    lv_anim_set_duration(&a, down ? BEZEL_IN_MS : BEZEL_OUT_MS);
    lv_anim_set_path_cb(&a, down ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_start(&a);
}

/* PWR has no press-down to hook: the AXP2101 hands us a *completed* short press
 * over I2C, so the lobe can only pop and return once the key is already back
 * up. It will read a beat behind the other two, and there is no register that
 * would fix that. */
static void bezel_pop(int i) {
    if (i < 0 || i > 2 || !s_bezel[i]) return;
    lv_anim_delete(&s_bezel_v[i], bezel_anim_exec);
    s_bezel_v[i] = 0;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_bezel_v[i]);
    lv_anim_set_exec_cb(&a, bezel_anim_exec);
    lv_anim_set_values(&a, 0, BEZEL_LOBE_DEEP);
    lv_anim_set_duration(&a, BEZEL_IN_MS);
    lv_anim_set_playback_duration(&a, BEZEL_OUT_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void bezel_init(void) {
    lv_obj_t *top = lv_layer_top();
    for (int i = 0; i < 3; i++) {
        lv_obj_t *o = lv_obj_create(top);
        lv_obj_remove_style_all(o);
        lv_obj_set_style_bg_color(o, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        /* The top layer sits above every screen, so anything clickable here
         * would swallow the tap that unlocks the device — on all of them. */
        lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE);
        s_bezel[i] = o;
        s_bezel_v[i] = 0;
    }
    /* montserrat_14, not the 64 px app icons: the arc is 26 px deep and an
     * app_icons_64 glyph would not fit inside it at any alignment. */
    s_bezel_icon = lv_label_create(s_bezel[1]);
    lv_obj_set_style_text_font(s_bezel_icon, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_bezel_icon, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text(s_bezel_icon, "");
    lv_obj_add_flag(s_bezel_icon, LV_OBJ_FLAG_HIDDEN);
    bezel_layout();
}

/* Defined below bright_apply(), which owns the only path to panel command 0x51
 * (pitfall #23) — so the transition cannot be written inline here. */
static void rot_fade_begin(void);
static void rot_fade_end(void);

static void rotation_apply(int r) {
    r &= 3;
    rot_fade_begin();
    if (!ui_lock()) { rot_fade_end(); return; }

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
    /* Repaint HERE, while the panel is dark. The fade used to guess at the
     * repaint with a fixed delay, which was wrong in both directions: the lock
     * screen can take several hundred ms (a 480x480 wallpaper re-decode when the
     * cache has evicted it) so the black-out read as the screen power-cycling —
     * and a screen slower than the guess had the ramp reveal a half-painted
     * frame, which is the banding this exists to hide. Rendering synchronously
     * makes the dark window exactly one repaint, whatever that costs today. */
    lv_refr_now(NULL);
    bsp_display_unlock();
    rot_fade_end();

    s_rot = r;
    /* The keys are bolted to the device, so their lobes have to be carried
     * round to whichever screen edge the panel just put them on. After s_rot,
     * and under its own lock — they are parked off-panel, so relaying them out
     * here cannot reveal a seam. */
    if (ui_lock()) {
        bezel_layout();
        bsp_display_unlock();
    }
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

static void clock_pref_save(void) {
    if (ble_prov_nvs_blocked()) return;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "clock24", s_clock_24 ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void lock_pref_save(void) {
    if (ble_prov_nvs_blocked()) return;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "alwayson", s_always_on ? 1 : 0);
        nvs_set_i32(h, "lockrings", s_lock_rings ? 1 : 0);
        nvs_set_i32(h, "aoring", s_ao_ring_pref ? 1 : 0);
        /* Seconds, not the slider index — see s_ao_dim_stops. */
        nvs_set_i32(h, "aodim", s_ao_dim_on ? 1 : 0);
        nvs_set_i32(h, "aodimsec", s_ao_dim_s);
        nvs_set_i32(h, "lockmid", s_lock_key_app);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void chg_save(void) {
    if (ble_prov_nvs_blocked()) return;
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "chgmode", s_chg_mode);
        nvs_set_i32(h, "chgonce", s_chg_once ? 1 : 0);
        nvs_set_i32(h, "chgeta", s_chg_eta_on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Runs before pmu_init() so the first CV write is already the user's choice
 * rather than the compiled default corrected a poll later. The armed one-shot
 * is persisted on purpose: a reboot part-way through a top-up should finish it,
 * not silently cancel it. */
static void chg_load(void) {
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "chgmode", &v) == ESP_OK && v >= CHG_FULL && v <= CHG_LIFESPAN) {
        s_chg_mode = (int)v;
    }
    if (nvs_get_i32(h, "chgonce", &v) == ESP_OK) s_chg_once = (v != 0);
    if (nvs_get_i32(h, "chgeta", &v) == ESP_OK) s_chg_eta_on = (v != 0);
    nvs_close(h);
}

/* Both display-orientation settings, on one handle: this runs once at boot. */
static void rot_off_load(void) {
    nvs_handle_t h;
    if (nvs_open("cfg", NVS_READONLY, &h) != ESP_OK) return;
    int32_t v;
    if (nvs_get_i32(h, "rotcfg", &v) == ESP_OK) s_rot_cfg = (int)(v & 7);
    if (nvs_get_i32(h, "autorot", &v) == ESP_OK) s_autorot = (v != 0);
    if (nvs_get_i32(h, "rothold", &v) == ESP_OK) s_rot_held = (int)(v & 3);
    if (nvs_get_i32(h, "clock24", &v) == ESP_OK) s_clock_24 = (v != 0);
    if (nvs_get_i32(h, "alwayson", &v) == ESP_OK) s_always_on = (v != 0);
    if (nvs_get_i32(h, "lockrings", &v) == ESP_OK) s_lock_rings = (v != 0);
    if (nvs_get_i32(h, "aoring", &v) == ESP_OK) s_ao_ring_pref = (v != 0);
    if (nvs_get_i32(h, "aodim", &v) == ESP_OK) s_ao_dim_on = (v != 0);
    /* Clamped to the ends of the stops table rather than trusted. A stored
     * delay is a bare number, and a zero would dim the instant you stopped
     * touching the glass — indistinguishable from a broken panel. */
    if (nvs_get_i32(h, "aodimsec", &v) == ESP_OK) {
        s_ao_dim_s = clampi((int)v, s_ao_dim_stops[0],
                            s_ao_dim_stops[AO_DIM_STOPS - 1]);
    }
    /* Range-checked, not trusted: a stored index can outlive the app it named
     * if one is compiled out, and app_request() on a hole calls a NULL build(). */
    if (nvs_get_i32(h, "lockmid", &v) == ESP_OK) {
        s_lock_key_app = (v >= 0 && v < APP_COUNT) ? (int)v : LOCK_KEY_OFF;
    }
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
        s_app != APP_POMO && s_app != APP_PET) {
        /* PET is pinned like FOCUS, but for the opposite reason: FOCUS reads
         * orientation as its dial; PET reads TILT as its joystick, and a
         * panel that counter-rotated under the tilt would move the ground
         * out from under the pet mid-walk. Whatever orientation the app is
         * entered with holds until the user goes home. */
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
    /* +1 the poll the pin first reads low, -1 the poll it first reads high, 0
     * otherwise. Deliberately un-debounced: the classified events below are
     * 50-70 ms late, which is too slow for a press animation to feel caused by
     * the finger. A bounce can flash the bezel lobe once; it is self-clearing. */
    int8_t raw_edge;
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
    b->raw_edge = 0;
    if (raw != b->last_raw) {
        b->last_raw = raw;
        b->change_ms = t;
        b->raw_edge = (raw == 0) ? 1 : -1;   /* active low; earliest signal there is */
        ESP_LOGI(TAG, "key gpio%d -> %d", (int)b->pin, raw);
        /* A tap can be over in less than the 50 ms the debouncer needs: low on
         * one 20 ms poll, high again by the next. The debounced path then never
         * records a press, so the release completes nothing and the tap simply
         * vanishes — reported from the glass as "an extremely quick press does
         * nothing". The opposite raw edges ARE the tap: an up edge while the
         * debounced state never left released is a completed short press. Real
         * contact bounce settles in under ~10 ms, so a high sample a full poll
         * after a low one means the finger genuinely lifted; and after
         * btn_swallow() stable tracks the held level, so a swallowed wake press
         * still cannot fire on its release. */
        if (raw == 1 && b->stable == 1) return BTN_SHORT;
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
 *
 * 10% is 25/255 out of the 0x51 register, and it was checked on the actual panel
 * through the actual cover glass rather than reasoned about — the slider was
 * dragged to minimum and the screen stayed readable. It is the whole safety
 * argument for the feature, so re-check it rather than trusting this line if the
 * panel, the cover glass or the brightness curve ever changes.
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

/* Desk-clock dim: ONE writer for both halves of it, so the panel level and the
 * wallpaper can never end up disagreeing about which state the cube is in. Same
 * rule as bright_apply() directly above, one level up.
 *
 * The ordering inside each branch looks arbitrary and is not. Hiding or showing
 * a 480x480 image invalidates the entire viewport, which LVGL ships as fifteen
 * sequential strips with no tear gate on this board (HARDWARE.md §5) — a sweep
 * you can watch cross the glass. Doing that repaint while the panel is already
 * at 3-12% is what makes it invisible, exactly as rotation_apply() repaints
 * inside its own blackout. Raising the level first and repainting after would
 * light the sweep instead of hiding it.
 *
 * lv_refr_now() rather than leaving it to the LVGL task, for the same reason
 * rotation_apply() does: the very next statement changes the brightness, and an
 * asynchronous repaint would land on the wrong side of it.
 *
 * s_lock_wall is NULL on every screen but the lock screen, and that is what
 * scopes the blackout. A cube parked on MUSIC by always-on gets the brightness
 * half only, because its content is the thing you asked to keep looking at. */
/* Shift the whole lock screen by (dx, dy).
 *
 * On the CHILDREN, never on the screen — and this is the whole reason the
 * helper exists rather than two setter calls at the call site. A screen is
 * created with lv_obj_create(NULL), so its `parent` is NULL, and
 * lv_obj_refr_pos() returns at `if(parent == NULL) { lv_obj_move_to(...);
 * return; }` (lv_obj_pos.c:781) BEFORE it ever reads translate_x/y twenty lines
 * later. Setting translate on a screen is therefore a silent no-op: no error,
 * no warning, and at 4 px on a dimmed panel nothing an eye could catch either.
 * The first version of this did exactly that.
 *
 * Walking the children rather than naming nine widgets keeps the property that
 * made a whole-screen shift attractive in the first place: everything moves
 * together so nothing can drift out of alignment with anything else, and a
 * widget added to this screen later joins in without knowing the drift exists.
 * That includes the wallpaper, which is hidden while this runs, and the ETA
 * caption, which is not mine.
 *
 * Safe against the now-playing drag: lock_drag_apply() positions the card with
 * lv_obj_set_x(), and translate composes with x rather than replacing it, so
 * the two cannot overwrite each other — and the touch that begins a drag lifts
 * the dim, which zeroes all of this, before the drag can travel. */
static void ao_drift_apply(int dx, int dy) {
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;
    uint32_t n = lv_obj_get_child_count(scr);
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *ch = lv_obj_get_child(scr, i);
        if (!ch) continue;
        lv_obj_set_style_translate_x(ch, dx, 0);
        lv_obj_set_style_translate_y(ch, dy, 0);
    }
}

static void ao_dim_set(bool on) {
    int lvl = s_bright;      /* one read: it is volatile and used three times */
    int dim = clampi(lvl * AO_DIM_PCT / 100, AO_DIM_FLOOR, lvl);

    if (on) {
        /* Called on every 20 ms pass for as long as the cube stays idle, so
         * this both STARTS the fade and ADVANCES it. Position comes from
         * elapsed wall time rather than from a counter, for the reason
         * pomo_set_running() re-stamps its tick: a pass the loop was late for
         * must not stretch the fade, and one bright_apply() that had to be
         * retried must not shorten it. */
        if (!s_ao_dimmed) {
            s_ao_dimmed     = true;
            s_ao_dim_from   = lvl;
            s_ao_dim_lvl    = lvl;
            s_ao_dim_began  = now_ms();
            s_ao_wall_hidden = false;
            ESP_LOGI(TAG, "desk-clock dim: fading %d%% -> %d%%", lvl, dim);
        }

        if (s_ao_dim_lvl > dim) {
            int64_t el = now_ms() - s_ao_dim_began;
            int step = (int)(el * AO_DIM_FADE_STEPS / AO_DIM_FADE_MS);
            if (step > AO_DIM_FADE_STEPS) step = AO_DIM_FADE_STEPS;
            int want = s_ao_dim_from -
                       (s_ao_dim_from - dim) * step / AO_DIM_FADE_STEPS;
            if (want < dim) want = dim;
            /* On a lock timeout, leave s_ao_dim_lvl where it was and come back
             * next pass: the fade resumes at whatever the clock says by then,
             * so a contended panel loses smoothness rather than correctness. */
            if (want != s_ao_dim_lvl && bright_apply(want)) s_ao_dim_lvl = want;
            return;                       /* the blackout waits for the bottom */
        }

        /* Bottom of the fade. The wallpaper goes now rather than at the top,
         * because hiding a 480x480 image invalidates the whole viewport and
         * LVGL ships that as fifteen sequential strips with no tear gate on
         * this board (HARDWARE.md §5) — a sweep you can watch cross the glass.
         * At 3-12% it is invisible, which is the same trick rotation_apply()
         * uses; at full brightness, halfway through a fade, it would not be. */
        if (s_lock_wall && !s_ao_wall_hidden) {
            if (!ui_lock()) return;                    /* retried next pass */
            lv_obj_add_flag(s_lock_wall, LV_OBJ_FLAG_HIDDEN);
            /* Amber goes on in the same pass as the blackout and under the same
             * lock: two writes, one repaint, one moment. Recolouring separately
             * would be a second full-screen invalidate for nothing. */
            if (s_lock_time)
                lv_obj_set_style_text_color(s_lock_time,
                                            lv_color_hex(AO_DIM_CLOCK), 0);
            if (s_screen_on) lv_refr_now(NULL);
            bsp_display_unlock();
            s_ao_wall_hidden = true;
            s_ao_drift_at = now_ms();       /* first step a full period from now */
            ESP_LOGI(TAG, "desk-clock dim ON (panel %d%%, wallpaper off, amber)",
                     dim);
        }

        /* The drift, once settled at the bottom. Deliberately not started until
         * then: a shift during the fade would be one more thing moving while
         * the panel is already changing, and the whole point is to be unnoticed.
         *
         * Whole-screen translate, so this is a full-viewport invalidate — the
         * same fifteen strips the blackout costs. It is affordable for exactly
         * the same reason and no other: it happens at 3-12% brightness, once a
         * minute, on a screen nobody is looking at. Do not lift this to a
         * shorter period without re-reading HARDWARE.md §5. */
        if (s_ao_wall_hidden && now_ms() - s_ao_drift_at >= AO_DRIFT_MS) {
            if (!ui_lock()) return;                    /* retried next pass */
            s_ao_drift_i = (s_ao_drift_i + 1) & 7;
            int dx = s_ao_drift[s_ao_drift_i][0] * AO_DRIFT_PX;
            int dy = s_ao_drift[s_ao_drift_i][1] * AO_DRIFT_PX;
            ao_drift_apply(dx, dy);
            if (s_screen_on) lv_refr_now(NULL);
            bsp_display_unlock();
            s_ao_drift_at = now_ms();
            /* Logged, once a minute and only while dimmed, because this is a
             * feature with no observable output: 4 px on a 12% panel is not
             * something an eye can confirm, and its first version was a silent
             * no-op that no hardware test could have caught (pitfall #36). A
             * line here is the only way anyone can ever tell it is alive. */
            ESP_LOGI(TAG, "desk-clock drift: step %d (%+d,%+d)",
                     s_ao_drift_i, dx, dy);
        }
        return;
    }

    if (!s_ao_dimmed) return;

    /* Coming back. Instant, and it can land mid-fade — the wallpaper may never
     * have gone, which is why the repaint is gated on having actually hidden it
     * rather than on s_lock_wall alone. */
    /* s_lock_wall NULL here is not "no wallpaper", it is app_open() having
     * already nulled it one line before calling us — the screen is about to be
     * freed. Repainting it would spend a full-viewport render, ~80 ms, on a
     * screen nobody will ever see, right inside the switch path. Skipping is
     * also correct rather than merely cheap: the incoming screen is a fresh
     * object, so it carries no translate, and s_lock_time is rebuilt with its
     * own colour. Nothing stale can survive the teardown. */
    if (s_lock_wall && s_ao_wall_hidden) {
        if (!ui_lock()) return;            /* flags untouched: retried next pass */
        lv_obj_remove_flag(s_lock_wall, LV_OBJ_FLAG_HIDDEN);
        /* All three come back under ONE lock and ONE repaint. Undoing them in
         * separate passes would show an amber clock over a restored wallpaper,
         * or worse, leave the screen parked 4 px off if a later pass timed out.
         * The drift in particular must be cleared unconditionally rather than
         * stepped back, because it is absolute, not relative. */
        if (s_lock_time)
            lv_obj_set_style_text_color(s_lock_time,
                                        lv_color_hex(s_lock_time_col), 0);
        ao_drift_apply(0, 0);
        /* Synchronous only while the panel is lit, which is the case this
         * ordering exists for. On the sleep path the screen is already dark and
         * blocking the main loop ~80 ms to render a frame into it would be the
         * opposite of §7b. */
        if (s_screen_on) lv_refr_now(NULL);
        bsp_display_unlock();
    }
    s_ao_dimmed      = false;
    s_ao_wall_hidden = false;
    s_ao_dim_lvl     = -1;

    /* Restoring the level last, so the repaint above happened while the panel
     * was still dim — the sweep is fifteen strips and this is where it hides.
     *
     * Guarded on s_screen_on because screen_toggle_power() drops the dim on its
     * way DOWN, after it has already written 0: restoring the user's level
     * unguarded there would relight a panel one line from dozing, and the wake
     * path writes s_bright itself. A lock timeout hands the write to the main
     * loop rather than losing it — the same retry channel screen_toggle_power()
     * uses, and the reason its gate tests !s_ao_dimmed rather than assuming the
     * panel is wherever the flag says. */
    if (s_screen_on && !bright_apply(lvl)) s_req_bright_apply = true;
    ESP_LOGI(TAG, "desk-clock dim OFF (panel %d%%)", lvl);
}

/* ---------------- rotation transition ----------------
 * The swap/mirror is instant. The full-screen repaint that must follow it is
 * not: 480 rows through 32-row draw buffers is fifteen chunked flushes, and
 * watching them arrive one band at a time is exactly the "slow render" look —
 * the hardware is fast, the *reveal* is what is slow.
 *
 * So do the repaint where nobody can see it. Brightness 0 on an AMOLED is
 * genuinely black, not a dimmed grey, so the panel blanks, the frame lands
 * whole, and the level ramps back.
 *
 * The ramp is an LVGL animation rather than a sleep loop because rotation is
 * applied from the main loop *and* from a UI callback, and blocking the latter
 * for a quarter second would stall every other widget on the screen. */
static int s_rot_fade_to;      /* level to return to; 0 = no transition running */

static void rot_fade_exec(void *var, int32_t v) { (void)var; bright_apply((int)v); }
static void rot_fade_done(lv_anim_t *a)         { (void)a;   s_rot_fade_to = 0; }

static void rot_fade_begin(void) {
    if (!s_screen_on) return;            /* never light a panel that is asleep */
    if (!ui_lock()) return;
    /* Capture what is actually on the glass, not s_bright — FOCUS dims through
     * the same writer, and restoring the user's level here would silently undo
     * it. A second turn arriving mid-ramp must not capture the ramp's own
     * partial value, hence the guard rather than an unconditional read. */
    if (s_rot_fade_to == 0) s_rot_fade_to = s_bright_applied;
    lv_anim_delete(&s_rot_fade_to, rot_fade_exec);
    bsp_display_unlock();
    bright_apply(0);
}

static void rot_fade_end(void) {
    if (!s_screen_on || s_rot_fade_to <= 0) return;
    if (!ui_lock()) { bright_apply(s_rot_fade_to); s_rot_fade_to = 0; return; }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_rot_fade_to);
    lv_anim_set_exec_cb(&a, rot_fade_exec);
    lv_anim_set_completed_cb(&a, rot_fade_done);
    lv_anim_set_values(&a, 0, s_rot_fade_to);
    /* No delay: rotation_apply() rendered synchronously, so the frame is already
     * on the glass when this starts. */
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    bsp_display_unlock();
}

/* Navigation is always deferred to the main task, so it can afford a very
 * short hardware fade-out without blocking touch handling.  The new screen is
 * then rendered completely at brightness zero and shares rot_fade_end()'s
 * non-blocking reveal. */
static bool nav_fade_begin(void) {
    if (!s_screen_on || s_bright_applied <= 0) return false;
    if (!ui_lock()) return false;

    int from = s_bright_applied;
    if (s_rot_fade_to == 0) s_rot_fade_to = from;
    lv_anim_delete(&s_rot_fade_to, rot_fade_exec);
    bsp_display_unlock();

    for (int step = 4; step >= 0; step--) {
        bright_apply((from * step) / 5);
        if (step) vTaskDelay(pdMS_TO_TICKS(8));
    }
    return true;
}

/* ---------------- screen switching ---------------- */

static void screen_toggle_power(void) {
    /* INVARIANT: the panel only ever sleeps from the lock screen.
     *
     * Nothing enforces this — it holds because auto-lock always moves to APP_LOCK
     * before the sleep timer can fire, so no app is ever on screen when the panel
     * goes dark. Every consumer quietly depends on it: the wake path logs "stays
     * locked" and does nothing but light the panel, which is only correct if the
     * lock screen is already what is behind it.
     *
     * That is why an app owning the whole screen is a redesign rather than a flag
     * (docs/MULTI-IMAGE.md) — it would violate a rule that was never written down.
     * Until then, warn rather than assert: a wrong screen after wake is a
     * confusing bug to chase from the symptom, and one log line names it. */
    if (s_screen_on && s_app != APP_LOCK) {
        ESP_LOGW(TAG, "panel sleeping from app %d, not the lock screen — "
                      "wake will land there; see docs/MULTI-IMAGE.md", s_app);
    }
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
        /* Drop the desk-clock dim on the way down, and drop it HERE: after the
         * flip, so ao_dim_set() skips its brightness write, and after the panel
         * is already at 0, so putting the wallpaper back costs one repaint that
         * nobody can see rather than a flash of the photo returning.
         *
         * It has to happen somewhere on this path. A dim that survived the
         * sleep would wake to a black lock screen at full brightness — read
         * from the glass as "my wallpaper disappeared" — and worse, the flag
         * would then agree with the panel, so nothing would ever re-dim it.
         * That is pitfall #23's shape with the wallpaper as the second output.
         *
         * The wake branch needs no equivalent: it can only be reached from
         * here, so the flag is already false by the time the panel lights. */
        ao_dim_set(false);
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

/* ---- themes and worlds: the design the phone chose, drawn ----
 *
 * A world contributes the stage (sky, ground, props); a theme contributes the
 * character and UI. The two mono themes (retro LCD, ink) override everything
 * with a two-tone ramp — that IS the aesthetic — so every color in the scene
 * routes through pet_col() and nothing hardcodes a hex. */
enum {
    PC_SKY = 0, PC_SKY2, PC_GROUND, PC_GROUND_HI, PC_SPOT, PC_STAR,
    PC_BODY, PC_BODY_DARK, PC_EYE, PC_ACCENT, PC_TEXT, PC_DIM, PC_PROP,
};

typedef struct {
    uint32_t sky, sky2, ground, ground_hi, spot, star, prop;
    bool moon;                          /* some skies have one, some do not */
} pet_world_pal_t;
static const pet_world_pal_t s_pet_world_pal[4] = {
    /* tiny planet  */ { 0x0A0F22, 0x241B3A, 0x2A9D8F, 0x6FD8C8, 0x21857A, 0xFFF3D6, 0xB8C4D9, true  },
    /* ocean floor  */ { 0x041830, 0x0A2E4E, 0xC2B280, 0xE0D2A0, 0xA89868, 0x8DE0D2, 0xFF8C69, false },
    /* forest glade */ { 0x0F1A2E, 0x16283A, 0x2E7D32, 0x66BB6A, 0x1B5E20, 0xE8F6B8, 0xC9A227, true  },
    /* city rooftop */ { 0x0B1020, 0x1B1430, 0x37474F, 0x546E7A, 0x263238, 0xE9C46A, 0xB0BEC5, true  },
};

typedef struct {
    uint32_t body, body_dark, eye, accent, text, dim;
    bool mono;
    uint32_t mono_bg, mono_fg, mono_mid;
} pet_theme_pal_t;
static const pet_theme_pal_t s_pet_theme_pal[4] = {
    /* modern    */ { 0xF4F1DE, 0x264653, 0x8DE0D2, 0xE76F51, 0xF4F1DE, 0x9AA7B8, false, 0, 0, 0 },
    /* retro lcd */ { 0, 0, 0, 0, 0, 0, true,  0x9BBC0F, 0x0F380F, 0x306230 },
    /* ink       */ { 0, 0, 0, 0, 0, 0, true,  0xF2F2F2, 0x141414, 0x6E6E6E },
    /* neon      */ { 0x00E5FF, 0x0077AA, 0x001018, 0xFF2D95, 0x00E5FF, 0x557788, false, 0x000000, 0, 0 },
};

static uint32_t pet_col(int role) {
    const pet_world_pal_t *w = &s_pet_world_pal[s_pet.world & 3];
    const pet_theme_pal_t *t = &s_pet_theme_pal[s_pet.theme & 3];
    if (t->mono) {
        switch (role) {
        case PC_SKY: case PC_SKY2: case PC_EYE:      return t->mono_bg;
        case PC_GROUND: case PC_BODY: case PC_TEXT:  return t->mono_fg;
        default:                                     return t->mono_mid;
        }
    }
    switch (role) {
    case PC_SKY:       return (s_pet.theme == 3) ? 0x000000 : w->sky;
    case PC_SKY2:      return (s_pet.theme == 3) ? 0x0A0018 : w->sky2;
    case PC_GROUND:    return w->ground;
    case PC_GROUND_HI: return w->ground_hi;
    case PC_SPOT:      return w->spot;
    case PC_STAR:      return w->star;
    case PC_PROP:      return w->prop;
    case PC_BODY:      return t->body;
    case PC_BODY_DARK: return t->body_dark;
    case PC_EYE:       return t->eye;
    case PC_ACCENT:    return t->accent;
    case PC_TEXT:      return t->text;
    default:           return t->dim;
    }
}

/* the ground and sky props the quake rattles; the planet stays put because
 * translating a 540 px circle invalidates a full-width band per frame */
static int s_pet_quake;             /* frames of world-shake left */
static int s_pet_quake_amp;

/* The ground is a circle whose top cap forms the horizon — and the RADIUS is
 * what makes a world a world. The pocket planet curves hard; the seabed and
 * the rooftop are so large they read as flat with sky to spare. Same math,
 * four geographies, which is what "the worlds all look the same" was missing:
 * under the mono themes palette differences vanish, so the ground SHAPE has
 * to carry the difference. */
#define PLANET_CX    240
#define WALK_MIN_X   120
#define WALK_MAX_X   360

typedef struct { int16_t horizon; int16_t r; } pet_geo_t;
static const pet_geo_t s_world_geo[4] = {
    /* tiny planet  */ { 240, 270 },     /* the classic high curvature */
    /* ocean floor  */ { 300, 1500 },    /* long low seabed, water above */
    /* forest glade */ { 276, 700 },     /* one rolling hill */
    /* city rooftop */ { 312, 2400 },    /* a slab; the sky is the view */
};
static int pet_geo_r(void)  { return s_world_geo[s_pet.world & 3].r; }
static int pet_geo_cy(void) {
    const pet_geo_t *g = &s_world_geo[s_pet.world & 3];
    return g->horizon + g->r;
}

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

/* the egg (stage PET_EGG) sits where the astronaut will stand */
static lv_obj_t *s_pet_egg;

/* ---- infinite travel ----
 *
 * Hold a tilt and the WORLD moves, not the walker: ground details and props
 * ride a 640 px virtual track around the planet (the stretch beyond the
 * panel maps below the horizon via ground_y, so things genuinely disappear
 * around the back), stars parallax at quarter speed, the moon at an eighth.
 * The walker eases to centre stage and just walks. Velocity ramps toward
 * the tilt instead of snapping — momentum is most of the fidget. */
#define WOBJ_N     12
#define WOBJ_WRAP  640
static lv_obj_t *s_wobj[WOBJ_N];
static int16_t   s_wobj_x[WOBJ_N];     /* virtual track x of the centre */
static int16_t   s_wobj_dy[WOBJ_N];    /* y offset from the surface line */
static uint8_t   s_wobj_n;
static int s_tilt_vel, s_tilt_vel_tgt; /* world-scroll velocity, -6..6 */
static int s_sky_par;                  /* sky parallax accumulator */

static void wobj_add(lv_obj_t *o, int track_x, int dy) {
    if (s_wobj_n >= WOBJ_N) return;
    s_wobj[s_wobj_n] = o;
    s_wobj_x[s_wobj_n] = (int16_t)track_x;
    s_wobj_dy[s_wobj_n] = (int16_t)dy;
    s_wobj_n++;
}

/* ---- the encounter reel: what the road serves up ----
 * Variable-ratio payouts on the infinite walk: crystals to walk through,
 * a stranger now and then, a golden flyby that drops a little treasure,
 * and every full lap a small ceremony. The road must keep paying, or the
 * walk is a treadmill. */
#define PET_LAP_M 500
static lv_obj_t *s_gem[2];
static int16_t   s_gem_x[2];
static bool      s_gem_on[2];
static uint32_t  s_gem_next_m;
static lv_obj_t *s_walker;             /* the stranger */
static int16_t   s_walker_x;
static int8_t    s_walker_dir;
static bool      s_walker_on, s_walker_waved;
static uint32_t  s_walker_next_m;
static int       s_travel_acc;         /* px toward the next metre */
static bool      s_ufo_gold, s_ufo_dropped;
static bool      s_was_traveling;
static char      s_trav_hud[40];
static lv_obj_t *s_pet_planet;         /* the ground, for tinting and liftoff */
static lv_obj_t *s_sign, *s_sign_label;   /* the distance signpost */
static int16_t   s_sign_x;
static bool      s_sign_on;

/* ---- jetpack: pitch the cube back and the sky opens ----
 * Ground hides, stars stream downward, altitude climbs with the pitch;
 * level out and you parachute home. The one number that persists is the
 * altitude record — a reason to try again tomorrow. */
static lv_obj_t *s_jet_flame;
static int  s_fly_mode;                /* 0 ground, 1 climbing, 2 descending */
static int  s_fly_alt, s_fly_peak;
static int  s_fly_pitch;               /* signed z-delta, written at 50 Hz */
static int  s_lean_mag;                /* current L/R lean, for the level gate */
/* Lift-burst events from the 50 Hz vertical-velocity sense: +1 the cube was
 * raised sharply, -1 lowered sharply. Written by the motion poll, consumed
 * by the view's flight machine. */
static volatile int8_t s_fly_burst;
static int64_t s_shake_ms;             /* last shake, file-scope so the fly
                                        * trigger can ignore shake spikes */
static int16_t s_gem_y[2];             /* sky-gem drop height while flying */
static bool    s_gem_sky[2];
/* star home rows, shared by the build and the flight streamer */
static const uint16_t s_star_by[STAR_N] = { 96, 52, 112, 40, 78, 34,
                                            96, 130, 168, 150, 168, 74 };

/* character parts */
static lv_obj_t *s_ch_wrap, *s_ch_body, *s_ch_pack, *s_ch_visor;
static lv_obj_t *s_ch_eye_l, *s_ch_eye_r, *s_ch_leg_l, *s_ch_leg_r;
static lv_obj_t *s_ch_arm_l, *s_ch_arm_r, *s_ch_ant, *s_ch_antdot;
static lv_obj_t *s_bubble;
static int s_bubble_life;

/* HUD */
static lv_obj_t *s_pet_name, *s_pet_mood;
static lv_obj_t *s_bar_food, *s_bar_fun, *s_bar_nrg;
/* Change-gate memory for the HUD labels; cleared on every scene build so a
 * fresh label is never left showing its placeholder by a stale gate. */
static char s_pet_prev_mood[48], s_pet_prev_name[24];

/* The designer QR overlay — tap the pet's name to open it. Same machinery as
 * the DAYS edit QR: the link is minted by the broker, single-use, and the
 * phone lands on /pet already holding a session code. */
static lv_obj_t *s_pet_qr_panel, *s_pet_qr, *s_pet_qr_note;
static lv_obj_t *s_pet_qr_btn, *s_pet_qr_btn_l, *s_pet_qr_tick;
static char s_pet_link_drawn[256];
static uint32_t s_pet_link_seen_ver = UINT32_MAX;
/* The panel's little ceremony: show the code, sync on demand with a visible
 * wait, then a green tick or a red cross — never a white screen that just
 * disappears and leaves the user guessing whether anything happened. */
enum { PET_QR_SHOWING = 0, PET_QR_SYNCING, PET_QR_DONE, PET_QR_FAIL };
static uint8_t s_pet_qr_state;

static int isin(int deg, int amp) {
    while (deg < 0) deg += 360;
    return (int)((lv_trigo_sin((int16_t)(deg % 360)) * amp) / 32767);
}

static int rnd(int n) { return (int)(esp_random() % (uint32_t)n); }

/* surface height under a given x */
static int ground_y(int x) {
    int dx = x - PLANET_CX;
    int r = pet_geo_r();
    int r2 = r * r - dx * dx;
    if (r2 < 0) r2 = 0;
    return pet_geo_cy() - (int)sqrtf((float)r2);
}

static const char *pet_mood_text(int *worst) {
    int m = s_pet.hunger;
    const char *s = "hungry";
    if (s_pet.happy < m) { m = s_pet.happy; s = "bored"; }
    if (s_nrg < m) { m = s_nrg; s = "sleepy"; }
    *worst = m;
    /* An unanswered cue overrides everything: it is the ask on the clock. */
    if (s_pet.cue == PET_CUE_HUNGRY) return "feed me?";
    if (s_pet.cue == PET_CUE_LONELY) return "play with me?";
    if (m > 60) return "having a good day";
    if (m > 30) return s;
    return (m == s_pet.hunger) ? "starving!" : (m == s_pet.happy ? "lonely!" : "exhausted!");
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
/* Nothing to feed, play with, or tuck in before hatching or after leaving —
 * these also arrive via the long-press key path, not just the hidden buttons. */
static bool pet_absent(void) {
    return s_pet.stage == PET_EGG || s_pet.stage == PET_AWAY;
}

/* ---- motion controls: the cube itself is the joystick ----
 *
 * Tilt the cube and the pet walks downhill toward the dipped edge, faster
 * the steeper the lean; shake it and the pet is knocked into a hop. Runs at
 * the main loop's PET-mode 50 Hz IMU cadence, only while PET is the active
 * app with the screen awake.
 *
 * The first version read the lean from s_base_rot and was dead on arrival:
 * the base detector only flips once an axis DOMINATES (45 degrees plus the
 * anti-ambiguity margin), so the pet ignored every tilt short of tipping the
 * cube onto its edge. This one reads the gravity COMPONENT along the screen
 * edge directly — the direction-to-axis mapping still goes through
 * rot_from_base(), the same calibrated table autorotate trusts, so all
 * eight mounting states work. A tilt whose dipped edge would autorotate the
 * panel to (s_rot + 3) & 3 is one dipping the screen's left edge; PET pins
 * the panel rotation (see imu_poll) precisely so that mapping cannot change
 * under the tilt mid-walk. */
#define PET_LEAN_TH      4200   /* ~0.26 g: a deliberate ~15 degree tip */
#define PET_LEAN_RELEASE 3000   /* hysteresis so a wobble does not stutter */

static int pet_base_accel(int b) {  /* accel component toward base edge b */
    switch (b & 3) {
    case 1:  return s_acc_x;
    case 3:  return -s_acc_x;
    case 2:  return s_acc_y;
    default: return -s_acc_y;       /* base 0 */
    }
}

/* Gravity along the screen edge that autorotate would call rotation
 * (s_rot + rot_delta): +1 is the screen's left edge, +3 its right. Derived
 * as +3/+1 from the bezel's edge arithmetic and shipped inverted — the
 * mapping has exactly one free handedness bit, and the user's hands settled
 * it in one test where four orientations of derivation kept lying. */
static int pet_lean_signal(int rot_delta) {
    int want = (s_rot + rot_delta) & 3;
    for (int b = 0; b < 4; b++)
        if (rot_from_base(b) == want) return pet_base_accel(b);
    return 0;
}

static void pet_motion_poll(void) {
    static int16_t px, py, pz;
    static bool primed;
    static int8_t logged_dir;

    if (pet_absent() || !s_screen_on || s_doze || !s_ch_wrap) {
        primed = false;
        logged_dir = 0;
        s_tilt_vel_tgt = 0;
        return;
    }

    int64_t t = now_ms();
    int jerk = 0;
    if (primed)
        jerk = abs(s_acc_x - px) + abs(s_acc_y - py) + abs(s_acc_z - pz);
    px = s_acc_x; py = s_acc_y; pz = s_acc_z;
    primed = true;

    /* A hop needs a genuine shake, not a brisk tilt. A tilt gesture crosses
     * any single-sample threshold for a poll or two on its way over, and on
     * the glass that read as "tilting makes it jump". A real shake is
     * violence SUSTAINED: the hot counter charges +2 per high-jerk poll and
     * drains -1 per calm one, so it only reaches the trigger after ~80 ms of
     * continuous thrash, and the peak requirement demands the thrash was
     * actually hard. Both gates were tuned from the user's own play capture:
     * deliberate shakes peaked 7-36k, tilt transients under 10k briefly. */
    static int shake_hot, shake_peak;
    if (jerk > 5500) {
        if (shake_hot < 20) shake_hot += 2;
        if (jerk > shake_peak) shake_peak = jerk;
    } else if (shake_hot > 0) {
        if (--shake_hot == 0) shake_peak = 0;
    }
    if (shake_hot >= 8 && shake_peak > 12000 && t - s_shake_ms > 1200) {
        s_shake_ms = t;
        lv_display_trigger_activity(NULL);
        set_act(ACT_JUMP, 34);
        /* the world gets it too: harder shake, bigger rattle */
        s_pet_quake = 24;
        s_pet_quake_amp = clampi(shake_peak / 2200, 5, 16);
        say(shake_peak > 22000 ? "earthquake!!" : "wobble!");
        s_pet.happy = clampi(s_pet.happy + 2, 0, 100);
        s_pet_dirty = true;
        pet_seen();
        ESP_LOGI(TAG, "pet imu: shake peak=%d", shake_peak);
        shake_hot = 0;
        shake_peak = 0;
    }

    /* While the hand is accelerating the cube, the accelerometer measures
     * the hand, not gravity — a forward tilt with a bit of right in it read
     * as whatever the motion happened to look like. Steer only from quiet
     * samples; a held tilt is quiet the moment the wrist stops. */
    if (jerk > 4500) return;

    /* A shake IS a violent transient lean; let it be a hop, not a sprint.
     * The window also swallows the rebound of the hand arresting the shake. */
    if (t - s_shake_ms < 400) return;

    /* A cube PARKED on its side is not a command. When the accelerometer has
     * been rock-still for ~4 s, whatever pose it rests in becomes the new
     * level, so a cube leaned against a book stops pinning the pet to a wall
     * — while a hand-held tilt (never still) steers relative to level.
     * Baselines seed from the first sample after the app opens: without
     * that, a cube lying screen-up reads a permanent full-scale pitch and
     * the jetpack fires itself on open. */
    static int base_l, base_r, base_z;
    static int gx_lp, gy_lp, gz_lp;    /* slow gravity estimate, LSB */
    static bool based;
    static int still_polls;
    int raw_l = pet_lean_signal(1), raw_r = pet_lean_signal(3);
    if (!based) {
        based = true;
        /* Only Z seeds from the live pose: flat-on-desk and upright grips
         * legitimately differ there and absolute zero means nothing. The
         * L/R baselines MUST stay absolute — seeding them from the opening
         * pose once declared a tilted pickup "level" and gave the pet a
         * permanent phantom lean (L=5483 on a level grip, straight from the
         * log) that also sat on the jetpack's level-gate and refused every
         * launch. Level is level; only the parked-still adaptation below
         * may redefine it. */
        base_l = 0;
        base_r = 0;
        base_z = s_acc_z;
        gx_lp = s_acc_x;
        gy_lp = s_acc_y;
        gz_lp = s_acc_z;
    }
    if (jerk < 900) {
        if (still_polls < 200) still_polls++;
        if (still_polls >= 200) {                /* adopt the resting pose */
            base_l += (raw_l - base_l) / 8;
            base_r += (raw_r - base_r) / 8;
            base_z += (s_acc_z - base_z) / 8;
        }
    } else {
        still_polls = 0;
    }
    /* how far the screen has pitched from its level — the flight trigger
     * only uses this as a "roughly level grip" gate now */
    s_fly_pitch = s_acc_z - base_z;

    /* ---- vertical-motion sense: the jetpack's real trigger ----
     * True altitude cannot survive double integration (accelerometer noise
     * drifts metres in seconds, and this board has no barometer), but a
     * LIFT is unmistakable: project acceleration onto the low-passed
     * gravity direction, subtract gravity, and leaky-integrate. A sharp
     * raise of the whole cube swings the integral hard positive; lowering
     * the hand swings it negative; a shake alternates and cancels. The
     * integral leaks to zero in ~0.3 s, so it cannot drift. */
    gx_lp += (s_acc_x - gx_lp) / 16;
    gy_lp += (s_acc_y - gy_lp) / 16;
    gz_lp += (s_acc_z - gz_lp) / 16;
    static int s_vv;
    float gxl = gx_lp, gyl = gy_lp, gzl = gz_lp;
    float gmag = sqrtf(gxl * gxl + gyl * gyl + gzl * gzl);
    if (gmag > 2000.f) {
        float av = ((float)s_acc_x * gxl + (float)s_acc_y * gyl +
                    (float)s_acc_z * gzl) / gmag - gmag;
        s_vv += (int)av;
        s_vv -= s_vv / 16;
        if (s_fly_mode == 0) {
            /* liftoff wants a sharp raise from a roughly level grip —
             * generous tolerance, hands are not spirit levels. Near-misses
             * log too (throttled), so tuning works from the owner's own
             * lifts instead of another guess. */
            static int64_t last_miss_log;
            if (s_vv > 14000 && s_lean_mag < 6500 &&
                abs(s_fly_pitch) < 8000 && t - s_shake_ms > 600) {
                s_fly_burst = 1;
                ESP_LOGI(TAG, "pet imu: lift v=%d", s_vv);
                s_vv = 0;
            } else if (s_vv > 9000 && t - last_miss_log > 1500) {
                last_miss_log = t;
                ESP_LOGI(TAG, "pet imu: lift near-miss v=%d lean=%d pitch=%d",
                         s_vv, s_lean_mag, abs(s_fly_pitch));
            }
        } else {
            if (s_vv > 12000)       { s_fly_burst = 1;  s_vv = 0; }
            else if (s_vv < -12000) { s_fly_burst = -1; s_vv = 0; }
        }
    }

    int left = raw_l - base_l, right = raw_r - base_r;
    int mag = left > right ? left : right;
    s_lean_mag = mag > 0 ? mag : 0;
    int dir = 0;
    if (left > PET_LEAN_TH && left >= right)  dir = -1;
    else if (right > PET_LEAN_TH)             dir = 1;

    if (dir != 0) {
        /* Motion play IS attention: without this, sixty seconds of pure
         * tilt-steering — no touch, no keys — auto-locked the app out from
         * under the player, and every control then looked dead at once.
         * FOCUS holds itself awake the same way. */
        lv_display_trigger_activity(NULL);
        /* The tilt sets a target VELOCITY for the world, not a destination
         * for the walker: the timer eases toward it and streams the planet
         * underneath — the infinite walk. Steeper is faster. */
        s_tilt_vel_tgt = dir * clampi(2 + (mag - PET_LEAN_TH) / 2500, 2, 6);
        if (dir != logged_dir) {         /* diagnostics on change, not cadence */
            logged_dir = (int8_t)dir;
            ESP_LOGI(TAG, "pet imu: lean %s (L=%d R=%d)",
                     dir < 0 ? "left" : "right", left, right);
        }
        pet_seen();
    } else if (mag < PET_LEAN_RELEASE) {
        logged_dir = 0;
        s_tilt_vel_tgt = 0;              /* momentum decays in the timer */
    }
}

static void pet_feed(void) {
    if (pet_absent()) return;
    /* A meal: most of it lands now, the rest when the snack is caught and
     * eaten (ACT_EAT), totalling the meal's +30. Meals reset the snack run. */
    s_pet.hunger = clampi(s_pet.hunger + 22, 0, 100);
    s_pet.snack_run = 0;
    s_pet_dirty = true;
    pet_seen();
    drop_food();
    log_event("pet fed");
}
static void pet_play(void) {
    if (pet_absent()) return;
    s_pet.happy = clampi(s_pet.happy + 20, 0, 100);
    s_nrg = clampi(s_nrg - 8, 0, 100);
    s_pet_dirty = true;
    pet_seen();
    set_act(ACT_DANCE, 140);
    say("yay!");
    log_event("pet danced");
}
static void pet_rest(void) {
    if (pet_absent()) return;
    s_nrg = clampi(s_nrg + 22, 0, 100);
    s_pet_dirty = true;
    pet_seen();
    set_act(ACT_NAP, 200);
    say("zzz");
    log_event("pet napped");
}
/* Decay, aging, evolution and cues all live in pet_engine_service() now,
 * beside the blob — the engine runs whichever screen is showing. */

/* tap empty space to send it there, tap the astronaut to pet it,
 * double-tap anywhere to make it dance */
static void scene_tap_cb(lv_event_t *e) {
    static int64_t last_tap;
    int64_t now = now_ms();
    bool dbl = (now - last_tap) < 400;
    last_tap = now;

    if (pet_absent()) {
        /* Tapping the egg is acknowledged — a shiver and a murmur — but an
         * egg cannot be hurried, and a departed pet is not here to answer. */
        if (s_pet.stage == PET_EGG) say("...!");
        pet_seen();
        return;
    }

    if (dbl) {
        s_pet.happy = clampi(s_pet.happy + 8, 0, 100);
        s_pet_dirty = true;
        pet_seen();
        set_act(ACT_DANCE, 140);
        say("wheee!");
        return;
    }

    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    if (abs(p.x - s_cx) < 46 && p.y > 180) {          /* on the astronaut */
        s_pet.happy = clampi(s_pet.happy + 4, 0, 100);
        s_pet_dirty = true;
        pet_seen();
        set_act(ACT_WAVE, 70);
        say("hehe");
    } else {
        walk_to(p.x);
    }
}

static bool home_gesture_from_bottom(lv_indev_t *indev);

/* swipe: up = jump, left/right = dash; bottom-edge up = home */
static void pet_gesture_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (pet_absent()) {
        if (home_gesture_from_bottom(indev)) {
            lv_indev_wait_release(indev);
            app_request(APP_DRAWER);
        }
        return;
    }
    switch (lv_indev_get_gesture_dir(indev)) {
    case LV_DIR_TOP:
        if (home_gesture_from_bottom(indev)) {
            lv_indev_wait_release(indev);
            app_request(APP_DRAWER);
        } else {
            set_act(ACT_JUMP, 34);
            say("hop!");
        }
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

/* HUD refresh, change-gated: lv_label_set_text has no equality short-circuit,
 * so an unconditional rewrite here is a flush per tick forever. The bars gate
 * themselves (lv_bar ignores an unchanged value). */
static void pet_hud_refresh(const char *mood) {
    if (s_fcount % 10) return;
    char name[24];
    uint32_t age_d = s_pet.hatched_utc
        ? (uint32_t)((time(NULL) - s_pet.hatched_utc) * PET_TIME_SCALE / 86400)
        : 0;
    if (s_pet.stage == PET_ADULT)
        snprintf(name, sizeof(name), "%s  %lud", s_pet.name, (unsigned long)age_d);
    else
        snprintf(name, sizeof(name), "%s  %s", s_pet.name, pet_stage_word());
    if (strcmp(mood, s_pet_prev_mood) != 0) {
        snprintf(s_pet_prev_mood, sizeof(s_pet_prev_mood), "%s", mood);
        lv_label_set_text(s_pet_mood, mood);
    }
    if (strcmp(name, s_pet_prev_name) != 0) {
        snprintf(s_pet_prev_name, sizeof(s_pet_prev_name), "%s", name);
        lv_label_set_text(s_pet_name, name);
    }
    lv_bar_set_value(s_bar_food, s_pet.hunger, LV_ANIM_OFF);
    lv_bar_set_value(s_bar_fun,  s_pet.happy,  LV_ANIM_OFF);
    lv_bar_set_value(s_bar_nrg,  s_nrg,  LV_ANIM_OFF);
}

/* Mirrors days_timer_cb's QR section: redraw the code only when the link
 * actually changed, show honest text while it is being minted or failing —
 * and run the sync button's little state machine on top. */
static void pet_qr_refresh(void) {
    if (!s_pet_qr_panel || lv_obj_has_flag(s_pet_qr_panel, LV_OBJ_FLAG_HIDDEN))
        return;

    if (s_pet_qr_state == PET_QR_SYNCING) {
        static const char *dots[] = { "SYNCING", "SYNCING.", "SYNCING..", "SYNCING..." };
        lv_label_set_text(s_pet_qr_btn_l, dots[(s_fcount / 12) & 3]);
        uint8_t res = s_pet_cfg_result;
        if (res == 1) {
            s_pet_qr_state = PET_QR_DONE;
            lv_obj_add_flag(s_pet_qr, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_pet_qr_btn, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_pet_qr_tick, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_pet_qr_tick, lv_color_hex(0x2E9E5B), 0);
            lv_label_set_text(lv_obj_get_child(s_pet_qr_tick, 0), LV_SYMBOL_OK);
            lv_label_set_text(s_pet_qr_note,
                              "SAVED TO YOUR CUBE\nTap anywhere to see it");
        } else if (res == 2) {
            s_pet_qr_state = PET_QR_FAIL;
            lv_obj_remove_flag(s_pet_qr_tick, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_pet_qr_tick, lv_color_hex(0xC0392B), 0);
            lv_label_set_text(lv_obj_get_child(s_pet_qr_tick, 0), LV_SYMBOL_CLOSE);
            lv_label_set_text(s_pet_qr_btn_l, "TRY AGAIN");
            lv_label_set_text(s_pet_qr_note,
                              "SYNC FAILED\nCheck Wi-Fi, then try again");
        }
        return;
    }
    if (s_pet_qr_state != PET_QR_SHOWING) return;

    char url[PET_LINK_URL_MAX];
    uint32_t version;
    pet_link_snapshot(url, sizeof(url), &version);
    bool fetching = __atomic_load_n(&s_pet_link_fetching, __ATOMIC_ACQUIRE) ||
                    __atomic_load_n(&s_req_pet_link, __ATOMIC_ACQUIRE);
    if (url[0] && s_pet_qr &&
        (version != s_pet_link_seen_ver || strcmp(url, s_pet_link_drawn) != 0)) {
        size_t n = strlen(url);
        if (lv_qrcode_update(s_pet_qr, url, (uint32_t)n) == LV_RESULT_OK) {
            snprintf(s_pet_link_drawn, sizeof(s_pet_link_drawn), "%s", url);
            lv_obj_remove_flag(s_pet_qr, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_pet_qr_note,
                              "SCAN, THEN DESIGN ON YOUR PHONE\n"
                              "Saved there? Press the button below.");
        } else {
            lv_obj_add_flag(s_pet_qr, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(s_pet_qr_note, "QR ENCODE FAILED\nTap to close and retry");
            ESP_LOGW(TAG, "pet: designer QR encode failed (%u bytes)", (unsigned)n);
        }
        s_pet_link_seen_ver = version;
    } else if (!url[0]) {
        lv_obj_add_flag(s_pet_qr, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_pet_qr_note, fetching ? "CREATING SECURE LINK..." :
                          "BROKER UNAVAILABLE\nTap to close and retry");
        s_pet_link_seen_ver = version;
    }
}

static void pet_qr_open_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);        /* the name sits over the tap-to-walk scene */
    if (!s_pet_qr_panel) return;
    pet_link_publish("");
    s_pet_link_drawn[0] = '\0';
    s_pet_link_seen_ver = UINT32_MAX;
    s_pet_qr_state = PET_QR_SHOWING;
    lv_obj_add_flag(s_pet_qr, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_pet_qr_tick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pet_qr_btn, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_pet_qr_btn_l, "I SAVED - SYNC NOW");
    lv_label_set_text(s_pet_qr_note, "CREATING SECURE LINK...");
    lv_obj_remove_flag(s_pet_qr_panel, LV_OBJ_FLAG_HIDDEN);
    __atomic_store_n(&s_req_pet_link, true, __ATOMIC_RELEASE);
}

static void pet_qr_sync_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_pet_qr_state == PET_QR_SYNCING) return;
    s_pet_qr_state = PET_QR_SYNCING;
    s_pet_cfg_result = 0;
    lv_obj_add_flag(s_pet_qr_tick, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_pet_qr_note, "TALKING TO THE BROKER...");
    __atomic_store_n(&s_req_pet_cfg, true, __ATOMIC_RELEASE);
}

static void pet_qr_close_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    /* Mid-sync the panel holds still: dismissing a spinner is how results
     * get lost. It resolves to a tick or a cross within seconds. */
    if (s_pet_qr_state == PET_QR_SYNCING) return;
    if (s_pet_qr_panel) lv_obj_add_flag(s_pet_qr_panel, LV_OBJ_FLAG_HIDDEN);
    /* Closing without pressing SYNC still fetches — a safety net for the
     * user who saved on the phone and just taps the panel away. */
    if (s_pet_qr_state == PET_QR_SHOWING) {
        __atomic_store_n(&s_req_pet_cfg, true, __ATOMIC_RELEASE);
        say("syncing...");
    }
    s_pet_qr_state = PET_QR_SHOWING;
}

static void pet_timer_cb(lv_timer_t *t) {
    if (!s_ch_wrap) return;
    s_fcount++;
    s_aframe++;

    pet_qr_refresh();

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
        /* a golden one drops its treasure as it passes overhead */
        if (s_ufo_gold && !s_ufo_dropped && s_ufo_x > 150) {
            s_ufo_dropped = true;
            for (int g = 0; g < 2; g++) {
                if (s_gem_on[g]) continue;
                s_gem_on[g] = true;
                s_gem_x[g] = (int16_t)(s_cx + (g ? 120 : -90) + rnd(50));
                lv_obj_remove_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
            }
            say("!!");
        }
        if (!s_ufo_life) lv_obj_add_flag(s_ufo, LV_OBJ_FLAG_HIDDEN);
    } else if (rnd(1400) == 0) {
        s_ufo_life = 190;
        s_ufo_x = -70;
        /* one flyby in five turns up gold */
        s_ufo_gold = rnd(5) == 0;
        s_ufo_dropped = false;
        lv_obj_set_style_bg_color(s_ufo,
            lv_color_hex(s_ufo_gold ? 0xFFD54A : pet_col(PC_PROP)), 0);
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

    /* An egg or an empty world: ambient sky only, plus the egg's small signs
     * of life. Everything below assumes a creature that exists. */
    if (s_pet.stage == PET_EGG || s_pet.stage == PET_AWAY) {
        if (s_pet_egg) {
            /* an occasional shiver, not a metronome — it is supposed to make
             * you look twice */
            int ph = s_fcount % 300;
            lv_obj_set_style_translate_x(s_pet_egg,
                ph < 24 ? isin(ph * 45, 2) : 0, 0);
            if (s_fcount % 420 == 200) say("...");
        }
        if (s_bubble_life > 0) {
            s_bubble_life--;
            lv_obj_set_style_opa(s_bubble,
                (lv_opa_t)(s_bubble_life > 35 ? 255 : s_bubble_life * 7), 0);
            if (!s_bubble_life) lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
        }
        pet_hud_refresh(s_pet.stage == PET_EGG
                            ? "any day now"
                            : "gone to see the stars - tap my name");
        return;
    }

    /* ---- infinite travel: the world streams under the walker ----
     * Velocity eases toward the tilt (momentum in, momentum out — the
     * marble feel), the walker recentres, and everything on the surface
     * track wraps around the planet. Sky gets parallax: stars at quarter
     * speed, the moon at an eighth, which is what sells the distance. */
    if ((s_fcount & 1) && s_tilt_vel != s_tilt_vel_tgt)
        s_tilt_vel += (s_tilt_vel_tgt > s_tilt_vel) ? 1 : -1;

    /* ---- jetpack: lift the cube, and the pet lifts with it ----
     * The trigger went through four shapes before landing on the owner's
     * own spec: not a pitch angle at all, but a sharp physical RAISE of the
     * whole cube (the vertical-velocity burst from the motion poll), from a
     * roughly level grip. In flight the same sense keeps flying it: raise
     * again to climb, lower the hand to descend. Pitch-angle triggers all
     * failed the hand they were built for — too eager at 18 degrees,
     * unreachable at 33 from a reclined grip, and a zero-lean gate reset on
     * the roll every real wrist leaks. */
    if (s_fly_mode == 0) {
        bool qr_open = s_pet_qr_panel &&
                       !lv_obj_has_flag(s_pet_qr_panel, LV_OBJ_FLAG_HIDDEN);
        int8_t burst = s_fly_burst;
        s_fly_burst = 0;
        if (burst == 1 && !qr_open) {
            s_fly_mode = 1;
            s_fly_alt = s_fly_peak = 0;
            s_tilt_vel = s_tilt_vel_tgt = 0;
            lv_obj_add_flag(s_pet_planet, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < s_wobj_n; i++)
                lv_obj_add_flag(s_wobj[i], LV_OBJ_FLAG_HIDDEN);
            if (s_sign_on) { s_sign_on = false; lv_obj_add_flag(s_sign, LV_OBJ_FLAG_HIDDEN); }
            if (s_walker_on) { s_walker_on = false; lv_obj_add_flag(s_walker, LV_OBJ_FLAG_HIDDEN); }
            for (int g = 0; g < 2; g++) {
                if (!s_gem_on[g]) continue;
                s_gem_on[g] = false;
                lv_obj_add_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(s_food_item, LV_OBJ_FLAG_HIDDEN);
            s_food_ready = false;
            s_walking_to_food = false;
            lv_obj_remove_flag(s_jet_flame, LV_OBJ_FLAG_HIDDEN);
            set_act(ACT_IDLE, 40);
            say("liftoff!");
            log_event("pet flew");
        }
    } else {
        lv_display_trigger_activity(NULL);
        /* burst-driven: another raise climbs, a lowered hand descends,
         * silence keeps doing whatever the last gesture said */
        int8_t burst = s_fly_burst;
        s_fly_burst = 0;
        if (burst == 1)       s_fly_mode = 1;
        else if (burst == -1) s_fly_mode = 2;
        int climb = (s_fly_mode == 1) ? 6 : -6;
        s_fly_alt += climb;
        if (s_fly_alt > s_fly_peak) s_fly_peak = s_fly_alt;
        if (s_fly_mode == 2 && s_fly_alt <= 0) {
            /* ---- touchdown ---- */
            s_fly_alt = 0;
            s_fly_mode = 0;
            lv_obj_remove_flag(s_pet_planet, LV_OBJ_FLAG_HIDDEN);
            for (int i = 0; i < s_wobj_n; i++) {
                lv_obj_remove_flag(s_wobj[i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_wobj[i],
                               s_wobj_x[i] - lv_obj_get_width(s_wobj[i]) / 2,
                               ground_y(s_wobj_x[i]) + s_wobj_dy[i]);
            }
            for (int g = 0; g < 2; g++) {
                if (!s_gem_sky[g]) continue;
                s_gem_sky[g] = false;
                s_gem_on[g] = false;
                lv_obj_add_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
            }
            lv_obj_add_flag(s_jet_flame, LV_OBJ_FLAG_HIDDEN);
            int m = s_fly_peak / 8;
            if (m > (int)s_pet.best_alt) {
                s_pet.best_alt = (uint16_t)m;
                s_pet.stardust += 10;
                say("new record!");
            } else {
                say("touchdown");
            }
            s_pet_dirty = true;
            s_pet_quake = 10;
            s_pet_quake_amp = 4;
        } else if (s_fly_mode) {
            /* the sky goes by: star field derived from altitude, so climb
             * and descent replay the same sky in reverse */
            if ((s_fcount & 1) == 0) {
                for (int i = 0; i < STAR_N; i++) {
                    int y = (s_star_by[i] + s_fly_alt * 2 + i * 17) % 500 - 10;
                    lv_obj_set_y(s_star[i], y);
                }
                lv_obj_set_y(s_moon, (58 + s_fly_alt / 2) % 600 - 60);
            }
            s_cx = clampi(s_cx + s_tilt_vel_tgt / 2, 100, 380);
            if (s_fcount & 1) lv_obj_set_height(s_jet_flame, 10 + rnd(10));
            if (s_fly_mode == 1 && climb > 0 && (s_fly_alt % 96) < climb) {
                for (int g = 0; g < 2; g++) {
                    if (s_gem_on[g]) continue;
                    s_gem_on[g] = true;
                    s_gem_sky[g] = true;
                    s_gem_x[g] = (int16_t)(90 + rnd(300));
                    s_gem_y[g] = -20;
                    lv_obj_remove_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
                    break;
                }
            }
        }
    }

    bool traveling = s_tilt_vel != 0 && s_fly_mode == 0;
    if (traveling) {
        s_face = s_tilt_vel > 0 ? 1 : -1;
        if (s_cx < 238)      s_cx += 2;
        else if (s_cx > 242) s_cx -= 2;
        if (s_act == ACT_WALK) set_act(ACT_IDLE, 40);  /* travel owns the legs */
        s_next_pick = s_fcount + 150;                  /* no daydreams mid-hike */

        int dx = -s_tilt_vel;
        for (int i = 0; i < s_wobj_n; i++) {
            int x = s_wobj_x[i] + dx;
            if (x >= 560)      x -= WOBJ_WRAP;
            else if (x < -80)  x += WOBJ_WRAP;
            s_wobj_x[i] = (int16_t)x;
            /* off-track x lands ground_y at the planet's centre — below the
             * panel — so things genuinely set behind the horizon */
            lv_obj_set_pos(s_wobj[i], x - lv_obj_get_width(s_wobj[i]) / 2,
                           ground_y(x) + s_wobj_dy[i]);
        }

        s_sky_par += dx;
        if (s_sky_par >= 4 * 480 || s_sky_par <= -4 * 480) s_sky_par = 0;
        if ((s_fcount & 3) == 0) {
            static const uint16_t par_sx[STAR_N] = { 34, 78, 132, 190, 250, 300,
                                                     352, 404, 60, 220, 330, 430 };
            for (int i = 0; i < STAR_N; i++) {
                int x = (par_sx[i] * 480 / 460 + s_sky_par / 4) % 480;
                if (x < 0) x += 480;
                lv_obj_set_x(s_star[i], x);
            }
            int mx = (76 + s_sky_par / 8) % 526;
            if (mx < 0) mx += 526;
            lv_obj_set_x(s_moon, mx - 46);
        }

        /* a landed snack rides the ground; the walker eats it in passing */
        if (s_food_ready) {
            s_food_x += dx;
            if (s_food_x < -20 || s_food_x > 500) {
                lv_obj_add_flag(s_food_item, LV_OBJ_FLAG_HIDDEN);
                s_food_ready = false;
                s_walking_to_food = false;
            } else if (abs(s_food_x - s_cx) < 24 && s_act != ACT_EAT) {
                s_walking_to_food = false;
                set_act(ACT_EAT, 90);
            }
        }

        /* ---- the road pays out ---- */
        s_travel_acc += abs(dx);
        while (s_travel_acc >= 10) {
            s_travel_acc -= 10;
            s_pet.odo_m++;
            if (s_pet.odo_m % PET_LAP_M == 0) {
                s_pet.laps++;
                s_pet.stardust += 25;
                s_pet_quake = 18;                 /* celebratory rattle */
                s_pet_quake_amp = 7;
                say("LAP!");
                s_pet_dirty = true;
                log_event("pet lap");
            }
            /* every 250 m the land subtly changes its mind — biome drift.
             * Color only, boundary only: retinting the ground is one big
             * invalidation, affordable at walking pace, never per frame.
             * Mono themes skip it; two tones are their whole contract. */
            if (s_pet.odo_m % 250 == 0 && s_pet_planet &&
                !s_pet_theme_pal[s_pet.theme & 3].mono) {
                lv_color_t g = lv_color_hex(pet_col(PC_GROUND));
                switch ((s_pet.odo_m / 250) & 3) {
                case 1:  g = lv_color_darken(g, 40); break;
                case 2:  g = lv_color_lighten(g, 28); break;
                case 3:  g = lv_color_darken(g, 78); break;
                default: break;
                }
                lv_obj_set_style_bg_color(s_pet_planet, g, 0);
            }
            /* a signpost stands 12 m short of every quarter-lap mark */
            if (!s_sign_on && (s_pet.odo_m + 12) % 125 == 0) {
                s_sign_on = true;
                s_sign_x = (int16_t)(s_cx + s_face * (330 + rnd(90)));
                lv_label_set_text_fmt(s_sign_label, "%lu",
                                      (unsigned long)(s_pet.odo_m + 12));
                lv_obj_remove_flag(s_sign, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (s_sign_on) {
            int x = s_sign_x + dx;
            if (x < -80 || x >= 560) {
                s_sign_on = false;
                lv_obj_add_flag(s_sign, LV_OBJ_FLAG_HIDDEN);
            } else {
                s_sign_x = (int16_t)x;
                lv_obj_set_pos(s_sign, x - 24, ground_y(x) - 54);
            }
        }
        if (s_pet.odo_m >= s_gem_next_m) {
            for (int g = 0; g < 2; g++) {
                if (s_gem_on[g]) continue;
                s_gem_on[g] = true;
                s_gem_x[g] = (int16_t)(s_cx + s_face * (300 + rnd(160)));
                lv_obj_remove_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
                break;
            }
            s_gem_next_m = s_pet.odo_m + 15 + rnd(35);
        }
        for (int g = 0; g < 2; g++) {
            if (!s_gem_on[g]) continue;
            int x = s_gem_x[g] + dx;
            if (x < -80 || x >= 560) {            /* left behind */
                s_gem_on[g] = false;
                lv_obj_add_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            s_gem_x[g] = (int16_t)x;
        }
        if (!s_walker_on && s_pet.odo_m >= s_walker_next_m) {
            s_walker_on = true;
            s_walker_waved = false;
            s_walker_dir = (int8_t)-s_face;       /* coming the other way */
            s_walker_x = (int16_t)(s_cx + s_face * (340 + rnd(120)));
            lv_obj_remove_flag(s_walker, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_walker_on) s_walker_x = (int16_t)(s_walker_x + dx);
    }

    if (s_was_traveling && !traveling) s_pet_dirty = true;   /* bank the odometer */
    s_was_traveling = traveling;

    /* The stranger walks on its own feet whether or not the world scrolls,
     * waves once as you cross, and leaves the way it came. */
    if (s_walker_on) {
        s_walker_x = (int16_t)(s_walker_x + s_walker_dir * 2);
        if (s_walker_x < -60 || s_walker_x > 540) {
            s_walker_on = false;
            lv_obj_add_flag(s_walker, LV_OBJ_FLAG_HIDDEN);
            s_walker_next_m = s_pet.odo_m + 60 + rnd(120);
        } else {
            lv_obj_set_pos(s_walker, s_walker_x - 10,
                           ground_y(s_walker_x) - 17 + isin(s_fcount * 20, 2));
            if (!s_walker_waved && abs(s_walker_x - s_cx) < 52) {
                s_walker_waved = true;
                s_pet.happy = clampi(s_pet.happy + 2, 0, 100);
                say("* waves *");
                if (s_act == ACT_IDLE) set_act(ACT_WAVE, 60);
            }
        }
    }

    /* Crystals shine and get collected whether moving, standing or flying. */
    for (int g = 0; g < 2; g++) {
        if (!s_gem_on[g]) continue;
        lv_obj_set_style_opa(s_gem[g],
            (lv_opa_t)(200 + isin((s_fcount * 9 + g * 120) % 360, 55)), 0);
        if (s_gem_sky[g]) {
            /* sky treasure sails past the climber */
            s_gem_y[g] = (int16_t)(s_gem_y[g] + (s_fly_mode == 1 ? 6 : 2));
            if (s_gem_y[g] > 500) {
                s_gem_on[g] = false;
                s_gem_sky[g] = false;
                lv_obj_add_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
                continue;
            }
            lv_obj_set_pos(s_gem[g], s_gem_x[g] - 6, s_gem_y[g]);
            int fly_y = clampi(360 - s_fly_alt, 170, 400);
            if (abs(s_gem_x[g] - s_cx) < 34 && abs(s_gem_y[g] - fly_y) < 40) {
                s_gem_on[g] = false;
                s_gem_sky[g] = false;
                lv_obj_add_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
                s_pet.stardust += 5;
                s_pet.happy = clampi(s_pet.happy + 1, 0, 100);
                s_pet_dirty = true;
                say("+5*");
            }
            continue;
        }
        lv_obj_set_pos(s_gem[g], s_gem_x[g] - 6, ground_y(s_gem_x[g]) - 15);
        if (abs(s_gem_x[g] - s_cx) < 26) {
            s_gem_on[g] = false;
            lv_obj_add_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
            s_pet.stardust += 5;
            s_pet.happy = clampi(s_pet.happy + 1, 0, 100);
            s_pet_dirty = true;
            say("+5*");
        }
    }

    /* the odometer takes over the mood line while the road is rolling;
     * the altimeter takes over from the odometer in the sky */
    if (traveling) {
        snprintf(s_trav_hud, sizeof(s_trav_hud), "%lum  lap %u",
                 (unsigned long)(s_pet.odo_m - s_pet.odo_m % 5),
                 (unsigned)s_pet.laps);
        mood = s_trav_hud;
    }
    if (s_fly_mode) {
        snprintf(s_trav_hud, sizeof(s_trav_hud), "ALT %dm  best %um",
                 s_fly_alt / 8, (unsigned)s_pet.best_alt);
        mood = s_trav_hud;
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

    if (s_fly_mode) {
        /* airborne: limbs trail, eyes wide, everything floats */
        bob   = isin(s_fcount * 6, 4);
        arm   = 12;
        leg_l = isin(s_fcount * 10, 3);
        leg_r = -leg_l;
        eye_h = 14;
    } else if (traveling && s_act == ACT_IDLE) {
        /* the travelling gait: same limbs as ACT_WALK, cadence scaled to
         * how hard the world is being tilted */
        int step = (s_fcount * (14 + abs(s_tilt_vel) * 4)) % 360;
        leg_l = isin(step, 7);
        leg_r = -leg_l;
        arm   = -leg_l;
        bob   = -abs(isin(step * 2, 3));
        if (worst <= 30) eye_h = 6;
    } else switch (s_act) {
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
            s_pet.hunger = clampi(s_pet.hunger + 8, 0, 100);
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

    /* ---- the quake: a shake rattles the WORLD, not just the pet ----
     * Sky props and the creature jitter on decaying sine offsets; the big
     * ground circle stays put because translating it invalidates a
     * full-width band per frame. Small objects, small dirty rects. */
    int qx = 0, qy = 0;
    if (s_pet_quake > 0) {
        s_pet_quake--;
        int a = (s_pet_quake_amp * s_pet_quake) / 24;
        qx = isin(s_pet_quake * 67, a);
        qy = isin(s_pet_quake * 53 + 90, a / 2);
        lv_obj_set_style_translate_x(s_moon, qx, 0);
        for (int i = 0; i < STAR_N; i += 2)
            lv_obj_set_style_translate_y(s_star[i], qy, 0);
        if (!s_pet_quake) {                     /* settle everything back */
            lv_obj_set_style_translate_x(s_moon, 0, 0);
            for (int i = 0; i < STAR_N; i += 2)
                lv_obj_set_style_translate_y(s_star[i], 0, 0);
        }
    }

    /* ---- place the character on the surface (or above it) ---- */
    int gy = ground_y(s_cx);
    int base_y = gy - CH_H;
    if (s_fly_mode) base_y = clampi(360 - s_fly_alt, 170, gy - CH_H);
    lv_obj_set_x(s_ch_wrap, s_cx - CH_W / 2 + lean + qx);
    lv_obj_set_y(s_ch_wrap, base_y + bob + qy);

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
        if (s_pet.species == 1)
            lv_obj_set_size(s_ch_body, CH_W - 4 + squash, 44 - squash);
        else
            lv_obj_set_size(s_ch_body, CH_W - 8 + squash, 40 - squash);
        p_sq = squash;
    }
    if (s_face != p_face) {                       /* shift the face to "look" a way */
        lv_obj_align(s_ch_visor, LV_ALIGN_TOP_MID, s_face * 3,
                     s_pet.species == 1 ? 8 : 6);
        p_face = s_face;
    }

    /* antenna light blinks slowly — the astronaut's alone; on the cat the
     * slot holds an ear, and a blinking ear is nobody's pet */
    if (s_pet.species == 0 && s_fcount % 20 == 0) {
        lv_obj_set_style_bg_color(s_ch_antdot,
            lv_color_hex((s_fcount / 20) % 2 ? pet_col(PC_ACCENT)
                                             : pet_col(PC_BODY_DARK)), 0);
    }

    /* speech bubble rides above the head */
    if (s_bubble_life > 0) {
        s_bubble_life--;
        lv_obj_set_x(s_bubble, s_cx + 18);
        lv_obj_set_y(s_bubble, gy - CH_H - 26 - (50 - s_bubble_life) / 3);
        lv_obj_set_style_opa(s_bubble, (lv_opa_t)(s_bubble_life > 35 ? 255 : s_bubble_life * 7), 0);
        if (!s_bubble_life) lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);
    }

    pet_hud_refresh(mood);
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

/* The home gesture must originate in the bottom 72 px. LVGL reports only the
 * direction once a gesture is recognised, so an upward drag from either side
 * otherwise looks identical to an iPhone-style bottom-edge swipe. Capture the
 * press globally on the pointer indev; that works even when the first touched
 * object is a slider, button, or scrolling card rather than the screen itself. */
#define HOME_GESTURE_EDGE_Y (480 - 72)
static int s_touch_start_y = -1;

static void touch_origin_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_event_get_user_data(e);
    if (!indev) return;

    /* A dark panel is not an inert one. esp_lcd_panel_disp_on_off() stops the
     * CO5300 emitting and scanning; it does not stop the touch controller, the
     * indev, LVGL, or a single widget. The whole UI stays live and invisible,
     * so every button on the screen you cannot see still takes clicks — the
     * lock screen's transport really did skip tracks from inside a pocket.
     *
     * This was guarded per-widget before: lock_tap_cb and lock_np_tap_cb each
     * tested s_screen_on, which covered the screen and the cover art and
     * nothing else. Prev, play and next were added later and inherited no such
     * test, because there was nothing central to inherit it FROM — a gate per
     * widget is a hole per widget anyone adds afterwards.
     *
     * One gate on the indev covers clicks, gestures and scrolls at once:
     * CLICKED is sent on release, and wait_release cancels the release for this
     * touch entirely. The wake request is raised here rather than left to the
     * handler being cancelled, so touch-to-wake still works — and it now works
     * from any screen rather than only from the two that happened to test for
     * it. Pocket lock refuses the request downstream, at its single consumer.
     *
     * Those two handlers keep their own checks: this registration lives inside
     * an `if (ui_lock())` at boot, so a lock timeout there would leave no gate
     * at all, and the lock screen is the one place that must not fail open. */
    if (!s_screen_on) {
        s_req_wake = true;
        s_touch_start_y = -1;          /* no gesture may be judged from it */
        lv_indev_wait_release(indev);
        return;
    }

    lv_point_t point;
    lv_indev_get_point(indev, &point);
    s_touch_start_y = point.y;
}

static bool home_gesture_from_bottom(lv_indev_t *indev) {
    return indev && s_touch_start_y >= HOME_GESTURE_EDGE_Y &&
           lv_indev_get_gesture_dir(indev) == LV_DIR_TOP;
}

/* Swipe UP from the bottom edge, like a phone. Shared by every app screen, so
 * home is one gesture everywhere — and in MUSIC, where all three keys are
 * rebound to volume, it is the only way home. */
static void gesture_home_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (home_gesture_from_bottom(indev)) {
        lv_indev_wait_release(indev);
        app_request(APP_DRAWER);
    }
}

/* ---- the snack bar ----
 *
 * One banner, three modes: always-on, FOCUS's rotation lock, and pocket lock.
 * Each is a toggle with no other trace on the glass at the moment it is
 * pressed, so all any of them has to say is which way it just went — amber for
 * on, slate for off. This was three copies of eleven style calls before the
 * third one, which is the point they would have started to drift.
 *
 * Wording is a convention, not a free field: "<MODE>  ON" / "<MODE>  OFF",
 * double-spaced, matching the always-on banner these were all modelled on. It
 * also keeps the widest string inside the panel — at 18 px with 3 px letter
 * spacing, "ROTATION LOCK DISABLED" measured 373 px, which puts its edges
 * 53 px from the glass and into the band the curved cover clips.
 *
 * Returned rather than merely built, because FOCUS freezes the panel and has to
 * counter-rotate its banner before it fades; callers on a screen that
 * autorotates normally can ignore the return.
 *
 * fade_out animates opacity and does NOT delete, so the delayed delete is not
 * optional — without it every toggle leaves an invisible label on the screen.
 * Deleting the screen first cancels the animation and the pending delete along
 * with it, so this is safe even if the app is torn down mid-fade. */
static lv_obj_t *toast_show(const char *text, bool on, lv_coord_t dx, lv_coord_t dy) {
    lv_obj_t *t = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(t, &hud_text_18, 0);
    lv_obj_set_style_text_letter_space(t, 3, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(on ? 0xF59E0B : 0x7C8AA5), 0);
    lv_obj_set_style_bg_color(t, lv_color_hex(0x08131C), 0);
    lv_obj_set_style_bg_opa(t, 235, 0);
    lv_obj_set_style_pad_all(t, 14, 0);
    lv_obj_set_style_radius(t, 14, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, lv_color_hex(on ? 0xF59E0B : 0x33465C), 0);
    lv_label_set_text(t, text);
    /* A label is not clickable by default, but this one lands over the lock
     * screen's tap-to-unlock area — stated rather than inherited. */
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(t, LV_ALIGN_CENTER, dx, dy);
    lv_obj_fade_out(t, 900, 900);
    lv_obj_delete_delayed(t, 1900);
    return t;
}

/* 76 px tall with widened hit test and 20 px type — the touch-target rule
 * from HARDWARE.md pitfall #24. The first pass shipped these at 48 px and
 * they mis-tapped on the glass, the same mistake MUSIC's transport and
 * CONTROL both made once and fixed at 76. */
static lv_obj_t *make_action_btn(lv_obj_t *parent, const char *txt,
                                 uint32_t color, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, 104, 76);
    lv_obj_set_style_bg_color(b, lv_color_hex(color), 0);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_ext_click_area(b, 8);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_label_set_text(l, txt);
    /* On a mono theme the button IS the ink color, so the label takes the
     * paper color; everywhere else near-black reads on every accent. */
    lv_obj_set_style_text_color(l, lv_color_hex(
        s_pet_theme_pal[s_pet.theme & 3].mono ? pet_col(PC_SKY) : 0x10162A), 0);
    lv_obj_center(l);
    return b;
}

/* Unreferenced whenever FACET_APP_PET is 0 — kept compiled rather than deleted
 * so it cannot bit-rot; --gc-sections drops it from such an image, so the
 * attribute and this note stay even while the app ships. */
__attribute__((unused))
/* The astronaut rig — species 0. Every species fills the same part slots so
 * the pose code in pet_timer_cb animates any of them. */
static void pet_build_astro(void) {
    s_ch_ant = rect(s_ch_wrap, 3, 10, 1, pet_col(PC_PROP));
    lv_obj_align(s_ch_ant, LV_ALIGN_TOP_MID, 10, -4);
    s_ch_antdot = rect(s_ch_wrap, 7, 7, 3, pet_col(PC_ACCENT));
    lv_obj_align(s_ch_antdot, LV_ALIGN_TOP_MID, 10, -10);

    s_ch_pack = rect(s_ch_wrap, 16, 26, 5, pet_col(PC_ACCENT));
    lv_obj_align(s_ch_pack, LV_ALIGN_TOP_LEFT, 0, 12);

    s_ch_leg_l = rect(s_ch_wrap, 11, 16, 4, pet_col(PC_BODY_DARK));
    lv_obj_align(s_ch_leg_l, LV_ALIGN_BOTTOM_MID, -10, 0);
    s_ch_leg_r = rect(s_ch_wrap, 11, 16, 4, pet_col(PC_BODY_DARK));
    lv_obj_align(s_ch_leg_r, LV_ALIGN_BOTTOM_MID, 10, 0);

    s_ch_arm_l = rect(s_ch_wrap, 9, 20, 4, pet_col(PC_BODY));
    lv_obj_align(s_ch_arm_l, LV_ALIGN_TOP_LEFT, 3, 22);
    s_ch_arm_r = rect(s_ch_wrap, 9, 20, 4, pet_col(PC_BODY));
    lv_obj_align(s_ch_arm_r, LV_ALIGN_TOP_RIGHT, -3, 22);

    /* body last so it sits over the pack and arm roots */
    s_ch_body = rect(s_ch_wrap, CH_W - 8, 40, 14, pet_col(PC_BODY));
    lv_obj_align(s_ch_body, LV_ALIGN_TOP_MID, 0, 6);

    s_ch_visor = rect(s_ch_body, 32, 18, 9, pet_col(PC_BODY_DARK));
    lv_obj_align(s_ch_visor, LV_ALIGN_TOP_MID, 0, 6);

    s_ch_eye_l = rect(s_ch_visor, 6, 12, 3, pet_col(PC_EYE));
    lv_obj_align(s_ch_eye_l, LV_ALIGN_CENTER, -7, 0);
    s_ch_eye_r = rect(s_ch_visor, 6, 12, 3, pet_col(PC_EYE));
    lv_obj_align(s_ch_eye_r, LV_ALIGN_CENTER, 7, 0);
}

/* BIT the cat — species 1, drawn vector-side until the sprite pipeline lands.
 * Ears ride the antenna slots (their blink is guarded by species), the tail
 * rides the pack slot, the face strip is a transparent container so the eye
 * pose logic works unchanged. */
static void pet_build_cat(void) {
    s_ch_ant = rect(s_ch_wrap, 12, 14, 4, pet_col(PC_BODY_DARK));   /* ears */
    lv_obj_align(s_ch_ant, LV_ALIGN_TOP_MID, -14, 2);
    s_ch_antdot = rect(s_ch_wrap, 12, 14, 4, pet_col(PC_BODY_DARK));
    lv_obj_align(s_ch_antdot, LV_ALIGN_TOP_MID, 14, 2);

    s_ch_pack = rect(s_ch_wrap, 6, 22, 3, pet_col(PC_BODY_DARK));   /* tail */
    lv_obj_align(s_ch_pack, LV_ALIGN_BOTTOM_LEFT, -2, -14);

    s_ch_leg_l = rect(s_ch_wrap, 12, 12, 5, pet_col(PC_BODY));
    lv_obj_align(s_ch_leg_l, LV_ALIGN_BOTTOM_MID, -12, 0);
    s_ch_leg_r = rect(s_ch_wrap, 12, 12, 5, pet_col(PC_BODY));
    lv_obj_align(s_ch_leg_r, LV_ALIGN_BOTTOM_MID, 12, 0);

    /* front paws in the arm slots, small so the wave reads as a paw lift */
    s_ch_arm_l = rect(s_ch_wrap, 9, 12, 4, pet_col(PC_BODY));
    lv_obj_align(s_ch_arm_l, LV_ALIGN_BOTTOM_MID, -20, -4);
    s_ch_arm_r = rect(s_ch_wrap, 9, 12, 4, pet_col(PC_BODY));
    lv_obj_align(s_ch_arm_r, LV_ALIGN_BOTTOM_MID, 20, -4);

    s_ch_body = rect(s_ch_wrap, CH_W - 4, 44, 18, pet_col(PC_BODY));
    lv_obj_align(s_ch_body, LV_ALIGN_TOP_MID, 0, 8);

    /* transparent face strip: the pose code moves and squints these */
    s_ch_visor = rect(s_ch_body, 36, 18, 9, pet_col(PC_BODY));
    lv_obj_set_style_bg_opa(s_ch_visor, LV_OPA_TRANSP, 0);
    lv_obj_align(s_ch_visor, LV_ALIGN_TOP_MID, 0, 8);

    s_ch_eye_l = rect(s_ch_visor, 6, 12, 3, pet_col(PC_BODY_DARK));
    lv_obj_align(s_ch_eye_l, LV_ALIGN_CENTER, -9, 0);
    s_ch_eye_r = rect(s_ch_visor, 6, 12, 3, pet_col(PC_BODY_DARK));
    lv_obj_align(s_ch_eye_r, LV_ALIGN_CENTER, 9, 0);

    /* blush, the cat's whole charm */
    lv_obj_t *bl = rect(s_ch_body, 7, 5, 2, pet_col(PC_ACCENT));
    lv_obj_align(bl, LV_ALIGN_TOP_MID, -17, 22);
    lv_obj_t *br = rect(s_ch_body, 7, 5, 2, pet_col(PC_ACCENT));
    lv_obj_align(br, LV_ALIGN_TOP_MID, 17, 22);
}

/* The hat rides the character wrap, so every pose and hop carries it. */
static void pet_build_hat(void) {
    int top = (s_pet.species == 1) ? 0 : -2;
    switch (s_pet.hat) {
    case 1: {                                            /* cap */
        lv_obj_t *dome = rect(s_ch_wrap, 24, 10, 5, pet_col(PC_ACCENT));
        lv_obj_align(dome, LV_ALIGN_TOP_MID, -2, top - 6);
        lv_obj_t *brim = rect(s_ch_wrap, 34, 4, 2, pet_col(PC_ACCENT));
        lv_obj_align(brim, LV_ALIGN_TOP_MID, 4, top + 2);
        break;
    }
    case 2: {                                            /* crown */
        lv_obj_t *band = rect(s_ch_wrap, 26, 8, 2, pet_col(PC_STAR));
        lv_obj_align(band, LV_ALIGN_TOP_MID, 0, top - 4);
        for (int i = 0; i < 3; i++) {
            lv_obj_t *pt = rect(s_ch_wrap, 6, 7, 1, pet_col(PC_STAR));
            lv_obj_align(pt, LV_ALIGN_TOP_MID, (i - 1) * 9, top - 10);
        }
        break;
    }
    case 3: {                                            /* bow */
        lv_obj_t *l = rect(s_ch_wrap, 10, 9, 3, pet_col(PC_ACCENT));
        lv_obj_align(l, LV_ALIGN_TOP_MID, -8, top - 6);
        lv_obj_t *r = rect(s_ch_wrap, 10, 9, 3, pet_col(PC_ACCENT));
        lv_obj_align(r, LV_ALIGN_TOP_MID, 8, top - 6);
        lv_obj_t *knot = rect(s_ch_wrap, 6, 6, 2, pet_col(PC_BODY_DARK));
        lv_obj_align(knot, LV_ALIGN_TOP_MID, 0, top - 5);
        break;
    }
    default:
        break;
    }
}

static void build_pet_app(lv_obj_t *scr) {
    s_scr_pet = scr;
    lv_obj_remove_flag(s_scr_pet, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(s_scr_pet, lv_color_hex(pet_col(PC_SKY)), 0);
    lv_obj_set_style_bg_grad_color(s_scr_pet, lv_color_hex(pet_col(PC_SKY2)), 0);
    lv_obj_set_style_bg_grad_dir(s_scr_pet, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_pad_all(s_scr_pet, 0, 0);

    const pet_world_pal_t *wp = &s_pet_world_pal[s_pet.world & 3];

    /* --- sky (or water, or night air): the same twelve twinkles play
     * stars, rising bubbles, fireflies and lit windows — only the color
     * changes, the stagger animation already reads right for all four --- */
    /* uint16_t, not uint8_t: the five x positions past 255 used to wrap and
     * pile those stars up against the left edge */
    static const uint16_t sx[STAR_N] = { 34, 78, 132, 190, 250, 300, 352, 404, 60, 220, 330, 430 };
    static const uint16_t sy[STAR_N] = { 96, 52, 112, 40, 78, 34, 96, 130, 168, 150, 168, 74 };
    for (int i = 0; i < STAR_N; i++) {
        int sz = (i % 3 == 0) ? 4 : 3;
        s_star[i] = rect(s_scr_pet, sz, sz, 2, pet_col(PC_STAR));
        lv_obj_set_pos(s_star[i], sx[i] * 480 / 460, sy[i]);
        s_star_ph[i] = (uint8_t)(i * 7);
    }

    s_moon = rect(s_scr_pet, 46, 46, 23, pet_col(PC_STAR));
    lv_obj_set_pos(s_moon, 76, 58);
    lv_obj_t *moon_dip = rect(s_moon, 14, 14, 7, pet_col(PC_SKY2));
    lv_obj_set_style_bg_opa(moon_dip, 90, 0);
    lv_obj_set_pos(moon_dip, 8, 12);
    if (!wp->moon) lv_obj_add_flag(s_moon, LV_OBJ_FLAG_HIDDEN);

    s_shoot = rect(s_scr_pet, 26, 3, 2, pet_col(PC_STAR));
    lv_obj_add_flag(s_shoot, LV_OBJ_FLAG_HIDDEN);

    /* the passer-by: UFO over the planet, a fish in the sea, a butterfly in
     * the glade, a little plane over the skyline — same flight path */
    s_ufo = rect(s_scr_pet, 54, 14, 7, pet_col(PC_PROP));
    lv_obj_add_flag(s_ufo, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *ufo_dome = rect(s_ufo, 24, 12, 6, pet_col(PC_STAR));
    lv_obj_align(ufo_dome, LV_ALIGN_TOP_MID, 0, -6);

    /* --- the ground: a big circle, only its cap is on screen --- */
    /* The pocket planet is a true circle; the big-radius worlds draw as a
     * flat slab. This is a renderer constraint, not a style choice: a
     * radius-2400 rounded rect sends LVGL's software corner mask
     * (circ_calc_aa4) into a Cache-error panic on every full redraw — the
     * crash loop of 2026-08-24. The walking surface still follows ground_y's
     * gentle curve; the few px of sag against the slab's straight horizon
     * are invisible at these radii. */
    int gr = pet_geo_r();
    lv_obj_t *planet;
    if (gr <= 300) {
        planet = rect(s_scr_pet, gr * 2, gr * 2, gr, pet_col(PC_GROUND));
        lv_obj_set_pos(planet, PLANET_CX - gr, pet_geo_cy() - gr);
    } else {
        int horizon = pet_geo_cy() - gr;
        planet = rect(s_scr_pet, 520, 480 - horizon + 40, 0, pet_col(PC_GROUND));
        lv_obj_set_pos(planet, -20, horizon);
    }
    s_pet_planet = planet;
    lv_obj_set_style_border_width(planet, 4, 0);
    lv_obj_set_style_border_color(planet, lv_color_hex(pet_col(PC_GROUND_HI)), 0);
    lv_obj_set_style_border_opa(planet, 190, 0);

    /* Everything on the surface rides the travel track — ground details and
     * props scroll around the planet while the walker holds centre stage.
     * Positions are (track x of centre, y offset from the surface line). */
    s_wobj_n = 0;
    s_tilt_vel = s_tilt_vel_tgt = 0;
    s_sky_par = 0;

    lv_obj_t *c1 = rect(s_scr_pet, 54, 20, 10, pet_col(PC_SPOT));
    wobj_add(c1, 137, 18);
    lv_obj_t *c2 = rect(s_scr_pet, 34, 14, 7, pet_col(PC_SPOT));
    wobj_add(c2, 331, 30);
    lv_obj_t *c3 = rect(s_scr_pet, 22, 10, 5, pet_col(PC_SPOT));
    wobj_add(c3, 235, 66);

    /* --- per-world set dressing, also on the track. Enough of it that a
     * lap keeps changing scenery even in a mono theme, where structure is
     * the only signature a world has. --- */
    switch (s_pet.world & 3) {
    case 1: {                                        /* the seabed */
        lv_obj_t *k1 = rect(s_scr_pet, 6, 44, 3, pet_col(PC_SPOT));
        wobj_add(k1, 99, -40);
        lv_obj_t *k2 = rect(s_scr_pet, 5, 30, 2, pet_col(PC_GROUND_HI));
        wobj_add(k2, 390, -26);
        lv_obj_t *k3 = rect(s_scr_pet, 6, 36, 3, pet_col(PC_GROUND_HI));
        wobj_add(k3, 505, -32);
        lv_obj_t *rock = rect(s_scr_pet, 30, 16, 8, pet_col(PC_SPOT));
        wobj_add(rock, -40, -12);
        break;
    }
    case 2: {                                        /* the glade */
        lv_obj_t *trunk = rect(s_scr_pet, 10, 46, 3, 0x5D4037);
        wobj_add(trunk, 377, -42);
        lv_obj_t *crown = rect(s_scr_pet, 62, 52, 26, pet_col(PC_GROUND_HI));
        wobj_add(crown, 377, -88);
        lv_obj_t *bush = rect(s_scr_pet, 34, 20, 10, pet_col(PC_GROUND_HI));
        wobj_add(bush, 96, -16);
        lv_obj_t *trunk2 = rect(s_scr_pet, 8, 30, 3, 0x5D4037);
        wobj_add(trunk2, 520, -26);
        lv_obj_t *crown2 = rect(s_scr_pet, 40, 34, 17, pet_col(PC_SPOT));
        wobj_add(crown2, 520, -56);
        break;
    }
    case 3: {                                        /* the rooftop */
        lv_obj_t *b1 = rect(s_scr_pet, 44, 90, 3, pet_col(PC_SKY2));
        wobj_add(b1, 82, -84);
        lv_obj_t *b2 = rect(s_scr_pet, 34, 62, 3, pet_col(PC_SKY2));
        wobj_add(b2, 400, -58);
        lv_obj_t *mast = rect(s_scr_pet, 4, 54, 2, pet_col(PC_PROP));
        wobj_add(mast, 500, -50);
        lv_obj_t *duct = rect(s_scr_pet, 40, 18, 4, pet_col(PC_SPOT));
        wobj_add(duct, -30, -14);
        break;
    }
    default: {                                       /* the pocket planet */
        lv_obj_t *flag_pole = rect(s_scr_pet, 3, 26, 1, pet_col(PC_PROP));
        wobj_add(flag_pole, 520, -24);
        lv_obj_t *flag = rect(s_scr_pet, 14, 9, 2, pet_col(PC_ACCENT));
        wobj_add(flag, 528, -24);
        lv_obj_t *c4 = rect(s_scr_pet, 40, 16, 8, pet_col(PC_SPOT));
        wobj_add(c4, -50, 22);
        break;
    }
    }
    /* park every track object where it belongs before the first scroll */
    for (int i = 0; i < s_wobj_n; i++) {
        lv_obj_set_pos(s_wobj[i], s_wobj_x[i] - lv_obj_get_width(s_wobj[i]) / 2,
                       ground_y(s_wobj_x[i]) + s_wobj_dy[i]);
    }

    /* --- the encounter reel's cast, hidden until the road deals them --- */
    for (int g = 0; g < 2; g++) {
        s_gem[g] = rect(s_scr_pet, 12, 12, 4, pet_col(PC_STAR));
        lv_obj_set_style_border_width(s_gem[g], 2, 0);
        lv_obj_set_style_border_color(s_gem[g], lv_color_hex(pet_col(PC_ACCENT)), 0);
        lv_obj_add_flag(s_gem[g], LV_OBJ_FLAG_HIDDEN);
        s_gem_on[g] = false;
    }
    s_walker = lv_obj_create(s_scr_pet);
    lv_obj_remove_style_all(s_walker);
    lv_obj_set_size(s_walker, 20, 18);
    lv_obj_remove_flag(s_walker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_walker, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *wb = rect(s_walker, 20, 14, 6, pet_col(PC_PROP));
    lv_obj_align(wb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *we1 = rect(s_walker, 3, 5, 1, pet_col(PC_SKY));
    lv_obj_align(we1, LV_ALIGN_BOTTOM_MID, -4, -6);
    lv_obj_t *we2 = rect(s_walker, 3, 5, 1, pet_col(PC_SKY));
    lv_obj_align(we2, LV_ALIGN_BOTTOM_MID, 4, -6);
    lv_obj_add_flag(s_walker, LV_OBJ_FLAG_HIDDEN);
    s_walker_on = false;
    s_travel_acc = 0;
    s_was_traveling = false;
    s_ufo_gold = s_ufo_dropped = false;
    s_gem_sky[0] = s_gem_sky[1] = false;

    /* the distance signpost, planted ahead of each lap-quarter */
    s_sign = lv_obj_create(s_scr_pet);
    lv_obj_remove_style_all(s_sign);
    lv_obj_set_size(s_sign, 48, 56);
    lv_obj_remove_flag(s_sign, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_sign, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *pole = rect(s_sign, 4, 30, 1, pet_col(PC_PROP));
    lv_obj_align(pole, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *board = rect(s_sign, 48, 26, 5, pet_col(PC_STAR));
    lv_obj_align(board, LV_ALIGN_TOP_MID, 0, 0);
    s_sign_label = lv_label_create(s_sign);
    lv_obj_set_style_text_color(s_sign_label, lv_color_hex(pet_col(PC_SKY)), 0);
    lv_label_set_text(s_sign_label, "");
    lv_obj_align(s_sign_label, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_add_flag(s_sign, LV_OBJ_FLAG_HIDDEN);
    s_sign_on = false;
    /* first payouts land soon after the first stroll begins */
    s_gem_next_m = s_pet.odo_m + 10 + rnd(20);
    s_walker_next_m = s_pet.odo_m + 40 + rnd(60);

    /* rocket on the pad, hidden until it flies */
    s_rocket = rect(s_scr_pet, 18, 34, 8, pet_col(PC_ACCENT));
    lv_obj_add_flag(s_rocket, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *nose = rect(s_rocket, 10, 10, 5, pet_col(PC_BODY));
    lv_obj_align(nose, LV_ALIGN_TOP_MID, 0, 2);
    s_flame = rect(s_scr_pet, 8, 16, 4, pet_col(PC_STAR));
    lv_obj_add_flag(s_flame, LV_OBJ_FLAG_HIDDEN);

    s_food_item = rect(s_scr_pet, 18, 18, 9, pet_col(PC_ACCENT));
    lv_obj_add_flag(s_food_item, LV_OBJ_FLAG_HIDDEN);

    /* --- the creature the phone chose --- */
    s_ch_wrap = lv_obj_create(s_scr_pet);
    lv_obj_remove_style_all(s_ch_wrap);
    lv_obj_set_size(s_ch_wrap, CH_W, CH_H);
    lv_obj_remove_flag(s_ch_wrap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(s_ch_wrap, 240 - CH_W / 2, 200);

    if (s_pet.species == 1) pet_build_cat();
    else                    pet_build_astro();
    pet_build_hat();

    /* the jetpack flame, tucked under the character until liftoff */
    s_jet_flame = rect(s_ch_wrap, 10, 16, 4, pet_col(PC_STAR));
    lv_obj_align(s_jet_flame, LV_ALIGN_BOTTOM_MID, 0, 16);
    lv_obj_add_flag(s_jet_flame, LV_OBJ_FLAG_HIDDEN);
    s_fly_mode = 0;
    s_fly_alt = s_fly_peak = 0;
    s_fly_burst = 0;

    s_bubble = lv_label_create(s_scr_pet);
    lv_obj_set_style_text_color(s_bubble, lv_color_hex(pet_col(PC_STAR)), 0);
    lv_label_set_text(s_bubble, "");
    lv_obj_add_flag(s_bubble, LV_OBJ_FLAG_HIDDEN);

    /* --- before hatching / after leaving, the creature is not here --- */
    s_pet_egg = NULL;
    if (s_pet.stage == PET_EGG) {
        lv_obj_add_flag(s_ch_wrap, LV_OBJ_FLAG_HIDDEN);
        int gy = ground_y(240);
        s_pet_egg = rect(s_scr_pet, 44, 54, 22, pet_col(PC_BODY));
        lv_obj_set_pos(s_pet_egg, 240 - 22, gy - 50);
        lv_obj_t *spot = rect(s_pet_egg, 12, 10, 5, pet_col(PC_ACCENT));
        lv_obj_set_pos(spot, 8, 14);
        lv_obj_t *spot2 = rect(s_pet_egg, 8, 7, 3, pet_col(PC_ACCENT));
        lv_obj_set_pos(spot2, 26, 30);
        /* the murmurs rise from the shell, not from a walking character */
        lv_obj_set_pos(s_bubble, 240 + 30, gy - 76);
    } else if (s_pet.stage == PET_AWAY) {
        lv_obj_add_flag(s_ch_wrap, LV_OBJ_FLAG_HIDDEN);
        /* a tiny sign planted where it used to stand */
        int gy = ground_y(262);
        lv_obj_t *post = rect(s_scr_pet, 4, 26, 2, pet_col(PC_PROP));
        lv_obj_set_pos(post, 262, gy - 26);
        lv_obj_t *board = rect(s_scr_pet, 34, 18, 4, pet_col(PC_STAR));
        lv_obj_set_pos(board, 247, gy - 42);
    }

    /* --- HUD: name + mood + three slim bars, kept out of the scene --- */
    s_pet_name = lv_label_create(s_scr_pet);
    lv_obj_set_style_text_color(s_pet_name, lv_color_hex(pet_col(PC_TEXT)), 0);
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
    bool mono = s_pet_theme_pal[s_pet.theme & 3].mono;
    s_bar_food = make_stat_bar(bars, mono ? pet_col(PC_TEXT) : 0xF4A261);
    s_bar_fun  = make_stat_bar(bars, mono ? pet_col(PC_TEXT) : 0xE76F51);
    s_bar_nrg  = make_stat_bar(bars, mono ? pet_col(PC_TEXT) : 0x8DE0D2);

    s_pet_mood = lv_label_create(s_scr_pet);
    lv_obj_set_width(s_pet_mood, CONTENT_W);
    lv_obj_set_style_text_align(s_pet_mood, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_pet_mood, lv_color_hex(pet_col(PC_DIM)), 0);
    lv_label_set_text(s_pet_mood, "having a good day");
    lv_obj_align(s_pet_mood, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 34);

    /* --- touch action bar (an egg or an empty world offers no verbs) --- */
    if (!pet_absent()) {
        lv_obj_t *acts = lv_obj_create(s_scr_pet);
        lv_obj_remove_style_all(acts);
        lv_obj_set_size(acts, 340, 76);
        lv_obj_align(acts, LV_ALIGN_BOTTOM_MID, 0, -BOTTOM_MARGIN);
        lv_obj_set_flex_flow(acts, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(acts, LV_FLEX_ALIGN_SPACE_BETWEEN,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_remove_flag(acts, LV_OBJ_FLAG_SCROLLABLE);
        /* Mono themes keep their two-tone promise on the verbs too. */
        make_action_btn(acts, "FEED",  mono ? pet_col(PC_TEXT) : 0xF4A261, act_feed_cb);
        make_action_btn(acts, "DANCE", mono ? pet_col(PC_TEXT) : 0xE76F51, act_play_cb);
        make_action_btn(acts, "NAP",   mono ? pet_col(PC_TEXT) : 0x8DE0D2, act_rest_cb);
    }

    /* Fresh labels must not be gated against a previous build's text. */
    s_pet_prev_mood[0] = '\0';
    s_pet_prev_name[0] = '\0';

    /* The name doubles as the door to the phone designer. */
    lv_obj_add_flag(s_pet_name, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_pet_name, 24);
    lv_obj_add_event_cb(s_pet_name, pet_qr_open_cb, LV_EVENT_CLICKED, NULL);

    /* Designer QR overlay, built last so it sits over everything. White
     * panel: a QR drawn onto a dark scene scans poorly (the setup QR learned
     * this); black modules on white with the quiet zone kept. */
    s_pet_qr_panel = lv_obj_create(s_scr_pet);
    lv_obj_remove_style_all(s_pet_qr_panel);
    lv_obj_set_size(s_pet_qr_panel, 480, 480);
    lv_obj_set_style_bg_color(s_pet_qr_panel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(s_pet_qr_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_pet_qr_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_pet_qr_panel, pet_qr_close_cb, LV_EVENT_CLICKED, NULL);
    s_pet_qr = lv_qrcode_create(s_pet_qr_panel);
    lv_qrcode_set_size(s_pet_qr, 200);
    lv_qrcode_set_dark_color(s_pet_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_pet_qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(s_pet_qr, true);
    lv_obj_align(s_pet_qr, LV_ALIGN_CENTER, 0, -58);
    lv_obj_remove_flag(s_pet_qr, LV_OBJ_FLAG_CLICKABLE);
    s_pet_qr_note = lv_label_create(s_pet_qr_panel);
    lv_obj_set_width(s_pet_qr_note, 300);
    lv_obj_set_style_text_align(s_pet_qr_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_pet_qr_note, lv_color_hex(0x10162A), 0);
    lv_label_set_text(s_pet_qr_note, "");
    lv_obj_align(s_pet_qr_note, LV_ALIGN_CENTER, 0, 74);

    /* The sync button — 72 px per the touch rule. Sized and placed so its
     * corners sit well inside the panel's corner arcs: on the glass the
     * first 320x76 at -42 crowded the curve and read as a layout mistake. */
    s_pet_qr_btn = lv_button_create(s_pet_qr_panel);
    lv_obj_set_size(s_pet_qr_btn, 292, 72);
    lv_obj_set_style_radius(s_pet_qr_btn, 24, 0);
    lv_obj_set_style_bg_color(s_pet_qr_btn, lv_color_hex(0xE76F51), 0);
    lv_obj_set_style_shadow_width(s_pet_qr_btn, 0, 0);
    lv_obj_align(s_pet_qr_btn, LV_ALIGN_BOTTOM_MID, 0, -58);
    lv_obj_add_event_cb(s_pet_qr_btn, pet_qr_sync_cb, LV_EVENT_CLICKED, NULL);
    s_pet_qr_btn_l = lv_label_create(s_pet_qr_btn);
    lv_obj_set_style_text_font(s_pet_qr_btn_l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_pet_qr_btn_l, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_pet_qr_btn_l, "I SAVED - SYNC NOW");
    lv_obj_center(s_pet_qr_btn_l);

    /* the verdict badge: a green tick or a red cross where the QR was */
    s_pet_qr_tick = lv_obj_create(s_pet_qr_panel);
    lv_obj_remove_style_all(s_pet_qr_tick);
    lv_obj_set_size(s_pet_qr_tick, 120, 120);
    lv_obj_set_style_radius(s_pet_qr_tick, 60, 0);
    lv_obj_set_style_bg_color(s_pet_qr_tick, lv_color_hex(0x2E9E5B), 0);
    lv_obj_set_style_bg_opa(s_pet_qr_tick, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_pet_qr_tick, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_pet_qr_tick, LV_ALIGN_CENTER, 0, -28);
    lv_obj_t *tick_l = lv_label_create(s_pet_qr_tick);
    lv_obj_set_style_text_font(tick_l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(tick_l, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(tick_l, LV_SYMBOL_OK);
    lv_obj_center(tick_l);
    lv_obj_add_flag(s_pet_qr_tick, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(s_pet_qr_panel, LV_OBJ_FLAG_HIDDEN);

    /* tap the world to send it walking, tap the astronaut to pet it */
    lv_obj_add_flag(s_scr_pet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scr_pet, scene_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_scr_pet, pet_gesture_cb, LV_EVENT_GESTURE, NULL);

    pet_seen();                     /* opening the app counts as a visit */
    /* Opening the app is also the natural "did anything change?" moment: a
     * design saved on the phone should appear on the next visit without the
     * QR ceremony. The cfg_ver gate makes the fetch free when nothing did. */
    __atomic_store_n(&s_req_pet_cfg, true, __ATOMIC_RELEASE);
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

static void render_perf_cb(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    int64_t now_us = esp_timer_get_time();

    switch (code) {
    case LV_EVENT_RENDER_START: {
        uint16_t mhz = (uint16_t)(esp_clk_cpu_freq() / 1000000);
        s_perf_render_started_us = now_us;

        taskENTER_CRITICAL(&s_perf_mux);
        if (s_perf_last_render_us) {
            uint32_t us = (uint32_t)(now_us - s_perf_last_render_us);
            s_render_perf.interval_count++;
            s_render_perf.interval_us_sum += us;
            if (us > s_render_perf.interval_us_max) s_render_perf.interval_us_max = us;
        }
        s_perf_last_render_us = now_us;
        if (!s_render_perf.cpu_mhz_min || mhz < s_render_perf.cpu_mhz_min) {
            s_render_perf.cpu_mhz_min = mhz;
        }
        if (mhz > s_render_perf.cpu_mhz_max) s_render_perf.cpu_mhz_max = mhz;
        taskEXIT_CRITICAL(&s_perf_mux);
        break;
    }
    case LV_EVENT_RENDER_READY: {
        uint32_t us = (uint32_t)(now_us - s_perf_render_started_us);
        taskENTER_CRITICAL(&s_perf_mux);
        s_render_perf.redraws++;
        s_render_perf.render_us_sum += us;
        if (us > s_render_perf.render_us_max) s_render_perf.render_us_max = us;
        s_refr_count++;
        taskEXIT_CRITICAL(&s_perf_mux);
        break;
    }
    case LV_EVENT_FLUSH_START: {
        const lv_area_t *area = lv_event_get_param(e);
        s_perf_flush_started_us = now_us;
        taskENTER_CRITICAL(&s_perf_mux);
        s_render_perf.flushes++;
        if (area) s_render_perf.pixels += (uint32_t)lv_area_get_size(area);
        taskEXIT_CRITICAL(&s_perf_mux);
        break;
    }
    case LV_EVENT_FLUSH_FINISH: {
        uint32_t us = (uint32_t)(now_us - s_perf_flush_started_us);
        taskENTER_CRITICAL(&s_perf_mux);
        s_render_perf.submit_us_sum += us;
        if (us > s_render_perf.submit_us_max) s_render_perf.submit_us_max = us;
        taskEXIT_CRITICAL(&s_perf_mux);
        break;
    }
    case LV_EVENT_FLUSH_WAIT_START:
        s_perf_wait_started_us = now_us;
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH: {
        uint32_t us = (uint32_t)(now_us - s_perf_wait_started_us);
        taskENTER_CRITICAL(&s_perf_mux);
        s_render_perf.wait_count++;
        s_render_perf.wait_us_sum += us;
        if (us > s_render_perf.wait_us_max) s_render_perf.wait_us_max = us;
        taskEXIT_CRITICAL(&s_perf_mux);
        break;
    }
    default:
        break;
    }
}

static void render_perf_report(int app) {
    render_perf_t p;
    taskENTER_CRITICAL(&s_perf_mux);
    p = s_render_perf;
    memset(&s_render_perf, 0, sizeof(s_render_perf));
    s_perf_last_render_us = 0;
    taskEXIT_CRITICAL(&s_perf_mux);

    if (!p.redraws) return;
    uint32_t render_avg = p.render_us_sum / p.redraws;
    uint32_t interval_avg = p.interval_count
                          ? p.interval_us_sum / p.interval_count : 0;
    uint32_t wait_avg = p.wait_count ? p.wait_us_sum / p.wait_count : 0;
    uint32_t submit_avg = p.flushes ? p.submit_us_sum / p.flushes : 0;
    ESP_LOGI(TAG,
             "render perf: app=%d cpu=%u-%uMHz redraws=%u interval=%u.%ums avg/%u.%ums max "
             "render=%u.%ums avg/%u.%ums max flushes=%u pixels/frame=%u "
             "submit=%u.%ums avg/%u.%ums max wait=%u.%ums avg/%u.%ums max",
             app, p.cpu_mhz_min, p.cpu_mhz_max, p.redraws,
             interval_avg / 1000, (interval_avg % 1000) / 100,
             p.interval_us_max / 1000, (p.interval_us_max % 1000) / 100,
             render_avg / 1000, (render_avg % 1000) / 100,
             p.render_us_max / 1000, (p.render_us_max % 1000) / 100,
             p.flushes, p.pixels / p.redraws,
             submit_avg / 1000, (submit_avg % 1000) / 100,
             p.submit_us_max / 1000, (p.submit_us_max % 1000) / 100,
             wait_avg / 1000, (wait_avg % 1000) / 100,
             p.wait_us_max / 1000, (p.wait_us_max % 1000) / 100);
}

/* LVGL setters invalidate even when a value is unchanged.  In a content-sized
 * card that also means a fresh layout pass for the whole scrolling column. */
static bool label_set_changed(lv_obj_t *lbl, const char *s) {
    if (!lbl) return false;
    const char *cur = lv_label_get_text(lbl);
    if (cur && strcmp(cur, s) == 0) return false;
    lv_label_set_text(lbl, s);
    return true;
}

static void label_set_fmt_changed(lv_obj_t *lbl, const char *fmt, ...) {
    if (!lbl) return;
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    label_set_changed(lbl, buf);
}

static void obj_set_hidden_changed(lv_obj_t *obj, bool hidden) {
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN) == hidden) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void bar_set_changed(lv_obj_t *bar, int value) {
    if (bar && lv_bar_get_value(bar) != value) {
        lv_bar_set_value(bar, value, LV_ANIM_OFF);
    }
}

static void bg_color_set_changed(lv_obj_t *obj, lv_color_t color, lv_part_t part) {
    if (obj && !lv_color_eq(lv_obj_get_style_bg_color(obj, part), color)) {
        lv_obj_set_style_bg_color(obj, color, part);
    }
}

static void text_color_set_changed(lv_obj_t *obj, lv_color_t color) {
    if (obj && !lv_color_eq(lv_obj_get_style_text_color(obj, 0), color)) {
        lv_obj_set_style_text_color(obj, color, 0);
    }
}

static lv_obj_t *s_cfg_wall_pool, *s_cfg_wall_state, *s_cfg_wall_bar, *s_cfg_wall_sub;
static lv_obj_t *s_cfg_days_val;
static lv_obj_t *s_cfg_rot_val;
static lv_obj_t *s_cfg_rot_sw, *s_cfg_rot_btn, *s_cfg_time_sw;
static lv_obj_t *s_cfg_lockkey_val, *s_cfg_lockkey_btn;
static lv_obj_t *s_cfg_always_sw, *s_cfg_rings_sw;
static lv_obj_t *s_cfg_aodim_sw, *s_cfg_aodim_sld;
static lv_obj_t *s_cfg_aodim_val, *s_cfg_aodim_sub;
static lv_obj_t *s_cfg_bright_val;
static lv_obj_t *s_cfg_vol_val;
static lv_obj_t *s_cfg_batt_bar, *s_cfg_batt_val, *s_cfg_batt_sub, *s_cfg_chgeta_sw;
static lv_obj_t *s_cfg_chgeta_sub;
static lv_obj_t *s_cfg_care_val, *s_cfg_care_sub, *s_cfg_care_sld, *s_cfg_care_btn;
static lv_obj_t *s_cfg_net_val;
static lv_obj_t *s_cfg_ble_val;
static lv_obj_t *s_cfg_ble_code;
static lv_obj_t *s_cfg_ble_qr;
/* The code currently encoded in the QR, so a re-encode happens once per session
 * rather than once per tick. File scope rather than a function-local static
 * because the card is rebuilt on every app open: a local would still hold the
 * previous session's code, match, and skip the update, leaving the freshly
 * created widget blank for a code the panel is displaying right beside it. */
static char      s_cfg_ble_qr_code[7];

/* The pairing QR is drawn ONLY inside the full-screen setup scene, never in
 * the scrolling CONTROL column. Every reboot captured on 2026-08-23 — a tlsf
 * double-free, two LoadProhibited walks of LVGL's own lists, and a newlib time
 * lock that stopped being a semaphore — needed the same two things: the QR
 * visible in the column, and the column being scrolled. Removing the QR
 * outright stopped the crashes for a session.
 *
 * Not attributed, and the honest reading is that the QR may be the canary
 * rather than the bug: its 5008-byte draw buffer (200x200 I1 + 8 B palette —
 * exactly the value found in registers at two of the panics) is allocated at
 * CONTROL build into internal SRAM, and something may dangle into whatever
 * block it lands in. Ruled out with instrumentation, not guesswork: heap
 * poisoning (COMPREHENSIVE), LV_USE_ASSERT_OBJ and the end-of-stack
 * watchpoint all stayed silent; the image cache never owns the canvas buffer
 * (decode_indexed copies it, or with RAM_LOAD off caches nothing); and the
 * only frees of that header are set_size at build and the destructor.
 *
 * If it comes back with a different face, the first two suspects for the
 * reopen are today's other additions to the teardown path: the lv_layer_top()
 * bezel lobes with lv_anim keyed on a static array, and the picker's scroll
 * callbacks on a list app_open deletes. Set this to 0 to drop the QR entirely
 * and pair by typing the six-digit code. */
#define CFG_SETUP_QR 1
static lv_obj_t *s_cfg_ble_spin;
/* The Wi-Fi setup scene: a full-screen panel over CONTROL, the same shape as
 * MUSIC's device picker. QR, code, spinner and the join result live HERE,
 * not in the scrolling column. */
static lv_obj_t *s_cfg_wifi, *s_cfg_wifi_st, *s_cfg_wifi_sub, *s_cfg_wifi_stop;
static lv_obj_t *s_cfg_wifi_tick;   /* the big result glyph: green check or red cross */
/* 0 = scene idle (nothing asked for), 1 = a session or join is in flight,
 * 2 = that join finished. The big glyph shows only in phase 2 (or on a
 * failed retry): opening the scene on an already-connected cube must not
 * greet you with a success mark for something you have not done yet. */
static int s_cfg_wifi_phase;
static lv_obj_t *s_cfg_ble_btn;
/* True from the tap until the session is either open or known to have failed.
 * Bringing pairing up runs inline on the main loop and measured ~4 s on
 * hardware, so this is the window in which the card must say something. */
static volatile bool s_cfg_ble_starting;
/* The CONTROL tick's change-gate for this card. At file scope so cfg_ble_cb can
 * invalidate it: the tick cannot run while the main loop is busy starting BLE,
 * so on the failure path it would otherwise wake to find the same key it last
 * saw (wi-fi up, no session), skip its branch, and leave the spinner turning
 * forever over a card that has finished. */
static int       s_cfg_ble_key = -1;
static lv_obj_t *s_cfg_sys_val;
static lv_obj_t *s_cfg_log;

/* Sliders in the scrolling column, registered so a scroll can make them inert.
 *
 * An lv_slider in the default mode jumps to wherever you press on the track —
 * that is what makes the whole 46 px bar a target rather than just the knob
 * (see cfg_slider) — but it also means merely *touching* a slider to begin a
 * scroll commits a value. Two different gestures hit this and they need
 * different guards, which is why there are two:
 *
 *   - starting a drag on a slider that turns out to be a scroll. LVGL hands the
 *     press to the column once it passes scroll_limit and sends the slider
 *     LV_EVENT_PRESS_LOST, so the snapshot taken on LV_EVENT_PRESSED is what
 *     puts the value back.
 *   - touching down to arrest a fling. There is no press to lose here; the
 *     finger simply lands on whatever is under it, so the slider has to be
 *     un-hittable *before* the touch arrives. Hence clearing CLICKABLE for the
 *     duration of the scroll.
 *
 * CLICKABLE and not LV_STATE_DISABLED or an opacity fade: in this app a faded
 * control means "this cannot act" (the autorotate switch with no IMU), and a
 * slider that greyed out every time the list moved would read as a fault. This
 * is invisible, and it costs no dirty area mid-scroll. */
/* Headroom on purpose. The desk-clock delay slider took this table to exactly
 * four, and overflowing it is a SILENT downgrade rather than an error:
 * cfg_slider() simply skips registering, and the unregistered slider then
 * commits a value whenever a finger lands on it to arrest a scroll — the exact
 * bug the two guards above exist to prevent, reappearing on whichever control
 * happened to be added last. */
#define CFG_SLIDER_MAX 6
static lv_obj_t *s_cfg_sliders[CFG_SLIDER_MAX];
static int32_t   s_cfg_slider_snap[CFG_SLIDER_MAX];
static int       s_cfg_slider_n;

/* TEMPORARY PERF HARNESS, REMOVE BEFORE COMMIT: drives the CONTROL column up
 * and down through lv_obj_scroll_by at boot, so render-throughput experiments
 * measure a repeatable synthetic scroll instead of whoever is awake to rub the
 * glass. Exercises the same full-column invalidation path as a finger; it only
 * bypasses the touch pipeline, which these experiments do not target. */
#define CFG_PERF_SCROLL_SELFTEST 0

/* Hold LEFT+RIGHT together to stream a screenshot of the glass out of the USB
 * console (tools/capture.py records it, tools/snap_rx.py rebuilds it). Two
 * frames go out: the screen, and lv_layer_top() as ARGB — the bezel lobes live
 * on the top layer and both are swelled in while the chord is held, so this is
 * also how the pop-out design is captured. Costs ~2-3 min of frozen UI per
 * shot; a debug affordance for a dev cube, requested by the user. */
#define CFG_SNAP_CHORD 1

/* One-shot experiment: prove the desk-clock burn-in drift actually MOVES
 * PIXELS. The log line only proves ao_drift_apply() was called, and the first
 * version of that function was a silent no-op (HARDWARE.md #36) — a styling
 * call that does nothing looks exactly like one that worked, and 4 px on a 12%
 * panel is invisible to an eye either way. A framebuffer snapshot is the only
 * instrument that settles it: it captures what LVGL rendered, so panel
 * brightness does not enter into it.
 *
 * Streams two frames of the same screen in the same clock minute, differing
 * only by an exaggerated 8 px drift, and freezes the main loop's own dim while
 * it runs so nothing else can move between them. Keep 0 in commits — same
 * contract as CFG_PERF_SCROLL_SELFTEST. */
#define CFG_DIM_SNAP 0

/* A/B soak: does the desk-clock dim actually save power? Nothing in this
 * firmware has ever measured that — the feature is argued from how an AMOLED
 * works, which is why HARDWARE.md §7b, whose first line is "All measured on
 * hardware", says nothing about it.
 *
 * The cube has to be ON BATTERY for the question to exist at all: on USB with a
 * full cell the PMU has opened BATFET and the board runs from VBUS with the
 * battery disconnected, so there is no drain to measure and no software can
 * create one (§7: BATFET cannot be commanded, there is no ship mode).
 *
 * So: pin brightness, force always-on so the panel never sleeps, and flip the
 * dim every AB_PHASE_MIN minutes, marking each flip in the existing power log.
 * Alternating twice rather than once because a single dim-then-bright pair
 * confounds the dim with everything that drifts over an hour — cell voltage
 * curve, Wi-Fi traffic, whether the now-playing card is up. Two paired phases
 * let the comparison survive that. Keep 0 in commits. */
#define CFG_DIM_AB 0
#define AB_PHASE_MIN 30
#define AB_BRIGHT    60   /* fixed, so both halves start from the same level */
#if CFG_DIM_SNAP
/* Declared HERE, under the flag, not beside the drift state it guards: that
 * state lives thousands of lines above this #define, where CFG_DIM_SNAP is
 * still undefined and the #if silently evaluates to 0 — the declaration
 * vanishes and only the use sites fail to compile. */
static volatile bool s_dim_snap_busy;   /* holds the main loop off mid-capture */
#endif

#if CFG_PERF_SCROLL_SELFTEST || CFG_SNAP_CHORD
#include "mbedtls/base64.h"
#endif
#if CFG_PERF_SCROLL_SELFTEST
static lv_obj_t *s_cfg_col;
#endif

#define CFG_ACCENT_WALL 0x22D3EE
#define CFG_ACCENT_DAYS 0x8B7CF6
#define CFG_ACCENT_DISP 0xA78BFA
#define CFG_ACCENT_LOCK 0xF59E0B
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
    /* The rounded corners and the translucent border are free. Measured
     * 2026-08-23 with both removed from every card: 2.75 Mpx/s against 2.77
     * with them, under the same scroll. Do not trade them away for speed;
     * the render cost is in the flat fill and text blend, not in the masks. */
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

/* The switch equivalent. cfg_switch() hands back the switch, but it lives in a
 * row next to its label, so fading only the switch leaves bright text over a
 * greyed control — worse than not fading at all. Dim the row; take CLICKABLE
 * off the switch, which is the object the indev actually hit-tests. */
static void cfg_switch_live(lv_obj_t *sw, bool live) {
    if (!sw) return;
    lv_obj_t *row = lv_obj_get_parent(sw);
    if (row) lv_obj_set_style_opa(row, live ? LV_OPA_COVER : LV_OPA_40, 0);
    if (live) lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    else      lv_obj_remove_flag(sw, LV_OBJ_FLAG_CLICKABLE);
}

/* A full-width slider sized for a finger rather than for a cursor. Both sliders
 * in this app go through here so they cannot drift apart. */
static int cfg_slider_index(const lv_obj_t *s) {
    for (int i = 0; i < s_cfg_slider_n; i++) if (s_cfg_sliders[i] == s) return i;
    return -1;
}

/* Snapshot on press, put it back if the column steals the press.
 *
 * The restore re-sends LV_EVENT_VALUE_CHANGED rather than only moving the knob,
 * because the app callbacks act on that event: cfg_bright_cb raises
 * s_req_bright_apply before its release check, so the panel has already been
 * told to change by the time the scroll is recognised. Re-sending is what undoes
 * it. Safe from inside a handler — LVGL re-enters lv_obj_send_event, and the
 * guard ignores VALUE_CHANGED so this cannot recurse. */
static void cfg_slider_guard_cb(lv_event_t *e) {
    lv_obj_t *s = lv_event_get_target(e);
    int i = cfg_slider_index(s);
    if (i < 0) return;

    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        s_cfg_slider_snap[i] = lv_slider_get_value(s);
        return;
    }
    /* LV_EVENT_PRESS_LOST */
    if (lv_slider_get_value(s) == s_cfg_slider_snap[i]) return;
    lv_slider_set_value(s, s_cfg_slider_snap[i], LV_ANIM_OFF);
    lv_obj_send_event(s, LV_EVENT_VALUE_CHANGED, NULL);
}

/* Sliders are un-hittable for as long as the column is moving, so a finger put
 * down to stop a fling lands on the column instead of on a control. Restoring
 * on SCROLL_END is safe against the arresting touch itself: LVGL binds a press
 * to an object when the finger lands, and at that moment the slider is still
 * not clickable, so the already-bound press stays with the column even after
 * the flag comes back. */
static void cfg_scroll_guard_cb(lv_event_t *e) {
    bool scrolling = lv_event_get_code(e) == LV_EVENT_SCROLL_BEGIN;
    for (int i = 0; i < s_cfg_slider_n; i++) {
        if (!s_cfg_sliders[i]) continue;
        if (scrolling) lv_obj_remove_flag(s_cfg_sliders[i], LV_OBJ_FLAG_CLICKABLE);
        else           lv_obj_add_flag(s_cfg_sliders[i],    LV_OBJ_FLAG_CLICKABLE);
    }
}

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

    /* After the app's own callback, so a restore on PRESS_LOST re-runs it with
     * the old value rather than racing it. */
    if (s_cfg_slider_n < CFG_SLIDER_MAX) {
        s_cfg_sliders[s_cfg_slider_n]     = s;
        s_cfg_slider_snap[s_cfg_slider_n] = val;
        s_cfg_slider_n++;
        lv_obj_add_event_cb(s, cfg_slider_guard_cb, LV_EVENT_PRESSED, NULL);
        lv_obj_add_event_cb(s, cfg_slider_guard_cb, LV_EVENT_PRESS_LOST, NULL);
    }
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

static void cfg_days_fetch_cb(lv_event_t *e) {
    (void)e;
    __atomic_store_n(&s_req_days_fetch, true, __ATOMIC_RELEASE);
    if (s_cfg_days_val) lv_label_set_text(s_cfg_days_val, "refresh queued...");
}

static void cfg_wifi_show(bool on) {
    if (!s_cfg_wifi) return;
    if (on) lv_obj_remove_flag(s_cfg_wifi, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(s_cfg_wifi, LV_OBJ_FLAG_HIDDEN);
}

/* Opens the setup scene, and starts a session if none is running. Flags only
 * for the start: it tears the Wi-Fi driver down, which is far too much to do
 * inside an LVGL callback. The scene paints its own "starting" state from the
 * tick, because unlike the old in-column card it is not fighting a relayout. */
static void cfg_ble_cb(lv_event_t *e) {
    cfg_wifi_show(true);
    if (s_cfg_ble_starting || ble_prov_active()) return;   /* just show it */
    s_req_ble_on = true;
    s_cfg_ble_starting = true;
    s_cfg_wifi_phase = 1;
    s_cfg_ble_key = -1;
    /* Paint the waiting state from HERE. The tick refreshes the scene every
     * 400 ms whether or not it is visible, so the panel was being revealed
     * with whatever the last tick drew — a green check, on a cube that was
     * already online — and only the next tick replaced it. */
    if (s_cfg_wifi_st)   lv_label_set_text(s_cfg_wifi_st, "starting bluetooth...");
    if (s_cfg_wifi_sub)  lv_label_set_text(s_cfg_wifi_sub, "");
    if (s_cfg_wifi_tick) lv_obj_add_flag(s_cfg_wifi_tick, LV_OBJ_FLAG_HIDDEN);
    if (s_cfg_ble_qr)    lv_obj_add_flag(s_cfg_ble_qr, LV_OBJ_FLAG_HIDDEN);
    if (s_cfg_ble_code)  lv_obj_add_flag(s_cfg_ble_code, LV_OBJ_FLAG_HIDDEN);
    if (s_cfg_ble_spin)  lv_obj_remove_flag(s_cfg_ble_spin, LV_OBJ_FLAG_HIDDEN);
    if (s_cfg_wifi_stop) lv_obj_remove_flag(s_cfg_wifi_stop, LV_OBJ_FLAG_HIDDEN);
}

/* The scene's own Stop. Distinct from the card button so that opening the
 * scene to READ a result never also kills a live session. */
static void cfg_wifi_stop_cb(lv_event_t *e) {
    if (ble_prov_active()) s_req_ble_off = true;
}

static void cfg_wifi_close_cb(lv_event_t *e) { cfg_wifi_show(false); }

/* What a join failure means, in words the person holding the cube can act on.
 * The phone is long gone by the time this is known — the radio came down to
 * let Wi-Fi up — so the panel is the only place it can be said. */
static const char *wifi_fail_text(int reason) {
    switch (reason) {
        case 201:                     return "network not found";
        case 2: case 15:
        case 202: case 204:           return "wrong password";
        case 203: case 205:           return "network refused the connection";
        case 200:                     return "no response from the network";
        default:                      return "could not connect";
    }
}

static void cfg_rotate_cb(lv_event_t *e) {
    rotation_bump();
}

static void cfg_autorot_cb(lv_event_t *e) {
    s_autorot = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    s_req_autorot_save = true;              /* NVS is the main loop's business */
    ESP_LOGI(TAG, "autorotate %s", s_autorot ? "ON" : "OFF");
}

static void cfg_clock_cb(lv_event_t *e) {
    s_clock_24 = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    s_req_clock_save = true;
    ESP_LOGI(TAG, "lock clock %s", s_clock_24 ? "24-hour" : "12-hour");
}

/* Defined with the app table, which this file only reaches further down. */
static int         lock_key_choices(void);
static int         lock_key_app_at(int slot);
static const char *lock_key_name(int app);
static const char *app_symbol(int app);

/* ---- shortcut picker: a CONTROL sub-scene ----
 *
 * Modelled on MUSIC's device picker: a full-screen panel over the app's own
 * screen, hidden rather than rebuilt, popped by the back key. A cycling button
 * was tried first and was wrong — you cannot see the options, only guess how
 * many taps away the one you want is. */
static lv_obj_t *s_cfg_pick, *s_cfg_picklist, *s_cfg_pickmore;
static int       s_cfg_more_n;      /* last count drawn, to change-gate */

/* How many rows are still below the fold, and say so — or stop saying it once
 * there is nothing left down there. A fixed "scroll for 2 more" that survives
 * reaching the bottom is worse than no hint: it tells you the list is lying. */
static void cfg_pick_more_update(void) {
    if (!s_cfg_pickmore || !s_cfg_picklist) return;
    int32_t below = lv_obj_get_scroll_bottom(s_cfg_picklist);
    int n = (below <= 4) ? 0        /* a little slack: rounding leaves a pixel or two */
                         : (int)((below + CFG_TOUCH_H + 11) / (CFG_TOUCH_H + 12));
    if (n == s_cfg_more_n) return;  /* this runs on every scroll frame */
    s_cfg_more_n = n;
    if (n == 0) {
        lv_obj_add_flag(s_cfg_pickmore, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text_fmt(s_cfg_pickmore, LV_SYMBOL_DOWN "   scroll for %d more", n);
    lv_obj_remove_flag(s_cfg_pickmore, LV_OBJ_FLAG_HIDDEN);
}

static void cfg_pick_scroll_cb(lv_event_t *e) { cfg_pick_more_update(); }

static void cfg_pick_show(bool on) {
    if (!s_cfg_pick) return;
    if (on) {
        /* Always open at the top; a picker that reopens halfway down looks
         * broken, and the hint would start out already half-spent. */
        if (s_cfg_picklist) lv_obj_scroll_to_y(s_cfg_picklist, 0, LV_ANIM_OFF);
        s_cfg_more_n = -1;                    /* force the first recount */
        cfg_pick_more_update();
        lv_obj_remove_flag(s_cfg_pick, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_cfg_pick, LV_OBJ_FLAG_HIDDEN);
    }
}

static void cfg_lockkey_label(void) {
    if (!s_cfg_lockkey_val) return;
    lv_label_set_text_fmt(s_cfg_lockkey_val, "Shortcut:  %s   " LV_SYMBOL_RIGHT,
                          lock_key_name(s_lock_key_app));
}

static void cfg_pick_row_cb(lv_event_t *e) {
    s_lock_key_app = (int)(intptr_t)lv_event_get_user_data(e);
    s_req_lock_pref_save = true;
    cfg_lockkey_label();
    ESP_LOGI(TAG, "lock middle key -> %s", lock_key_name(s_lock_key_app));
    cfg_pick_show(false);
}

static void cfg_pick_cancel_cb(lv_event_t *e) { cfg_pick_show(false); }

/* Opens the picker. */
static void cfg_lockkey_cb(lv_event_t *e) { cfg_pick_show(true); }

static void cfg_pick_row(lv_obj_t *parent, int app, const char *text,
                         lv_event_cb_t cb) {   /* text: NULL = name the app */
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, lv_pct(100), CFG_TOUCH_H);
    lv_obj_set_ext_click_area(b, 6);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x1B2432), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_color(b, lv_color_hex(CFG_ACCENT_LOCK), 0);
    /* The row already in force is drawn at full border opacity — a list of
     * identical rows does not say which one you are on. */
    lv_obj_set_style_border_opa(b, (!text && app == s_lock_key_app) ? 255 : 70, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(CFG_ACCENT_LOCK), LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)app);

    lv_obj_t *l = lv_label_create(b);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(0xE2E8F0), 0);
    const char *sym = text ? NULL : app_symbol(app);
    if (text)      lv_label_set_text(l, text);
    else if (sym)  lv_label_set_text_fmt(l, "%s   %s", sym, lock_key_name(app));
    else           lv_label_set_text(l, lock_key_name(app));
    lv_obj_center(l);
}

/* The Wi-Fi setup scene. Full-screen over CONTROL, hidden until asked for,
 * popped by the back key or its own close button. Nothing here is inside the
 * scrolling column, which is the whole point: the QR used to live in a card
 * between BATTERY and NETWORK, and scrolling past it rebooted the cube. */
static void cfg_wifi_build(lv_obj_t *scr) {
    s_cfg_wifi = lv_obj_create(scr);
    lv_obj_remove_style_all(s_cfg_wifi);
    lv_obj_set_size(s_cfg_wifi, 480, 480);
    lv_obj_center(s_cfg_wifi);
    lv_obj_set_style_bg_color(s_cfg_wifi, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_cfg_wifi, 250, 0);
    lv_obj_remove_flag(s_cfg_wifi, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cfg_wifi, LV_OBJ_FLAG_HIDDEN);
    /* Opaque, but it must still swallow taps aimed at the column underneath. */
    lv_obj_add_flag(s_cfg_wifi, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(s_cfg_wifi);
    lv_obj_set_style_text_font(t, &hud_text_18, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(CFG_ACCENT_NET), 0);
    lv_obj_set_style_text_letter_space(t, 4, 0);
    lv_label_set_text(t, "WI-FI SETUP");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 18);

    /* A visible way out, in the corner the device picker uses for the same
     * job. Closing does NOT stop a session: you can leave and come back. */
    lv_obj_t *x = lv_button_create(s_cfg_wifi);
    lv_obj_remove_style_all(x);
    lv_obj_set_size(x, 64, 64);
    lv_obj_set_pos(x, 400, 68);
    lv_obj_set_ext_click_area(x, 10);
    lv_obj_set_style_radius(x, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x10161F), 0);
    lv_obj_set_style_bg_opa(x, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(x, lv_color_hex(0x64748B), LV_STATE_PRESSED);
    lv_obj_add_event_cb(x, cfg_wifi_close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *xl = lv_label_create(x);
    lv_obj_set_style_text_font(xl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(xl, lv_color_hex(0x64748B), 0);
    lv_label_set_text(xl, LV_SYMBOL_CLOSE);
    lv_obj_center(xl);

    s_cfg_wifi_st = lv_label_create(s_cfg_wifi);
    lv_obj_set_width(s_cfg_wifi_st, CONTENT_W);
    lv_obj_set_height(s_cfg_wifi_st, 26);
    lv_obj_set_style_text_font(s_cfg_wifi_st, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_cfg_wifi_st, lv_color_hex(0xC7D2E0), 0);
    lv_obj_set_style_text_align(s_cfg_wifi_st, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_cfg_wifi_st, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_cfg_wifi_st, "");
    lv_obj_align(s_cfg_wifi_st, LV_ALIGN_TOP_MID, 0, 92);

    /* The spinner shares the QR's slot so the scene does not jump when one
     * replaces the other. Its animation runs on the LVGL task, which is what
     * lets it keep turning while the main loop is blocked in a scan. */
    s_cfg_ble_spin = lv_spinner_create(s_cfg_wifi);
    lv_obj_set_size(s_cfg_ble_spin, 64, 64);
    lv_obj_set_style_arc_color(s_cfg_ble_spin, lv_color_hex(0x1B2432), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_cfg_ble_spin, lv_color_hex(CFG_ACCENT_NET),
                               LV_PART_INDICATOR);
    lv_obj_align(s_cfg_ble_spin, LV_ALIGN_TOP_MID, 0, 192);
    lv_obj_add_flag(s_cfg_ble_spin, LV_OBJ_FLAG_HIDDEN);

    s_cfg_ble_qr = CFG_SETUP_QR ? lv_qrcode_create(s_cfg_wifi) : NULL;
    if (s_cfg_ble_qr) {
        lv_qrcode_set_size(s_cfg_ble_qr, 200);
        /* Black on WHITE, with the quiet zone left on. A QR drawn onto a dark
         * panel directly -- or with a transparent border -- is the classic
         * unscannable one: the encoder needs the light modules light and the
         * four-module margin is part of the symbol, not padding. */
        lv_qrcode_set_dark_color(s_cfg_ble_qr, lv_color_hex(0x000000));
        lv_qrcode_set_light_color(s_cfg_ble_qr, lv_color_hex(0xFFFFFF));
        lv_qrcode_set_quiet_zone(s_cfg_ble_qr, true);
        lv_obj_align(s_cfg_ble_qr, LV_ALIGN_TOP_MID, 0, 124);
        lv_obj_add_flag(s_cfg_ble_qr, LV_OBJ_FLAG_HIDDEN);
    }

    /* The result, in the slot the QR and spinner share: a big green check
     * you can read from across the desk. montserrat_48 is the largest
     * compiled-in face carrying LV_SYMBOL_OK; the hud fonts have no symbols.
     * Same green as the charging bolt and the battery bar, so "good" is one
     * colour everywhere on this device. */
    s_cfg_wifi_tick = lv_label_create(s_cfg_wifi);
    lv_obj_set_style_text_font(s_cfg_wifi_tick, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_cfg_wifi_tick, lv_color_hex(0x22C55E), 0);
    lv_label_set_text(s_cfg_wifi_tick, LV_SYMBOL_OK);
    lv_obj_align(s_cfg_wifi_tick, LV_ALIGN_TOP_MID, 0, 196);
    lv_obj_add_flag(s_cfg_wifi_tick, LV_OBJ_FLAG_HIDDEN);

    s_cfg_ble_code = lv_label_create(s_cfg_wifi);
    lv_obj_set_width(s_cfg_ble_code, CONTENT_W);
    lv_obj_set_style_text_font(s_cfg_ble_code, &hud_text_18, 0);
    lv_obj_set_style_text_color(s_cfg_ble_code, lv_color_hex(0x60A5FA), 0);
    lv_obj_set_style_text_letter_space(s_cfg_ble_code, 4, 0);
    lv_obj_set_style_text_align(s_cfg_ble_code, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_cfg_ble_code, "");
    lv_obj_align(s_cfg_ble_code, LV_ALIGN_TOP_MID, 0, 334);
    lv_obj_add_flag(s_cfg_ble_code, LV_OBJ_FLAG_HIDDEN);

    s_cfg_wifi_sub = lv_label_create(s_cfg_wifi);
    lv_obj_set_width(s_cfg_wifi_sub, CONTENT_W);
    lv_obj_set_height(s_cfg_wifi_sub, 40);
    lv_obj_set_style_text_font(s_cfg_wifi_sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cfg_wifi_sub, lv_color_hex(0x94A3B8), 0);
    lv_obj_set_style_text_align(s_cfg_wifi_sub, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_cfg_wifi_sub, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_cfg_wifi_sub, "");
    lv_obj_align(s_cfg_wifi_sub, LV_ALIGN_TOP_MID, 0, 360);

    /* 48 px plus a 12 px extended hit area: a 76 px control does not fit under
     * a 200 px QR inside the safe area, and this one is a single tap. */
    s_cfg_wifi_stop = lv_button_create(s_cfg_wifi);
    lv_obj_set_size(s_cfg_wifi_stop, CONTENT_W, 48);
    lv_obj_align(s_cfg_wifi_stop, LV_ALIGN_TOP_MID, 0, 388);
    lv_obj_set_ext_click_area(s_cfg_wifi_stop, 12);
    lv_obj_set_style_radius(s_cfg_wifi_stop, 16, 0);
    lv_obj_set_style_bg_color(s_cfg_wifi_stop, lv_color_hex(0x1B2432), 0);
    lv_obj_set_style_border_width(s_cfg_wifi_stop, 1, 0);
    lv_obj_set_style_border_color(s_cfg_wifi_stop, lv_color_hex(CFG_ACCENT_NET), 0);
    lv_obj_set_style_border_opa(s_cfg_wifi_stop, 150, 0);
    lv_obj_set_style_bg_color(s_cfg_wifi_stop, lv_color_hex(CFG_ACCENT_NET),
                              LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_cfg_wifi_stop, cfg_wifi_stop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_cfg_wifi_stop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *sl = lv_label_create(s_cfg_wifi_stop);
    lv_obj_set_style_text_font(sl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(sl, lv_color_hex(0xE2E8F0), 0);
    lv_label_set_text(sl, LV_SYMBOL_CLOSE "  Stop Wi-Fi setup");
    lv_obj_center(sl);
}

static void cfg_pick_build(lv_obj_t *scr) {
    s_cfg_pick = lv_obj_create(scr);
    lv_obj_remove_style_all(s_cfg_pick);
    lv_obj_set_size(s_cfg_pick, 480, 480);
    lv_obj_center(s_cfg_pick);
    lv_obj_set_style_bg_color(s_cfg_pick, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_cfg_pick, 250, 0);
    lv_obj_remove_flag(s_cfg_pick, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cfg_pick, LV_OBJ_FLAG_HIDDEN);
    /* Opaque, but it must still swallow taps aimed at the card underneath. */
    lv_obj_add_flag(s_cfg_pick, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t = lv_label_create(s_cfg_pick);
    lv_obj_set_style_text_font(t, &hud_text_18, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(CFG_ACCENT_LOCK), 0);
    lv_obj_set_style_text_letter_space(t, 4, 0);
    lv_label_set_text(t, "MIDDLE KEY ON LOCK");
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 18);

    s_cfg_picklist = lv_obj_create(s_cfg_pick);
    lv_obj_remove_style_all(s_cfg_picklist);
    /* Exactly three 76 px rows plus their two 12 px gutters. A window that
     * cuts a row in half is the honest way to say the list continues, and it
     * pairs with the hint below. */
    lv_obj_set_size(s_cfg_picklist, CONTENT_W, CFG_TOUCH_H * 3 + 24);
    lv_obj_align(s_cfg_picklist, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_flex_flow(s_cfg_picklist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_cfg_picklist, 12, 0);
    lv_obj_set_scroll_dir(s_cfg_picklist, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_cfg_picklist, LV_SCROLLBAR_MODE_OFF);
    /* Same reason as MUSIC's device list: scrolling this is a vertical drag,
     * and letting it bubble would swipe CONTROL home out from under the finger. */
    lv_obj_remove_flag(s_cfg_picklist, LV_OBJ_FLAG_GESTURE_BUBBLE);

    for (int slot = 0; slot < lock_key_choices(); slot++) {
        cfg_pick_row(s_cfg_picklist, lock_key_app_at(slot), NULL, cfg_pick_row_cb);
    }

    /* A visible way out, because a full-screen panel whose only exit is a key
     * nobody told you about is one people feel trapped in — the device picker
     * learned that too.
     *
     * Outside the list and deliberately unlike a row: the first version was a
     * sixth entry styled exactly like the apps above it, and it read as an app
     * called Cancel. Chrome has to look like chrome. */
    /* Five options in a three-row window read as a list of three — the two
     * below the fold may as well not exist. The label is built unconditionally
     * and cfg_pick_more_update() decides whether it says anything, so the one
     * place that knows the scroll position is the only place that decides. */
    s_cfg_pickmore = lv_label_create(s_cfg_pick);
    lv_obj_set_style_text_font(s_cfg_pickmore, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cfg_pickmore, lv_color_hex(0x64748B), 0);
    lv_label_set_text(s_cfg_pickmore, "");
    lv_obj_align(s_cfg_pickmore, LV_ALIGN_TOP_MID, 0, 346);
    lv_obj_remove_flag(s_cfg_pickmore, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_cfg_pickmore, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_cfg_picklist, cfg_pick_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_cfg_picklist, cfg_pick_scroll_cb, LV_EVENT_SCROLL_END, NULL);

    /* A hairline, then bare text. Two earlier tries both read as another
     * choice: a sixth list row styled like the apps (taken for an app called
     * Cancel), then a bordered pill below them (still a card, still tappable-
     * looking). Anything with a border belongs to the list. The rule the panel
     * settles on: bordered card = a thing you can pick, bare text below a rule
     * = a way out. */
    lv_obj_t *rule = lv_obj_create(s_cfg_pick);
    lv_obj_remove_style_all(rule);
    lv_obj_set_size(rule, CONTENT_W, 1);
    lv_obj_align(rule, LV_ALIGN_TOP_MID, 0, 368);
    lv_obj_set_style_bg_color(rule, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_bg_opa(rule, 60, 0);
    lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *back = lv_button_create(s_cfg_pick);
    /* First: it clears every local style, so size and align have to follow it
     * or they are wiped along with the fill and border. */
    lv_obj_remove_style_all(back);            /* no fill, no border, no radius */
    lv_obj_set_size(back, CONTENT_W, 56);
    lv_obj_align(back, LV_ALIGN_TOP_MID, 0, 380);
    lv_obj_set_ext_click_area(back, 10);
    lv_obj_add_event_cb(back, cfg_pick_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = lv_label_create(back);
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0x64748B), 0);
    lv_obj_set_style_text_color(bl, lv_color_hex(0xC7D2E0), LV_STATE_PRESSED);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "   Back");
    lv_obj_center(bl);
}

static void cfg_always_cb(lv_event_t *e) {
    /* The cosmetic half only. Flipping the MODE from a settings column while
     * the mode's real switch is a key-hold on the lock screen made two
     * owners for one state; this one now just decides whether the mode
     * wears its amber ring. */
    s_ao_ring_pref = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    s_req_lock_pref_save = true;
    ESP_LOGI(TAG, "desk-clock ring %s", s_ao_ring_pref ? "ON" : "OFF");
}

static void cfg_rings_cb(lv_event_t *e) {
    s_lock_rings = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    /* Deliberately does NOT touch desk clock. An earlier version force-
     * disabled it here so its indicator ring could not vanish while the mode
     * ran — and that coupling was worse than the problem: it silently threw
     * away a setting the user chose. The amber cue ring simply hides with
     * the rings; the mode itself is the user's business. */
    s_req_lock_pref_save = true;
    ESP_LOGI(TAG, "lock-screen rings %s", s_lock_rings ? "ON" : "OFF");
}

/* Minutes above a minute: "dim after  300 s" makes the reader do the division,
 * and the whole point of a stops table is that every value on it is one a
 * person would have said out loud. */
static void ao_dim_label(char *buf, size_t n, int secs) {
    if (secs >= 60 && secs % 60 == 0) {
        snprintf(buf, n, "dim after  %d min", secs / 60);
    } else {
        snprintf(buf, n, "dim after  %d s", secs);
    }
}

/* No brightness write here, and none is missing. This runs on the LVGL task and
 * the panel belongs to the main loop (pitfall #13); switching the feature off
 * simply makes the idle block's expression false on its next 20 ms pass, which
 * restores through ao_dim_set() — the one writer. Same shape as every other
 * setting in this app: record, raise a flag, let the loop act. */
static void cfg_aodim_cb(lv_event_t *e) {
    s_ao_dim_on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    s_req_lock_pref_save = true;
    ESP_LOGI(TAG, "desk-clock dim pref %s", s_ao_dim_on ? "ON" : "OFF");
}

/* Same shape as cfg_care_cb and for the same reasons: the readout is rewritten
 * on every VALUE_CHANGED so the delay reads true while the knob is moving — see
 * cfg_vol_cb for why that is safe now — and NVS waits for the main loop, because
 * committing per VALUE_CHANGED erases flash on every pixel of a drag. */
static void cfg_aodim_delay_cb(lv_event_t *e) {
    int i = clampi((int)lv_slider_get_value(lv_event_get_target(e)),
                   0, AO_DIM_STOPS - 1);
    s_ao_dim_s = s_ao_dim_stops[i];
    if (s_cfg_aodim_val) {
        char buf[32];
        ao_dim_label(buf, sizeof buf, s_ao_dim_s);
        lv_label_set_text(s_cfg_aodim_val, buf);
    }
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;

    s_req_lock_pref_save = true;
    ESP_LOGI(TAG, "desk-clock dim delay -> %d s", s_ao_dim_s);
}

/* Stated as a percentage, not as volts and not as a mode name. The percentage
 * is the number printed two lines above it in this same card, and the one every
 * other device states; "balanced 4.1V" asks the reader to learn a mapping first.
 * Volts are what we actually write, so the percentage is DERIVED from them
 * rather than written down beside them. The old table said 75/85/100 against
 * 4.00/4.10/4.20 V while a comment two lines up admitted 4.10 V reads ~87% —
 * the label and the hardware disagreed by four points, in writing, and the
 * charge countdown inherited it: it counted down to "85%" while the gauge sailed
 * through 86 and 87 with the charger still working, then withdrew its estimate
 * early because the target had notionally been passed. One source of truth
 * removes the whole class. */
static int chg_mode_pct(int mode) {
    int mv = chg_cv_mv(chg_mode_cv_code(mode));
    if (mv <= 0) return 100;
    /* The firmware's own voltage scale (HARDWARE.md §7: 3.30 V ~ 0%,
     * 4.20 V ~ 100%). Rounded at BOTH steps — truncating the division first
     * turns 77.8 into 77 and then into 75, which is how the old hand-written
     * table's error survived a re-derivation that was supposed to remove it. */
    int pct = ((mv - 3300) * 100 + 450) / 900;
    pct = ((pct + 2) / 5) * 5;              /* to the nearest 5 */
    return clampi(pct, 5, 100);
}

/* One line saying what the limit buys you. A bare "85%" states a number without
 * saying it is a ceiling or why anyone would want one; the percentage alone
 * needed a manual, which is the definition of the wrong label. Kept under ~34
 * characters so it stays one montserrat_20 line — cfg_text wraps, and a label
 * that grows a second line re-lays out the card under the finger. */
static const char *chg_mode_hint(int mode) {
    switch (mode) {
        case CHG_LIFESPAN: return "stops earliest, least wear";
        case CHG_BALANCED: return "stops early, less battery wear";
        default:           return "no limit, most battery wear";
    }
}

/* The enum ascends by how protective the mode is; the slider has to ascend by
 * how much charge you get, because right-means-more is not negotiable on a
 * control that sits directly under a percentage. Converted at the UI boundary
 * rather than by renumbering the enum, which would silently change the meaning
 * of an already-stored "chgmode". */
static int chg_mode_to_slider(int mode) { return CHG_LIFESPAN - mode; }
static int chg_slider_to_mode(int val)  { return CHG_LIFESPAN - val; }

/* Three stops on a slider rather than three buttons: it inherits the 76 px
 * touch sizing and the widened hit area that this panel needs, and a row of
 * buttons would have to fight the card's flex column for width. Percentage and
 * hint both follow the knob, for the reason cfg_vol_cb spells out above; three
 * stops is also the case that needed it most, since the knob alone cannot say
 * which of three unlabelled positions it landed on. chg_mode_pct() is pure
 * arithmetic on the CV table, not a charger read, so it is free to call per
 * step. */
static void cfg_care_cb(lv_event_t *e) {
    int val = clampi((int)lv_slider_get_value(lv_event_get_target(e)),
                     CHG_FULL, CHG_LIFESPAN);
    s_chg_mode = chg_slider_to_mode(val);
    if (s_cfg_care_val) {
        lv_label_set_text_fmt(s_cfg_care_val, "charge limit  %d%%",
                              chg_mode_pct(s_chg_mode));
    }
    if (s_cfg_care_sub) lv_label_set_text(s_cfg_care_sub, chg_mode_hint(s_chg_mode));
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;

    s_req_chg_save = true;
    ESP_LOGI(TAG, "battery care -> charge to %d%%", chg_mode_pct(s_chg_mode));
}

/* No RELEASED guard, unlike the sliders in this card: a switch commits once on
 * a tap rather than streaming values through a drag, so there is no label to
 * re-lay out under a moving finger. The NVS write is still deferred to the main
 * loop, because this callback holds the LVGL lock and flash erases must not. */
static void cfg_chgeta_cb(lv_event_t *e) {
    s_chg_eta_on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    s_req_chg_save = true;
    ESP_LOGI(TAG, "charge countdown %s", s_chg_eta_on ? "ON" : "OFF");
}

/* Arms a single full charge, which reverts itself at charge-done. It is also
 * the only way to re-learn the fuel gauge: the coulomb counter only recalibrates
 * across a complete cycle, and a capped cube never gives it one. */
static void cfg_care_once_cb(lv_event_t *e) {
    s_chg_once = !s_chg_once;
    s_chg_once_seen = false;
    s_chg_once_idle = 0;
    s_req_chg_save = true;
    ESP_LOGI(TAG, "one-shot full charge %s", s_chg_once ? "ARMED" : "cancelled");
}

/* Same shape as cfg_vol_cb and for the same three reasons: the readout is
 * rewritten on every VALUE_CHANGED, which is safe because the label's geometry
 * is pinned (see cfg_vol_cb); the panel write is left to the main loop, because
 * it is QSPI IO that needs the LVGL lock this callback already holds; and NVS
 * waits too, because committing per VALUE_CHANGED would erase flash on every
 * pixel of the drag. The glass is already brightening under the finger — the
 * number was the one part of this control that lagged behind it. */
static void cfg_bright_cb(lv_event_t *e) {
    s_bright = clampi((int)lv_slider_get_value(lv_event_get_target(e)),
                      BRIGHT_MIN, 100);
    s_req_bright_apply = true;
    if (s_cfg_bright_val) {
        lv_label_set_text_fmt(s_cfg_bright_val, "brightness  %d%%", s_bright);
    }
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;

    s_req_bright_save = true;
}

/* The number follows the knob; only the commits wait for the finger to lift.
 * This is the comment the other three sliders on this panel point at.
 *
 * Rewriting the label on every LV_EVENT_VALUE_CHANGED USED to be a crash, and
 * the reason is worth keeping: the text length changes, the label sat in a
 * LV_SIZE_CONTENT flex card, so the card and the whole scrolling column re-laid
 * out dozens of times a second — which moves the slider itself while LVGL is
 * midway through delivering an input event to that very slider. What removed
 * the hazard was not the release guard but the fix underneath it: every readout
 * above a slider here is now pinned to lv_pct(100) by one montserrat_20 line
 * (26 px), so the text can change all it likes and the geometry does not. The
 * guard outlived the bug it was standing in for, and a value that only appears
 * after you let go reads as a control that did not hear you. Those pinned
 * heights are now load-bearing: a readout on this panel that goes back to
 * LV_SIZE_CONTENT brings the crash back with it.
 *
 * NVS is written from the main loop rather than here, because a commit erases
 * flash and can block for tens of milliseconds with the LVGL lock held. The
 * confirmation clip waits for release too — one per drag, not one per pixel. */
static void cfg_vol_cb(lv_event_t *e) {
    s_vol = (int)lv_slider_get_value(lv_event_get_target(e));
    if (s_cfg_vol_val) {
        /* integer dB, not %f: newlib's float formatting is stack-hungry and this
         * runs on the LVGL task, whose 8 KB is already carrying the renderer */
        lv_label_set_text_fmt(s_cfg_vol_val, "volume  %d%%   /   %+d dB",
                              s_vol, (int)vol_db(s_vol));
    }
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;

    s_req_vol_save = true;
    sfx_play(SFX_DONE);      /* the loudest clip: the one to judge a level by */
}

static void cfg_timer_cb(lv_timer_t *t) {
    if (!s_cfg_sys_val) return;

    /* ---- wallpaper ---- */
    int have = __builtin_popcount(s_wall_have);
    label_set_fmt_changed(s_cfg_wall_pool, "%d / %d wallpapers ready", have, WALL_SLOTS);

    /* clear a finished result after a few seconds so the card returns to rest */
    if ((s_dl_state == DL_OK || s_dl_state == DL_FAIL) &&
        now_ms() - s_dl_ended_ms > 6000) {
        s_dl_state = DL_IDLE;
    }

    switch (s_dl_state) {
    case DL_QUERY:
        obj_set_hidden_changed(s_cfg_wall_bar, false);
        bar_set_changed(s_cfg_wall_bar, 0);
        bg_color_set_changed(s_cfg_wall_bar, lv_color_hex(CFG_ACCENT_WALL),
                             LV_PART_INDICATOR);
        text_color_set_changed(s_cfg_wall_state, lv_color_hex(CFG_ACCENT_WALL));
        label_set_changed(s_cfg_wall_state, "asking unsplash...");
        break;
    case DL_IMAGE:
        obj_set_hidden_changed(s_cfg_wall_bar, false);
        bar_set_changed(s_cfg_wall_bar, s_dl_pct);
        bg_color_set_changed(s_cfg_wall_bar, lv_color_hex(CFG_ACCENT_WALL),
                             LV_PART_INDICATOR);
        text_color_set_changed(s_cfg_wall_state, lv_color_hex(CFG_ACCENT_WALL));
        if (s_dl_total > 0) {
            label_set_fmt_changed(s_cfg_wall_state, "downloading  %d%%   %d KB",
                                  s_dl_pct, s_dl_kb);
        } else {
            label_set_fmt_changed(s_cfg_wall_state, "downloading  %d KB", s_dl_kb);
        }
        break;
    case DL_OK:
        obj_set_hidden_changed(s_cfg_wall_bar, false);
        bar_set_changed(s_cfg_wall_bar, 100);
        bg_color_set_changed(s_cfg_wall_bar, lv_color_hex(0x35C759),
                             LV_PART_INDICATOR);
        text_color_set_changed(s_cfg_wall_state, lv_color_hex(0x35C759));
        label_set_fmt_changed(s_cfg_wall_state, LV_SYMBOL_OK "  saved  /  %d KB", s_dl_kb);
        break;
    case DL_FAIL:
        obj_set_hidden_changed(s_cfg_wall_bar, false);
        bar_set_changed(s_cfg_wall_bar, 100);
        bg_color_set_changed(s_cfg_wall_bar, lv_color_hex(0xFF453A),
                             LV_PART_INDICATOR);
        text_color_set_changed(s_cfg_wall_state, lv_color_hex(0xFF453A));
        label_set_changed(s_cfg_wall_state, LV_SYMBOL_WARNING "  fetch failed");
        break;
    default:
        obj_set_hidden_changed(s_cfg_wall_bar, true);
        text_color_set_changed(s_cfg_wall_state, lv_color_hex(0x64748B));
        label_set_changed(s_cfg_wall_state,
                          s_req_wallpaper ? "queued..." : "refreshes every 6 hours");
        break;
    }

    if (s_dl_theme[0] && s_dl_state != DL_IDLE) {
        label_set_fmt_changed(s_cfg_wall_sub, "theme  %s", s_dl_theme);
    } else if (s_wall_credit[0]) {
        label_set_fmt_changed(s_cfg_wall_sub, "on screen  %s / Unsplash", s_wall_credit);
    } else {
        label_set_changed(s_cfg_wall_sub, "");
    }

    /* ---- days ---- */
    days_blob_t days;
    days_snapshot(&days, NULL);
    bool days_queued = __atomic_load_n(&s_req_days_fetch, __ATOMIC_ACQUIRE);
    bool days_fetching = __atomic_load_n(&s_days_fetching, __ATOMIC_ACQUIRE);
    char days_status[96];
    if (days_fetching) {
        snprintf(days_status, sizeof(days_status), "refreshing countdown...");
    } else if (days_queued) {
        snprintf(days_status, sizeof(days_status), "refresh queued...");
    } else if (days.target[0]) {
        snprintf(days_status, sizeof(days_status),
                 "target  %s\nready  /  daily auto-refresh", days.target);
    } else {
        snprintf(days_status, sizeof(days_status),
                 "no countdown saved\nset one at the /days web page");
    }
    label_set_changed(s_cfg_days_val, days_status);

    /* ---- display ---- */
    label_set_fmt_changed(s_cfg_rot_val,
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

    /* The amber-ring preference is meaningless without rings to host it —
     * an amber ring cannot appear in a ring set that does not exist — so it
     * fades with them. Only the cosmetic switch fades: the desk-clock MODE
     * lives on the lock screen's right-key hold and never asks this card. */
    if (s_cfg_always_sw &&
        s_lock_rings != lv_obj_has_flag(s_cfg_always_sw, LV_OBJ_FLAG_CLICKABLE)) {
        cfg_switch_live(s_cfg_always_sw, s_lock_rings);
    }

    /* ---- battery ---- */
    int pct = s_batt_pct;
    bar_set_changed(s_cfg_batt_bar, pct < 0 ? 100 : pct);
    bg_color_set_changed(s_cfg_batt_bar,
        lv_color_hex(pct < 0 ? 0x4A9EFF
                    : pct >= 50 ? 0x35C759
                    : pct >= 20 ? 0xFFB020 : 0xFF453A), LV_PART_INDICATOR);
    /* "charging" is no longer the same question as "plugged in" — with a cap in
     * force the charger stops long before the cube leaves the dock, so say which
     * of the three states it actually is. */
    const char *power = s_batt_charging ? "   " LV_SYMBOL_CHARGE " charging"
                      : s_bypass        ? "   on USB - bypass"
                      : s_vbus          ? "   plugged" : "";
    if (pct < 0) {
        label_set_fmt_changed(s_cfg_batt_val, "external power%s", power);
    } else {
        label_set_fmt_changed(s_cfg_batt_val, "%d%%   %d mV%s", pct, s_batt_mv, power);
    }
    int drain = battery_drain_mv_h();
    int eta = s_chg_eta_mins;
    /* While current is flowing the drain figure is not just uninteresting, it
     * is unmeasurable — the line said "drain measuring..." for the whole of
     * every charge. The countdown is the number that belongs in that slot. */
    if (eta > 0 && s_chg_eta_on) {
        char eta_buf[32];
        chg_eta_text(eta_buf, sizeof eta_buf, eta);
        label_set_fmt_changed(s_cfg_batt_sub, "%s   /   %s", eta_buf,
                              s_doze ? "dozing" : "active");
    } else if (drain > 0) {
        label_set_fmt_changed(s_cfg_batt_sub, "drain  %d mV/h   /   %s", drain,
                              s_doze ? "dozing" : "active");
    } else {
        label_set_fmt_changed(s_cfg_batt_sub, "drain  measuring...   /   %s",
                              s_doze ? "dozing" : "active");
    }
    if (s_cfg_care_val) {
        if (s_chg_once) {
            label_set_changed(s_cfg_care_val, "charging to 100% once");
        } else {
            label_set_fmt_changed(s_cfg_care_val, "charge limit  %d%%",
                                  chg_mode_pct(s_chg_mode));
        }
    }
    if (s_cfg_care_sub) {
        if (s_chg_once) {
            label_set_fmt_changed(s_cfg_care_sub, "then back to the %d%% limit",
                                  chg_mode_pct(s_chg_mode));
        } else {
            label_set_changed(s_cfg_care_sub, chg_mode_hint(s_chg_mode));
        }
    }
    if (s_cfg_care_btn) {
        /* Nothing to top up to when Full is already the target. */
        cfg_button_live(s_cfg_care_btn, s_chg_mode != CHG_FULL);
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
            "wi-fi received", "finishing", "could not connect",
        };
        bool act = ble_prov_active();
        ble_prov_state_t st = act ? ble_prov_state() : BLE_PROV_OFF;
        bool joining = !act && !s_wifi_torn_down && s_creds_pending && !s_wifi_up;
        bool failed  = joining && s_wifi_reason != 0;

        /* The scene. Every setter below is change-gated, and none of these
         * widgets sit in a content-sized flex card, so writing them each tick
         * costs nothing when nothing moved. */
        const char *text, *sub = "";
        char subbuf[96];
        bool spin = false, show_qr = false, show_stop = false;
        bool in_flight = s_cfg_ble_starting || act || s_wifi_torn_down || joining;
        if (in_flight) s_cfg_wifi_phase = 1;
        else if (s_cfg_wifi_phase == 1) s_cfg_wifi_phase = 2;
        bool result = (s_cfg_wifi_phase == 2);
        int glyph = 0;                 /* 0 none, 1 green check, 2 red cross */
        if (s_cfg_ble_starting) {
            text = "starting bluetooth..."; spin = true; show_stop = true;
        } else if (act) {
            text = st <= BLE_PROV_ERR ? ble_st[st] : "idle";
            show_stop = true;
            if (st == BLE_PROV_ADV || st == BLE_PROV_LINKED ||
                st == BLE_PROV_AUTHED) {
                show_qr = true;
                sub = "scan the code with your phone, or type it";
            } else if (st == BLE_PROV_HANDOFF || st == BLE_PROV_DONE) {
                spin = true;
                sub = "switching networks...";
            } else if (st == BLE_PROV_ERR) {
                sub = "the phone could not pair - try again";
            }
        } else if (s_wifi_torn_down) {
            text = "restoring wi-fi..."; spin = true;
        } else if (failed) {
            text = wifi_fail_text(s_wifi_reason);
            snprintf(subbuf, sizeof(subbuf), "%.32s  (reason %d)  -  retrying",
                     s_ssid, s_wifi_reason);
            sub = subbuf; glyph = 2;
        } else if (joining) {
            snprintf(subbuf, sizeof(subbuf), "connecting to  %.32s", s_ssid);
            text = subbuf; spin = true;
        } else if (s_wifi_up) {
            text = "connected"; glyph = result ? 1 : 0;
            snprintf(subbuf, sizeof(subbuf), "%.32s   %s", s_ssid, s_ip);
            sub = subbuf;
        } else if (s_wifi_disabled) {
            text = "wi-fi is off";
            sub = "you chose to disconnect - set up to reconnect";
            glyph = result ? 2 : 0;
        } else {
            text = "not connected";
            sub = s_wifi_reason ? wifi_fail_text(s_wifi_reason) : "";
            glyph = result ? 2 : 0;
        }
        label_set_changed(s_cfg_wifi_st, text);
        label_set_changed(s_cfg_wifi_sub, sub);
        obj_set_hidden_changed(s_cfg_ble_spin, !spin);
        obj_set_hidden_changed(s_cfg_ble_code, !show_qr);
        if (s_cfg_ble_qr) obj_set_hidden_changed(s_cfg_ble_qr, !show_qr);
        obj_set_hidden_changed(s_cfg_wifi_stop, !show_stop);
        if (glyph && s_cfg_wifi_tick) {
            /* One label, two faces. Colour is change-gated by hand: a style
             * setter invalidates even when the value is unchanged. */
            lv_color_t want = lv_color_hex(glyph == 1 ? 0x22C55E : 0xEF4444);
            if (!lv_color_eq(lv_obj_get_style_text_color(s_cfg_wifi_tick, 0), want)) {
                lv_obj_set_style_text_color(s_cfg_wifi_tick, want, 0);
            }
            label_set_changed(s_cfg_wifi_tick,
                              glyph == 1 ? LV_SYMBOL_OK : LV_SYMBOL_CLOSE);
        }
        obj_set_hidden_changed(s_cfg_wifi_tick, glyph == 0);

        /* The card in the column: one line, and a button that only ever OPENS
         * the scene. Change-gated for the reason the comment above gives. */
        label_set_changed(s_cfg_ble_val,
                          s_cfg_ble_starting || act ? "pairing in progress" :
                          s_wifi_torn_down || joining ? "connecting..." :
                          s_wifi_up ? "wi-fi connected" : "not connected");
        label_set_changed(lv_obj_get_child(s_cfg_ble_btn, 0),
                          s_cfg_ble_starting || act ? "Open Wi-Fi setup"
                                                    : "Set up / change Wi-Fi");

        /* The code itself changes only per session, but it is cheap to keep in
         * step and it must be right the moment the scene is shown. */
        if (act) {
            label_set_fmt_changed(s_cfg_ble_code, "CODE  %s", ble_prov_code());

            /* The QR is not cheap the same way a label is: an update re-runs the
             * encoder and repaints a 33x33 bitmap, so it is gated on the code
             * rather than refreshed per tick. The URL carries the code in a
             * FRAGMENT, which never reaches the page's host, so scanning it does
             * not hand the pairing code to GitHub's logs. It is no more secret
             * than the digits printed directly below the QR. */
            if (CFG_SETUP_QR && s_cfg_ble_qr &&
                strcmp(s_cfg_ble_qr_code, ble_prov_code()) != 0) {
                char url[160];
                int n = snprintf(url, sizeof(url), "%s#c=%s",
                                 SETUP_URL, ble_prov_code());
                if (n > 0 && n < (int)sizeof(url) &&
                    lv_qrcode_update(s_cfg_ble_qr, url, (uint32_t)n) == LV_RESULT_OK) {
                    snprintf(s_cfg_ble_qr_code, sizeof(s_cfg_ble_qr_code),
                             "%s", ble_prov_code());
                    /* Once per session, and it is the only way to tell from the
                     * console that the panel is showing a scannable symbol for
                     * the right code -- a QR is not something the status line
                     * can summarise, and the alternative is a photograph. */
                    ESP_LOGI(TAG, "setup QR -> %s", url);
                } else {
                    ESP_LOGW(TAG, "setup QR encode failed (%d bytes)", n);
                }
            }
        }
    }

    /* ---- network ---- */
    wifi_ap_record_t ap;
    bool have_ap = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    int rssi = have_ap ? ap.rssi : 0;
    const char *strength = !have_ap  ? "-" :
                           rssi >= -50 ? "excellent" :
                           rssi >= -60 ? "good" :
                           rssi >= -70 ? "fair" : "weak";
    label_set_fmt_changed(s_cfg_net_val,
                          "%s%s\nip  %s\nsignal  %s",
                          s_wifi_up ? LV_SYMBOL_OK "  " : "",
                          s_wifi_up ? s_ssid : (s_wifi_disabled ? "wi-fi off" : "offline"),
                          s_ip[0] ? s_ip : "-",
                          strength);

    /* ---- system ---- */
    unsigned long up_s = (unsigned long)(now_ms() / 1000);
    char uptime[20];
    if (up_s < 60) {
        snprintf(uptime, sizeof(uptime), "%lus", up_s);
    } else if (up_s < 3600) {
        snprintf(uptime, sizeof(uptime), "%lum %02lus", up_s / 60, up_s % 60);
    } else if (up_s < 86400) {
        snprintf(uptime, sizeof(uptime), "%luh %02lum", up_s / 3600,
                 (up_s / 60) % 60);
    } else {
        snprintf(uptime, sizeof(uptime), "%lud %02luh", up_s / 86400,
                 (up_s / 3600) % 24);
    }
    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    unsigned psram_used_x10 = (unsigned)(((psram_total - psram_free) * 10) / (1024 * 1024));
    unsigned psram_total_x10 = (unsigned)((psram_total * 10) / (1024 * 1024));
    label_set_fmt_changed(s_cfg_sys_val,
                          "uptime  %s\n"
                          "render  %u.%u fps\n"
                          "memory  %u KB available\n"
                          "psram  %u.%u MB used  /  %u.%u MB\n"
                          "sdcard  %s  /  %lu log rows\n"
                          "firmware  %s",
                          uptime,
                          (unsigned)(s_last_fps_x10 / 10), (unsigned)(s_last_fps_x10 % 10),
                          (unsigned)(hp_free() / 1024),
                          psram_used_x10 / 10, psram_used_x10 % 10,
                          psram_total_x10 / 10, psram_total_x10 % 10,
                          s_sd_ok ? "mounted" : "none", (unsigned long)s_tele_rows,
                          esp_app_get_description()->version);

    if (s_log_mtx && xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
        char buf[LOG_LINES * sizeof(s_log[0]) + 8];
        snprintf(buf, sizeof(buf), "%s\n%s\n%s", s_log[0], s_log[1], s_log[2]);
        xSemaphoreGive(s_log_mtx);
        label_set_changed(s_cfg_log, buf);
    }
}

static void build_control_app(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x05070B), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* The previous screen's sliders are already deleted by the time we get here
     * (free first, build second), so the table holds dangling pointers until the
     * cards below repopulate it. Reset before anything can walk it. */
    s_cfg_slider_n = 0;
    memset(s_cfg_sliders, 0, sizeof(s_cfg_sliders));

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &hud_text_18, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE8FBFF), 0);
    lv_obj_set_style_text_letter_space(title, 6, 0);
    lv_label_set_text(title, "CONTROL");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, TOP_MARGIN);

    /* Use the same panel-safe width as the keyboard. At y=436 its centred
     * x=28..452 footprint still clears the rounded corners, while the extra
     * 60 px makes the intentionally thick sliders look proportional. */
    lv_obj_t *col = lv_obj_create(scr);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, KB_W, 372);
    lv_obj_align(col, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(col, 12, 0);
    lv_obj_set_style_pad_all(col, 2, 0);
    /* Let the final row clear the viewport instead of stopping half-visible at
     * the bottom edge. This padding is scrollable content, not dead screen area. */
    lv_obj_set_style_pad_bottom(col, CFG_TOUCH_H + 24, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_ACTIVE);
    /* Registered on the column rather than on each slider: SCROLL_BEGIN/END are
     * raised by whatever is scrolling, and the sliders are not it. */
    lv_obj_add_event_cb(col, cfg_scroll_guard_cb, LV_EVENT_SCROLL_BEGIN, NULL);
    lv_obj_add_event_cb(col, cfg_scroll_guard_cb, LV_EVENT_SCROLL_END, NULL);
#if CFG_PERF_SCROLL_SELFTEST
    s_cfg_col = col;
#endif

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

    /* ---- days ---- */
    c = cfg_card(col, "DAYS", CFG_ACCENT_DAYS);
    s_cfg_days_val = cfg_text(c, 0xC7D2E0);
    cfg_button(c, LV_SYMBOL_REFRESH "  Refresh countdown",
               CFG_ACCENT_DAYS, cfg_days_fetch_cb);

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

    /* ---- lock screen ---- */
    c = cfg_card(col, "LOCK SCREEN", CFG_ACCENT_LOCK);
    /* Rings first, then the switch that depends on them. The second switch
     * is COSMETIC: whether desk-clock mode (toggled by holding the right key
     * on the lock screen — its only real switch) announces itself with an
     * amber ring. With the rings off it fades, because an amber ring cannot
     * appear in a ring set that does not exist — while the MODE stays none
     * of this card's business. */
    s_cfg_rings_sw = cfg_switch(c, "Lock-screen rings", CFG_ACCENT_LOCK,
                                s_lock_rings, cfg_rings_cb);
    s_cfg_always_sw = cfg_switch(c, "Amber ring in desk clock", CFG_ACCENT_LOCK,
                                 s_ao_ring_pref, cfg_always_cb);
    s_cfg_time_sw = cfg_switch(c, "24-hour time", CFG_ACCENT_LOCK,
                               s_clock_24, cfg_clock_cb);

    /* Grouped with the lock-screen toggles because that is where the user
     * looks for it — this switch governs a caption on the LOCK SCREEN, and
     * that is what someone reaching for it has in mind. It does also gate the
     * CONTROL battery sub-line, which is the argument for putting it in
     * BATTERY instead; that argument lost, because a setting belongs where it
     * will be looked for rather than where its implementation reaches.
     *
     * "Charge countdown" was the first label and it was not one: it names an
     * implementation, and a reader who has never seen the caption cannot tell
     * from those two words what would appear or where. The sub-line says what
     * it is in the words someone would use asking for it. Under ~34 characters
     * so it stays one montserrat_20 line — cfg_text wraps, and a label that
     * grows a second line re-lays out the card under the finger. */
    s_cfg_chgeta_sw = cfg_switch(c, "Charge time estimate", CFG_ACCENT_LOCK,
                                 s_chg_eta_on, cfg_chgeta_cb);
    s_cfg_chgeta_sub = cfg_text(c, 0x64748B);
    lv_obj_set_height(s_cfg_chgeta_sub, 26);   /* one montserrat_20 line, uncl'd */
    lv_label_set_text(s_cfg_chgeta_sub, "show charging time estimation");

    /* The desk-clock dim belongs in THIS card, not in DISPLAY, because it only
     * ever acts while the lock screen's own mode is holding the panel awake.
     * DISPLAY's brightness slider is the level this takes a percentage OF, not
     * a sibling of it. */
    s_cfg_aodim_sw = cfg_switch(c, "Dim in desk clock", CFG_ACCENT_LOCK,
                                s_ao_dim_on, cfg_aodim_cb);
    s_cfg_aodim_val = cfg_text(c, 0xC7D2E0);
    lv_obj_set_height(s_cfg_aodim_val, 26);    /* one montserrat_20 line, uncl'd */
    {
        char buf[32];
        ao_dim_label(buf, sizeof buf, s_ao_dim_s);
        lv_label_set_text(s_cfg_aodim_val, buf);
    }
    s_cfg_aodim_sub = cfg_text(c, 0x64748B);
    lv_obj_set_height(s_cfg_aodim_sub, 26);    /* one montserrat_20 line, uncl'd */
    /* Says what undoes it. A control that darkens the screen on a timer needs
     * to state its own escape hatch on the card, or the first time it fires it
     * reads as the panel failing. Kept under ~34 characters so it stays one
     * montserrat_20 line — cfg_text wraps, and a label that grows a second line
     * re-lays out the card under the finger. */
    lv_label_set_text(s_cfg_aodim_sub, "touch or a key brings it back");
    /* Seven stops rather than a seconds range, and deliberately NOT faded while
     * the switch above is off — unlike every other dependent control in this
     * app. cfg_scroll_guard_cb() owns LV_OBJ_FLAG_CLICKABLE on every registered
     * slider and re-asserts it on SCROLL_END, so cfg_button_live()'s fade would
     * quietly come undone the first time the column moved, leaving a control
     * that looks dead and is not. Choosing a delay before switching the feature
     * on is harmless and it is remembered. */
    s_cfg_aodim_sld = cfg_slider(c, 0, AO_DIM_STOPS - 1,
                                 ao_dim_to_slider(s_ao_dim_s),
                                 CFG_ACCENT_LOCK, 0xFDE68A, cfg_aodim_delay_cb);

    /* The middle key is the only one genuinely free on the lock screen: the
     * left key locks, the right key's HOLD is the desk-clock toggle, and PWR
     * falls through to nothing at all. Point it at an app.
     *
     * The right key's tap was tried and reverted. It is nominally free, but
     * the same key's hold is always-on, so a hold that came up a little short
     * launched an app instead of toggling the clock — two unrelated verbs on
     * one key, separated only by how long you held it.
     *
     * PWR being a PMU key is safe here: we read only the latched SHORT-press
     * IRQ (reg 0x49 bit 3) and never enable long-press, so this adds no reboot
     * risk. The PMU's own ~10 s hard cut is hardware and stays either way. */
    s_cfg_lockkey_btn = cfg_button(c, "", CFG_ACCENT_LOCK, cfg_lockkey_cb);
    s_cfg_lockkey_val = lv_obj_get_child(s_cfg_lockkey_btn, 0);   /* its label */
    cfg_lockkey_label();
    /* This button now sits under a slider, so it needs the same clearance the
     * BATTERY card's does: cfg_slider adds CFG_EXT_CLICK (18 px) of invisible
     * hit area below its track and cfg_button adds 6 px above itself, which is
     * more than the card's 14 px gutter — finishing a drag near the bottom of
     * the delay slider would otherwise fire this button. */
    lv_obj_set_style_margin_top(s_cfg_lockkey_btn, 18, 0);

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

    s_cfg_care_val = cfg_text(c, 0xC7D2E0);
    lv_obj_set_height(s_cfg_care_val, 26);     /* one montserrat_20 line, uncl'd */
    lv_label_set_text_fmt(s_cfg_care_val, "charge limit  %d%%",
                          chg_mode_pct(s_chg_mode));
    s_cfg_care_sub = cfg_text(c, 0x64748B);
    lv_obj_set_height(s_cfg_care_sub, 26);     /* one montserrat_20 line, uncl'd */
    lv_label_set_text(s_cfg_care_sub, chg_mode_hint(s_chg_mode));
    s_cfg_care_sld = cfg_slider(c, CHG_FULL, CHG_LIFESPAN,
                                chg_mode_to_slider(s_chg_mode),
                                CFG_ACCENT_BATT, 0xA7F3D0, cfg_care_cb);
    s_cfg_care_btn = cfg_button(c, "CHARGE TO 100% ONCE", CFG_ACCENT_BATT,
                                cfg_care_once_cb);
    /* The card's 14 px gutter is not enough under a slider. cfg_slider adds
     * CFG_EXT_CLICK (18 px) of invisible hit area below the track and cfg_button
     * adds 6 px above itself, so the two touch targets overlapped and finishing
     * a drag near the bottom of the track fired the button. 14 + 18 clears both
     * with room to spare. */
    lv_obj_set_style_margin_top(s_cfg_care_btn, 18, 0);

    /* ---- wi-fi setup ---- */
    /* Directly above NETWORK: setup is what you reach for when the readout
     * below says "offline" or you want to change networks. BLE is only the
     * transport, so the user-facing wording names the task instead. */
    c = cfg_card(col, "WI-FI SETUP", CFG_ACCENT_NET);
    s_cfg_ble_val = cfg_text(c, 0xC7D2E0);
    lv_obj_set_height(s_cfg_ble_val, 26);      /* one montserrat_20 line, uncl'd */
    s_cfg_ble_btn = cfg_button(c, "Set up / change Wi-Fi",
                               CFG_ACCENT_NET, cfg_ble_cb);
    /* The card is rebuilt on every app open, so the cache of what the QR holds
     * has to be cleared with it or the update is skipped as a no-op. */
    s_cfg_ble_qr_code[0] = '\0';

    /* ---- network ---- */
    c = cfg_card(col, "NETWORK", CFG_ACCENT_NET);
    s_cfg_net_val = cfg_text(c, 0xC7D2E0);

    /* ---- system ---- */
    c = cfg_card(col, "SYSTEM", CFG_ACCENT_SYS);
    s_cfg_sys_val = cfg_text(c, 0x94A3B8);
    s_cfg_log = cfg_text(c, 0x475569);

    lv_obj_add_event_cb(scr, gesture_home_cb, LV_EVENT_GESTURE, NULL);
    /* Last, so the panel is the topmost child and covers the whole column when
     * it is shown. Built once with the screen and hidden, never created per
     * open — the same shape as MUSIC's device picker. */
    cfg_wifi_build(scr);
    cfg_pick_build(scr);

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

/* Clockwise from the top edge, matching the layout on screen. */
static const uint8_t s_pomo_min[POMO_SLOTS] = { 60, 10, 5, 30 };

#define POMO_TAP_GRACE_MS 1200   /* DONE ignores the tap that *ended* near zero */

typedef enum { POMO_IDLE = 0, POMO_RUN, POMO_PAUSE, POMO_DONE } pomo_state_t;

/* Session state is file-scope, not owned by the screen: a countdown keeps
 * running if you navigate away, and the app is only ever a view onto it. */
static pomo_state_t s_pomo_state;
static int      s_pomo_sel = 0;
static int      s_pomo_total_s, s_pomo_left_s;
static int64_t  s_pomo_tick_ms;
static int64_t  s_pomo_done_at;
static uint32_t s_pomo_sessions;

/* Rotation lock: the cube stops selecting a duration, so a session survives
 * being picked up, knocked, or carried to another desk. It is a plain toggle
 * held only in RAM — never in the blob — so a reboot always comes back unlocked
 * and the indicator on the glass is the only thing that has to be believed.
 * Deliberately NOT cleared when a session ends: re-arming it for every session
 * is the whole cost the feature exists to remove. */
static bool     s_pomo_rot_lock;

static bool     s_pomo_flat;
static int64_t  s_pomo_flat_since;
static int      s_pomo_last_rot = -1;
static int64_t  s_pomo_active_ms;      /* last touch or movement */
static bool     s_pomo_dimmed;
static int      s_acc_ref_x, s_acc_ref_y, s_acc_ref_z;

/* widgets */
static lv_obj_t *s_pomo_dial[POMO_SLOTS];
static lv_obj_t *s_pomo_clock, *s_pomo_word, *s_pomo_ring, *s_pomo_arc, *s_pomo_fill;
static lv_obj_t *s_pomo_tick, *s_pomo_hint;   /* the DONE state: green check + tap hint */
static lv_obj_t *s_pomo_padlock;              /* rotation-lock indicator, hidden when off */
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
 * uses, relative to the panel rotation that FOCUS freezes on entry. This makes
 * the current display-up direction slot 0 (60 min), whatever its orientation.
 *
 * There is deliberately no fudge factor here. One was added after misreading two
 * photographs — a photo has no gravity reference, so "the top label looks upright
 * in the picture" says nothing about which way the cube was actually facing. */
static int pomo_top_edge(void) {
    return (rot_from_base(s_base_rot) - s_rot + 4) & 3;
}

/* Which duration a start would use. Every caller that means "the duration the
 * user chose" goes through here rather than through pomo_top_edge(), which
 * means "which way is up" — the two are the same thing only while the cube is
 * free to choose. Under rotation lock the pinned slot wins, so a tap on a
 * sideways cube starts the session that is highlighted rather than the one
 * gravity happens to point at. */
static int pomo_pick_edge(void) {
    return s_pomo_rot_lock ? s_pomo_sel : pomo_top_edge();
}

/* 0.1-degree units, clockwise. Edge i must be pre-rotated by -90*i so that
 * turning the cube by +90*i cancels it out and the label reads upright. */
static int32_t pomo_edge_angle(int i) {
    return (3600 - 900 * i) % 3600;
}

static void pomo_build_dial(lv_obj_t *scr) {
    /* Radius 160, not 176: at 176 a label's outer edge sat 50 px from the
     * glass, inside the ~55 px band the curved cover clips (HARDWARE.md), and
     * the top read as crowded. 160 puts it 66 px clear. */
    static const lv_coord_t dx[POMO_SLOTS] = {   0,  160,    0, -160 };
    static const lv_coord_t dy[POMO_SLOTS] = { -160,   0,  160,    0 };

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

    /* Highlight whichever label is physically at the top — or, under rotation
     * lock, the slot that is actually pinned. The highlight has to track the
     * SELECTION and not the top edge, or a locked cube turned 90 degrees lights
     * a duration it is not going to run. Only `wr` may drive the counter-
     * rotation below: the readout stays fixed to the reader either way. */
    int hl = s_pomo_rot_lock ? s_pomo_sel : wr;
    for (int i = 0; i < POMO_SLOTS; i++) {
        bool active = (i == hl);
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
        /* The digits sit dead centre now, so only the word's offset needs to
         * rotate with the cube. The zero table is kept (rather than dropping the
         * align call) so a future nudge is a table edit, not a re-derivation. */
        static const lv_coord_t cx[4] = {   0,   0,   0,   0 };
        static const lv_coord_t cy[4] = {   0,   0,   0,   0 };
        static const lv_coord_t wx[4] = {   0,  58,   0, -58 };
        static const lv_coord_t wy[4] = {  58,   0, -58,   0 };

        static const lv_coord_t hx[4] = {   0,  86,   0, -86 };
        static const lv_coord_t hy[4] = {  86,   0, -86,   0 };

        /* The padlock rides 95 px world-ABOVE the digits, in the band between
         * the top dial label (its box ends at 146) and the digits (theirs
         * starts at 27). Same sign convention as the word's table, negated:
         * world-up is the direction world-down is not. */
        static const lv_coord_t px[4] = {   0, -95,   0,  95 };
        static const lv_coord_t py[4] = { -95,   0,  95,   0 };

        lv_obj_set_style_transform_rotation(s_pomo_clock, counter, 0);
        lv_obj_set_style_transform_rotation(s_pomo_word, counter, 0);
        lv_obj_set_style_transform_rotation(s_pomo_tick, counter, 0);
        lv_obj_set_style_transform_rotation(s_pomo_hint, counter, 0);
        lv_obj_set_style_transform_rotation(s_pomo_padlock, counter, 0);
        lv_obj_align(s_pomo_clock, LV_ALIGN_CENTER, cx[wr], cy[wr]);
        lv_obj_align(s_pomo_word,  LV_ALIGN_CENTER, wx[wr], wy[wr]);
        lv_obj_align(s_pomo_hint,  LV_ALIGN_CENTER, hx[wr], hy[wr]);
        lv_obj_align(s_pomo_padlock, LV_ALIGN_CENTER, px[wr], py[wr]);

        /* keep the depleting ring starting from world-up, not screen-up */
        lv_arc_set_rotation(s_pomo_arc,  (270 - 90 * wr + 360) % 360);
        lv_arc_set_rotation(s_pomo_fill, (270 - 90 * wr + 360) % 360);
    }

    /* The padlock is the only readout of a mode with no other trace on the
     * glass, so it shows for as long as the mode is on rather than as a toast.
     * Change-gated like every other flag flip in here: this runs at 4 Hz and
     * the setters invalidate whether or not anything moved. */
    if (lv_obj_has_flag(s_pomo_padlock, LV_OBJ_FLAG_HIDDEN) == s_pomo_rot_lock) {
        if (s_pomo_rot_lock) lv_obj_remove_flag(s_pomo_padlock, LV_OBJ_FLAG_HIDDEN);
        else                 lv_obj_add_flag(s_pomo_padlock, LV_OBJ_FLAG_HIDDEN);
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
        /* The check replaces the digits — 00:00 is a number, DONE is a state.
         * The hint waits out the tap grace so it never invites a tap that would
         * be swallowed. Every flag flip is change-gated: this block runs every
         * tick and the setters invalidate whether or not anything moved. */
        if (!lv_obj_has_flag(s_pomo_clock, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(s_pomo_clock, LV_OBJ_FLAG_HIDDEN);
        if (lv_obj_has_flag(s_pomo_tick, LV_OBJ_FLAG_HIDDEN))
            lv_obj_remove_flag(s_pomo_tick, LV_OBJ_FLAG_HIDDEN);
        if (age > POMO_TAP_GRACE_MS && lv_obj_has_flag(s_pomo_hint, LV_OBJ_FLAG_HIDDEN))
            lv_obj_remove_flag(s_pomo_hint, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pomo_fill, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_arc_width(s_pomo_arc, w, LV_PART_MAIN);
        lv_arc_set_bg_angles(s_pomo_arc, 0, 360);
        lv_obj_set_style_arc_color(s_pomo_arc, lv_color_hex(0x35C759), LV_PART_MAIN);
    } else {
        if (lv_obj_has_flag(s_pomo_clock, LV_OBJ_FLAG_HIDDEN))
            lv_obj_remove_flag(s_pomo_clock, LV_OBJ_FLAG_HIDDEN);
        if (!lv_obj_has_flag(s_pomo_tick, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(s_pomo_tick, LV_OBJ_FLAG_HIDDEN);
        if (!lv_obj_has_flag(s_pomo_hint, LV_OBJ_FLAG_HIDDEN))
            lv_obj_add_flag(s_pomo_hint, LV_OBJ_FLAG_HIDDEN);
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

/* Rotation lock: the right key's HOLD verb in FOCUS. On BTN_LONG, so the banner
 * is already up while the finger is still down and lifting is simply what
 * leaves it — a tap can never reach this, by the same construction that keeps
 * the lock screen's two right-key verbs apart.
 *
 * The banner counter-rotates and has its offset rotated by hand, for the reason
 * every label in this app does (see pomo_refresh): FOCUS freezes the panel, so
 * anything aligned in screen space reads sideways the moment the cube is
 * turned — and a cube being turned is exactly when this gets pressed. It is the
 * one caller of toast_show() that needs the object back. */
static void pomo_rot_lock_toggle(void) {
    s_pomo_rot_lock = !s_pomo_rot_lock;
    ESP_LOGI(TAG, "pomodoro: rotation lock %s (pinned %d min)",
             s_pomo_rot_lock ? "ON" : "OFF", s_pomo_min[s_pomo_sel]);
    sfx_play(SFX_TICK);

    if (!ui_lock()) return;
    /* Put the padlock in or out now rather than up to 250 ms later, so the
     * indicator and the banner arrive together instead of a beat apart. */
    if (s_pomo_clock) pomo_refresh();

    static const lv_coord_t bx[4] = {   0,  122,    0, -122 };
    static const lv_coord_t by[4] = { 122,    0, -122,    0 };
    int wr = pomo_top_edge();

    lv_obj_t *toast = toast_show(s_pomo_rot_lock ? "ROTATION LOCK  ON"
                                                 : "ROTATION LOCK  OFF",
                                 s_pomo_rot_lock, bx[wr], by[wr]);
    /* The pivot is in box coordinates and the box is LV_SIZE_CONTENT, so it is
     * not known until layout has run. Measuring beats hardcoding a width the
     * two strings would have to agree on. */
    lv_obj_update_layout(toast);
    lv_obj_set_style_transform_pivot_x(toast, lv_obj_get_width(toast) / 2, 0);
    lv_obj_set_style_transform_pivot_y(toast, lv_obj_get_height(toast) / 2, 0);
    lv_obj_set_style_transform_rotation(toast, (3600 - 900 * wr) % 3600, 0);
    bsp_display_unlock();
}

static void pomo_tap_cb(lv_event_t *e) {
    /* Tap starts an idle timer and toggles a live one. DONE deliberately ignores
     * taps: the finish screen retires itself after POMO_DONE_MS, and a stray tap
     * on it would otherwise start a whole new session by accident. */
    switch (s_pomo_state) {
    case POMO_IDLE:  pomo_begin(pomo_pick_edge(), true); break;
    case POMO_RUN:   pomo_set_running(false, "tap");     break;
    case POMO_PAUSE: pomo_set_running(true,  "tap");     break;
    case POMO_DONE:
        /* The finish screen is persistent now, so a tap is how the next session
         * starts — with whatever duration is at the top, same as idle. The
         * grace period exists because someone tapping to pause right as the
         * timer hits zero would otherwise launch a fresh session unseen. */
        if (now_ms() - s_pomo_done_at > POMO_TAP_GRACE_MS) {
            pomo_begin(pomo_pick_edge(), true);
        }
        break;
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
    /* Height must equal the font's line_height (55 for hud_clock_76): LVGL lays
     * text from the top of the box, so any excess height floats the glyphs above
     * the box centre — and the pivot and align both speak in box coordinates.
     * A 90 px box put the digits 17.5 px high of where "centred" claimed. */
    lv_obj_set_size(s_pomo_clock, 300, 55);
    lv_obj_set_style_text_font(s_pomo_clock, &hud_clock_76, 0);
    lv_obj_set_style_text_align(s_pomo_clock, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_pivot_x(s_pomo_clock, 150, 0);
    lv_obj_set_style_transform_pivot_y(s_pomo_clock, 27, 0);
    lv_label_set_text(s_pomo_clock, "00:00");
    lv_obj_align(s_pomo_clock, LV_ALIGN_CENTER, 0, 0);

    s_pomo_word = lv_label_create(scr);
    lv_obj_set_size(s_pomo_word, 300, 20);           /* = hud_text_18 line box */
    lv_obj_set_style_text_font(s_pomo_word, &hud_text_18, 0);
    lv_obj_set_style_text_align(s_pomo_word, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_transform_pivot_x(s_pomo_word, 150, 0);
    lv_obj_set_style_transform_pivot_y(s_pomo_word, 10, 0);
    lv_label_set_text(s_pomo_word, "");
    lv_obj_align(s_pomo_word, LV_ALIGN_CENTER, 0, 58);

    /* DONE-state widgets. The check sits dead centre where the digits were, so
     * it needs no offset table — rotation spins it about its own middle. It is
     * montserrat_36 because that is the compiled-in font carrying LV_SYMBOL_OK
     * (the hud fonts are ASCII-only), scaled 1.5x to the digits' visual weight.
     * The hint rides 86 px world-below, past the DONE word at 58. */
    s_pomo_tick = lv_label_create(scr);
    lv_obj_set_size(s_pomo_tick, 60, 36);
    lv_obj_set_style_text_font(s_pomo_tick, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_align(s_pomo_tick, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_pomo_tick, lv_color_hex(0x35C759), 0);
    lv_obj_set_style_transform_pivot_x(s_pomo_tick, 30, 0);
    lv_obj_set_style_transform_pivot_y(s_pomo_tick, 18, 0);
    lv_obj_set_style_transform_scale_x(s_pomo_tick, 384, 0);
    lv_obj_set_style_transform_scale_y(s_pomo_tick, 384, 0);
    lv_label_set_text(s_pomo_tick, LV_SYMBOL_OK);
    lv_obj_align(s_pomo_tick, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_pomo_tick, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pomo_tick, LV_OBJ_FLAG_CLICKABLE);

    s_pomo_hint = lv_label_create(scr);
    lv_obj_set_size(s_pomo_hint, 300, 20);
    lv_obj_set_style_text_font(s_pomo_hint, &hud_text_18, 0);
    lv_obj_set_style_text_align(s_pomo_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_pomo_hint, lv_color_hex(0x7A8BA5), 0);
    lv_obj_set_style_transform_pivot_x(s_pomo_hint, 150, 0);
    lv_obj_set_style_transform_pivot_y(s_pomo_hint, 10, 0);
    lv_label_set_text(s_pomo_hint, "TAP TO START ANOTHER");
    lv_obj_align(s_pomo_hint, LV_ALIGN_CENTER, 0, 86);
    lv_obj_add_flag(s_pomo_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_pomo_hint, LV_OBJ_FLAG_CLICKABLE);

    /* The padlock is DRAWN, not set as text: LVGL's symbol set has no padlock
     * glyph and the hud fonts are ASCII-only, so any character that looks like
     * one renders as an empty box (CLAUDE.md). Two children under one container
     * — an arc for the shackle, a rounded slab for the body — and the container
     * carries the transform, which rotates the pair as a unit. It is 16x22, so
     * the transient layer a rotated widget allocates is negligible. Amber
     * regardless of state: the word below owns run/pause/done, this owns the
     * mode, and colouring them alike would merge two independent readouts. */
    s_pomo_padlock = lv_obj_create(scr);
    lv_obj_set_size(s_pomo_padlock, 16, 22);
    lv_obj_set_style_bg_opa(s_pomo_padlock, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_pomo_padlock, 0, 0);
    lv_obj_set_style_pad_all(s_pomo_padlock, 0, 0);
    lv_obj_set_style_transform_pivot_x(s_pomo_padlock, 8, 0);
    lv_obj_set_style_transform_pivot_y(s_pomo_padlock, 11, 0);
    lv_obj_remove_flag(s_pomo_padlock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_pomo_padlock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_pomo_padlock, LV_ALIGN_CENTER, 0, -95);
    lv_obj_add_flag(s_pomo_padlock, LV_OBJ_FLAG_HIDDEN);

    /* Shackle: the top half of a ring. LVGL angle 0 is 3 o'clock going
     * clockwise, so 180->360 sweeps through 12 o'clock. Its lower legs run
     * behind the body, which is what makes the two shapes read as one lock. */
    lv_obj_t *sh = lv_arc_create(s_pomo_padlock);
    lv_obj_set_size(sh, 12, 12);
    lv_obj_align(sh, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_style(sh, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(sh, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(sh, 2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sh, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sh, lv_color_hex(0xFFB454), LV_PART_MAIN);
    lv_arc_set_bg_angles(sh, 180, 360);

    lv_obj_t *body = lv_obj_create(s_pomo_padlock);
    lv_obj_set_size(body, 16, 12);
    lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(body, lv_color_hex(0xFFB454), 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(body, 0, 0);
    lv_obj_set_style_radius(body, 3, 0);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(scr, gesture_home_cb, LV_EVENT_GESTURE, NULL);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, pomo_tap_cb, LV_EVENT_CLICKED, NULL);

    s_pomo_drawn_rot = -1;
    s_pomo_active_ms = now_ms();
    if (s_pomo_state == POMO_IDLE) {
        s_pomo_sel = pomo_pick_edge();
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
    if (wr != s_pomo_last_rot && s_pomo_rot_lock) {
        /* Locked: swallow the turn, but still bank it. Leaving s_pomo_last_rot
         * stale would make unlocking fire the accumulated turn immediately —
         * the release, not the press, would restart the session — which is the
         * one thing this mode exists to prevent. No detent either: the tick
         * would promise a change that is not coming. */
        s_pomo_last_rot = wr;
    } else if (wr != s_pomo_last_rot && !flat) {
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
        /* DONE is persistent, on purpose: the finish screen keeps the panel on —
         * always-on or not — until a tap starts the next session or the user
         * navigates away. It used to retire to the lock screen after 7 s, which
         * meant the one glance that mattered ("did it finish?") usually found a
         * clock. This only pins the screen while FOCUS is the active app:
         * pomo_poll() returns early on every other screen, so a finished session
         * left in the background changes nothing there. The inactivity dim still
         * applies, which is what makes indefinitely-on affordable on an AMOLED. */
        lv_display_trigger_activity(NULL);
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

/* ---------------- DAYS: one beautiful answer to "how long?" ---------------- */

static lv_obj_t *s_days_today, *s_days_time, *s_days_num, *s_days_unit;
static lv_obj_t *s_days_bar, *s_days_start, *s_days_target;
static lv_obj_t *s_days_card, *s_days_text, *s_days_sync;
static lv_obj_t *s_days_qr_panel, *s_days_qr, *s_days_qr_note;
static char s_days_link_drawn[DAYS_LINK_URL_MAX];
static uint32_t s_days_link_seen_ver = UINT32_MAX;
static uint32_t s_days_painted_ver = UINT32_MAX;
static int s_days_painted_day = INT32_MIN;
static int s_days_painted_minute = -1;
static int s_days_painted_progress = -1;
static uint32_t s_days_painted_accent;

static bool days_parse_ymd(const char *s, int *year, int *month, int *day) {
    int y, m, d, n = 0;
    if (!s || strlen(s) != 10 ||
        sscanf(s, "%4d-%2d-%2d%n", &y, &m, &d, &n) != 3 || n != 10) return false;
    static const uint8_t mdays[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    if (y < 1970 || y > 9999 || m < 1 || m > 12 || d < 1) return false;
    int max = mdays[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) max = 29;
    if (d > max) return false;
    if (year) *year = y;
    if (month) *month = m;
    if (day) *day = d;
    return true;
}

/* Gregorian civil date -> monotonically increasing day number. Unlike mktime,
 * this does not turn a DST transition into a 23/25-hour "day". */
static int64_t days_civil_index(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned shifted_month = (unsigned)((int)m + (m > 2 ? -3 : 9));
    unsigned doy = (153 * shifted_month + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + doe;
}

static void days_date_short(const char *date, char *out, size_t cap, bool year) {
    static const char *mon[] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    int y, m, d;
    if (!days_parse_ymd(date, &y, &m, &d)) {
        snprintf(out, cap, "--");
    } else if (year) {
        snprintf(out, cap, "%s %d %d", mon[m - 1], d, y);
    } else {
        snprintf(out, cap, "%s %d", mon[m - 1], d);
    }
}

static uint32_t color_mix(uint32_t a, uint32_t b, int p) {
    p = clampi(p, 0, 100);
    int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    return (uint32_t)(ar + (br - ar) * p / 100) << 16 |
           (uint32_t)(ag + (bg - ag) * p / 100) << 8 |
           (uint32_t)(ab + (bb - ab) * p / 100);
}

/* Cool cyan at the beginning, violet through the middle, then amber/coral as
 * the date gets close. The number and bar always share this exact colour. */
static uint32_t days_accent(int progress) {
    if (progress < 55) return color_mix(0x22D3EE, 0x8B7CF6, progress * 100 / 55);
    if (progress < 85) return color_mix(0x8B7CF6, 0xF59E0B,
                                        (progress - 55) * 100 / 30);
    return color_mix(0xF59E0B, 0xFF453A, (progress - 85) * 100 / 15);
}

static void days_label_text(lv_obj_t *label, const char *text) {
    if (label && strcmp(lv_label_get_text(label), text) != 0) {
        lv_label_set_text(label, text);
    }
}

static void days_refresh(void) {
    if (!s_days_num) return;

    time_t now;
    struct tm ti = {0};
    time(&now);
    localtime_r(&now, &ti);
    bool clock_ok = ti.tm_year >= (2024 - 1900);
    int today = clock_ok ? (int)days_civil_index(ti.tm_year + 1900,
                                                 (unsigned)ti.tm_mon + 1,
                                                 (unsigned)ti.tm_mday) : INT32_MIN;

    days_blob_t data;
    uint32_t version;
    days_snapshot(&data, &version);

    int ty, tm, td, sy, sm, sd;
    bool have_target = days_parse_ymd(data.target, &ty, &tm, &td);
    bool have_set = days_parse_ymd(data.set_on, &sy, &sm, &sd);
    int target = have_target ? (int)days_civil_index(ty, (unsigned)tm, (unsigned)td) : 0;
    int set_on = have_set ? (int)days_civil_index(sy, (unsigned)sm, (unsigned)sd) : today;

    int remaining = (clock_ok && have_target) ? target - today : 0;
    int total = (have_set && have_target) ? target - set_on : 0;
    int progress = 0;
    if (clock_ok && have_target) {
        if (remaining <= 0) progress = 100;
        else if (total > 0) progress = clampi((today - set_on) * 100 / total, 0, 100);
    }
    uint32_t accent = days_accent(progress);

    /* The data/date branch repaints only when either actually changed. Time is
     * minute-granular, so this screen is truly static between minute ticks. */
    if (version != s_days_painted_ver || today != s_days_painted_day) {
        char number[16], unit[24], start[28], target_text[32];
        snprintf(number, sizeof(number), "%d", abs(remaining));
        if (!clock_ok) snprintf(unit, sizeof(unit), "SETTING CLOCK");
        else if (!have_target) snprintf(unit, sizeof(unit), "NO DATE YET");
        else if (remaining == 0) snprintf(unit, sizeof(unit), "TODAY");
        else if (remaining == 1) snprintf(unit, sizeof(unit), "DAY TO GO");
        else if (remaining > 1) snprintf(unit, sizeof(unit), "DAYS TO GO");
        else if (remaining == -1) snprintf(unit, sizeof(unit), "DAY AGO");
        else snprintf(unit, sizeof(unit), "DAYS AGO");

        char short_date[20];
        days_date_short(data.set_on, short_date, sizeof(short_date), false);
        snprintf(start, sizeof(start), "START %s", short_date);
        days_date_short(data.target, short_date, sizeof(short_date), true);
        snprintf(target_text, sizeof(target_text), "TARGET %s", short_date);

        days_label_text(s_days_num, number);
        days_label_text(s_days_unit, unit);
        days_label_text(s_days_start, have_set ? start : "");
        days_label_text(s_days_target, have_target ? target_text : "");
#if CFG_PERF_SCROLL_SELFTEST
        /* Screenshot builds only: the walk dumps this screen into the README,
         * and the real countdown message is personal. Gated on the harness
         * define, so production builds render the stored text untouched. */
        days_label_text(s_days_text, "IMPORTANT TASK");
#else
        days_label_text(s_days_text, data.text[0] ? data.text
                         : "SET A DATE AND A MESSAGE FROM THE DAYS WEB PAGE");
#endif
        s_days_painted_ver = version;
        s_days_painted_day = today;
    }

    if (clock_ok && ti.tm_hour * 60 + ti.tm_min != s_days_painted_minute) {
        static const char *wday[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
        static const char *mon[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
                                     "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
        char date[28], clock[8];
        snprintf(date, sizeof(date), "%s  %s %d", wday[ti.tm_wday], mon[ti.tm_mon], ti.tm_mday);
        snprintf(clock, sizeof(clock), "%02d:%02d", ti.tm_hour, ti.tm_min);
        days_label_text(s_days_today, date);
        days_label_text(s_days_time, clock);
        s_days_painted_minute = ti.tm_hour * 60 + ti.tm_min;
    }

    if (progress != s_days_painted_progress) {
        lv_bar_set_value(s_days_bar, progress, LV_ANIM_ON);
        s_days_painted_progress = progress;
    }
    if (accent != s_days_painted_accent) {
        lv_color_t c = lv_color_hex(accent);
        lv_obj_set_style_text_color(s_days_num, c, 0);
        lv_obj_set_style_text_color(s_days_unit, c, 0);
        lv_obj_set_style_bg_color(s_days_bar, c, LV_PART_INDICATOR);
        lv_obj_set_style_border_color(s_days_card, c, 0);
        s_days_painted_accent = accent;
    }
    days_label_text(s_days_sync,
                    __atomic_load_n(&s_days_fetching, __ATOMIC_ACQUIRE) ? "SYNCING..." :
                    (have_target ? "TAP TO EDIT" : "TAP TO SET A DATE"));
}

static void days_timer_cb(lv_timer_t *timer) {
    (void)timer;
    if (s_days_qr_panel && !lv_obj_has_flag(s_days_qr_panel, LV_OBJ_FLAG_HIDDEN)) {
        char url[DAYS_LINK_URL_MAX];
        uint32_t version;
        days_link_snapshot(url, sizeof(url), &version);
        bool fetching = __atomic_load_n(&s_days_link_fetching, __ATOMIC_ACQUIRE) ||
                        __atomic_load_n(&s_req_days_link, __ATOMIC_ACQUIRE);
        if (url[0] && s_days_qr &&
            (version != s_days_link_seen_ver || strcmp(url, s_days_link_drawn) != 0)) {
            size_t n = strlen(url);
            if (lv_qrcode_update(s_days_qr, url, (uint32_t)n) == LV_RESULT_OK) {
                snprintf(s_days_link_drawn, sizeof(s_days_link_drawn), "%s", url);
                lv_obj_remove_flag(s_days_qr, LV_OBJ_FLAG_HIDDEN);
                days_label_text(s_days_qr_note,
                                "SCAN WITH YOUR PHONE\nLink expires in 5 minutes");
            } else {
                lv_obj_add_flag(s_days_qr, LV_OBJ_FLAG_HIDDEN);
                days_label_text(s_days_qr_note, "QR ENCODE FAILED\nTap to close and retry");
                ESP_LOGW(TAG, "days: edit QR encode failed (%u bytes)", (unsigned)n);
            }
            s_days_link_seen_ver = version;
        } else if (!url[0]) {
            lv_obj_add_flag(s_days_qr, LV_OBJ_FLAG_HIDDEN);
            days_label_text(s_days_qr_note, fetching ? "CREATING SECURE LINK..." :
                            "BROKER UNAVAILABLE\nTap to close and retry");
            s_days_link_seen_ver = version;
        }
    }
    days_refresh();
}

static void days_tap_cb(lv_event_t *e) {
    (void)e;
    if (!s_days_qr_panel) return;
    days_link_publish("");
    s_days_link_drawn[0] = '\0';
    s_days_link_seen_ver = UINT32_MAX;
    lv_obj_add_flag(s_days_qr, LV_OBJ_FLAG_HIDDEN);
    days_label_text(s_days_qr_note, "CREATING SECURE LINK...");
    lv_obj_remove_flag(s_days_qr_panel, LV_OBJ_FLAG_HIDDEN);
    __atomic_store_n(&s_req_days_link, true, __ATOMIC_RELEASE);
}

static void days_qr_close_cb(lv_event_t *e) {
    lv_event_stop_bubbling(e);
    if (s_days_qr_panel) lv_obj_add_flag(s_days_qr_panel, LV_OBJ_FLAG_HIDDEN);
    /* Saving happens on the phone. Closing the handoff is the user's explicit
     * signal to fetch now, while the normal daily refresh remains unchanged. */
    __atomic_store_n(&s_req_days_fetch, true, __ATOMIC_RELEASE);
    days_label_text(s_days_sync, "SYNCING...");
}

static void build_days_app(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x03060A), 0);
    lv_obj_set_style_pad_all(scr, 0, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, days_tap_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(scr, gesture_home_cb, LV_EVENT_GESTURE, NULL);

    /* The header is deliberately asymmetric: today's date gets visual weight,
     * while the clock is smaller and tucked into the opposite corner. Both stay
     * inside the 58..422 px safe column. */
    s_days_today = lv_label_create(scr);
    lv_obj_set_width(s_days_today, 245);
    lv_obj_set_style_text_font(s_days_today, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_days_today, lv_color_hex(0xE8EDF5), 0);
    lv_label_set_text(s_days_today, "TODAY");
    lv_obj_set_pos(s_days_today, 64, 46);

    s_days_time = lv_label_create(scr);
    lv_obj_set_width(s_days_time, 100);
    lv_obj_set_style_text_font(s_days_time, &hud_text_18, 0);
    lv_obj_set_style_text_align(s_days_time, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_days_time, lv_color_hex(0x718096), 0);
    lv_label_set_text(s_days_time, "--:--");
    lv_obj_set_pos(s_days_time, 316, 48);

    lv_obj_t *eyebrow = lv_label_create(scr);
    lv_obj_set_width(eyebrow, CONTENT_W);
    lv_obj_set_style_text_font(eyebrow, &hud_text_18, 0);
    lv_obj_set_style_text_align(eyebrow, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(eyebrow, 4, 0);
    lv_obj_set_style_text_color(eyebrow, lv_color_hex(0x536176), 0);
    lv_label_set_text(eyebrow, "DAYS UNTIL");
    lv_obj_align(eyebrow, LV_ALIGN_TOP_MID, 0, 92);

    s_days_num = lv_label_create(scr);
    lv_obj_set_size(s_days_num, CONTENT_W, 84);
    lv_obj_set_style_text_font(s_days_num, &hud_clock_76, 0);
    lv_obj_set_style_text_align(s_days_num, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_days_num, "0");
    lv_obj_align(s_days_num, LV_ALIGN_TOP_MID, 0, 121);

    s_days_unit = lv_label_create(scr);
    lv_obj_set_width(s_days_unit, CONTENT_W);
    lv_obj_set_style_text_font(s_days_unit, &hud_text_18, 0);
    lv_obj_set_style_text_align(s_days_unit, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_days_unit, 3, 0);
    lv_label_set_text(s_days_unit, "NO DATE YET");
    lv_obj_align(s_days_unit, LV_ALIGN_TOP_MID, 0, 207);

    s_days_bar = lv_bar_create(scr);
    lv_obj_set_size(s_days_bar, 330, 14);
    lv_bar_set_range(s_days_bar, 0, 100);
    lv_bar_set_value(s_days_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_radius(s_days_bar, 7, 0);
    lv_obj_set_style_radius(s_days_bar, 7, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_days_bar, lv_color_hex(0x121A25), 0);
    lv_obj_set_style_bg_opa(s_days_bar, LV_OPA_COVER, 0);
    lv_obj_align(s_days_bar, LV_ALIGN_TOP_MID, 0, 254);

    s_days_start = lv_label_create(scr);
    lv_obj_set_width(s_days_start, 150);
    lv_obj_set_style_text_color(s_days_start, lv_color_hex(0x5D6B80), 0);
    lv_obj_set_style_text_font(s_days_start, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_days_start, "");
    lv_obj_set_pos(s_days_start, 75, 280);

    s_days_target = lv_label_create(scr);
    lv_obj_set_width(s_days_target, 210);
    lv_obj_set_style_text_align(s_days_target, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(s_days_target, lv_color_hex(0x8492A6), 0);
    lv_obj_set_style_text_font(s_days_target, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_days_target, "");
    lv_obj_set_pos(s_days_target, 195, 280);

    s_days_card = lv_obj_create(scr);
    lv_obj_remove_style_all(s_days_card);
    lv_obj_set_size(s_days_card, CONTENT_W, 98);
    lv_obj_set_style_radius(s_days_card, 24, 0);
    lv_obj_set_style_bg_color(s_days_card, lv_color_hex(0x0B1018), 0);
    lv_obj_set_style_bg_opa(s_days_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_days_card, 1, 0);
    lv_obj_set_style_border_opa(s_days_card, 105, 0);
    lv_obj_remove_flag(s_days_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_days_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(s_days_card, LV_ALIGN_TOP_MID, 0, 316);

    s_days_text = lv_label_create(s_days_card);
    lv_obj_set_width(s_days_text, 316);
    lv_obj_set_style_text_font(s_days_text, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_align(s_days_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_days_text, lv_color_hex(0xE8EDF5), 0);
    lv_obj_set_style_text_line_space(s_days_text, 7, 0);
    lv_label_set_long_mode(s_days_text, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_days_text, "SET A DATE AND A MESSAGE FROM THE DAYS WEB PAGE");
    lv_obj_center(s_days_text);

    s_days_sync = lv_label_create(scr);
    lv_obj_set_width(s_days_sync, CONTENT_W);
    lv_obj_set_style_text_font(s_days_sync, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_align(s_days_sync, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_letter_space(s_days_sync, 2, 0);
    lv_obj_set_style_text_color(s_days_sync, lv_color_hex(0x536176), 0);
    lv_label_set_text(s_days_sync, "SYNCING...");
    lv_obj_align(s_days_sync, LV_ALIGN_BOTTOM_MID, 0, -44);

    /* Editing is a full-screen, user-bound handoff. The QR contains a five-
     * minute one-time code, never BROKER_TOKEN; the phone exchanges it for a
     * 30-minute bearer that the broker accepts only on this user's countdown. */
    s_days_qr_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_days_qr_panel);
    lv_obj_set_size(s_days_qr_panel, 480, 480);
    lv_obj_center(s_days_qr_panel);
    lv_obj_set_style_bg_color(s_days_qr_panel, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_days_qr_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_days_qr_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_days_qr_panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_days_qr_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_days_qr_panel, days_qr_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(s_days_qr_panel);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8B7CF6), 0);
    lv_obj_set_style_text_letter_space(title, 2, 0);
    lv_label_set_text(title, "EDIT DAYS");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);

    s_days_qr = lv_qrcode_create(s_days_qr_panel);
    lv_qrcode_set_size(s_days_qr, 228);
    lv_qrcode_set_dark_color(s_days_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_days_qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(s_days_qr, true);
    lv_obj_align(s_days_qr, LV_ALIGN_TOP_MID, 0, 92);
    lv_obj_add_flag(s_days_qr, LV_OBJ_FLAG_HIDDEN);

    s_days_qr_note = lv_label_create(s_days_qr_panel);
    lv_obj_set_width(s_days_qr_note, 370);
    lv_obj_set_style_text_font(s_days_qr_note, &hud_text_18, 0);
    lv_obj_set_style_text_color(s_days_qr_note, lv_color_hex(0xC7D2E0), 0);
    lv_obj_set_style_text_align(s_days_qr_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_days_qr_note, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_days_qr_note, "CREATING SECURE LINK...");
    lv_obj_align(s_days_qr_note, LV_ALIGN_TOP_MID, 0, 348);

    lv_obj_t *close = lv_label_create(s_days_qr_panel);
    lv_obj_set_style_text_font(close, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(close, lv_color_hex(0x536176), 0);
    lv_obj_set_style_text_letter_space(close, 2, 0);
    lv_label_set_text(close, "TAP TO RETURN AND SYNC");
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -38);

    s_days_painted_ver = UINT32_MAX;
    s_days_painted_day = INT32_MIN;
    s_days_painted_minute = -1;
    s_days_painted_progress = -1;
    s_days_painted_accent = 0;
    s_days_link_drawn[0] = '\0';
    s_days_link_seen_ver = UINT32_MAX;
    __atomic_store_n(&s_req_days_fetch, true, __ATOMIC_RELEASE);
    days_refresh();                         /* cached value, before the GET */
    s_app_timer = lv_timer_create(days_timer_cb, 1000, NULL);
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
#define SP_ART_PX     296
/* Immersive by default: the secondary controls slide off and the cover fills the
 * space. One animation drives every property from a single 0..256 progress value,
 * because the cover, the label chip, both labels, the left column and the slider
 * all have to move together — six widgets animated independently is six chances
 * for them to disagree mid-transition.
 *
 *   p = 0    immersive  cover 296 @ centre 240, y30   chrome off-screen
 *   p = 256  chrome     cover 264 @ centre 256, y40   chrome at rest
 *
 * The chrome centre is 256, not 240: the left column ends at x102 and the slider
 * starts at x410, so the midpoint of the space the cover actually occupies is 256.
 * Screen-centring left a 6 px gutter one side and 38 the other. */
#define SP_CHROME_FULL   256
#define SP_CHROME_HIDE_MS 4000    /* auto-hide after this much quiet */
/* The cover lives in PSRAM, not on the card. LVGL's bin decoder can stream rows
 * off FATFS per draw chunk, which is what made a file cheap — but that is ~15 card
 * reads per frame, and it collapses the moment the image has to be scaled (the
 * lock screen draws this 148 px cover into a 100 px slot). 43,824 bytes against
 * 8 MB of free PSRAM removes the card from both the write and the render path, and
 * an in-memory descriptor needs no decoder at all: LVGL blits RGB565 straight out
 * of the buffer. */
/* Baseline JPEG on the wire, not raw RGB565. A 296 px cover measured **5,946
 * bytes against 175,244** — 29x less to move, which turns a ~1 s download into
 * ~0.05 s and lets the lookahead actually stay ahead of a fast skipper.
 *
 * Raw pixels were the right call when the only decoder was TJpgD, which never
 * populates LVGL's image cache and so re-decodes on every draw chunk forever.
 * That stopped being true twice over: esp_lv_decoder is vendored, DOES register
 * with the cache, and freeing 104 KB of internal SRAM made its scratch affordable.
 *
 * A truncated transfer is now self-detecting: JPEG ends in FFD9, so the integrity
 * check proves the body arrived rather than merely counting bytes. */
#define SP_ART_MAX    (64 * 1024)

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
/* Set whenever what-comes-next may have changed: a new track, or shuffle being
 * toggled (which reorders the queue without changing the current track). The
 * queue is re-asked once per such event rather than on every 3 s poll — asking
 * every poll would be a broker round trip per poll for an answer that almost
 * never moves. */
static volatile bool s_sp_queue_dirty = true;

/* Raised by anything the user did — play, skip, volume, shuffle, transfer, like —
 * and lowered once the worker picks that command up. The prefetcher yields to
 * this rather than to "is the queue non-empty", which was the previous test and
 * was wrong: a POLL is enqueued every 3 s and the work takes longer than that, so
 * the queue is essentially never empty and the lookahead never ran at all. */
static int64_t s_sp_swipe_at;     /* last accepted swipe; sp_tap_cb ignores its shadow */
static volatile bool s_sp_urgent;


/* Distinct from s_sp_queue_dirty on purpose. dirty means "what comes next may
 * have changed, re-ask the broker"; pending means "the list is current, some
 * slots still have no cover". Conflating them made every single-cover pass also
 * re-fetch the list: four broker round trips to fill three slots. */
static volatile bool s_sp_pf_pending;

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
/* The broker owns refresh tokens now. When it has none (fresh install, erased
 * volume, or Spotify revoked the grant), /spotify/token returns the one-time
 * URL the MUSIC screen encodes. Fixed buffers follow the same task/UI contract
 * as track and artist above: a torn read can repaint stale text, never a pointer. */
static char s_sp_pair_url[256];
static int64_t s_sp_token_retry_ms;

/* Spotify answers 403 on every endpoint while this account is missing from the
 * app's Development-mode allowlist, and keeps doing so until someone edits the
 * dashboard. Both live on the spotify task, so neither needs an atomic. */
static int64_t s_sp_poll_hold_ms;
static int     s_sp_poll_denies;

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

/* Legacy single-user token refresh. Kept only for a brokerless deployment; when
 * a broker is configured the firmware never reads the compiled refresh token. */
static bool sp_refresh_legacy(void) {
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
    return ok;
}

/* Ask the broker for this cube's short-lived access token. Its bearer is also
 * the user identity: the broker maps unique bearers to isolated refresh-token
 * records, so neither a username nor a Spotify credential travels in the URL. */
static bool sp_refresh_broker(bool force) {
    char path[40];
    snprintf(path, sizeof(path), "/spotify/token%s", force ? "?force=1" : "");
    size_t got = 0;
    bool fetched = broker_fetch(path, NULL, NULL, (uint8_t *)s_sp_body,
                                SP_BODY_MAX, &got);
    s_sp_len = (int)got;
    if (s_sp_body) s_sp_body[got < SP_BODY_MAX ? got : SP_BODY_MAX - 1] = '\0';

    if (fetched && s_brk_status == 200 && got > 0) {
        cJSON *j = cJSON_Parse(s_sp_body);
        if (j) {
            cJSON *at = cJSON_GetObjectItem(j, "access_token");
            cJSON *ex = cJSON_GetObjectItem(j, "expires_in");
            if (cJSON_IsString(at)) {
                snprintf(s_sp_access, sizeof(s_sp_access), "%s", at->valuestring);
                int secs = cJSON_IsNumber(ex) ? ex->valueint : 3600;
                s_sp_expires_ms = now_ms() + (int64_t)secs * 1000;
                s_sp_pair_url[0] = '\0';
                cJSON_Delete(j);
                ESP_LOGI(TAG, "spotify: broker token valid %d s", secs);
                return true;
            }
            cJSON_Delete(j);
        }
        ESP_LOGW(TAG, "spotify: broker token response malformed");
        return false;
    }

    if (s_brk_status == 428 && got > 0) {
        cJSON *j = cJSON_Parse(s_sp_body);
        if (j) {
            cJSON *u = cJSON_GetObjectItem(j, "authorization_url");
            if (cJSON_IsString(u)) {
                snprintf(s_sp_pair_url, sizeof(s_sp_pair_url), "%s", u->valuestring);
                ESP_LOGI(TAG, "spotify: authorisation required");
            }
            cJSON_Delete(j);
        }
    } else if (s_brk_status == 401) {
        /* A QR cannot fix a broker bearer mismatch. Do not leave an older,
         * expired pairing URL on-screen and imply that it can. */
        s_sp_pair_url[0] = '\0';
        ESP_LOGE(TAG, "spotify: broker rejected BROKER_TOKEN");
    } else {
        ESP_LOGW(TAG, "spotify: broker token failed, HTTP %d", s_brk_status);
    }
    return false;
}

static bool sp_refresh_token_force(bool force) {
    bool ok;
    if (BROKER_URL[0] && BROKER_TOKEN[0]) ok = sp_refresh_broker(force);
    else                                 ok = sp_refresh_legacy();
    s_sp_authfail = !ok;
    s_sp_token_retry_ms = ok ? 0 : now_ms() + 5000;
    return ok;
}

static bool sp_refresh_token(void) { return sp_refresh_token_force(false); }

static bool sp_token_ok(void) {
    /* Refresh early rather than discovering expiry via a 401: that path leaves
     * the socket undrained (HARDWARE.md 7f) and costs a reconnect. */
    if (s_sp_access[0] && now_ms() < s_sp_expires_ms - 120000) return true;
    if (s_sp_token_retry_ms && now_ms() < s_sp_token_retry_ms) return false;
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
        if (sp_refresh_token_force(true)) {
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
    if (s_sp_poll_hold_ms && now_ms() < s_sp_poll_hold_ms) return;

    int code = sp_call(HTTP_METHOD_GET, "/me/player", NULL);

    if (code == 204 || (code == 200 && s_sp_len < 4)) {
        s_sp_poll_hold_ms = 0;
        s_sp_poll_denies  = 0;
        s_sp_have_state = false;        /* nothing playing anywhere */
        return;
    }
    if (code != 200 || s_sp_len == 0) {
        /* A refused account is not a transient error, and a 3 s poll against it
         * is the load generator ARCHITECTURE.md warns about. Back off to a
         * minute; any answer that is not a refusal clears it on the next pass. */
        if (code == 403) {
            if (s_sp_poll_denies < 4) s_sp_poll_denies++;
            s_sp_poll_hold_ms = now_ms() + 15000LL * s_sp_poll_denies;
            ESP_LOGW(TAG, "spotify: poll HTTP 403 - account not registered for "
                          "this app? holding %llds",
                     (long long)(15 * s_sp_poll_denies));
            return;
        }
        ESP_LOGW(TAG, "spotify: poll HTTP %d, %d bytes", code, s_sp_len);
        return;
    }

    s_sp_poll_hold_ms = 0;
    s_sp_poll_denies  = 0;

    cJSON *j = cJSON_Parse(s_sp_body);
    if (!j) {
        /* A body larger than SP_BODY_MAX truncates and lands here, silently. */
        ESP_LOGW(TAG, "spotify: poll body unparseable (%d bytes)", s_sp_len);
        return;
    }

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

        char prev_id[26];
        snprintf(prev_id, sizeof(prev_id), "%s", s_sp_track_id);

        cJSON *tid = cJSON_GetObjectItem(item, "id");

        if (cJSON_IsString(tid)) {
            snprintf(s_sp_track_id, sizeof(s_sp_track_id), "%s", tid->valuestring);
            /* A stale heart is worse than no heart: it invites a tap that
             * un-likes something. Drop it until this track is checked. */
            if (strcmp(s_sp_track_id, s_sp_liked_id) != 0) s_sp_liked_known = false;
            /* Any change of track invalidates the lookahead — including the user
             * starting a different playlist from their phone, which arrives here
             * as nothing more than a new id. */
            if (strcmp(s_sp_track_id, prev_id) != 0) s_sp_queue_dirty = true;
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

/* ---- lookahead ----
 *
 * The next few tracks' covers, names and accents, fetched before they are asked
 * for, so a swipe draws instantly instead of nudging and waiting ~1 s for a poll
 * plus a 139 KB download.
 *
 * The queue itself comes from the broker rather than Spotify: /me/player/queue
 * answers with the next twenty tracks in full, 55,569 bytes on a real account,
 * and cJSON would put ~800 small nodes of that in INTERNAL SRAM (allocations under
 * SPIRAM_MALLOC_ALWAYSINTERNAL=128 do not go to PSRAM). The broker returns 408
 * bytes of exactly what gets drawn — see broker/queue.go.
 *
 * Buffers live in PSRAM, not on the card. The card would be free to hold them,
 * but reading 139 KB back off FATFS during the swipe puts the latency straight
 * back into the moment this exists to make instant. */
/* Six tracks of lookahead. Each slot costs 336 bytes of .bss plus one PSRAM
 * cover buffer allocated on first use, so the depth is bounded by patience
 * rather than memory — steady state is still one cover per track change, and
 * only the initial fill gets longer. The broker caps the queue request too;
 * raising this past queueMaxItems in broker/queue.go silently gets you fewer. */
#define SP_PF_N   6
#define SP_QUEUE_MAX 1024

typedef struct {
    char id[26];
    char name[64];
    char artist[64];
    char url[160];
    uint32_t accent;
    uint8_t *buf;          /* PSRAM, SP_ART_MAX; allocated on first use */
    size_t   len;          /* JPEG is variable length, unlike the raw format */
    bool ready;            /* buf holds this entry's cover */
} sp_pf_t;

static sp_pf_t s_sp_pf[SP_PF_N];


static void sp_art_show(void);   /* defined below; the promoter needs it */

/* Hand a prefetched cover to the display by swapping buffer pointers — O(1),
 * against a 139 KB copy. The slot keeps the old buffer to refill later. */
static bool sp_pf_promote(const char *art_url) {
    for (int i = 0; i < SP_PF_N; i++) {
        sp_pf_t *e = &s_sp_pf[i];
        if (!e->ready || e->len == 0 || strcmp(e->url, art_url) != 0) continue;

        uint8_t *tmp = s_sp_art_buf;
        size_t   tl  = s_sp_art_len;
        s_sp_art_buf = e->buf;  s_sp_art_len = e->len;
        e->buf = tmp;           e->len = tl;
        e->ready = false;                 /* its bytes are on screen now */

        snprintf(s_sp_art_have, sizeof(s_sp_art_have), "%s", art_url);
        /* Stamp the track that is PLAYING, not the queue entry we matched.
         *
         * Promotion matches on art URL on purpose, so two tracks off one album
         * share a single fetch — which means the entry's own id routinely is not
         * the current track's. The UI tick hides the cover whenever
         * s_sp_art_id != s_sp_track_id, so stamping e->id here handed it bytes it
         * then immediately rejected: the cover appeared and vanished within a
         * frame, leaving the placeholder over an accent-tinted screen.
         *
         * The failure rate scaled with the *hit* rate, so every improvement to
         * the prefetcher made the artwork disappear more often — which reads as
         * the prefetch being broken, and is the opposite of true. */
        snprintf(s_sp_art_id, sizeof(s_sp_art_id), "%s", s_sp_track_id);
        s_sp_accent = e->accent;
        s_sp_art_failed[0] = '\0';
        sp_art_show();
        ESP_LOGI(TAG, "spotify: art from lookahead (slot %d)", i);
        return true;
    }
    return false;
}

/* Download one cover into a caller-supplied PSRAM buffer and report the accent
 * the broker derived from it. Split out of sp_fetch_art() so the prefetcher can
 * use the identical path — same URL encoding, same header validation, same
 * failure handling — rather than a parallel copy that drifts. */
static bool sp_art_get(const char *art_url, uint8_t *buf, size_t *len_out,
                       uint32_t *accent_out) {
    char url[512];
    int n = snprintf(url, sizeof(url), "/art?s=%d&u=", SP_ART_PX);
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)art_url;
         *p && n < (int)sizeof(url) - 4; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            url[n++] = (char)*p;
        } else {
            url[n++] = '%'; url[n++] = hex[*p >> 4]; url[n++] = hex[*p & 0xF];
        }
    }
    url[n] = '\0';

    size_t got = 0;
    bool ok = broker_fetch(url, NULL, NULL, buf, SP_ART_MAX, &got);

    /* A body that started and stopped is a stalled link, not a refusal. The server
     * side is ruled out — it serves this in 4-157 ms and tolerates a reader
     * trickling 1 KB every 3 s for 75 s — so the far end did not give up on us.
     * Retrying immediately recovers what a 30 s backoff would have made a visible
     * gap. Only once, and only for a partial: a genuine 4xx retried in a loop is
     * how you turn one bad response into a hammering. */
    /* A body that started and stopped is a stalled link, not a refusal — the far
     * end serves this in milliseconds and tolerates very slow readers. Retry once,
     * immediately, rather than blacking the cover out for 30 s. */
    if (!ok && got > 0) {
        ESP_LOGW(TAG, "spotify: art stalled at %u B (rssi %d) — retrying once",
                 (unsigned)got, wifi_rssi());
        got = 0;
        ok = broker_fetch(url, NULL, NULL, buf, SP_ART_MAX, &got);
    }
    if (!ok) return false;

    /* Nothing decodes these bytes on the way through, so a short body or a broker
     * answering with something else would be blitted to the panel as garbage.
     * There is no decoder in this path to reject it — the check has to be here. */
    /* Start of Image and End of Image. Unlike a byte count this proves the body
     * actually finished — a stalled transfer has no FFD9 — and nothing downstream
     * would reject a half JPEG on our behalf. */
    if (got < 8 || buf[0] != 0xFF || buf[1] != 0xD8 ||
        buf[got - 2] != 0xFF || buf[got - 1] != 0xD9) {
        ESP_LOGW(TAG, "spotify: art not a complete JPEG (%u B)", (unsigned)got);
        return false;
    }
    if (len_out) *len_out = got;
    if (accent_out) *accent_out = s_dl_accent;   /* 0 when the cover has no usable hue */
    return true;
}

/* Point the on-screen image at whatever s_sp_art_buf currently holds. */
static void sp_art_show(void) {
    /* RAW: the bytes are encoded, so LVGL hands them to a registered decoder
     * rather than blitting them. esp_lv_decoder recognises the JPEG magic and,
     * crucially, puts the decoded bitmap in the image cache — so it decodes once
     * per cover, not once per draw chunk. */
    s_sp_art_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
    s_sp_art_dsc.header.cf     = LV_COLOR_FORMAT_RAW;
    s_sp_art_dsc.header.w      = SP_ART_PX;
    s_sp_art_dsc.header.h      = SP_ART_PX;
    s_sp_art_dsc.header.stride = 0;
    s_sp_art_dsc.data          = s_sp_art_buf;
    s_sp_art_dsc.data_size     = s_sp_art_len;

    if (ui_lock()) {
        lv_image_cache_drop(&s_sp_art_dsc);      /* same pointer, new bytes */
        if (s_sp_art) {
            lv_image_set_src(s_sp_art, &s_sp_art_dsc);
            lv_obj_remove_flag(s_sp_art, LV_OBJ_FLAG_HIDDEN);
            if (s_sp_art_ph) lv_obj_add_flag(s_sp_art_ph, LV_OBJ_FLAG_HIDDEN);
        }
        if (s_lock_np_art) {
            lv_image_set_src(s_lock_np_art, &s_sp_art_dsc);
            lv_obj_remove_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN);
        }
        /* Success is only claimable with the swap actually done. Stamping these
         * on the lock-timeout path told the UI tick the cover was up while the
         * object was still hidden — nothing would ever retry, and the ring spun
         * forever. Left unstamped, the next poll finds url==have with ready still
         * false and calls back in here: the failure heals in one cycle. */
        snprintf(s_sp_art_shown, sizeof(s_sp_art_shown), "%s", s_sp_art_have);
        s_sp_art_ready = true;
        bsp_display_unlock();
    }
}

/* Fill at most one slot, then hand the worker back.
 *
 * The first version fetched all three synchronously on the task that also serves
 * play/next/volume — each a fresh TLS handshake plus 139 KB — so a swipe queued
 * behind three to five seconds of background work and the app felt broken.
 * Background work does not get to hold the worker interactive commands run on. */
/* Returns true only if this pass actually filled a slot.
 *
 * The return value is load-bearing, not informational: the caller bursts while
 * slots remain pending, and a slot whose download FAILS stays pending. Retrying
 * it inside the same burst is an infinite loop that pins the single worker task
 * and starves every poll and user command behind it — which does not present as
 * a slow prefetch, it presents as Spotify going silently dead, at random,
 * only on tracks whose cover happened to fail. */
static bool sp_pf_fill_one(void) {
    if (s_sp_urgent) return false;            /* the user is waiting; try later */

    for (int i = 0; i < SP_PF_N; i++) {
        sp_pf_t *e = &s_sp_pf[i];
        if (e->ready || !e->url[0]) continue;
        if (!e->buf) {
            e->buf = heap_caps_malloc(SP_ART_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!e->buf) { s_sp_pf_pending = false; return false; }  /* out of PSRAM */
        }
        if (sp_art_get(e->url, e->buf, &e->len, &e->accent)) {
            e->ready = true;
            ESP_LOGI(TAG, "spotify: prefetched %d \"%.20s\"", i + 1, e->name);
            return true;
        }
        return false;                         /* leave it pending for a later pass */
    }
    s_sp_pf_pending = false;                  /* nothing left to fill */
    return false;
}

/* Refresh the lookahead: ask the broker what is coming, keep anything already
 * held, then fill the gaps. After the first pass a track change slides the window
 * by one, so the steady-state cost is a single download per track — the same as
 * having no lookahead at all. */
static void sp_fetch_queue(void) {
    if (!s_sp_access[0] || BROKER_URL[0] == '\0' || !s_sp_body) {
        ESP_LOGW(TAG, "queue: skipped (tok=%d broker=%d buf=%d)",
                 s_sp_access[0] ? 1 : 0, BROKER_URL[0] ? 1 : 0, s_sp_body ? 1 : 0);
        return;
    }

    char url[160];
    snprintf(url, sizeof(url), "/queue?n=%d&s=%d", SP_PF_N, SP_ART_PX);

    size_t got = 0;
    if (!broker_fetch(url, "X-Spotify-Token", s_sp_access,
                      (uint8_t *)s_sp_body, SP_QUEUE_MAX - 1, &got) || got == 0) {
        ESP_LOGW(TAG, "queue: fetch failed (%u B)", (unsigned)got);
        return;
    }
    ESP_LOGI(TAG, "queue: %u B", (unsigned)got);
    s_sp_body[got] = '\0';

    cJSON *j = cJSON_Parse(s_sp_body);
    if (!j) { ESP_LOGW(TAG, "spotify: queue unparseable (%u B)", (unsigned)got); return; }
    cJSON *arr = cJSON_GetObjectItem(j, "q");

    sp_pf_t next[SP_PF_N] = {0};
    int n = 0;
    if (cJSON_IsArray(arr)) {
        for (int i = 0; i < cJSON_GetArraySize(arr) && n < SP_PF_N; i++) {
            cJSON *e = cJSON_GetArrayItem(arr, i);
            cJSON *id = cJSON_GetObjectItem(e, "i"), *nm = cJSON_GetObjectItem(e, "n");
            cJSON *ar = cJSON_GetObjectItem(e, "a"), *u  = cJSON_GetObjectItem(e, "u");
            if (!cJSON_IsString(id) || !cJSON_IsString(u)) continue;
            snprintf(next[n].id, sizeof(next[n].id), "%s", id->valuestring);
            snprintf(next[n].url, sizeof(next[n].url), "%s", u->valuestring);
            if (cJSON_IsString(nm)) ascii_fold(nm->valuestring, next[n].name, sizeof(next[n].name));
            if (cJSON_IsString(ar)) ascii_fold(ar->valuestring, next[n].artist, sizeof(next[n].artist));
            n++;
        }
    }
    cJSON_Delete(j);
    ESP_LOGI(TAG, "queue: %d upcoming", n);
    if (n == 0) return;

    /* Carry over any cover we already hold, matching on URL rather than track id:
     * two tracks off one album share art, and re-downloading it would be waste. */
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < SP_PF_N; k++) {
            if (s_sp_pf[k].ready && strcmp(s_sp_pf[k].url, next[i].url) == 0) {
                next[i].buf = s_sp_pf[k].buf; next[i].accent = s_sp_pf[k].accent;
                /* len must travel with buf: next[] is zero-initialised, so
                 * forgetting it carries the cover over as ready-with-0-bytes.
                 * Promoting that hands LVGL an empty src, the decoder refuses
                 * it, and the RAW fall-through draws nothing — every cover
                 * after it too, since the queue refresh now runs per skip.
                 * That was the "art never shows up" bug of 2026-08-12. */
                next[i].len = s_sp_pf[k].len;
                next[i].ready = true;
                s_sp_pf[k].buf = NULL; s_sp_pf[k].ready = false;
                break;
            }
        }
    }
    /* Whatever is left over keeps its buffer for reuse rather than being freed. */
    for (int i = 0, k = 0; i < SP_PF_N; i++) {
        if (!s_sp_pf[i].buf) continue;
        while (k < SP_PF_N && next[k].buf) k++;
        if (k < SP_PF_N) next[k].buf = s_sp_pf[i].buf;
        else             heap_caps_free(s_sp_pf[i].buf);
        s_sp_pf[i].buf = NULL;
    }
    memcpy(s_sp_pf, next, sizeof(s_sp_pf));

    /* ONE cover per pass, and only when nothing is waiting.
     *
     * The first version fetched all three here, synchronously, on the same task
     * that serves play/next/volume — each one a fresh TLS handshake plus 139 KB.
     * A swipe then queued behind three to five seconds of background downloading
     * and the whole app felt broken. Background work does not get to hold the
     * worker that interactive commands run on.
     *
     * Re-arming the dirty flag spreads the remaining covers across later polls, so
     * the lookahead fills over ~9 s of listening instead of stalling one moment. */
    s_sp_pf_pending = true;
    sp_pf_fill_one();
}

static void sp_fetch_art(void) {
    static int lastskip;
    int skip = !s_sd_ok ? 1 : !s_sp_art_url[0] ? 2 : BROKER_URL[0] == '\0' ? 3 : 0;
    if (skip) {
        if (skip != lastskip) {
            lastskip = skip;
            ESP_LOGW(TAG, "spotify: no art — %s", skip == 1 ? "no SD card" :
                     skip == 2 ? "poll gave no art URL" : "no broker configured");
        }
        return;
    }
    lastskip = 0;

    /* Already on hand — the next track off the same album, or a swipe that went
     * next-then-back. Two things have to happen here and both were once missing:
     *
     * Stamp the id: these bytes are this track's art now, and the UI tick hides
     * the cover whenever the stamped id disagrees with the playing track. Without
     * the stamp this branch produced an INFINITE loader — ready never became
     * current, the ring spun over a correct cover sitting in RAM, and the next
     * poll took this same branch and changed nothing, forever.
     *
     * Re-show when cleared: sp_art_clear() hid the object; setting a flag does
     * not unhide it. Gated on ready so the steady state — this branch runs on
     * every poll once the art is stable — does not re-decode per poll. */
    if (strcmp(s_sp_art_url, s_sp_art_have) == 0) {
        snprintf(s_sp_art_id, sizeof(s_sp_art_id), "%s", s_sp_track_id);
        if (!s_sp_art_ready) sp_art_show();
        return;
    }

    /* Already prefetched? Then this is free: swap the buffers rather than copying
     * 139 KB, and skip the download entirely. This is the whole point of the
     * prefetcher — by the time the poll confirms a swipe, the cover is in hand. */
    if (sp_pf_promote(s_sp_art_url)) return;

    if (strcmp(s_sp_art_url, s_sp_art_failed) == 0 &&
        (now_ms() - s_sp_art_failed_at) < SP_ART_RETRY_MS) return;

    if (!s_sp_art_buf) {
        s_sp_art_buf = heap_caps_malloc(SP_ART_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_sp_art_buf) { ESP_LOGE(TAG, "spotify: no PSRAM for art"); return; }
    }

    uint32_t accent = 0;
    if (sp_art_get(s_sp_art_url, s_sp_art_buf, &s_sp_art_len, &accent)) {
        snprintf(s_sp_art_have, sizeof(s_sp_art_have), "%s", s_sp_art_url);
        snprintf(s_sp_art_id, sizeof(s_sp_art_id), "%s", s_sp_track_id);
        s_sp_accent = accent;
        s_sp_art_failed[0] = '\0';
        sp_art_show();
        ESP_LOGI(TAG, "spotify: art updated (%u B jpeg)", (unsigned)s_sp_art_len);
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
        bool skipped = false;                 /* did this command change track? */
        if (xQueueReceive(s_sp_q, &cmd, portMAX_DELAY) != pdTRUE) continue;
        if (cmd != SP_CMD_POLL) s_sp_urgent = false;   /* it is being served now */
        if (!s_wifi_up) continue;

        switch (cmd) {
        /* sp_push_volume() is a no-op unless a level is owed, so putting it on the
         * poll costs nothing and guarantees a dropped command is picked up. */
        case SP_CMD_POLL:
            sp_poll_state();
            sp_push_volume();
            sp_check_liked();
            sp_fetch_art();
            /* Last, and only when something actually changed: this is the one
             * place that pays a broker round trip plus up to SP_PF_N downloads,
             * and it must never delay drawing the track the user is on now. */
            if (s_sp_urgent) {
                /* leave both flags standing; the user is mid-action */
            } else if (s_sp_queue_dirty) {
                s_sp_queue_dirty = false;
                sp_fetch_queue();             /* re-ask, then fill one */
            } else if (s_sp_pf_pending) {
                /* Fill in a burst, not one cover per poll. One-per-poll was safe
                 * and useless: polls are 3 s apart, so six slots took the better
                 * part of a minute and any touch restarted the wait — the cache
                 * was never ahead of the user, which is the only state in which
                 * it is worth having.
                 *
                 * Bursting is only safe because of the check between each cover.
                 * The original failure here was three synchronous downloads
                 * sitting in front of the user's commands on the one worker task;
                 * s_sp_urgent is raised by any user action, so the most a swipe
                 * can now wait is the single download already in flight. */
                for (int n = 0; n < SP_PF_N && s_sp_pf_pending && !s_sp_urgent; n++)
                    if (!sp_pf_fill_one()) break;
            }
            break;
        case SP_CMD_DEVICES: sp_poll_devices(); break;
        case SP_CMD_LIKE:    sp_toggle_like(); break;
        /* No confirm poll: the bar is already showing the value we just sent,
         * and a read-back would only fight the next press. */
        case SP_CMD_VOLUME:  sp_push_volume(); break;

        /* Optimistic UI: the icon already flipped, so a poll follows to confirm
         * rather than to discover. */
        case SP_CMD_PLAY:    sp_call(HTTP_METHOD_PUT,  "/me/player/play", NULL);  goto confirm;
        case SP_CMD_PAUSE:   sp_call(HTTP_METHOD_PUT,  "/me/player/pause", NULL); goto confirm;
        case SP_CMD_NEXT:    sp_call(HTTP_METHOD_POST, "/me/player/next", NULL);
                             skipped = true; goto confirm;
        case SP_CMD_PREV:    sp_call(HTTP_METHOD_POST, "/me/player/previous", NULL);
                             skipped = true; goto confirm;
        case SP_CMD_SHUFFLE: {
            char p[64];
            snprintf(p, sizeof(p), "/me/player/shuffle?state=%s",
                     s_sp_shuffle ? "true" : "false");
            s_sp_queue_dirty = true;      /* shuffle reorders what comes next */
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

    confirm: {
        /* Confirm adaptively instead of sleeping a flat 350 ms.
         *
         * That delay existed so Spotify had advanced before we read back, and it
         * was pure dead time with the cover blank — paid in full on every skip
         * even when the server had caught up in a fraction of it. Now: ask early,
         * and only wait again if the track really has not changed yet.
         *
         * Only skips get the loop. Play and pause do not change the track, so
         * polling until it changes would spin them out to the full timeout — the
         * opposite of the intent. */
        char before[26];
        snprintf(before, sizeof(before), "%s", s_sp_track_id);
        int64_t t_cmd = now_ms();

        vTaskDelay(pdMS_TO_TICKS(70));
        sp_poll_state();
        if (skipped) {
            for (int i = 0; i < 5 && strcmp(s_sp_track_id, before) == 0; i++) {
                vTaskDelay(pdMS_TO_TICKS(60));
                sp_poll_state();
            }
        }

        /* The cover before the heart. Both are round trips, but only one of them
         * is what the user is staring at — running sp_check_liked() first put a
         * whole HTTPS call between the swipe and the artwork. */
        sp_fetch_art();
        if (skipped) ESP_LOGI(TAG, "spotify: cover up %lldms after skip",
                              (long long)(now_ms() - t_cmd));
        sp_push_volume();
        sp_check_liked();
        /* A skip is precisely when what-comes-next changed, so refill here rather
         * than waiting up to 3 s for the next poll to notice. Still last, and
         * still behind the urgent check — the cover for the track the user is on
         * has already been drawn by this point. */
        if (skipped && !s_sp_urgent) {
            if (s_sp_queue_dirty) { s_sp_queue_dirty = false; sp_fetch_queue(); }
            for (int n = 0; n < SP_PF_N && s_sp_pf_pending && !s_sp_urgent; n++)
                if (!sp_pf_fill_one()) break;
        }
    }
    }
}

static void sp_send(sp_cmd_t c) {
    if (!s_sp_q) return;
    if (c != SP_CMD_POLL) s_sp_urgent = true;
    xQueueSend(s_sp_q, &c, 0);
}

static void sp_init(void) {
    s_sp_q = xQueueCreate(6, sizeof(sp_cmd_t));
    if (!s_sp_q) return;
    if (xTaskCreateWithCaps(sp_task, "spotify", 12288, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "!! spotify stack fell back to INTERNAL SRAM — costs ~8 KB "
                      "of the scarce pool; expect a lower floor");
        s_stack_fallback = true;
        xTaskCreate(sp_task, "spotify", 12288, NULL, 4, NULL);
    }
}

/* ---- the screen ---- */

static lv_obj_t *s_sp_lbl_track, *s_sp_lbl_artist;
static lv_obj_t *s_sp_btn_play_lbl, *s_sp_btn_shuf, *s_sp_btn_dev;
static lv_obj_t *s_sp_btn_prev, *s_sp_btn_next, *s_sp_btn_like;
/* The volume HUD: a vertical fill beside the cover plus a glyph that goes to
 * mute at zero. Hidden until a key says otherwise, so the cover keeps the space
 * and nothing permanent is spent on it. */
static lv_obj_t *s_sp_vol_bar, *s_sp_vol_icon, *s_sp_chip;
static int s_sp_devdrawn = -1;   /* signature of the drawn device list */
static int s_sp_devlit = -1;     /* last device-button tint, -1 = unset */
static int s_sp_vol_painted = -1; /* last level drawn on the gauge */
static bool s_sp_chrome;          /* secondary controls shown? hidden by default */
static int32_t s_sp_chrome_p;     /* 0 = immersive, SP_CHROME_FULL = chrome */
static int64_t s_sp_chrome_at;    /* when last shown, for the auto-hide */
static lv_obj_t *s_sp_devpanel, *s_sp_devlist;
static lv_obj_t *s_sp_pair_panel, *s_sp_pair_qr;
static char s_sp_pair_drawn[256];
static lv_obj_t *s_sp_scr;      /* the MUSIC screen, for the accent backdrop */
static uint32_t s_sp_bg_drawn;  /* last backdrop colour, 0 = unset */

/* Lay the whole scene out from one progress value. Driven by the animation, so it
 * stays integer maths with no allocation and no logging. Six widgets animated
 * independently would be six chances to disagree mid-transition; one source of
 * truth cannot. */
static void sp_chrome_apply(void *unused, int32_t p) {
    if (!s_sp_art) return;
    const int32_t F = SP_CHROME_FULL;

    /* Fixed size, deliberately. Resizing it scaled a JPEG every frame, which is
     * both expensive and how the IntegerDivideByZero crash happened:
     * lv_color_format_get_size(LV_COLOR_FORMAT_RAW) is 0, so the transform path
     * computed buf_stride = blend_w * 0 and divided by it
     * (lv_draw_sw_img.c:515). The chrome sliding in over the cover reads just as
     * well and costs nothing per frame. */
    int size = SP_ART_PX;
    int cx   = 240;
    int y    = 30;
    int x    = cx - size / 2;
    (void)F; (void)p;

    lv_obj_set_size(s_sp_art, size, size);
    lv_obj_set_pos(s_sp_art, x, y);
    if (s_sp_art_ph) {
        lv_obj_set_size(s_sp_art_ph, size, size);
        lv_obj_set_pos(s_sp_art_ph, x, y);
    }
    /* The chip rides 6 px inside the cover's bottom edge, so it tracks both size
     * and position rather than being pinned to a screen coordinate. */
    if (s_sp_chip) {
        lv_obj_set_size(s_sp_chip, size - 12, 60);
        lv_obj_set_pos(s_sp_chip, cx - (size - 12) / 2, y + size - 66);
    }
    if (s_sp_lbl_track)  lv_obj_set_pos(s_sp_lbl_track,  cx - 120, y + size - 59);
    if (s_sp_lbl_artist) lv_obj_set_pos(s_sp_lbl_artist, cx - 120, y + size - 32);

    /* Translate rather than reposition: it composes with the layout above, and
     * unlike a transform it allocates no transient layer per redraw (HARDWARE 5). */
    int32_t ltx = -150 + (150 * p / F);
    int32_t rtx =   96 - ( 96 * p / F);
    if (s_sp_btn_shuf) lv_obj_set_style_translate_x(lv_obj_get_parent(s_sp_btn_shuf), ltx, 0);
    if (s_sp_btn_like) lv_obj_set_style_translate_x(lv_obj_get_parent(s_sp_btn_like), ltx, 0);
    if (s_sp_btn_dev)  lv_obj_set_style_translate_x(lv_obj_get_parent(s_sp_btn_dev),  ltx, 0);
    if (s_sp_vol_bar)  lv_obj_set_style_translate_x(s_sp_vol_bar,  rtx, 0);
    if (s_sp_vol_icon) lv_obj_set_style_translate_x(s_sp_vol_icon, rtx, 0);
}

/* Transport stays put in both states — it is the one thing you always want under
 * a thumb. Only the secondary controls come and go. */
static void sp_chrome_set(bool show, bool animate) {
    s_sp_chrome_at = now_ms();
    if (show == s_sp_chrome && animate) return;
    s_sp_chrome = show;

    int32_t to = show ? SP_CHROME_FULL : 0;
    if (!animate) { s_sp_chrome_p = to; sp_chrome_apply(NULL, to); return; }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &s_sp_chrome_p);
    lv_anim_set_exec_cb(&a, sp_chrome_apply);
    lv_anim_set_values(&a, s_sp_chrome_p, to);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    s_sp_chrome_p = to;
}

/* A drag across a clickable screen still delivers LV_EVENT_CLICKED on release, so
 * every swipe was also toggling the chrome. Ignore a click that lands in the
 * shadow of a gesture. */
static void sp_tap_cb(lv_event_t *e) {
    if (now_ms() - s_sp_swipe_at < 500) return;

    /* Dead zone over the transport. The buttons consume their own clicks, but the
     * gaps between them fall through to the screen — so a near-miss on play, which
     * on this touchscreen is common, toggled the chrome instead. That is the worst
     * place on the screen to put a surprise: you were reaching for pause.
     *
     * 330 is just above the transport row (buttons start at y338). Below it, a tap
     * that missed a button does nothing, which is the correct answer for a miss. */
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_point_t pt;
        lv_indev_get_point(indev, &pt);
        if (pt.y >= 330) return;
    }
    sp_chrome_set(!s_sp_chrome, true);
}

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
    if (s_sp_spin)   lv_obj_remove_flag(s_sp_spin, LV_OBJ_FLAG_HIDDEN);
    s_sp_art_ready = false;
    s_sp_art_shown[0] = '\0';
}

static void sp_play_cb(lv_event_t *e) {
    if (s_lock_np && now_ms() - s_lock_swipe_at < 500) return;
    s_sp_playing = !s_sp_playing;
    if (s_sp_btn_play_lbl) {
        lv_label_set_text(s_sp_btn_play_lbl, s_sp_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    sp_send(s_sp_playing ? SP_CMD_PLAY : SP_CMD_PAUSE);
}
static void sp_next_cb(lv_event_t *e) {
    if (s_lock_np && now_ms() - s_lock_swipe_at < 500) return;
    if (s_sp_no_next) return;
    sp_art_clear();
    sp_send(SP_CMD_NEXT);
}
static void sp_prev_cb(lv_event_t *e) {
    if (s_lock_np && now_ms() - s_lock_swipe_at < 500) return;
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
static void sp_devclose_cb(lv_event_t *e) { sp_show_devices(false); }

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
/* LVGL re-sends LV_EVENT_GESTURE for as long as the finger stays down, so a
 * single flick arrives several times and skipped two or three tracks.
 *
 * Two guards, because they catch different things. lv_indev_wait_release() stops
 * the repeats inside one touch — that is the actual defect. The cooldown then
 * stops a fast second flick from landing before the first has been confirmed by a
 * poll, which is a real gesture but almost never an intended one. */
#define SP_SWIPE_MS 550

static void sp_gesture_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    lv_dir_t d = lv_indev_get_gesture_dir(indev);

    bool picker = s_sp_devpanel && !lv_obj_has_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN);
    /* Every direction below acts once per touch. */
    lv_indev_wait_release(indev);

    if (d == LV_DIR_TOP && home_gesture_from_bottom(indev)) {
        /* Back before home, matching what the right key does through app_back():
         * from a sub-scene the swipe pops it rather than leaving the app. */
        if (picker) sp_show_devices(false);
        else        app_request(APP_DRAWER);
        return;
    }
    if (picker) return;          /* a flick here belongs to the list */
    if (d != LV_DIR_LEFT && d != LV_DIR_RIGHT) return;

    int64_t now = now_ms();
    if (now - s_sp_swipe_at < SP_SWIPE_MS) return;
    s_sp_swipe_at = now;

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
static void sp_vol_hud_paint(bool with_title) {
    if (!s_sp_vol_bar) return;

    int v = s_sp_vol < 0 ? 0 : s_sp_vol;
    lv_slider_set_value(s_sp_vol_bar, v, LV_ANIM_OFF);

    uint32_t c;
    if (!s_sp_vol_ok) {
        c = 0x64748B;
        label_set_changed(s_sp_vol_icon, ICON_VOL_MUTE);
        if (with_title && s_sp_lbl_track) label_set_changed(s_sp_lbl_track, "no volume control");
    } else if (v == 0) {
        c = 0xF43F5E;                               /* muted reads as a warning */
        label_set_changed(s_sp_vol_icon, ICON_VOL_MUTE);
        if (with_title && s_sp_lbl_track) label_set_changed(s_sp_lbl_track, "MUTED");
    } else {
        c = 0x1DB954;
        label_set_changed(s_sp_vol_icon, ICON_VOL_UP);
        if (with_title && s_sp_lbl_track) {
            char t[20];
            snprintf(t, sizeof(t), "VOLUME %d", v);
            label_set_changed(s_sp_lbl_track, t);   /* the HUD repaints every tick */
        }
    }
    lv_obj_set_style_text_color(s_sp_vol_icon, lv_color_hex(c), 0);
    lv_obj_set_style_bg_color(s_sp_vol_bar, lv_color_hex(c), LV_PART_INDICATOR);
}

/* Dragging the gauge sets the level directly. Guarded on s_sp_vol_ok because a
 * device that refuses remote volume must not appear to accept a drag.
 *
 * The readout follows the knob on every VALUE_CHANGED and the PUT waits for
 * RELEASED — the two halves of this callback answer to different costs. The
 * borrowed title line is absolutely positioned on this screen, so repainting it
 * mid-drag moves nothing (unlike CONTROL's flex cards, see cfg_vol_cb); a
 * Spotify round trip per pixel of drag is a different matter entirely. */
static void sp_vol_slider_cb(lv_event_t *e) {
    if (!s_sp_vol_ok) return;
    int v = lv_slider_get_value(s_sp_vol_bar);
    if (v != s_sp_vol) {
        s_sp_vol = v;
        s_sp_vol_painted = v;
        s_sp_vol_shown = now_ms();
        sp_vol_hud_paint(true);
    }
    if (lv_event_get_code(e) != LV_EVENT_RELEASED) return;

    /* sp_push_volume() weighs the level against s_sp_vol_sent, so a drag that
     * ended where it began costs nothing on the wire. */
    sp_send(SP_CMD_VOLUME);
}

static void sp_vol_hud_show(void) {
    s_sp_vol_shown = now_ms();
    /* Reveal the controls, don't just keep them alive: the slider IS the volume
     * readout, and hiding it while the user is changing volume means pressing a
     * key with nothing to look at. Auto-hide then retires it 4 s after the last
     * press, so it costs nothing once they stop. */
    sp_chrome_set(true, true);
    if (!s_sp_vol_bar) return;
    sp_vol_hud_paint(true);
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
    if (!s_sp_devpanel || !lv_obj_has_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN) ||
        (s_sp_pair_panel && !lv_obj_has_flag(s_sp_pair_panel, LV_OBJ_FLAG_HIDDEN))) return false;

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

    bool show_pair = s_sp_authfail && s_sp_pair_url[0] != '\0';
    if (s_sp_pair_panel) {
        bool hidden = lv_obj_has_flag(s_sp_pair_panel, LV_OBJ_FLAG_HIDDEN);
        if (show_pair && hidden) lv_obj_remove_flag(s_sp_pair_panel, LV_OBJ_FLAG_HIDDEN);
        if (!show_pair && !hidden) lv_obj_add_flag(s_sp_pair_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (show_pair && s_sp_pair_qr && strcmp(s_sp_pair_drawn, s_sp_pair_url) != 0) {
        size_t n = strlen(s_sp_pair_url);
        if (lv_qrcode_update(s_sp_pair_qr, s_sp_pair_url, (uint32_t)n) == LV_RESULT_OK) {
            snprintf(s_sp_pair_drawn, sizeof(s_sp_pair_drawn), "%s", s_sp_pair_url);
        } else {
            ESP_LOGW(TAG, "spotify: pairing QR encode failed (%u bytes)", (unsigned)n);
        }
    }

    if (s_sp_authfail) {
        label_set_changed(s_sp_lbl_track, "not authorised");
        label_set_changed(s_sp_lbl_artist,
                          s_sp_pair_url[0] ? "scan to connect" : "broker unavailable");
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

    /* Chrome retires itself. Driven from this tick rather than an lv_timer of its
     * own — teardown only deletes s_app_timer, so a second timer would be a leak
     * waiting for someone to forget it. A volume key press counts as activity, so
     * adjusting volume does not have the controls vanish under your thumb. */
    if (s_sp_chrome && (now_ms() - s_sp_chrome_at) > SP_CHROME_HIDE_MS) {
        sp_chrome_set(false, true);
    }

    /* The volume HUD is on demand: it appears when a key moves the level and
     * retires itself, so the cover keeps the space the rest of the time. */
    if (s_sp_vol_bar) {
        bool up = s_sp_vol_shown && (now_ms() - s_sp_vol_shown) < SP_VOL_HUD_MS;
        /* The gauge itself never hides now — only the borrowed title expires. It is
         * repainted on change, or while a key is still being pressed; unconditionally
         * every tick would invalidate the bar forever for nothing. */
        /* Never repaint while a thumb is on it — the poll would yank the knob back
         * to the server's value mid-drag. */
        if (lv_obj_has_state(s_sp_vol_bar, LV_STATE_PRESSED)) { /* leave it alone */ }
        else if (up || s_sp_vol != s_sp_vol_painted) {
            s_sp_vol_painted = s_sp_vol;
            sp_vol_hud_paint(up);
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
    /* Keyed on the track, not the URL: /me/player and the broker's queue each pick
     * the "nearest" image from their own copy of the list, and when those two
     * disagree a perfectly good cover looked stale — it hid and downloaded again. */
    bool art_current = s_sp_art_ready && s_sp_art_id[0] &&
                       strcmp(s_sp_art_id, s_sp_track_id) == 0;

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

    /* The ring is a promise that a cover is coming. Nothing is coming when there
     * is no URL to fetch — MUSIC open against a paused account — or when the last
     * attempt at this one failed and is sitting out its cooldown. A ring turning
     * against either is a lie, and a permanent one: it invalidates every frame
     * for as long as the screen is up. Change-gated, like everything else on this
     * tick, because the flag setters invalidate whether or not anything moved. */
    if (s_sp_spin && !lv_obj_has_flag(s_sp_art_ph, LV_OBJ_FLAG_HIDDEN)) {
        /* The tick is the ring's only writer — a hide from another task just
         * fought this gate and lost one frame later. Keyed on the art URL: if the
         * track has no artwork at all the note sits alone and nothing pretends
         * otherwise, and a failed fetch keeps the ring through its cooldown
         * because a retry genuinely is coming. */
        bool want = s_sp_art_url[0] != '\0';
        if (want == lv_obj_has_flag(s_sp_spin, LV_OBJ_FLAG_HIDDEN)) {
            if (want) lv_obj_remove_flag(s_sp_spin, LV_OBJ_FLAG_HIDDEN);
            else      lv_obj_add_flag(s_sp_spin, LV_OBJ_FLAG_HIDDEN);
        }
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

    /* Shuffle, like and devices stack down the left edge instead of straddling the
     * top. The cover was never limited by width — it was limited by the band
     * between that top row and the track title, so vacating y36..100 takes the art
     * from 148 to 230 px, which is 2.4x the area. Volume becomes a permanent gauge
     * down the right edge rather than an overlay that comes and goes.
     *
     * Every position here was checked against the panel's r=110 corner arcs; the
     * tightest is the play button, 44 px from the bottom edge. */
    s_sp_btn_shuf = sp_round_btn(scr, LV_SYMBOL_SHUFFLE, NULL, 64, 90, 76,
                                 sp_shuf_cb, 0x1DB954, 0x10161F);
    /* Matches the glyph Spotify itself uses for Connect — see ICON_DEVICES. */
    s_sp_btn_dev = sp_round_btn(scr, ICON_DEVICES, &hud_icons_30, 64, 282, 76,
                                sp_devbtn_cb, 0x1DB954, 0x10161F);
    s_sp_btn_like = sp_round_btn(scr, ICON_HEART_OPEN, &hud_icons_30, 64, 186, 76,
                                 sp_like_cb, 0xF43F5E, 0x10161F);

    /* cover, smaller than before on purpose */
    s_sp_art_ph = lv_obj_create(scr);
    lv_obj_remove_style_all(s_sp_art_ph);
    lv_obj_set_size(s_sp_art_ph, SP_ART_PX, SP_ART_PX);
    lv_obj_set_style_bg_color(s_sp_art_ph, lv_color_hex(0x11161F), 0);
    lv_obj_set_style_bg_opa(s_sp_art_ph, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_sp_art_ph, 20, 0);
    lv_obj_remove_flag(s_sp_art_ph, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *ph = lv_label_create(s_sp_art_ph);
    lv_obj_set_style_text_font(ph, &app_icons_64, 0);
    lv_obj_set_style_text_color(ph, lv_color_hex(0x1E293B), 0);
    lv_label_set_text(ph, ICON_MUSIC);
    lv_obj_center(ph);

    /* A ring around the note while bytes are on the way. The wait is real —
     * poll, then fetch — and a still placeholder makes that read as a stall
     * rather than as progress. LVGL animates this off its own timer, so it
     * stays fluid while the network task works, and it costs one small
     * invalidation per frame instead of a full redraw. */
    s_sp_spin = lv_spinner_create(s_sp_art_ph);
    lv_obj_set_size(s_sp_spin, 116, 116);
    lv_obj_center(s_sp_spin);
    lv_spinner_set_anim_params(s_sp_spin, 1100, 65);
    lv_obj_remove_flag(s_sp_spin, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_sp_spin, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_sp_spin, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_sp_spin, lv_color_hex(0x1B2230), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_sp_spin, lv_color_hex(0x1DB954), LV_PART_INDICATOR);

    s_sp_art = lv_image_create(scr);
    lv_obj_set_size(s_sp_art, SP_ART_PX, SP_ART_PX);
    lv_image_set_inner_align(s_sp_art, LV_IMAGE_ALIGN_COVER);
    lv_obj_add_flag(s_sp_art, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_sp_art, LV_OBJ_FLAG_CLICKABLE);

    /* Volume HUD, in the gap to the left of the cover. The cover is 148 wide and
     * centred, so it occupies x166..314; an 18 px fill at x128 sits in clear
     * space and still clears the r=110 corner arcs, which only cut x<110. The
     * readout goes top-middle, in the slot the like button vacated, where it is
     * big enough to read at a glance without crowding the corner buttons. */
    /* 34 px wide, not 18: this is a drag target on a resistive-feeling panel, and
     * a thin one is unusable. The knob is drawn deliberately oversized for the same
     * reason — it is the part a thumb aims at. */
    s_sp_vol_bar = lv_slider_create(scr);
    lv_obj_set_size(s_sp_vol_bar, 34, 240);
    lv_obj_set_pos(s_sp_vol_bar, 410, 90);
    lv_slider_set_range(s_sp_vol_bar, 0, 100);
    lv_obj_set_style_radius(s_sp_vol_bar, 17, 0);
    lv_obj_set_style_radius(s_sp_vol_bar, 17, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_sp_vol_bar, lv_color_hex(0x18202C), 0);
    lv_obj_set_style_bg_opa(s_sp_vol_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_sp_vol_bar, 10, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_sp_vol_bar, lv_color_hex(0xF2E9DC), LV_PART_KNOB);
    /* Both events, one callback: it repaints on VALUE_CHANGED and only commits
     * on RELEASED, so the number tracks the thumb without spending a Spotify PUT
     * per pixel of drag. */
    lv_obj_add_event_cb(s_sp_vol_bar, sp_vol_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_sp_vol_bar, sp_vol_slider_cb, LV_EVENT_RELEASED, NULL);
    /* LVGL sets LV_OBJ_FLAG_GESTURE_BUBBLE on every child that has a parent
     * (lv_obj.c:593), so a drag here reached the screen's gesture handler and
     * swipe-up-for-home fired: dragging the volume threw you out of the app. Any
     * widget that owns a drag has to stop gestures escaping to the screen. */
    lv_obj_remove_flag(s_sp_vol_bar, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_sp_vol_icon = lv_label_create(scr);
    lv_obj_set_style_text_font(s_sp_vol_icon, &hud_icons_30, 0);
    lv_obj_set_style_text_color(s_sp_vol_icon, lv_color_hex(0x1DB954), 0);
    label_set_changed(s_sp_vol_icon, ICON_VOL_UP);
    lv_obj_set_pos(s_sp_vol_icon, 415, 52);

    /* Flat, not a gradient — RGB565 bands visibly on a dark ramp (HARDWARE.md 5).
     * Inset 6 px from the cover so it reads as a chip floating on the artwork
     * rather than as the cover having been cropped. */
    s_sp_chip = lv_obj_create(scr);
    lv_obj_remove_style_all(s_sp_chip);
    lv_obj_set_size(s_sp_chip, SP_ART_PX - 12, 60);
    lv_obj_set_style_bg_color(s_sp_chip, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_sp_chip, 185, 0);
    lv_obj_set_style_radius(s_sp_chip, 16, 0);
    lv_obj_remove_flag(s_sp_chip, LV_OBJ_FLAG_CLICKABLE);

    s_sp_lbl_track = lv_label_create(scr);
    lv_obj_set_width(s_sp_lbl_track, 240);
    lv_obj_set_style_text_font(s_sp_lbl_track, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_sp_lbl_track, lv_color_hex(0xF2E9DC), 0);
    lv_obj_set_style_text_align(s_sp_lbl_track, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_sp_lbl_track, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(s_sp_lbl_track, "connecting...");

    s_sp_lbl_artist = lv_label_create(scr);
    lv_obj_set_width(s_sp_lbl_artist, 240);
    lv_obj_set_style_text_font(s_sp_lbl_artist, &hud_text_18, 0);
    lv_obj_set_style_text_color(s_sp_lbl_artist, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_text_align(s_sp_lbl_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_sp_lbl_artist, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_sp_lbl_artist, "");

    /* transport: the things you actually press, sized accordingly */
    /* Glyph sizes are 36 for the skips and 48 for play/pause, against the 14 px
     * default and 20 px they started at — icons sized for a button a third as
     * wide, which read as timid rather than as small. The two sizes are the
     * hierarchy: play is the primary verb and gets the bigger circle *and* the
     * bigger glyph, while a matched 48 made all three compete. 36 is enabled in
     * sdkconfig.defaults purely for this; nothing sits between 20 and 48. */
    s_sp_btn_prev = sp_round_btn(scr, LV_SYMBOL_PREV, &lv_font_montserrat_36,
                                 94, 386, 96, sp_prev_cb, 0x334155, 0x141B26);
    s_sp_btn_play_lbl = sp_round_btn(scr, LV_SYMBOL_PLAY, &lv_font_montserrat_48,
                                     240, 386, 116, sp_play_cb, 0x1DB954, 0x16241C);
    s_sp_btn_next = sp_round_btn(scr, LV_SYMBOL_NEXT, &lv_font_montserrat_36,
                                 386, 386, 96, sp_next_cb, 0x334155, 0x141B26);

    /* The title chip is created after the left rail and its edge can overlap the
     * device circle by a few pixels. Raise the whole button, not just its glyph,
     * so the ring and icon both remain intact. Do this before the picker is
     * created, because that full-screen scene must still sit above everything. */
    if (s_sp_btn_dev) lv_obj_move_foreground(lv_obj_get_parent(s_sp_btn_dev));

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

    /* A visible way out. The right key already popped this scene and the swipe now
     * does too, but a full-screen panel whose only exits are undiscoverable is a
     * panel people feel trapped in — you have to be told, and nobody is.
     *
     * Placed where the device button itself sits on the scene underneath, so the
     * corner you tapped to get in is the corner you tap to get out. The list moved
     * down to 112 to clear it; at y88 the button's 64 px circle overlapped the
     * first card and would have stolen touches meant for a device. */
    sp_round_btn(s_sp_devpanel, LV_SYMBOL_CLOSE, NULL, 400, 68, 64,
                 sp_devclose_cb, 0x64748B, 0x10161F);

    s_sp_devlist = lv_obj_create(s_sp_devpanel);
    lv_obj_remove_style_all(s_sp_devlist);
    lv_obj_set_size(s_sp_devlist, CONTENT_W, 288);
    lv_obj_align(s_sp_devlist, LV_ALIGN_TOP_MID, 0, 112);
    lv_obj_set_flex_flow(s_sp_devlist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_sp_devlist, 12, 0);
    lv_obj_set_scroll_dir(s_sp_devlist, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_sp_devlist, LV_SCROLLBAR_MODE_OFF);
    /* Same reason as the volume slider: scrolling this list is a vertical drag, and
     * letting it bubble would close the picker out from under the finger. */
    lv_obj_remove_flag(s_sp_devlist, LV_OBJ_FLAG_GESTURE_BUBBLE);

    /* Reauthorisation is a full scene, not a tiny QR squeezed into the player.
     * It appears only when the authenticated broker says this user's stored
     * refresh token is absent or revoked. The phone then owns login, consent and
     * any Spotify 2FA; the cube only has to show an unambiguous handoff. */
    s_sp_pair_panel = lv_obj_create(scr);
    lv_obj_remove_style_all(s_sp_pair_panel);
    lv_obj_set_size(s_sp_pair_panel, 480, 480);
    lv_obj_center(s_sp_pair_panel);
    lv_obj_set_style_bg_color(s_sp_pair_panel, lv_color_hex(0x05070B), 0);
    lv_obj_set_style_bg_opa(s_sp_pair_panel, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_sp_pair_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sp_pair_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *pt = lv_label_create(s_sp_pair_panel);
    lv_obj_set_style_text_font(pt, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(pt, lv_color_hex(0x1DB954), 0);
    lv_obj_set_style_text_letter_space(pt, 2, 0);
    lv_label_set_text(pt, "CONNECT SPOTIFY");
    lv_obj_align(pt, LV_ALIGN_TOP_MID, 0, 42);

    s_sp_pair_qr = lv_qrcode_create(s_sp_pair_panel);
    lv_qrcode_set_size(s_sp_pair_qr, 228);
    lv_qrcode_set_dark_color(s_sp_pair_qr, lv_color_hex(0x000000));
    lv_qrcode_set_light_color(s_sp_pair_qr, lv_color_hex(0xFFFFFF));
    lv_qrcode_set_quiet_zone(s_sp_pair_qr, true);
    lv_obj_align(s_sp_pair_qr, LV_ALIGN_TOP_MID, 0, 92);

    lv_obj_t *ps = lv_label_create(s_sp_pair_panel);
    lv_obj_set_width(ps, 360);
    lv_obj_set_style_text_font(ps, &hud_text_18, 0);
    lv_obj_set_style_text_color(ps, lv_color_hex(0xC7D2E0), 0);
    lv_obj_set_style_text_align(ps, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(ps, LV_LABEL_LONG_WRAP);
    lv_label_set_text(ps, "SCAN WITH YOUR PHONE\nSpotify handles login and 2FA");
    lv_obj_align(ps, LV_ALIGN_TOP_MID, 0, 348);

    lv_obj_add_event_cb(scr, sp_gesture_cb, LV_EVENT_GESTURE, NULL);
    /* Buttons consume their own clicks, so this only ever sees the background or
     * the cover — exactly the "tap to toggle" surface we want. */
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, sp_tap_cb, LV_EVENT_CLICKED, NULL);

    s_sp_chrome = false;
    s_sp_chrome_p = 0;
    sp_chrome_apply(NULL, 0);      /* immersive by default, no animation on open */

    /* The cover is the backdrop and everything else sits on top of it. LVGL
     * draws children in creation order, and the art is created after the left
     * column — so without this the artwork paints over shuffle, like and
     * devices, and over the transport row once it is wide enough to reach them.
     * The track chip and labels are created after the art and deliberately stay
     * above it. */
    lv_obj_move_background(s_sp_art);
    lv_obj_move_background(s_sp_art_ph);

    s_sp_devdrawn = -1;
    s_sp_devlit = -1;
    s_sp_vol_painted = -1;   /* draw the gauge once on open, not only on change */
    s_sp_art_shown[0] = '\0';
    s_sp_pair_drawn[0] = '\0';
    sp_send(SP_CMD_POLL);
    sp_send(SP_CMD_DEVICES);
    s_app_timer = lv_timer_create(sp_timer_cb, 400, NULL);
}

/* ---------------- app drawer ---------------- */

/* PIP ships in this image. The flag stays because it is a build-time choice,
 * not a constant: flipping it to 0 compiles the app out again, leaving a
 * zeroed hole in s_apps that everything walking the table must skip — see
 * app_enabled() and HARDWARE.md pitfall #31. */
#define FACET_APP_PET 1

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
#if FACET_APP_PET
    [APP_PET]    = { "PIP",    "pet",    ICON_PETS,      0xF59E0B, build_pet_app,    pet_save },
#endif
    [APP_MUSIC]  = { "MUSIC",  "music",  ICON_MUSIC,     0x1DB954, build_music_app,  NULL      },
    [APP_DAYS]   = { "DAYS",   "days",   ICON_EVENT,     0x8B7CF6, build_days_app,   NULL      },
    /* Red, and not the amber the app itself still uses for a running session:
     * PIP directly above it is 0xF59E0B, and two ambers side by side in a 2x2
     * made the tiles hard to tell apart at a glance. */
    /* Red tile, amber session — deliberate, and confirmed with the user after
     * seeing it on hardware. Every other app's tile colour matches its in-app
     * accent, so this is the one place that pattern breaks and it looks like an
     * oversight. It is not: the red reads as "timer" in the drawer and the amber
     * reads as "running" inside. Don't unify them.
     *
     * The in-app amber is 0xFFB454 at pomo_refresh(). */
    [APP_POMO]   = { "FOCUS",  "pomo",   ICON_TARGET,    0xFF453A, build_pomo_app,   pomo_save },
};

/* An app can be compiled out (FACET_APP_PET), which leaves a zeroed hole in this
 * table rather than shortening it — the enum still indexes past it. Everything
 * that walks the table must skip holes, or the drawer draws a blank tile, the key
 * cycles onto nothing, and app_open() calls a NULL build(). */
static bool app_enabled(int i) {
    return i >= 0 && i < APP_COUNT && s_apps[i].build != NULL;
}

/* The lock-screen quick-action menu: "off", then every compiled-in app in enum
 * order. Built from the table rather than listed by hand, so it cannot drift
 * from it and a compiled-out app leaves no dead slot. The stored value is the
 * app index; these only convert to and from the menu position. */
static int lock_key_choices(void) {
    int n = 1;
    for (int i = 0; i < APP_COUNT; i++) if (app_enabled(i)) n++;
    return n;
}

static int lock_key_app_at(int slot) {
    if (slot <= 0) return LOCK_KEY_OFF;
    for (int i = 0; i < APP_COUNT; i++) {
        if (!app_enabled(i)) continue;
        if (--slot == 0) return i;
    }
    return LOCK_KEY_OFF;
}

static const char *lock_key_name(int app) {
    return app_enabled(app) ? s_apps[app].name : "off";
}

/* A montserrat symbol per app, for the glyph inside the middle bezel lobe. The
 * table's own icons are app_icons_64 glyphs — 64 px against a 26 px arc — so
 * they cannot be reused here. NULL means "draw nothing". */
static const char *app_symbol(int app) {
    switch (app) {
        case APP_CONTROL: return LV_SYMBOL_SETTINGS;
        case APP_MUSIC:   return LV_SYMBOL_AUDIO;
        case APP_DAYS:    return LV_SYMBOL_BELL;
        case APP_POMO:    return LV_SYMBOL_LOOP;
        case APP_PET:     return LV_SYMBOL_HOME;
        default:          return NULL;
    }
}

static void tile_cb(lv_event_t *e) {
    app_request((int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e)));
}

/* The scroll hint: a faint pulsing chevron under the grid. Four tiles fill
 * the viewport exactly, so nothing about the resting drawer says "there is
 * more" — the hint is that sentence. It hides once the user has scrolled,
 * because at that point they know, and an arrow pointing down from the last
 * row would be a lie. */
static void drawer_hint_opa_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa(obj, (lv_opa_t)v, 0);
}

static void drawer_scroll_cb(lv_event_t *e) {
    lv_obj_t *hint = lv_event_get_user_data(e);
    bool scrolled = lv_obj_get_scroll_y(lv_event_get_target(e)) > 24;
    if (scrolled == lv_obj_has_flag(hint, LV_OBJ_FLAG_HIDDEN)) return;
    if (scrolled) {
        lv_anim_delete(hint, drawer_hint_opa_cb);
        lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
    }
}

static void build_drawer(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Two 196 px tiles per row with a 16 px gap fill 408 px of the 480 px
     * panel; the outer tile corners land at (36,36), still inside the
     * display's corner radius, so nothing is clipped. Five apps outgrow the
     * 2x2, so rows past the second scroll into view. The viewport stays
     * 412 px: growing it to show a sliver of row three would lift the top
     * corners to (36,22), which is OUTSIDE the r=110 corner arc — (36,36)
     * is already near the limit. */
    lv_obj_t *grid = lv_obj_create(scr);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 412, 412);
    lv_obj_center(grid);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    /* Tracks pack from the TOP, not the centre. Centring tracks in a
     * container their content overflows makes the content spill both ends:
     * the drawer opened half-scrolled ("row one cut, PIP peeking") and the
     * scroller, whose limits assume top-packed content, sprang every upward
     * drag straight back down. START shows exactly the first four tiles and
     * gives the scroller honest limits. */
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    /* remove_style_all stripped the theme's scrollbar, so give the ACTIVE
     * mode something visible: a thin accent sliver during the drag is the
     * only hint the drawer holds more than four tiles. */
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(grid, lv_color_hex(0x3A4556), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(grid, 140, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(grid, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(grid, 2, LV_PART_SCROLLBAR);
    /* The grid owns a drag now; without this the drag would also reach any
     * screen-level gesture handler (HARDWARE.md pitfall #25). */
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_GESTURE_BUBBLE);

    int shown = 0;
    for (int i = 0; i < APP_COUNT; i++) {
        if (!app_enabled(i)) continue;          /* compiled out of this image */
        lv_color_t accent = lv_color_hex(s_apps[i].color);

        lv_obj_t *tile = lv_button_create(grid);
        lv_obj_set_size(tile, 196, 196);
        lv_obj_set_style_radius(tile, 34, 0);
        /* Flat fill, no gradient, no shadow. The drawer scrolls now, and a
         * scroll repaints every visible tile per frame: an 18 px shadow is a
         * blur re-run per tile per frame (the known 20 fps route), and a
         * vertical gradient both fills slower than flat and bands on RGB565.
         * The accent glow the shadow used to give survives in the border. */
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x10151F), 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_set_style_border_color(tile, accent, 0);
        lv_obj_set_style_border_opa(tile, 150, 0);
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
        /* the pet's tile answers to its given name, not its factory one */
        lv_label_set_text(l, (i == APP_PET && s_pet.name[0]) ? s_pet.name
                                                             : s_apps[i].name);
        lv_obj_align(l, LV_ALIGN_CENTER, 0, 46);
        shown++;
    }

    if (shown > 4) {
        lv_obj_t *hint = lv_label_create(scr);
        lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(hint, lv_color_hex(0x7A8BA5), 0);
        lv_label_set_text(hint, LV_SYMBOL_DOWN);
        /* In the strip below the grid: grid bottom is y446, panel edge 480.
         * Centred, so the corner arcs are nowhere near it. */
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -6);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, hint);
        lv_anim_set_exec_cb(&a, drawer_hint_opa_cb);
        lv_anim_set_values(&a, 30, 150);
        lv_anim_set_duration(&a, 900);
        lv_anim_set_playback_duration(&a, 900);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_start(&a);
        lv_obj_add_event_cb(grid, drawer_scroll_cb, LV_EVENT_SCROLL, hint);
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

/* A pool, not a single file.  One slot is selected per boot and kept hot in
 * LVGL's decoded-image cache.  Picking a different compressed 480x480 PNG on
 * every lock made home -> lock spend 700-950 ms on decode behind a black panel;
 * changing the picture at boot preserves variety without putting that cost in
 * a gesture.
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

/* Built-in fallback wallpaper: neon-lit palms on black (Andre Tan / Unsplash),
 * 480x480, denoised and palette-quantised to 64 colours offline so it embeds
 * at ~75 KB. It exists for the states where the pool has nothing to offer —
 * no card, a fresh card, or every cached file failing validation — which used
 * to mean a bare black lock screen. Served straight from flash as a C-array
 * PNG: LodePNG's LV_IMAGE_SRC_VARIABLE path reads the dimensions out of the
 * PNG bytes itself, so only data/data_size need filling, and the dsc must be
 * a stable static because the decoded bitmap is cached keyed on its address. */
#define WALL_DEFAULT_CREDIT "Andre Tan"
extern const uint8_t wall_default_start[] asm("_binary_wall_default_png_start");
extern const uint8_t wall_default_end[]   asm("_binary_wall_default_png_end");
static lv_image_dsc_t s_wall_default_dsc;

static const void *wall_default_src(void) {
    if (!s_wall_default_dsc.data) {
        /* EMBED_FILES emits no .align directive (verified in the generated
         * .S — this build landed the blob at an odd address), and LodePNG
         * reads the PNG dimensions through a uint32_t cast of the data
         * pointer. The LX7 happens to tolerate unaligned loads, but the bytes
         * are staged once into an aligned PSRAM copy rather than leaning on
         * that; the raw flash pointer stays as the fallback if PSRAM is
         * somehow exhausted. */
        size_t n = (size_t)(wall_default_end - wall_default_start);
        uint8_t *buf = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf) memcpy(buf, wall_default_start, n);
        s_wall_default_dsc.header.cf = LV_COLOR_FORMAT_RAW;
        s_wall_default_dsc.data      = buf ? buf : wall_default_start;
        s_wall_default_dsc.data_size = (uint32_t)n;
    }
    return &s_wall_default_dsc;
}

/* Mean luminance (0-255) of a decodable image source, sampled on an 8 px grid.
 * Goes through the ordinary decoder, so on a primed slot it is a cache hit
 * costing microseconds, and on a miss it warms the exact cache entry the
 * upcoming draw would have filled anyway — measuring is not an extra decode.
 * Returns -1 when nothing decodes. Caller holds the LVGL lock. */
static int wall_src_lum(const void *src) {
    lv_image_decoder_dsc_t dsc;
    if (lv_image_decoder_open(&dsc, src, NULL) != LV_RESULT_OK) return -1;
    const lv_draw_buf_t *b = dsc.decoded;
    if (!b || !b->data) { lv_image_decoder_close(&dsc); return -1; }
    uint32_t bpp = lv_color_format_get_size(b->header.cf);
    uint64_t sum = 0;
    uint32_t n = 0;
    for (uint32_t y = 0; y < b->header.h; y += 8) {
        const uint8_t *row = b->data + (size_t)y * b->header.stride;
        for (uint32_t x = 0; x < b->header.w; x += 8, n++) {
            const uint8_t *p = row + (size_t)x * bpp;
            if (bpp == 2) {                       /* RGB565, little-endian */
                uint16_t c = (uint16_t)(p[0] | (p[1] << 8));
                sum += (((c >> 11) & 0x1F) * 527 + 23) >> 6;
                sum += (((c >> 5) & 0x3F) * 259 + 33) >> 6;
                sum += ((c & 0x1F) * 527 + 23) >> 6;
            } else {                              /* 3- or 4-byte true colour */
                sum += p[0] + p[1] + p[2];
            }
        }
    }
    lv_image_decoder_close(&dsc);
    return n ? (int)(sum / ((uint64_t)n * 3)) : -1;
}

/* A cached wallpaper is only usable if it really is a PNG. Earlier builds
 * saved a progressive JPEG under a .png name, and a stale bad file would
 * otherwise never be replaced — the fetch only runs when the file is missing.
 *
 * Both ends are checked, and the tail matters more than the head: a download
 * cut off mid-body has a perfect 8-byte signature and a missing tail, and with
 * only the head checked such a file passed as usable forever — the "stale
 * partial wallpaper" bug. Every PNG ends with the fixed 12-byte IEND chunk, so
 * the last 8 bytes ("IEND" + its constant CRC) are as cheap to verify as the
 * signature and prove the writer reached the end of the stream. */
static bool wallpaper_valid(const char *path) {
    static const uint8_t sig[8]  = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
    static const uint8_t iend[8] = { 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82 };
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    uint8_t hdr[8] = {0}, tail[8] = {0};
    bool ok = fread(hdr, 1, sizeof(hdr), f) == sizeof(hdr) &&
              memcmp(hdr, sig, sizeof(sig)) == 0 &&
              fseek(f, -8, SEEK_END) == 0 && ftell(f) >= 8 &&
              fread(tail, 1, sizeof(tail), f) == sizeof(tail) &&
              memcmp(tail, iend, sizeof(iend)) == 0;
    fclose(f);
    return ok;
}

static void wall_scan(void) {
    s_wall_have = 0;
    if (!s_sd_ok) return;
    for (int i = 0; i < WALL_SLOTS; i++) {
        char p[64];
        wall_png(i, p, sizeof(p));
        if (wallpaper_valid(p)) s_wall_have |= (uint16_t)(1u << i);
        else remove(p);                  /* truncated or wrong format */

        /* A reset mid-download leaves the streaming target behind; the normal
         * failure path already removes it. */
        char part[80];
        snprintf(part, sizeof(part), "%s.part", p);
        remove(part);
    }
    remove(UNSPLASH_JSON ".part");
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
    if (empty >= 0) return empty;

    /* Do not replace either the visible wallpaper or the decoded next one. */
    for (int tries = 0; tries < 8; tries++) {
        int pick = (int)(esp_random() % WALL_SLOTS);
        if ((pick != s_wall_slot && pick != s_wall_primed) || WALL_SLOTS == 1) {
            return pick;
        }
    }
    for (int pick = 0; pick < WALL_SLOTS; pick++) {
        if (pick != s_wall_slot && pick != s_wall_primed) return pick;
    }
    return 0;
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
    for (int pick = 0; pick < WALL_SLOTS; pick++) {
        if ((s_wall_have & (1u << pick)) &&
            (pick != s_wall_slot || have == 1)) return pick;
    }
    return -1;
}

/* Pay the one unavoidable PNG decode during boot, before input polling starts,
 * rather than after the user presses Lock.  Closing releases our reference but
 * leaves the decoded buffer in LVGL's LRU cache. */
static void wall_cache_prime(void) {
    if (!s_sd_ok || !lv_display_get_default()) return;
    if (s_wall_primed >= 0 && (s_wall_have & (1u << s_wall_primed))) return;

    int slot = wall_display_slot();
    if (slot < 0) return;

    char lvpath[72];
    wall_lv(slot, lvpath, sizeof(lvpath));
    int64_t t0 = esp_timer_get_time();
    if (!ui_lock()) return;

    lv_image_decoder_dsc_t dsc;
    lv_result_t res = lv_image_decoder_open(&dsc, lvpath, NULL);
    if (res == LV_RESULT_OK) lv_image_decoder_close(&dsc);
    bsp_display_unlock();

    if (res == LV_RESULT_OK) {
        s_wall_primed = slot;
        ESP_LOGI(TAG, "wallpaper slot %d primed in %lld ms", slot,
                 (long long)((esp_timer_get_time() - t0) / 1000));
    } else {
        ESP_LOGW(TAG, "wallpaper slot %d failed to prime", slot);
    }
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

/* One long-lived connection to the broker, shared by the cover fetch and the
 * queue fetch — they are the same host, and esp_http_client_set_url() only closes
 * the socket when host or port changes, never for a path (HARDWARE.md §7f).
 *
 * This matters more than it looks. Every fetch used to build a fresh client, so
 * each cover and each queue refresh paid a full TLS handshake: **390 ms before a
 * single byte of image**, against 6 ms on a warm connection. Filling three
 * lookahead slots was over a second of pure handshake.
 *
 * Confined to the Spotify task, because the handle carries no lock and every
 * field is mutated in place. */
static esp_http_client_handle_t s_brk_http;

static bool broker_fetch(const char *path, const char *xname, const char *xval,
                         uint8_t *buf, size_t cap, size_t *out_len) {
    s_dl_total = 0; s_dl_got = 0; s_dl_pct = 0; s_dl_kb = 0; s_dl_accent = 0;
    s_brk_status = 0;
    s_dl_mem = buf; s_dl_mem_cap = cap; s_dl_mem_len = 0;

    if (!s_brk_http) {
        char base[192];
        snprintf(base, sizeof(base), "%s%s", BROKER_URL, path);
        esp_http_client_config_t cfg = {
            .url = base,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 12000,
            .event_handler = dl_evt,
            .keep_alive_enable = true,
            .max_redirection_count = 5,
            /* The queue request carries the Spotify access token as a header, and
             * that alone is ~300 chars — past the 512-byte default the client uses
             * to assemble a request. It logged "Buffer length is small to fit all
             * the headers" on every queue fetch. */
            .buffer_size_tx = 1024,
        };
        s_brk_http = esp_http_client_init(&cfg);
        if (!s_brk_http) { s_dl_mem = NULL; return false; }
    } else {
        esp_http_client_set_url(s_brk_http, path);   /* path only: socket survives */
    }

    char auth[96];
    snprintf(auth, sizeof(auth), "Bearer %.72s", BROKER_TOKEN);
    esp_http_client_set_header(s_brk_http, "Authorization", auth);
    esp_http_client_set_method(s_brk_http, HTTP_METHOD_GET);
    if (xname && xval && xval[0]) esp_http_client_set_header(s_brk_http, xname, xval);
    else esp_http_client_delete_header(s_brk_http, "X-Spotify-Token");

    esp_err_t err = esp_http_client_perform(s_brk_http);
    int status = esp_http_client_get_status_code(s_brk_http);
    s_brk_status = status;
    bool ok = (err == ESP_OK && status == 200);

    /* A dead socket is never retried into — perform() will not redial on its own,
     * so a transport failure has to close it or every later fetch fails too. */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "broker %.24s: %s (HTTP %d, %u B) — redialling",
                 path, esp_err_to_name(err), status, (unsigned)s_dl_mem_len);
        esp_http_client_close(s_brk_http);
    }

    *out_len = s_dl_mem_len;
    s_dl_mem = NULL; s_dl_mem_cap = s_dl_mem_len = 0;
    return ok;
}

static bool asset_fetch_auth(const char *url, const char *path, const char *bearer) {
    if (!s_sd_ok) return false;

    char tmp[80];
    snprintf(tmp, sizeof(tmp), "%s.part", path);

    bool ok = false;
    /* A truncated body is a link event rather than a server one (HARDWARE.md #26):
     * the far end has not given up, so one immediate retry is cheap and usually
     * lands. Log the byte count with the RSSI so the next one explains itself. */
    for (int attempt = 0; attempt < 2 && !ok; attempt++) {
        s_dl_total = 0;
        s_dl_got = 0;
        s_dl_pct = 0;
        s_dl_kb = 0;
        s_dl_accent = 0;

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
        if (c) {
            if (bearer && bearer[0]) {
                char auth[96];
                snprintf(auth, sizeof(auth), "Bearer %.72s", bearer);
                esp_http_client_set_header(c, "Authorization", auth);
            }
            esp_err_t err = esp_http_client_perform(c);
            int status = esp_http_client_get_status_code(c);
            /* perform() can return ESP_OK on a body that stopped short of the
             * advertised Content-Length (a clean FIN mid-transfer). Renaming
             * that file into place is how a partial image used to enter the
             * pool as a permanent citizen, so the byte count is part of
             * "success", not a nice-to-have. No length advertised = trusted,
             * as before. */
            ok = (err == ESP_OK && status == 200 &&
                  (s_dl_total <= 0 || s_dl_got == s_dl_total));
            ESP_LOGI(TAG, "asset: HTTP %d, heap %u", status,
                     (unsigned)hp_free());
            esp_http_client_cleanup(c);
        }
        fclose(s_dl_file);
        s_dl_file = NULL;

        if (!ok) {
            remove(tmp);
            ESP_LOGW(TAG, "asset: %lld/%lld B, rssi %d%s",
                     (long long)s_dl_got, (long long)s_dl_total, wifi_rssi(),
                     attempt == 0 ? " - retrying once" : " - giving up");
        }
    }

    if (ok) {
        remove(path);
        ok = (rename(tmp, path) == 0);
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
            /* Both ends of the file, same test the boot scan applies. Catches
             * what the length check cannot: a server that never advertised a
             * Content-Length, or bytes that were never a PNG at all. A slot
             * must not be marked usable on the transport's word alone. */
            if (ok && !wallpaper_valid(dest)) {
                ESP_LOGW(TAG, "slot %d: downloaded file failed PNG validation",
                         slot);
                remove(dest);
                ok = false;
            }
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

/* Bumped to 3 when the charge-care columns landed. The header is only written
 * for a file that does not exist yet, so adding a column to the old name would
 * have left rows that no longer line up with the header above them. */
#define TELEMETRY_PATH   BSP_SD_MOUNT_POINT "/logs/pwrlog3.csv"
#define TELEMETRY_PERIOD 60000

/* One-shot pool audit: decode every slot and log its mean luminance. "Black
 * wallpaper" has indistinguishable causes from the glass (missing file, decoder
 * rejection, or a genuinely dark photo under the lock screen's dim); this
 * names which slot is which in one boot. Costs ~600 ms per slot, so it stays 0
 * in commits — same contract as CFG_PERF_SCROLL_SELFTEST. */
#define CFG_WALL_POOL_AUDIT 0

#if CFG_WALL_POOL_AUDIT
static void wall_pool_audit(void) {
    for (int i = 0; i < WALL_SLOTS; i++) {
        if (!(s_wall_have & (1u << i))) {
            ESP_LOGW(TAG, "audit w%d: no file", i);
            continue;
        }
        char lvpath[72];
        wall_lv(i, lvpath, sizeof(lvpath));
        if (!ui_lock()) return;
        int lum = wall_src_lum(lvpath);
        bsp_display_unlock();
        if (lum < 0) ESP_LOGW(TAG, "audit w%d: DECODE FAILED", i);
        else ESP_LOGI(TAG, "audit w%d: mean_lum=%d/255%s", i, lum,
                      lum < 40 ? "  <- NEAR BLACK" : "");
    }
}
#endif

static void sd_init(void) {
    esp_err_t err = bsp_sdcard_mount();
    if (err == ESP_OK) {
        s_sd_ok = true;
        ESP_LOGI(TAG, "microSD mounted at %s", BSP_SD_MOUNT_POINT);
        store_init_dirs();
        remove(WALLPAPER_OLD);          /* pre-pool single wallpaper */
        wall_scan();
#if CFG_WALL_POOL_AUDIT
        wall_pool_audit();
#endif
        wall_cache_prime();
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
    ESP_LOGW(TAG, "=== BEGIN pwrlog ===");
    char line[224];
    int n = 0;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = '\0';
        printf("CSV %s\n", line);
        n++;
        if ((n % 40) == 0) vTaskDelay(pdMS_TO_TICKS(20));  /* let USB-CDC drain */
    }
    fclose(f);
    ESP_LOGW(TAG, "=== END pwrlog (%d lines) ===", n);
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
        fprintf(f, "build,clock,uptime_s,event,batt_mv,batt_pct,charging,vbus,"
                   "chg_state,cap_mv,doze,screen,"
                   "wifi,app,heap_free,heap_min,heap_largest,fps\n");
    }

    char clock[16] = "";
    time_t tnow;
    struct tm ti;
    time(&tnow);
    localtime_r(&tnow, &ti);
    if (ti.tm_year >= (2024 - 1900)) strftime(clock, sizeof(clock), "%H:%M:%S", &ti);

    fprintf(f, "%s,%s,%lld,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%u,%u,%u.%u\n",
            build_id(), clock, (long long)(now_ms() / 1000), event,
            s_batt_mv, s_batt_pct, s_batt_charging ? 1 : 0,
            s_vbus ? 1 : 0, s_chg_state, chg_cv_mv(chg_cv_code()),
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
 * The wallpaper, clock, date and battery gauge are the permanent visual core.
 * When Spotify is active, the album art and complete transport remain available
 * in a fixed card below them. Desk-clock mode colours the base ring amber.
 */

/* Idempotent, and it has to be: lwIP asserts outright if the operating mode is
 * set while the client is already running, which is a panic and a reboot.
 *
 * The caller's gate is `!s_time_synced` (the GOT_IP handler), and that is NOT
 * the same question. A Wi-Fi provisioning session tears the radio down for as
 * long as the pairing takes, so SNTP can easily be started at boot and still
 * not have synced 30 s later when the rejoin fires a second GOT_IP. That is the
 * exact sequence that rebooted the cube every time a network was changed from
 * the phone: started at 4.4 s, never synced, re-initialised at 37.7 s, assert.
 *
 * Stop rather than skip: the netif is destroyed and rebuilt across a session,
 * so the running client is bound to something that no longer exists. */
static bool s_sntp_up;

static void time_sync_start(void) {
    setenv("TZ", TIMEZONE, 1);
    tzset();
    if (s_sntp_up) {
        esp_sntp_stop();
        s_sntp_up = false;
        ESP_LOGI(TAG, "SNTP stopped before re-init (netif was replaced)");
    }
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, NTP_SERVER);
    esp_sntp_init();
    s_sntp_up = true;
    ESP_LOGI(TAG, "SNTP started (%s, TZ=%s)", NTP_SERVER, TIMEZONE);
}

static int s_lock_slow;

/* Called directly when the screen is built so the readouts are correct on the
 * very first frame — a persistent "slow" counter used to make a freshly rebuilt
 * lock screen skip its first update, which is how the placeholder text kept
 * reappearing. */
/* Driven from lock_refresh() at 1 Hz rather than from a timer of its own. */
static void lock_clock_layout(void) {
    if (!s_lock_time) return;
    int y = s_lock_np_up ? -140 : -18;
    /* Centre the visually dominant digits. AM/PM is an annotation, not part of
     * the centring box; including it made the clock itself look left-shifted. */
    lv_obj_align(s_lock_time, LV_ALIGN_CENTER, 0, y);
    if (s_lock_meridiem && !s_clock_24) {
        lv_obj_align_to(s_lock_meridiem, s_lock_time, LV_ALIGN_OUT_RIGHT_MID, 8, 2);
    }
}

static uint8_t lock_snapshot_state(void) {
    bool art_ok = s_sp_art_ready && s_sp_art_id[0] &&
                  strcmp(s_sp_art_id, s_sp_track_id) == 0;
    return (s_sp_playing ? 1u : 0u) |
           (art_ok ? 2u : 0u) |
           (s_sp_no_prev ? 4u : 0u) |
           (s_sp_no_next ? 8u : 0u);
}

static void lock_snapshot_clear(void) {
    if (s_lock_np_drag_img) {
        lv_obj_add_flag(s_lock_np_drag_img, LV_OBJ_FLAG_HIDDEN);
        lv_image_set_src(s_lock_np_drag_img, NULL);
    }
    if (s_lock_np_snapshot) {
        lv_draw_buf_destroy(s_lock_np_snapshot);
        s_lock_np_snapshot = NULL;
    }
    s_lock_np_snapshot_track[0] = '\0';
    s_lock_np_snapshot_state = 0;
}

/* LVGL can render snapshots into ARGB8888, but its RGB565A8 snapshot target is
 * intentionally disabled. Convert once after capture instead: the resulting
 * image keeps the rounded/translucent alpha plane while cutting source traffic
 * by 25% and using the renderer's dedicated RGB565 + A8 blend path. */
static lv_draw_buf_t *lock_snapshot_compact(lv_draw_buf_t *src) {
    if (!src) return NULL;
    uint32_t w = src->header.w;
    uint32_t h = src->header.h;
    lv_draw_buf_t *dst = lv_draw_buf_create(w, h, LV_COLOR_FORMAT_RGB565A8,
                                            LV_STRIDE_AUTO);
    if (!dst) return src;                    /* ARGB fallback is still functional */

    uint8_t *alpha = dst->data + dst->header.stride * h;
    uint32_t alpha_stride = dst->header.stride / 2;
    for (uint32_t y = 0; y < h; y++) {
        const lv_color32_t *s = (const lv_color32_t *)(src->data +
                                                       src->header.stride * y);
        uint16_t *d = (uint16_t *)(dst->data + dst->header.stride * y);
        uint8_t *a = alpha + alpha_stride * y;
        for (uint32_t x = 0; x < w; x++) {
            d[x] = (uint16_t)(((uint16_t)(s[x].red   >> 3) << 11) |
                              ((uint16_t)(s[x].green >> 2) << 5)  |
                              ((uint16_t)(s[x].blue  >> 3)));
            a[x] = s[x].alpha;
        }
    }
    lv_draw_buf_destroy(src);
    return dst;
}

/* Flatten the expensive widget subtree once. During a drag LVGL then has one
 * alpha bitmap to composite, rather than re-rendering the rounded card, decoded
 * album art, text and three large controls for every pointer sample. */
static bool lock_snapshot_capture(void) {
    if (!s_lock_np || !s_lock_np_drag_img || !s_lock_np_up) return false;

    uint8_t state = lock_snapshot_state();
    if (s_lock_np_snapshot && state == s_lock_np_snapshot_state &&
        strcmp(s_lock_np_snapshot_track, s_sp_track_id) == 0) {
        return true;
    }

    int64_t started = now_ms();
    lock_snapshot_clear();
    lv_obj_update_layout(s_lock_np);
    lv_draw_buf_t *argb = lv_snapshot_take(s_lock_np, LV_COLOR_FORMAT_ARGB8888);
    if (!argb) {
        ESP_LOGE(TAG, "lock: card snapshot allocation/render failed");
        return false;
    }
    s_lock_np_snapshot = lock_snapshot_compact(argb);
    lv_image_set_src(s_lock_np_drag_img, s_lock_np_snapshot);
    snprintf(s_lock_np_snapshot_track, sizeof(s_lock_np_snapshot_track), "%s",
             s_sp_track_id);
    s_lock_np_snapshot_state = state;
    ESP_LOGI(TAG, "lock: card snapshot ready in %lld ms (%u bytes PSRAM free)",
             (long long)(now_ms() - started),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

static void lock_np_refresh(void) {
    if (!s_lock_np) return;

    bool playing = s_sp_have_state && s_sp_track[0];

    bool show = playing && !s_lock_np_off;
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
        lock_clock_layout();
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
    bool art_ok = s_sp_art_ready && s_sp_art_id[0] &&
                  strcmp(s_sp_art_id, s_sp_track_id) == 0;
    if (art_ok && lv_obj_has_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN)) {
        lv_image_set_src(s_lock_np_art, &s_sp_art_dsc);
        lv_obj_remove_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN);
    } else if (!art_ok && !lv_obj_has_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_lock_np_art, LV_OBJ_FLAG_HIDDEN);
    }

    if (!s_lock_pointer_down && !s_lock_drag_active) lock_snapshot_capture();
}

static void lock_refresh(void) {
    if (!s_lock_time) return;
    lock_np_refresh();

    time_t now;
    struct tm ti;
    time(&now);
    localtime_r(&now, &ti);

    if (ti.tm_year < (2024 - 1900)) {
        bool clock_changed = label_set_changed(s_lock_time, "00:00");
        label_set_changed(s_lock_date, "");
        obj_set_hidden_changed(s_lock_meridiem, true);
        if (clock_changed) lock_clock_layout();
    } else {
        char hm[8], ap[4], date[40];
        strftime(hm, sizeof(hm), s_clock_24 ? "%H:%M" : "%I:%M", &ti);
        if (!s_clock_24 && hm[0] == '0') memmove(hm, hm + 1, strlen(hm));
        strftime(date, sizeof(date), "%a %d %b", &ti);
        for (char *p = date; *p; p++) *p = toupper((unsigned char)*p);
        bool clock_changed = label_set_changed(s_lock_time, hm);
        label_set_changed(s_lock_date, date);
        if (s_clock_24) {
            obj_set_hidden_changed(s_lock_meridiem, true);
        } else {
            strftime(ap, sizeof(ap), "%p", &ti);
            label_set_changed(s_lock_meridiem, ap);
            obj_set_hidden_changed(s_lock_meridiem, false);
        }
        if (clock_changed) lock_clock_layout();
        s_time_synced = true;
    }

    int pct = s_batt_pct;
    int arc_end;
    lv_color_t arc_color;
    if (pct < 0) {
        label_set_changed(s_lock_batt, "EXT PWR");
        arc_end = 360;
        arc_color = lv_color_hex(s_batt_charging ? 0x22C55E : 0x22D3EE);
    } else {
        label_set_fmt_changed(s_lock_batt, "%d%%", pct);
        /* A real 360-degree gauge: at 100% the ring is actually complete.
         * The previous 250-degree maximum looked like a partially charged
         * battery even while the label said 100%. */
        arc_end = (360 * pct) / 100;
        arc_color = lv_color_hex(s_batt_charging ? 0x22C55E
                                  : (pct >= 50 ? 0x22D3EE
                                     : (pct >= 20 ? 0xF59E0B : 0xEF4444)));
    }
    /* The bolt means "on the charger", not "current is flowing". With a charge
     * cap in force the cube reaches its target and stops hours before it leaves
     * the dock, and a bolt that vanishes there reads as the dock having failed.
     * So it shows on VBUS and breathes only while current actually flows —
     * steady bolt is the resting state of a docked, capped cube. */
    static bool was_charging;
    bool charge_was_hidden = s_lock_charge &&
                             lv_obj_has_flag(s_lock_charge, LV_OBJ_FLAG_HIDDEN);
    obj_set_hidden_changed(s_lock_charge, !s_vbus);
    if (!s_batt_charging) {
        s_lock_charge_phase = s_lock_charge_div = 0;
        if (s_lock_charge &&
            lv_obj_get_style_text_opa(s_lock_charge, 0) != LV_OPA_COVER) {
            lv_obj_set_style_text_opa(s_lock_charge, LV_OPA_COVER, 0);
        }
    } else if (charge_was_hidden || !was_charging) {
        /* Also seed the fade when charging starts under an already-visible
         * bolt — plugging in now reveals it before any current flows. */
        s_lock_charge_phase = s_lock_charge_div = 0;
        lv_obj_set_style_text_opa(s_lock_charge, 96, 0);
    }
    was_charging = s_batt_charging;

    /* The countdown is hidden, not blanked, in every case the estimator has no
     * answer for: the first minutes of a charge before a rate exists, a cube
     * already past its limit, and a docked cube the cap has stopped. An empty
     * label would still hold its line and open a gap under the percentage. */
    int eta = s_chg_eta_mins;
    if (eta > 0) {
        char eta_buf[32];
        chg_eta_text(eta_buf, sizeof eta_buf, eta);
        for (char *p = eta_buf; *p; p++) *p = toupper((unsigned char)*p);
        label_set_changed(s_lock_eta, eta_buf);
    }
    /* Suppressed while the now-playing panel is up, and that is a space fact
     * rather than a preference: lock_clock_layout() raises the clock from
     * CENTER-18 to CENTER-140 to make room for the card, which puts the
     * hud_clock_48 digits at roughly y70..130. The percentage already occupies
     * y42..64, so the band this caption needs no longer exists — it drew
     * straight through the clock. The transport card wins the same way the
     * clock yields to it, and the countdown is still on the CONTROL card. */
    obj_set_hidden_changed(s_lock_eta, eta <= 0 || s_lock_np_up || !s_chg_eta_on);

    if (lv_arc_get_bg_angle_start(s_lock_batt_arc) != 0 ||
        lv_arc_get_bg_angle_end(s_lock_batt_arc) != arc_end) {
        lv_arc_set_bg_angles(s_lock_batt_arc, 0, arc_end);
    }
    if (!lv_color_eq(lv_obj_get_style_arc_color(s_lock_batt_arc, LV_PART_MAIN),
                     arc_color)) {
        lv_obj_set_style_arc_color(s_lock_batt_arc, arc_color, LV_PART_MAIN);
    }

    /* The desk-clock cue is its own amber ring at 394 px, inside the 412 px
     * base and the 430 px battery gauge (444 px clipped at the corners; one
     * shared path hid the cue at 100%). It shows only when the mode is on
     * AND the rings are drawn — with rings off the mode still runs, it just
     * has no jewellery. */
    obj_set_hidden_changed(s_lock_ring, !s_lock_rings);
    obj_set_hidden_changed(s_lock_ao_ring,
                           !(s_lock_rings && s_ao_ring_pref && s_always_on));
    obj_set_hidden_changed(s_lock_inner_ring, !s_lock_rings);
    obj_set_hidden_changed(s_lock_batt_arc, !s_lock_rings);
}

/* The live card subtree measured 130-140 ms per translated frame because LVGL
 * rebuilt its rounded scrim, art, text and controls on every pointer sample.
 * During a swipe we move the flattened snapshot instead. It is still the whole
 * card under the finger, but the renderer has one prepared pixel layer to draw. */
#define LOCK_SWIPE_THRESHOLD 72
#define LOCK_SWIPE_OFFSCREEN 460

static void lock_drag_apply(int distance) {
    if (!s_lock_np_drag_img) return;
    distance = clampi(distance, 0, LOCK_SWIPE_OFFSCREEN);
    s_lock_drag_x = (int16_t)distance;
    int x = s_lock_drag_origin_x - distance;
    if (lv_obj_get_x(s_lock_np_drag_img) != x) lv_obj_set_x(s_lock_np_drag_img, x);
}

static void lock_drag_anim_exec(void *obj, int32_t distance) {
    if ((lv_obj_t *)obj == s_lock_np_drag_img) lock_drag_apply((int)distance);
}

static void lock_drag_restore(lv_anim_t *a) {
    (void)a;
    if (s_lock_np_drag_img) lv_obj_add_flag(s_lock_np_drag_img, LV_OBJ_FLAG_HIDDEN);
    if (s_lock_np && s_lock_np_up && !s_lock_np_off) {
        lv_obj_remove_flag(s_lock_np, LV_OBJ_FLAG_HIDDEN);
    }
    s_lock_drag_active = false;
    s_lock_drag_x = 0;
}

static void lock_drag_dismissed(lv_anim_t *a) {
    (void)a;
    s_lock_np_off = true;
    s_lock_drag_active = false;
    s_lock_drag_x = 0;
    lock_np_refresh();
    lock_snapshot_clear();
    ESP_LOGI(TAG, "lock: now-playing dismissed by swipe");
}

static bool lock_drag_begin(void) {
    if (!lock_snapshot_capture() || !s_lock_np_drag_img || !s_lock_np) return false;

    lv_obj_update_layout(s_lock_np);
    s_lock_drag_origin_x = lv_obj_get_x(s_lock_np);
    lv_obj_set_pos(s_lock_np_drag_img, s_lock_drag_origin_x, lv_obj_get_y(s_lock_np));
    lv_obj_move_foreground(s_lock_np_drag_img);
    lv_obj_remove_flag(s_lock_np_drag_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_lock_np, LV_OBJ_FLAG_HIDDEN);
    s_lock_drag_active = true;
    lock_drag_apply(0);
    return true;
}

static void lock_drag_finish(void) {
    if (!s_lock_np || !s_lock_drag_active || !s_lock_np_drag_img) return;

    bool dismiss = s_lock_drag_x >= LOCK_SWIPE_THRESHOLD;
    ESP_LOGI(TAG, "lock: right-to-left swipe release distance=%d %s",
             s_lock_drag_x, dismiss ? "dismiss" : "snap-back");

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lock_np_drag_img);
    lv_anim_set_exec_cb(&a, lock_drag_anim_exec);
    lv_anim_set_values(&a, s_lock_drag_x,
                       dismiss ? LOCK_SWIPE_OFFSCREEN : 0);
    lv_anim_set_duration(&a, dismiss ? 170 : 160);
    lv_anim_set_path_cb(&a, dismiss ? lv_anim_path_ease_in
                                     : lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, dismiss ? lock_drag_dismissed
                                         : lock_drag_restore);
    if (dismiss) s_lock_swipe_at = now_ms();
    if (!dismiss && s_lock_drag_x == 0) lock_drag_restore(NULL);
    else lv_anim_start(&a);
}

/* Sample the pointer from the 60 Hz lock timer instead of relying on a gesture
 * event.  Gesture events arrive only after the threshold has already been
 * crossed, which made the panel feel dead until it suddenly disappeared. */
static void lock_drag_poll(void) {
    lv_indev_t *pointer = NULL;
    for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
        if (lv_indev_get_type(in) == LV_INDEV_TYPE_POINTER) {
            pointer = in;
            break;
        }
    }
    if (!pointer) return;

    bool down = lv_indev_get_state(pointer) == LV_INDEV_STATE_PRESSED;
    lv_point_t pt;
    lv_indev_get_point(pointer, &pt);

    if (down && !s_lock_pointer_down) {
        s_lock_pointer_down = true;
        s_lock_dragging = false;
        s_lock_drag_moved = false;

        lv_area_t card;
        bool on_card = false;
        if (s_lock_np) {
            lv_obj_get_coords(s_lock_np, &card);
            on_card = pt.x >= card.x1 && pt.x <= card.x2 &&
                      pt.y >= card.y1 && pt.y <= card.y2;
        }
        if (s_lock_np_up && s_lock_np && on_card && !s_lock_np_off &&
            !s_lock_drag_active) {
            s_lock_dragging = true;
            s_lock_drag_start_x = pt.x;
            s_lock_drag_start_y = pt.y;
            s_lock_drag_x = 0;
        }
    }

    if (down && s_lock_dragging) {
        /* Only a horizontally dominant right-to-left motion commits the drag.
         * A vertical or rightward start is cancelled, so touching elsewhere or
         * scrolling a finger around the panel cannot move the card. */
        int distance = s_lock_drag_start_x - pt.x;
        int vertical = abs(pt.y - s_lock_drag_start_y);
        if (!s_lock_drag_active) {
            if ((distance < -12) || (vertical > 12 && vertical > distance)) {
                s_lock_dragging = false;
            } else if (distance >= 4 && distance >= vertical) {
                if (!lock_drag_begin()) s_lock_dragging = false;
            }
        }
        if (s_lock_drag_active) {
            if (distance < 0) distance = 0;
            if (distance > LOCK_SWIPE_OFFSCREEN) distance = LOCK_SWIPE_OFFSCREEN;
            lock_drag_apply(distance);
        }
        if (s_lock_drag_active && distance >= 8) {
            s_lock_drag_moved = true;
            s_lock_swipe_at = now_ms();    /* suppress the release's CLICKED */
        }
    }

    if (!down && s_lock_pointer_down) {
        s_lock_pointer_down = false;
        if (s_lock_drag_active) lock_drag_finish();
        s_lock_dragging = false;
        s_lock_drag_moved = false;
    }
}

static void lock_timer_cb(lv_timer_t *t) {
    if (!s_lock_time || !s_screen_on) return;      /* no flushes while dozing */

    lock_drag_poll();

    /* The charging cue is a restrained two-second pulse on the 20 px bolt only.
     * It makes active charging unmistakable without animating the screen-sized
     * battery ring or invalidating the wallpaper. */
    if (s_batt_charging && s_lock_charge &&
        !lv_obj_has_flag(s_lock_charge, LV_OBJ_FLAG_HIDDEN) &&
        ++s_lock_charge_div >= 4) {
        s_lock_charge_div = 0;
        s_lock_charge_phase = (s_lock_charge_phase + 1) & 31;
        int triangle = s_lock_charge_phase < 16
                     ? s_lock_charge_phase : 31 - s_lock_charge_phase;
        lv_opa_t opa = (lv_opa_t)(96 + triangle * 10);
        if (lv_obj_get_style_text_opa(s_lock_charge, 0) != opa) {
            lv_obj_set_style_text_opa(s_lock_charge, opa, 0);
        }
    }

    if (s_lock_slow++ % 60) return;                /* readouts at ~1 Hz */
    lock_refresh();
}

/* Unlocking is a swipe up; a tap is not a verb here at all.
 *
 * Tap-to-unlock was in conflict with the desk-clock dim the moment that landed.
 * A dimmed panel invites exactly one thing — touch it to see it properly — and
 * that same touch was also the gesture that threw you into the drawer. The two
 * cannot share an input: one is a glance, the other is a decision.
 *
 * So a tap now does nothing except be a touch, and that is enough. LVGL stamps
 * `last_activity_time` on the press (lv_indev.c:268), which is one of the two
 * terms in the main loop's `idle`, so the dim lifts on the next 20 ms pass
 * without this function knowing the dim exists. Restating it here as an
 * explicit un-dim call would be a second writer of a decision the idle
 * expression already owns.
 *
 * The bottom-edge rule that gesture_home_cb applies everywhere else is
 * deliberately NOT applied here. On an app screen the edge is what separates
 * "go home" from an ordinary upward drag inside content; the lock screen has no
 * scrollable content to be confused with, and a locked device you have to swipe
 * from precisely the right 72 px is a device that feels stuck. */
static void lock_gesture_cb(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_TOP) return;
    /* Asleep, this cannot be reached: touch_origin_cb cancels the touch before
     * a gesture can form, so a swipe on a dark panel wakes and stops there —
     * the same rule a tap has always had. */
    if (!s_screen_on) return;
    /* LV_EVENT_GESTURE repeats for as long as the finger is down (pitfall #25),
     * and app_open()'s own 250 ms debounce is not the right guard for a screen
     * this one leaves. */
    lv_indev_wait_release(indev);
    ESP_LOGI(TAG, "lock: swipe up -> home");
    app_request(APP_DRAWER);      /* always home, not "wherever you locked from" */
}

static void lock_tap_cb(lv_event_t *e) {
    /* Asleep? The first touch only wakes — it must not unlock, the same way a
     * phone shows you the lock screen before letting you in. Kept even though
     * touch_origin_cb now raises the same request for every screen: that gate
     * is registered inside an `if (ui_lock())` at boot, and the lock screen is
     * the one place that must not fail open. */
    if (!s_screen_on) {
        s_req_wake = true;
        return;
    }
    /* Deliberately empty otherwise. See lock_gesture_cb above: the tap's whole
     * job is to have happened. */
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
    if (now_ms() - s_lock_swipe_at < 500) return;
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

    /* Test hook for the embedded fallback below: a full pool never exercises
     * it naturally, and a decoder rejection is silent on the glass (pitfall
     * #27), so this forces the no-usable-slot branch for one flashed run.
     * Keep it 0 in commits, same contract as CFG_PERF_SCROLL_SELFTEST. */
#define CFG_WALL_DEFAULT_TEST 0
    /* Consume the prepared next slot. A cache hit keeps the hidden render short,
     * while separating current from primed restores a new image on every lock. */
    int slot = -1;
    if (s_sd_ok && !CFG_WALL_DEFAULT_TEST) {
        if (s_wall_primed >= 0 && (s_wall_have & (1u << s_wall_primed))) {
            slot = s_wall_primed;
        } else {
            slot = wall_display_slot();
        }
        s_wall_primed = -1;
    }
    /* No usable slot — no card, an empty pool, or everything failed the boot
     * validation — falls back to the wallpaper embedded in the app image, so
     * the lock screen is never bare black. Same widget, different source. */
    {
        char lvpath[72];
        const void *src;
        if (slot >= 0) {
            wall_lv(slot, lvpath, sizeof(lvpath));
            src = lvpath;               /* set_src copies the path string */
        } else {
            src = wall_default_src();
        }
        /* The old flat 43% dim (opa 110) kept the clock legible over bright
         * photos — and erased dark ones. A nebula at mean luminance 29/255
         * landed at an effective ~12/255, which from the glass is "black
         * wallpaper, no image", indistinguishable from the missing-file bug it
         * was reported as (an on-device audit found slots at 29, 44 and 51
         * against the pool's space themes, all decoding perfectly). So dim
         * toward a target effective brightness instead: a bright photo keeps
         * the old 110 floor, one already at or below the target draws at full
         * opacity. Measuring costs nothing extra — wall_src_lum() goes through
         * the decoder, so it IS the cache warm-up the draw needed anyway. */
        int lum = wall_src_lum(src);
        uint32_t opa = 110;
        if (lum > 0) {
            opa = 255u * WALL_DIM_TARGET / (uint32_t)lum;
            if (opa > 255) opa = 255;
            if (opa < 110) opa = 110;
        }
        /* Names what is actually behind the clock. "Black wallpaper" has three
         * different causes — missing file, decoder rejection, dark photo — and
         * from the glass they are identical. One line per build separates them
         * without a debug flash. */
        ESP_LOGI(TAG, "lock wallpaper: %s%d, lum %d, opa %u",
                 slot >= 0 ? "slot " : "default, slot ", slot, lum,
                 (unsigned)opa);
        lv_obj_t *wall = lv_image_create(scr);
        /* Also held file-scope: the desk-clock dim hides this to leave genuinely
         * unlit black behind the clock. Its NULL row in app_open()'s teardown
         * is part of that change, not an optional extra. */
        s_lock_wall = wall;
        lv_image_set_src(wall, src);
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
        lv_obj_set_style_image_opa(wall, (lv_opa_t)opa, 0);
        lv_obj_remove_flag(wall, LV_OBJ_FLAG_CLICKABLE);
        s_wall_slot = slot;

        /* attribution for whatever is actually on screen, shown in STATUS */
        s_wall_credit[0] = '\0';
        if (slot >= 0) {
            char cpath[64];
            wall_txt(slot, cpath, sizeof(cpath));
            FILE *cf = fopen(cpath, "r");
            if (cf) {
                if (fgets(s_wall_credit, sizeof(s_wall_credit), cf)) {
                    s_wall_credit[strcspn(s_wall_credit, "\r\n")] = '\0';
                }
                fclose(cf);
            }
        } else {
            snprintf(s_wall_credit, sizeof(s_wall_credit), "%s",
                     WALL_DEFAULT_CREDIT);
        }
    }

    /* Always On gets a separate inner ring so neither the battery gauge nor the
     * panel's rounded-corner mask can cover it. */
    s_lock_ring = lv_arc_create(scr);
    lv_obj_set_size(s_lock_ring, 412, 412);
    lv_obj_center(s_lock_ring);
    lv_obj_remove_style(s_lock_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_lock_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_lock_ring, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_lock_ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_lock_ring, lv_color_hex(0x123A52), LV_PART_MAIN);
    lv_arc_set_bg_angles(s_lock_ring, 0, 360);

    /* The desk-clock cue: its OWN amber ring, present only while the mode is
     * on. It used to be a recolor of the base ring above, which coupled two
     * unrelated settings — rings off force-disabled desk clock "because the
     * indicator would vanish". Decoupled by request: the mode runs with or
     * without rings; the cue simply needs rings to have somewhere to live. */
    s_lock_ao_ring = lv_arc_create(scr);
    lv_obj_set_size(s_lock_ao_ring, 394, 394);
    lv_obj_center(s_lock_ao_ring);
    lv_obj_remove_style(s_lock_ao_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_lock_ao_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_lock_ao_ring, 5, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_lock_ao_ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_lock_ao_ring, lv_color_hex(0xF59E0B), LV_PART_MAIN);
    lv_arc_set_bg_angles(s_lock_ao_ring, 0, 360);
    lv_obj_add_flag(s_lock_ao_ring, LV_OBJ_FLAG_HIDDEN);

    s_lock_inner_ring = lv_arc_create(scr);
    lv_obj_set_size(s_lock_inner_ring, 372, 372);
    lv_obj_center(s_lock_inner_ring);
    lv_obj_remove_style(s_lock_inner_ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_lock_inner_ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_lock_inner_ring, 1, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_lock_inner_ring, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_lock_inner_ring, lv_color_hex(0x0C2438), LV_PART_MAIN);
    lv_arc_set_bg_angles(s_lock_inner_ring, 0, 360);

    s_lock_batt_arc = lv_arc_create(scr);
    lv_obj_set_size(s_lock_batt_arc, 430, 430);
    lv_obj_center(s_lock_batt_arc);
    lv_obj_remove_style(s_lock_batt_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_lock_batt_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_lock_batt_arc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_lock_batt_arc, 0, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_lock_batt_arc, lv_color_hex(0x22D3EE), LV_PART_MAIN);
    lv_arc_set_rotation(s_lock_batt_arc, 270);       /* gauge starts at 12 o'clock */
    lv_arc_set_bg_angles(s_lock_batt_arc, 0, 0);

    s_lock_time = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lock_time, &hud_clock_76, 0);
    /* Recorded, not duplicated: the dim recolours this label to amber and has
     * to put it back. A second copy of the constant over there would go stale
     * the first time anyone retunes the clock's colour here, and the symptom
     * would be a clock that comes back from a dim the wrong shade. */
    s_lock_time_col = 0xE8FBFF;
    lv_obj_set_style_text_color(s_lock_time, lv_color_hex(s_lock_time_col), 0);
    lv_label_set_text(s_lock_time, "--:--");
    lv_obj_align(s_lock_time, LV_ALIGN_CENTER, 0, -18);

    s_lock_meridiem = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lock_meridiem, &hud_text_18, 0);
    lv_obj_set_style_text_color(s_lock_meridiem, lv_color_hex(0x5FD3EC), 0);
    lv_obj_set_style_text_letter_space(s_lock_meridiem, 2, 0);
    lv_label_set_text(s_lock_meridiem, "");
    lv_obj_add_flag(s_lock_meridiem, LV_OBJ_FLAG_HIDDEN);

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

    s_lock_charge = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lock_charge, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_lock_charge, lv_color_hex(0x22C55E), 0);
    lv_label_set_text(s_lock_charge, LV_SYMBOL_CHARGE);
    lv_obj_align(s_lock_charge, LV_ALIGN_TOP_MID, 52, TOP_MARGIN + 13);
    lv_obj_add_flag(s_lock_charge, LV_OBJ_FLAG_HIDDEN);

    /* Time to the charge limit, directly under the percentage it belongs to.
     * The secondary cyan and not the bolt's green, on purpose: this is a
     * caption on the number above it, not a second signal competing with the
     * bolt for attention. It exists only while current is flowing, so the
     * screen's resting state — clock, date, ring — is unchanged.
     * Fixed width and centred text, like the date: the string changes length
     * as the estimate falls and a natural-width label would shuffle sideways.
     *
     * Montserrat 14 rather than hud_text_18, which is a deliberate typeface
     * break: hud_text_18 is the smallest Orbitron subset compiled in, so at
     * 18 px the caption matched the percentage above it and the two read as
     * one two-line block rather than a number and its note. There is no
     * Orbitron 14 to reach for — subsetting one would be ~50 KB of flash for
     * this one string — and montserrat_14 is already linked. Letter spacing
     * drops to 1 with it: 2 was tuned for Orbitron's wider figures and looks
     * gappy on Montserrat at this size. */
    s_lock_eta = lv_label_create(scr);
    lv_obj_set_style_text_font(s_lock_eta, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_lock_eta, lv_color_hex(0x5FD3EC), 0);
    lv_obj_set_style_text_letter_space(s_lock_eta, 1, 0);
    lv_obj_set_width(s_lock_eta, CONTENT_W);
    lv_obj_set_style_text_align(s_lock_eta, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_lock_eta, "");
    /* +44, not +34. The percentage occupies y42..60, so the old offset left a
     * 4 px gap that read as a collision; this gives 10 px of clear space and
     * still sits well above the clock's own band. */
    lv_obj_align(s_lock_eta, LV_ALIGN_TOP_MID, 0, TOP_MARGIN + 44);
    lv_obj_add_flag(s_lock_eta, LV_OBJ_FLAG_HIDDEN);

    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, lock_tap_cb, LV_EVENT_CLICKED, NULL);
    /* Not gesture_home_cb: that one requires the swipe to start in the bottom
     * 72 px, which is the right rule for a screen with content to scroll and
     * the wrong one for a locked device. See lock_gesture_cb. */
    lv_obj_add_event_cb(scr, lock_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_lock_slow = 0;
    s_lock_pointer_down = false;
    s_lock_dragging = false;
    s_lock_drag_active = false;
    s_lock_drag_moved = false;
    s_lock_charge_phase = s_lock_charge_div = 0;
    s_lock_drag_start_x = s_lock_drag_start_y = 0;
    s_lock_drag_origin_x = s_lock_drag_x = 0;
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

    /* Hidden until a horizontal drag commits. This sibling displays the
     * flattened card while the live, interactive subtree is temporarily hidden. */
    s_lock_np_drag_img = lv_image_create(scr);
    lv_obj_remove_flag(s_lock_np_drag_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_lock_np_drag_img, LV_OBJ_FLAG_HIDDEN);

    lock_np_refresh();
    s_app_timer = lv_timer_create(lock_timer_cb, 16, NULL);   /* pointer sampling */
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
    /* The right-key hold on the lock screen is the mode's ONLY switch —
     * CONTROL carries just the cosmetic preference for its amber ring. The
     * hold works with the rings off; the mode then simply runs unadorned. */
    s_always_on = !s_always_on;
    s_req_lock_pref_save = true;
    ESP_LOGI(TAG, "always-on %s", s_always_on ? "ON" : "OFF");

    if (!ui_lock()) return;
    lock_refresh();                          /* recolours the base ring */
    toast_show(s_always_on ? "ALWAYS ON SCREEN  ON"
                           : "ALWAYS ON SCREEN  OFF", s_always_on, 0, 118);
    bsp_display_unlock();
}

/* Pocket lock: hold the LEFT key on the lock screen to stop touch waking the
 * panel. For a cube going into a bag or a coat pocket, where the alternative is
 * a screen lit against fabric for the length of a commute.
 *
 * Same key, two verbs, kept apart the way the right key's two are: a short
 * press still sleeps the panel, because BTN_SHORT only fires on a release that
 * beat LONG_PRESS_MS while BTN_LONG fires with the finger still down. So the
 * hold can never also sleep, and a tap can never toggle this.
 *
 * Only touch is refused, and only while the panel is off. Keys are untouched —
 * they are the way back in, and unlike the glass they are not what a pocket
 * leans on. The gate itself lives at the single consumer of s_req_wake rather
 * than in the two touch callbacks that raise it, so neither of them has to know
 * this mode exists. */
static void pocket_lock_toggle(void) {
    s_pocket_lock = !s_pocket_lock;
    ESP_LOGI(TAG, "pocket lock %s", s_pocket_lock ? "ON" : "OFF");

    if (!ui_lock()) return;
    toast_show(s_pocket_lock ? "POCKET LOCK  ON"
                             : "POCKET LOCK  OFF", s_pocket_lock, 0, 118);
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
    if (s_app == APP_CONTROL) {
        if (s_cfg_wifi && !lv_obj_has_flag(s_cfg_wifi, LV_OBJ_FLAG_HIDDEN)) {
            if (ui_lock()) { cfg_wifi_show(false); bsp_display_unlock(); }
            return true;
        }
        if (s_cfg_pick && !lv_obj_has_flag(s_cfg_pick, LV_OBJ_FLAG_HIDDEN)) {
            if (ui_lock()) { cfg_pick_show(false); bsp_display_unlock(); }
            return true;
        }
        return false;
    }
    if (s_app == APP_MUSIC) {
        if (s_sp_devpanel && !lv_obj_has_flag(s_sp_devpanel, LV_OBJ_FLAG_HIDDEN)) {
            if (ui_lock()) { sp_show_devices(false); bsp_display_unlock(); }
            return true;
        }
        return false;
    }
    if (s_app == APP_POMO) {
        /* Consumed, and deliberately consumed to do NOTHING. The fallthrough
         * from here is Home, and Home tore the screen out from under a running
         * countdown on the key most likely to be brushed while the cube is
         * being turned. FOCUS is left the two ways every app can be left — the
         * middle key, and the swipe up this screen already carries — so the tap
         * costs nothing by being inert, and the right key's meaning here is now
         * entirely in its hold. */
        return true;
    }
    if (s_app == APP_DAYS && s_days_qr_panel &&
        !lv_obj_has_flag(s_days_qr_panel, LV_OBJ_FLAG_HIDDEN)) {
        if (ui_lock()) {
            lv_obj_add_flag(s_days_qr_panel, LV_OBJ_FLAG_HIDDEN);
            __atomic_store_n(&s_req_days_fetch, true, __ATOMIC_RELEASE);
            days_label_text(s_days_sync, "SYNCING...");
            bsp_display_unlock();
        }
        return true;
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
        /* Was cancel-session. Cancel had no indicator and no undo, and it sat
         * on the one key a hand finds while turning the cube; rotation lock is
         * what that hold is for now. Nothing else binds cancel — a session is
         * ended by letting it finish or by leaving it paused. */
        pomo_rot_lock_toggle();
        break;
    case APP_DAYS:
        /* Hold right = refresh now. */
        __atomic_store_n(&s_req_days_fetch, true, __ATOMIC_RELEASE);
        break;
    default:                                        /* drawer: step through apps */
        for (int n = 0; n < APP_COUNT; n++) {   /* step past any compiled-out app */
            drawer_pick = (drawer_pick + 1) % APP_COUNT;
            if (app_enabled(drawer_pick)) break;
        }
        app_request(drawer_pick);
        break;
    }
}

/* ---------------- app switching ---------------- */

/* Runs on the main task.  Navigation is rendered with the AMOLED at brightness
 * zero, then revealed through the panel's brightness register.  A software
 * slide has to redraw both 480x480 screens through 32-row buffers and exposes
 * those bands; the hardware fade makes the same partial pipeline atomic to the
 * eye while retaining the one-screen-at-a-time memory bound. */
static void app_open(int idx) {
    static int64_t last_switch;
    int64_t t0 = esp_timer_get_time();
    if (t0 - last_switch < 250000) return;      /* debounce double taps */
    last_switch = t0;

    /* A compiled-out app is a zeroed hole in s_apps (see app_enabled), and
     * this function was the one table consumer that never checked — a request
     * for a hole reached "opened %s" with a NULL name and panicked in ROM
     * strlen. No user gesture can produce such a request (the drawer draws no
     * tile), but the self-test harness did, ten crashes in one night, and any
     * future stray request would too. Negative ids are LOCK and DRAWER. */
    if (idx >= 0 && !app_enabled(idx)) {
        ESP_LOGW(TAG, "app_open(%d): app compiled out, ignoring", idx);
        return;
    }

    /* Leaving CONTROL ends any pairing session. The code is only shown on that
     * card, so a session outliving it would advertise with no way to read the
     * code — and would hold the Wi-Fi driver down the whole time. The main
     * loop's restore path brings Wi-Fi back. Deliberately before ui_lock():
     * teardown does not touch LVGL and can take a moment. */
    if (s_app == APP_CONTROL && idx != APP_CONTROL && ble_prov_active()) {
        s_req_ble_off = true;
    }

    /* Lift the desk-clock dim BEFORE nav_fade_begin(), which captures
     * s_bright_applied as the level to ramp back to — entering it dimmed would
     * hand the incoming screen the dim level permanently, with nothing left on
     * the glass to explain why.
     *
     * The wallpaper pointer is dropped first so the lift is brightness-only.
     * That widget is about to be deleted a few lines below, so un-hiding it
     * would buy an ~80 ms lit full-screen repaint of an image nobody will ever
     * see, immediately before the fade-out. (The teardown block below still
     * nulls it too — this is the load-bearing one, that one is the rule.)
     *
     * This is what covers the screen changes with no finger behind them: the
     * lock screen's middle-key shortcut, the PET rebuild driven from the main
     * loop, and the self-test walk. */
    s_lock_wall = NULL;
    ao_dim_set(false);

    bool reveal = nav_fade_begin();
    if (!ui_lock()) {
        if (reveal) rot_fade_end();
        return;
    }

    /* let the outgoing app flush its state before its widgets disappear */
    if (s_app >= 0 && s_app < APP_COUNT && s_apps[s_app].save) {
        s_apps[s_app].save();
    }
    /* Closing PET is the natural moment the dashboard's picture goes stale;
     * report what the visit changed. */
    if (s_app == APP_PET) pet_report_publish();
    if (s_app_timer) { lv_timer_delete(s_app_timer); s_app_timer = NULL; }
    s_scr_home = s_scr_setup = s_scr_pet = NULL;
    s_status_label = NULL; s_batt_bar = NULL; s_batt_label = NULL;
    s_bolt_label = NULL; s_fps_label = NULL; s_events_label = NULL;
    s_ch_wrap = NULL; s_pet_egg = NULL;
    s_gem[0] = s_gem[1] = NULL; s_walker = NULL;
    s_gem_on[0] = s_gem_on[1] = false; s_walker_on = false;
    s_gem_sky[0] = s_gem_sky[1] = false;
    s_pet_planet = NULL; s_sign = NULL; s_sign_label = NULL;
    s_jet_flame = NULL; s_sign_on = false; s_fly_mode = 0;
    s_pet_qr_panel = NULL; s_pet_qr = NULL; s_pet_qr_note = NULL;
    s_pet_qr_btn = NULL; s_pet_qr_btn_l = NULL; s_pet_qr_tick = NULL;
    s_pet_qr_state = PET_QR_SHOWING;
    s_cfg_wall_pool = NULL; s_cfg_wall_state = NULL;
    s_cfg_wall_bar = NULL; s_cfg_wall_sub = NULL;
    s_cfg_days_val = NULL;
    s_cfg_rot_val = NULL; s_cfg_vol_val = NULL;
    s_cfg_rot_sw = NULL; s_cfg_rot_btn = NULL; s_cfg_time_sw = NULL;
    s_cfg_lockkey_val = NULL; s_cfg_lockkey_btn = NULL;
    s_cfg_pick = NULL; s_cfg_picklist = NULL; s_cfg_pickmore = NULL;
    s_cfg_always_sw = NULL; s_cfg_rings_sw = NULL;
    s_cfg_aodim_sw = NULL; s_cfg_aodim_sld = NULL;
    s_cfg_aodim_val = NULL; s_cfg_aodim_sub = NULL;
    s_cfg_chgeta_sw = NULL; s_cfg_chgeta_sub = NULL;
    s_cfg_bright_val = NULL;
    s_cfg_batt_bar = NULL;
    s_cfg_batt_val = NULL; s_cfg_batt_sub = NULL;
    s_cfg_care_val = NULL; s_cfg_care_sub = NULL;
    s_cfg_care_sld = NULL; s_cfg_care_btn = NULL;
    s_cfg_net_val = NULL; s_cfg_sys_val = NULL; s_cfg_log = NULL;
    /* Same rule as every pointer above: nothing walks this table after the
     * screen is gone today, but a table of freed objects is the exact shape of
     * bug that is expensive to find later, so it does not get to be the one
     * exception. */
    s_cfg_slider_n = 0;
    memset(s_cfg_sliders, 0, sizeof(s_cfg_sliders));
#if CFG_PERF_SCROLL_SELFTEST
    s_cfg_col = NULL;
#endif
    s_cfg_ble_val = NULL; s_cfg_ble_code = NULL; s_cfg_ble_btn = NULL;
    s_cfg_ble_qr = NULL; s_cfg_ble_qr_code[0] = '\0';
    /* The widgets are gone, so the next build must repaint from scratch rather
     * than compare against a key describing a card that no longer exists. */
    s_cfg_ble_spin = NULL; s_cfg_ble_key = -1;
    s_cfg_wifi = NULL; s_cfg_wifi_st = NULL; s_cfg_wifi_sub = NULL;
    s_cfg_wifi_stop = NULL; s_cfg_wifi_tick = NULL; s_cfg_wifi_phase = 0;
    s_sp_art = NULL; s_sp_art_ph = NULL; s_sp_lbl_track = NULL;
    s_sp_lbl_artist = NULL; s_sp_btn_play_lbl = NULL; s_sp_btn_shuf = NULL;
    s_sp_btn_dev = NULL; s_sp_devpanel = NULL; s_sp_devlist = NULL;
    s_sp_pair_panel = NULL; s_sp_pair_qr = NULL; s_sp_pair_drawn[0] = '\0';
    s_sp_btn_prev = NULL; s_sp_btn_next = NULL; s_sp_btn_like = NULL;
    s_sp_vol_bar = NULL; s_sp_vol_icon = NULL;
    /* s_sp_spin was added without a row here and dangled after MUSIC closed —
     * sp_art_clear() runs from the lock card's transport buttons too, so every
     * skip from the lock screen flipped a flag bit inside freed, recycled LVGL
     * memory. The symptom was nowhere near the cause: the lock screen's own
     * widgets misbehaving (dead gestures) after MUSIC had been opened once.
     * When adding a MUSIC widget static, its NULL row here is part of the
     * change, not an optional extra. */
    s_sp_spin = NULL; s_sp_chip = NULL;
    s_sp_scr = NULL; s_sp_bg_drawn = 0;
    s_pomo_clock = NULL; s_pomo_word = NULL;
    s_pomo_tick = NULL; s_pomo_hint = NULL; s_pomo_padlock = NULL;
    s_pomo_ring = NULL; s_pomo_arc = NULL; s_pomo_fill = NULL;
    for (int i = 0; i < POMO_SLOTS; i++) s_pomo_dial[i] = NULL;
    s_days_today = NULL; s_days_time = NULL; s_days_num = NULL;
    s_days_unit = NULL; s_days_bar = NULL; s_days_start = NULL;
    s_days_target = NULL; s_days_card = NULL; s_days_text = NULL;
    s_days_sync = NULL; s_days_qr_panel = NULL; s_days_qr = NULL;
    s_days_qr_note = NULL; s_days_link_drawn[0] = '\0';
    lock_snapshot_clear();
    s_lock_time = NULL; s_lock_date = NULL; s_lock_meridiem = NULL;
    s_lock_batt = NULL;
    s_lock_charge = NULL; s_lock_eta = NULL;
    s_lock_ring = NULL; s_lock_inner_ring = NULL;
    s_lock_batt_arc = NULL; s_lock_ao_ring = NULL;
    s_lock_wall = NULL;     /* already dropped at the top; kept for the rule */
    s_lock_np = NULL;
    s_lock_np_art = NULL; s_lock_np_ph = NULL;
    s_lock_np_track = NULL; s_lock_np_prev = NULL; s_lock_np_play = NULL;
    s_lock_np_next = NULL;
    s_lock_np_drag_img = NULL;
    /* Must clear, not just null: wall_service() runs on the network task and would
     * otherwise keep suppressing wallpaper downloads forever after the lock screen
     * is torn down, with nothing left on screen to explain why. */
    s_lock_np_up = false;
    s_lock_np_bg = 0;
    s_lock_pointer_down = false;
    s_lock_dragging = false;
    s_lock_drag_active = false;
    s_lock_drag_moved = false;
    s_lock_charge_phase = s_lock_charge_div = 0;
    s_lock_drag_origin_x = s_lock_drag_x = 0;
    s_lock_rule = NULL;

    /* Free the outgoing app BEFORE building the next one. Keeping both alive
     * once drove internal heap to 16 bytes, wedging the flush path. */
    lv_obj_t *old_scr = lv_screen_active();
    /* Forget whatever the touch driver was holding on the outgoing screen
     * BEFORE it is freed. The indev keeps pointer.scroll_obj and a scroll-throw
     * animation keyed on the INDEV, not the object, so a screen deleted with a
     * throw still in flight leaves a live pointer into freed memory until the
     * next touch release. That is a dangling reference on exactly this path
     * and it is stock LVGL. Cheap, and it was the leading candidate for the
     * 2026-08-23 scroll-and-leave-CONTROL reboots. */
    lv_indev_reset(NULL, NULL);
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_screen_load(scr);
    if (old_scr && old_scr != scr) lv_obj_delete(old_scr);

    if (app_enabled(idx))             s_apps[idx].build(scr);
    else if (idx == APP_LOCK)         build_lock_screen(scr);
    else                              build_drawer(scr);
    s_app = idx;

    /* Entering Spotify is the one explicit signal that lock-screen playback is
     * wanted again. Merely locking or visiting home preserves a dismissal. */
    if (idx == APP_MUSIC && s_lock_np_off) {
        s_lock_np_off = false;
        ESP_LOGI(TAG, "lock: now-playing dismissal cleared by MUSIC");
    }

    /* Prepare a different wallpaper after the lock screen is already visible.
     * The main loop waits for an idle touch window before taking the LVGL lock,
     * so this never lengthens the black Home -> Lock transition. */
    if (idx == APP_LOCK && __builtin_popcount(s_wall_have) > 1) {
        s_wall_prime_after = now_ms() + 1200;
        s_req_wall_prime = true;
    }

    /* Force all 15 strips through the panel while it is black.  The first
     * brightness command in the reveal drains the final queued DMA transfer,
     * so no half-painted frame can become visible. */
    lv_refr_now(NULL);

    bsp_display_unlock();
    if (reveal) rot_fade_end();

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
        if (!app_enabled(i)) continue;
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

#if CFG_PERF_SCROLL_SELFTEST || CFG_SNAP_CHORD
/* Stream one object out of the console as base64 pixels, bracketed by
 * SNAP_BEGIN/SNAP_END markers, for tools/snap_rx.py to rebuild into an image.
 * Callable from the LVGL task (harness timers) or from the main loop under
 * ui_lock (the chord); the UI freezes for the duration, which is the deal a
 * debug capture makes. printf blocks when the USB-CDC ring fills, so the dump
 * paces itself, and the WDT is fed inside the loop because the main loop is
 * subscribed and a frame takes minutes (reset from an unsubscribed task is a
 * harmless error return). */
static lv_draw_buf_t *snap_take(lv_obj_t *obj, lv_color_format_t cf, const char *name) {
    lv_draw_buf_t *snap = lv_snapshot_take(obj, cf);
    if (!snap) ESP_LOGW(TAG, "snap %s: take failed", name);
    return snap;
}

/* Synchronous variant — only the SELFTEST harness uses it now; the chord
 * path renders, flashes the borders, and writes a BMP to the card. */
static __attribute__((unused)) void snap_stream(lv_draw_buf_t *snap, const char *name) {
    if (!snap) return;
    printf("SNAP_BEGIN %s %u %u %u %u\n", name,
           (unsigned)snap->header.w, (unsigned)snap->header.h,
           (unsigned)snap->header.cf, (unsigned)snap->header.stride);
    /* Sequence-numbered chunks: printf is not atomic against other tasks'
     * console writes, so ~1 line in a few thousand arrives corrupted or not at
     * all. With the index in-band the host detects the gap and fills 384 zero
     * bytes — a black stripe in one screenshot instead of a lost frame. */
    static unsigned char b64[513];
    const uint8_t *d = snap->data;
    unsigned seq = 0;
    for (size_t off = 0; off < snap->data_size; off += 384, seq++) {
        size_t n = snap->data_size - off, olen = 0;
        if (n > 384) n = 384;
        mbedtls_base64_encode(b64, sizeof b64, &olen, d + off, n);
        b64[olen] = 0;
        printf("S:%u:%s\n", seq, (char *)b64);
        if ((off & 0x3FFF) == 0) {
            vTaskDelay(1);                        /* let the console drain */
            esp_task_wdt_reset();                 /* minutes-long from main loop */
            /* A dump is 54 s and the settle after it 12 more — past the 60 s
             * auto-lock. Trigger from inside the loop or the NEXT screen's
             * snapshot is silently the lock screen (it was, twice). */
            lv_display_trigger_activity(NULL);
        }
    }
    printf("SNAP_END %s\n", name);
    lv_draw_buf_destroy(snap);
}

#if CFG_DIM_SNAP
/* Two frames, one clock minute, one variable. Runs on the LVGL task, which is
 * where snap_stream() is safe to call from, and holds the main loop's dim off
 * for the whole ~2 min so the only thing that can differ between the frames is
 * the drift. 8 px rather than the production 4, so the shift cannot be confused
 * with a one-pixel rounding difference in the diff. */
static void dim_snap_cb(lv_timer_t *t) {
    lv_timer_delete(t);
    s_dim_snap_busy = true;

    ao_drift_apply(0, 0);
    lv_refr_now(NULL);
    ESP_LOGW(TAG, "dimsnap: frame A (drift 0,0), dimmed=%d", (int)s_ao_dimmed);
    snap_stream(snap_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565, "driftA"),
                "driftA");

    /* Now the state nobody has ever seen: driven straight rather than waited
     * for, because always-on may be off and the dim is otherwise unreachable
     * without a finger. ao_dim_set() advances its own fade one step per call,
     * so this pumps it until the blackout lands. */
    int64_t t0 = now_ms();
    while (!s_ao_wall_hidden && now_ms() - t0 < 10000) {
        ao_dim_set(true);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    ESP_LOGW(TAG, "dimsnap: frame B (dimmed=%d wall_hidden=%d)",
             (int)s_ao_dimmed, (int)s_ao_wall_hidden);
    lv_refr_now(NULL);
    snap_stream(snap_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565, "dimmed"),
                "dimmed");
    ao_dim_set(false);

    ao_drift_apply(0, 0);
    lv_refr_now(NULL);
    s_dim_snap_busy = false;
    ESP_LOGW(TAG, "dimsnap: done");
}
#endif

#if CFG_SNAP_CHORD
/* Capture handler for both chords. The screen snapshot alone NEVER contains
 * the bezel lobes — they live on lv_layer_top() — so the plain shot is clean
 * even though two keys are physically held while it renders. with_top adds
 * the ARGB top-layer frame for snap_rx to composite, which is how a posed
 * lobe gets into the picture. Ends by swallowing every key and draining the
 * PMU's latched PWR events, so releases that happen during the minutes-long
 * stream fire no lock/home/back afterwards. */
/* Border flash: four thin bars blink in from the edges and fade — the
 * capture language of this device. White = armed / frame taken, green =
 * saved to the card, red = failed. Each bar's dirty rect is under 3k px, one
 * flush, cannot band (§5); top layer, CLICKABLE cleared per the top-layer
 * rule; fade_out paired with delete_delayed (HARDWARE.md §5). */
static void snap_flash(uint32_t hex) {
    if (!ui_lock()) return;
    lv_obj_t *bars[4];
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(b);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(b, lv_color_hex(hex), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        bars[i] = b;
    }
    lv_obj_set_size(bars[0], 480, 6); lv_obj_align(bars[0], LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_size(bars[1], 480, 6); lv_obj_align(bars[1], LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_size(bars[2], 6, 468); lv_obj_align(bars[2], LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_size(bars[3], 6, 468); lv_obj_align(bars[3], LV_ALIGN_RIGHT_MID, 0, 0);
    lv_refr_now(NULL);
    for (int i = 0; i < 4; i++) {
        lv_obj_fade_out(bars[i], 250, 80);
        lv_obj_delete_delayed(bars[i], 360);
    }
    bsp_display_unlock();
}

/* Write one screenshot to the card as a plain 24-bit BMP — viewable anywhere,
 * no host tooling needed. The top layer (bezel lobes), when present, is
 * alpha-composited during the row conversion, so a posed lobe costs no extra
 * buffer. ~691 KB, about a second on this card; WDT fed per 32 rows because
 * this runs on the subscribed main loop. 8.3 filenames only (LFN is off). */
static bool snap_write_bmp(const char *path, lv_draw_buf_t *scr, lv_draw_buf_t *top) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    const int w = 480, h = 480;
    const uint32_t row_out = (w * 3 + 3) & ~3u, img = row_out * h;
    uint8_t hdr[54] = { 'B', 'M' };
    uint32_t fsz = 54 + img;
    int32_t dim = w;
    memcpy(hdr + 2, &fsz, 4); hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &dim, 4);
    memcpy(hdr + 22, &dim, 4);
    hdr[26] = 1; hdr[28] = 24;
    memcpy(hdr + 34, &img, 4);
    bool ok = fwrite(hdr, 1, 54, f) == 54;
    static uint8_t row[480 * 3 + 4];
    for (int y = h - 1; ok && y >= 0; y--) {
        const uint8_t *b = scr->data + (size_t)y * scr->header.stride;
        const uint8_t *tp = top ? top->data + (size_t)y * top->header.stride : NULL;
        uint8_t *o = row;
        for (int x = 0; x < w; x++) {
            uint16_t px = (uint16_t)(b[2 * x] | (b[2 * x + 1] << 8));
            uint32_t r = ((px >> 11) & 31) * 255 / 31;
            uint32_t g = ((px >> 5) & 63) * 255 / 63;
            uint32_t bl = (px & 31) * 255 / 31;
            if (tp) {
                uint8_t tb = tp[4 * x], tg = tp[4 * x + 1],
                        tr = tp[4 * x + 2], ta = tp[4 * x + 3];
                if (ta) {
                    r  = (tr * ta + r  * (255 - ta)) / 255;
                    g  = (tg * ta + g  * (255 - ta)) / 255;
                    bl = (tb * ta + bl * (255 - ta)) / 255;
                }
            }
            *o++ = (uint8_t)bl; *o++ = (uint8_t)g; *o++ = (uint8_t)r;
        }
        memset(o, 0, row_out - (unsigned)w * 3);
        ok = fwrite(row, 1, row_out, f) == row_out;
        if ((y & 31) == 0) esp_task_wdt_reset();
    }
    fclose(f);
    return ok;
}

static void snap_capture(bool with_top, char tag) {
    if (!s_sd_ok) {
        ESP_LOGW(TAG, "snap: no card mounted");
        snap_flash(0xFF3B30);
        return;
    }
    if (!ui_lock()) { ESP_LOGW(TAG, "snap: LVGL lock timeout, skipped"); return; }
    lv_draw_buf_t *scr = snap_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565, "snap");
    lv_draw_buf_t *top = with_top
        ? snap_take(lv_layer_top(), LV_COLOR_FORMAT_ARGB8888, "snap") : NULL;
    /* Frames are rendered — park the lobes NOW, not after the write. This
     * loop stays blocked for the ~1 s card write, but the LVGL task is free,
     * so the retract animation plays immediately and a lifted finger sees the
     * lobe leave at normal speed instead of hanging through the capture. */
    for (int i = 0; i < 3; i++) bezel_press(i, false);
    bsp_display_unlock();
    /* No flash here: one signal per capture. Green below = saved, red =
     * failed, and the armed path's white blink is the only other light.
     * A mid-capture "frame taken" blink read as a third, meaningless lamp. */

    bool ok = false;
    if (scr) {
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        mkdir("/sdcard/snaps", 0777);
        char path[40];
        snprintf(path, sizeof path, "/sdcard/snaps/%c%02d%02d%02d.bmp",
                 tag, tm.tm_hour, tm.tm_min, tm.tm_sec);
        ok = snap_write_bmp(path, scr, top);
        ESP_LOGI(TAG, "snap: %s %s", path, ok ? "saved" : "WRITE FAILED");
    }
    if (scr) lv_draw_buf_destroy(scr);
    if (top) lv_draw_buf_destroy(top);

    snap_flash(ok ? 0x22C55E : 0xFF3B30);   /* green = saved, red = failed */

    btn_swallow(&s_key_left);
    btn_swallow(&s_key_right);
    (void)pmu_pwrkey_edges();
    (void)pmu_pwrkey_pressed();

    /* The capture blocks this loop for ~2 s, so a finger lifted during it
     * releases into btn_swallow's silent level-adopt and the PMU drain above —
     * the release EDGES the bezel retraction listens for never fire, and the
     * lobes stay swelled in forever (they did). Park all three explicitly;
     * bezel_press is a no-op for any lobe already home. */
    if (ui_lock()) {
        for (int i = 0; i < 3; i++) bezel_press(i, false);
        bsp_display_unlock();
    }
}
#endif

#if CFG_PERF_SCROLL_SELFTEST
/* TEMPORARY PERF HARNESS — see the define. Runs inside the LVGL task
 * (lv_timer), so no lock is needed. Trigger_activity keeps the 60 s auto-lock
 * from tearing CONTROL down under the test. lv_obj_scroll_by's dy is
 * subtracted from scroll_y, so a negative step moves toward the bottom. */
static void perf_snap_dump(const char *name) {
    snap_stream(snap_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565, name), name);
}

/* MUSIC showcase: instead of the generic walk, drive the crown-jewel app the
 * way a hand would — three covers across two real track skips, the lock screen
 * with its now-playing card, the same card swiped away, then the two screens
 * the walk lost to a crash. sp_send() is the same producer the touch
 * callbacks use, and this runs on the LVGL task like they do. 12 s settle:
 * a poll is 3 s and a cover fetch rides on it. */
/* Mini-run for the shots the other sequences kept losing: PET and the drawer
 * first — every heap-ghost crash so far bit on a LATE app switch, so the
 * scarce shots go before the churn — then the lock screen stripped bare
 * (rings off via the RAM flag only, no NVS save request, now-playing card
 * dismissed) for the README's minimal-lock image. Flags restored after. */
#define CFG_PERF_MINI 1
#if CFG_PERF_MINI
static void perf_mini_tick(lv_timer_t *t) {
    static int idx;
    lv_display_trigger_activity(NULL);
    switch (idx++) {
    /* PET is requested through app_enabled(), never by bare enum: a build
     * with FACET_APP_PET 0 leaves a hole in s_apps, and requesting a hole
     * was one night's entire crash loop (HARDWARE.md #31). */
    case 0: app_request(APP_DRAWER);                                  break;
    case 1: perf_snap_dump("drawer");
            app_request(app_enabled(APP_PET) ? APP_PET : APP_DRAWER); break;
    case 2: if (app_enabled(APP_PET)) perf_snap_dump("pet");
            s_lock_rings = false; s_lock_np_off = true;
            app_request(APP_LOCK);                                    break;
    case 3: perf_snap_dump("lock_noring");
            s_lock_rings = true; s_lock_np_off = false;
            app_request(APP_MUSIC);                                   break;
    case 4: /* second settle period: give MUSIC two full polls so the edge
             * chrome (shuffle/like/devices column, volume slider) has every
             * chance to arrive before the frame is judged */          break;
    default:
        perf_snap_dump("music_full");
        lv_timer_delete(t);
        ESP_LOGI(TAG, "perf selftest: mini complete");
        return;
    }
    if (t) lv_timer_reset(t);
}
#endif

#define CFG_PERF_MUSIC_SHOWCASE 1
#if CFG_PERF_MUSIC_SHOWCASE
static __attribute__((unused)) void perf_showcase_tick(lv_timer_t *t) {
    static int idx;
    lv_display_trigger_activity(NULL);   /* a 54 s dump must not auto-lock */
    switch (idx++) {
    case 0: app_request(APP_MUSIC);                                   break;
    case 1: perf_snap_dump("music1"); sp_send(SP_CMD_NEXT);           break;
    case 2: perf_snap_dump("music2"); sp_send(SP_CMD_NEXT);           break;
    case 3: perf_snap_dump("music3"); app_request(APP_LOCK);          break;
    case 4: perf_snap_dump("lock_playing");
            s_lock_np_off = true;  app_request(APP_DRAWER);           break;
    case 5: app_request(APP_LOCK);   /* rebuild, card dismissed */    break;
    case 6: perf_snap_dump("lock_clean");
            s_lock_np_off = false; app_request(APP_PET);              break;
    case 7: perf_snap_dump("pet");   app_request(APP_DRAWER);        break;
    default:
        perf_snap_dump("drawer");
        lv_timer_delete(t);
        ESP_LOGI(TAG, "perf selftest: showcase complete");
        return;
    }
    /* Dumps overrun the period tenfold; restart it. NULL when the kick calls
     * step 0 directly — the real timer is already freshly created then. */
    if (t) lv_timer_reset(t);
}
#endif

/* After the scroll test: visit each screen, let it settle, snapshot it. The
 * README has no pictures and every visual-defect report so far has been a
 * photo of the glass; this makes the device hand over its own framebuffer. */
static const struct { int app; const char *name; } s_walk[] = {
    { APP_CONTROL, "control" }, { APP_MUSIC, "music" }, { APP_POMO, "focus" },
    { APP_DAYS, "days" }, { APP_PET, "pet" }, { APP_DRAWER, "drawer" },
    { APP_LOCK, "lock" },
};

static void perf_walk_tick(lv_timer_t *t) {
    static int idx;
    /* Snapshot what the PREVIOUS tick opened, now that it has had a full
     * period to build, fetch and settle. */
    if (idx > 0) perf_snap_dump(s_walk[idx - 1].name);
    /* Skip compiled-out apps — a hole in s_apps is not a screen. */
    while (idx < (int)(sizeof(s_walk) / sizeof(s_walk[0])) &&
           s_walk[idx].app >= 0 && !app_enabled(s_walk[idx].app)) idx++;
    /* The dump overruns the timer period by a factor of ten, and an overdue
     * lv_timer refires inside the SAME lv_timer_handler pass — so without
     * this the whole walk runs back-to-back with the LVGL lock held, the main
     * loop never gets to consume app_request, and every "screen" is a
     * snapshot of wherever the walk started. Reset so the period restarts
     * from the end of the dump, which is what hands the main loop its 4.5 s
     * window to actually open the next app. */
    lv_timer_reset(t);
    if (idx >= (int)(sizeof(s_walk) / sizeof(s_walk[0]))) {
        lv_timer_delete(t);
        ESP_LOGI(TAG, "perf selftest: walk complete");
        return;
    }
    lv_display_trigger_activity(NULL);
    app_request(s_walk[idx].app);
    idx++;
}

/* unused while CFG_PERF_MUSIC_SHOWCASE displaces the scroll+walk path */
static __attribute__((unused)) void perf_scroll_tick(lv_timer_t *t) {
    static int64_t t0;
    static int32_t dir = -1;
    if (!s_cfg_col) return;                 /* CONTROL not built yet, or gone */
    if (!t0) t0 = esp_timer_get_time();
    if (esp_timer_get_time() - t0 > 40LL * 1000000) {
        lv_timer_delete(t);
        ESP_LOGI(TAG, "perf scroll selftest: scroll done, starting walk");
        /* 9 s settle per screen: MUSIC's first poll plus cover fetch takes
         * ~6.5 s, and a shorter period freezes its snapshot on the boot
         * placeholder. */
        lv_timer_create(perf_walk_tick, 9000, NULL);
        return;
    }
    lv_display_trigger_activity(NULL);
    if (lv_obj_get_scroll_bottom(s_cfg_col) <= 0) dir = 1;
    else if (lv_obj_get_scroll_top(s_cfg_col) <= 0) dir = -1;
    lv_obj_scroll_by(s_cfg_col, 0, dir * 40, LV_ANIM_OFF);
}

static void perf_selftest_kick(lv_timer_t *t) {
    lv_timer_delete(t);
#if CFG_PERF_MINI
    ESP_LOGI(TAG, "perf selftest: mini (pet, drawer, bare lock)");
    lv_timer_create(perf_mini_tick, 12000, NULL);
    perf_mini_tick(NULL);
#elif CFG_PERF_MUSIC_SHOWCASE
    ESP_LOGI(TAG, "perf selftest: MUSIC showcase");
    lv_timer_create(perf_showcase_tick, 12000, NULL);
    perf_showcase_tick(NULL);   /* step 0 opens MUSIC now, not in 12 s */
#else
    ESP_LOGI(TAG, "perf scroll selftest: opening CONTROL");
    app_request(APP_CONTROL);
    lv_timer_create(perf_scroll_tick, 30, NULL);
#endif
}
#endif  /* CFG_PERF_SCROLL_SELFTEST */
#endif  /* CFG_PERF_SCROLL_SELFTEST || CFG_SNAP_CHORD */

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
    /* pet_load and pomo_load used to run HERE — before sd_init() — which
     * silently pinned them to the NVS fallback forever: every v2 pet save
     * went to the card, every boot re-read a fossil v1 blob from NVS and
     * re-migrated it, and the pet lived the same 45 minutes on repeat.
     * State loads belong AFTER the card mounts, where days_load already
     * was. */
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
    /* Rendering is the latency-critical path.  Keeping the LVGL worker's hot
     * call frames in 40 MHz PSRAM made every large redraw pay external-memory
     * latency; the 8 KB internal stack is a better use of the recovered SRAM. */
    disp_cfg.lv_adapter_cfg.stack_in_psram = false;
    /* LV_OS_NONE moved the ENTIRE render pipeline — blending, layers, and the
     * TJpgD cover decode — onto this one task's stack; the 8 KB default was
     * sized when draw threads carried that. Measured overflowing twice in one
     * minute under MUSIC art updates ("A stack overflow in task lvgl"), with
     * a wild-jump IllegalInstruction as the trampling variant. 16 KB holds. */
    disp_cfg.lv_adapter_cfg.task_stack_size = 16384;

    lv_display_t *disp = bsp_display_start_with_config(&disp_cfg);
    if (!disp) {
        ESP_LOGE(TAG, "DISPLAY INIT FAILED — continuing headless (board stays flashable)");
        log_mem("display-fail");
    } else {
        bright_apply(s_bright);
        log_mem("display-up");

        if (ui_lock()) {
            /* The display stays plain RGB565 and esp_lvgl_adapter byte-swaps
             * every flush for the big-endian CO5300 (~0.5 ms per 15,360 px
             * slice, ~10% of a scroll frame). Rendering RGB565_SWAPPED directly
             * would remove that pass, but the ESP32-S3 SIMD blenders only exist
             * for plain RGB565, and they are worth far more than the swap. Do
             * not switch the format without a swapped-target SIMD set. */
            lv_display_add_event_cb(disp, render_perf_cb, LV_EVENT_ALL, NULL);

#if CFG_PERF_SCROLL_SELFTEST
            lv_timer_create(perf_selftest_kick, 8000, NULL);
#endif

            /* LVGL only emits a gesture once the finger has travelled 50 px AND
             * held a velocity of 3 — defaults tuned for a phone. On this panel
             * that lost about two thirds of real swipes: measured 2 registering
             * out of 6 deliberate ones, which reads as "the gesture is broken"
             * rather than "you swiped too gently".
             *
             * 28 px and velocity 1 accept the slower, shorter drag this cover
             * glass actually produces. Over-triggering is already guarded against
             * everywhere it matters: lv_indev_wait_release() gives one action per
             * touch, MUSIC has a 550 ms cooldown, and both screens ignore a click
             * that lands in a swipe's shadow. */
            for (lv_indev_t *in = lv_indev_get_next(NULL); in; in = lv_indev_get_next(in)) {
                if (lv_indev_get_type(in) != LV_INDEV_TYPE_POINTER) continue;
                lv_indev_set_gesture_min_distance(in, 28);
                lv_indev_set_gesture_min_velocity(in, 1);
                lv_indev_add_event_cb(in, touch_origin_cb, LV_EVENT_PRESSED, in);
            }
            bsp_display_unlock();
        }
        /* Boot lands on the LOCK screen, not the drawer. A cube that has just
         * been powered on or reflashed is in exactly the state waking from
         * sleep leaves it in, and that path has always landed here — coming up
         * on the drawer instead meant a reboot was the one way to find the
         * device already unlocked, which is both inconsistent and the wrong
         * default for something that spends its life on a desk. It also means
         * the desk-clock dim and its wallpaper blackout are reachable from a
         * cold boot without touching anything. */
        app_open(APP_LOCK);
        log_mem("ui-built");
#if CFG_DIM_AB
        /* Pinned, not merely preferred: always-on so the panel never sleeps or
         * auto-locks out from under the soak, and one fixed brightness so the
         * two phases start from the same level. Assigned rather than saved, so
         * the user's own setting comes back on the next ordinary boot. */
        s_always_on = true;
        s_ao_dim_on = true;
        /* The DELAY has to be pinned too, and forgetting it nearly cost a
         * two-hour run: it is a saved preference, so whatever the CONTROL
         * slider was last left on decides how much of each phase is actually
         * dimmed. At the 10-minute stop, a 30-minute "dimmed" phase spends a
         * third of itself bright and the comparison measures almost nothing. */
        s_ao_dim_s = 30;
        s_bright = AB_BRIGHT;
        bright_apply(s_bright);
        ESP_LOGW(TAG, "dim A/B: armed, %d min per phase, bright=%d%%",
                 AB_PHASE_MIN, AB_BRIGHT);
#endif
#if CFG_DIM_SNAP
        /* 40 s: past boot settle and past the 10 s dim delay, so both frames
         * are of a genuinely dimmed lock screen — wallpaper hidden, clock
         * amber — which is the state the drift actually runs in. */
        lv_timer_create(dim_snap_cb, 40000, NULL);
#endif


    }

    sd_init();
    days_load();
    pet_load();
    pomo_load();
    /* The drawer built above knew only the factory pet name; now that the
     * real one is loaded, rebuild it before anyone looks too closely. */
    if (s_app == APP_DRAWER) app_request(APP_DRAWER);
    rtc_init();
    chg_load();          /* before pmu_init: the first CV write is the user's */
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
    /* After rotation_apply(), so the lobes land on the edge the keys are
     * actually on. They live on lv_layer_top(), which belongs to the display
     * rather than to a screen, so they outlive every app teardown. */
    if (ui_lock()) {
        bezel_init();
        bsp_display_unlock();
    }

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

    int64_t last_stats = now_ms(), last_perf = now_ms();
    int64_t last_batt = 0, last_imu = 0, last_pet_save = now_ms();
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
        uint8_t pwr_edge = pmu_pwrkey_edges();         /* PWR down/up  */

#if CFG_SNAP_CHORD
        /* LEFT+RIGHT: instant clean screenshot (screen only — held keys'
         * lobes live on the top layer, which the clean shot skips). White
         * blink = frame taken, green blink = saved to the card, red = no
         * card or write failed. LEFT held + MIDDLE press: white blink =
         * ARMED; for 2 s every key is capture-inert so any single button can
         * be held to pose its lobe; the shot then fires WITH the top layer
         * and blinks green when saved. All keys are swallowed after any
         * capture, so no release ever locks or navigates. */
        static bool chord_latch;
        static int64_t snap_at;                /* 0 = no delayed shot armed */
        static bool pwr_eat;                   /* eat MIDDLE's completed press */
        if (pwr_eat && pwr) { pwr = false; pwr_eat = false; }
        if (snap_at) {
            kleft = kright = BTN_NONE; pwr = false;   /* posing, not commanding */
            if (t >= snap_at) {
                snap_at = 0;
                snap_capture(true, 'l');
            }
        } else {
            if (s_key_left.stable == 0 && s_key_right.stable == 0) {
                if (!chord_latch) {
                    chord_latch = true;
                    btn_swallow(&s_key_left);
                    btn_swallow(&s_key_right);
                    kleft = kright = BTN_NONE;
                    snap_capture(false, 's');
                }
            } else if (s_key_left.stable == 1 && s_key_right.stable == 1) {
                chord_latch = false;
            }
            if (s_key_left.stable == 0 && (pwr_edge & 1)) {
                snap_at = t + 2000;
                btn_swallow(&s_key_left);
                kleft = BTN_NONE;
                pwr_eat = true;
                snap_flash(0xFFFFFF);          /* the "armed" blink */
                ESP_LOGI(TAG, "snap: armed, fires in 2 s — pose a key");
            }
        }
#endif

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
                kleft = BTN_NONE; kright = BTN_NONE; pwr = false; pwr_edge = 0;
                s_last_btn = t;
            }
        }

        /* Bezel pop-out. Driven from the raw pin edge rather than the
         * classified event, which is 50-70 ms late — far too slow to feel
         * caused by the finger. Skipped while the screen is off: that press is
         * a wake, and the block above has already swallowed it. */
        if (s_screen_on && (s_key_left.raw_edge || s_key_right.raw_edge || pwr_edge)) {
            if (ui_lock()) {
                if (s_key_left.raw_edge)  bezel_press(0, s_key_left.raw_edge > 0);
                if (s_key_right.raw_edge) bezel_press(2, s_key_right.raw_edge > 0);
                if (pwr_edge & 1) {
                    /* Only the lock screen gives this key a configurable
                     * destination, so only there does the lobe name one.
                     * Elsewhere PWR is plain Home and an app glyph would be a
                     * lie. Set on the way down, with the arc. */
                    bezel_icon_set((s_app == APP_LOCK && app_enabled(s_lock_key_app))
                                   ? app_symbol(s_lock_key_app) : NULL);
                }
                /* A tap shorter than one 20 ms poll delivers both edges at
                 * once; pop out-and-back rather than dropping one of them. */
                if ((pwr_edge & 3) == 3)   bezel_pop(1);
                else if (pwr_edge & 1)     bezel_press(1, true);
                else if (pwr_edge & 2)     bezel_press(1, false);
                bsp_display_unlock();
            }
        }

        /* MUSIC rebinds all three keys to volume, so it is offered them first.
         * It declines on the device-picker scene, which leaves the right key free
         * to pop that sub-scene the usual way. Home there is the swipe-up
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
        } else if (s_app == APP_LOCK && kleft == BTN_LONG) {
            /* Ahead of the plain LEFT branch, not inside the APP_LOCK branch
             * below it: that branch is only reached when LEFT produced nothing,
             * so putting it there would make this unreachable — the exact shape
             * of the bug that once made the lock screen's right-key hold dead
             * code. A short press still sleeps the panel; the two verbs cannot
             * collide, because BTN_SHORT only fires on a release that beat
             * LONG_PRESS_MS and BTN_LONG fires with the finger still down.
             *
             * On a cube with CFG_SNAP_CHORD on, arming a lobe screenshot
             * (LEFT held, then MIDDLE) passes through 800 ms of held LEFT and
             * so also toggles this. Dev-only, and visible when it happens. */
            ESP_LOGI(TAG, "LEFT hold on lock -> pocket lock toggle");
            pocket_lock_toggle();
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
            /* Same key, other verb: a quick press shuffles the wallpaper. No
             * clash with the hold is possible by construction — BTN_SHORT only
             * fires on a release that beat LONG_PRESS_MS, and BTN_LONG fires
             * while the finger is still down, so lifting quickly can never
             * toggle desk clock and holding through 800 ms can never shuffle.
             * Rebuilding the lock screen is the existing change-the-picture
             * path: it consumes the primed slot (a cache hit, so the hidden
             * render stays short), re-arms the primer for the next press, and
             * the nav fade hides the full-frame repaint no TE pin can make
             * coherent. app_open()'s 250 ms debounce absorbs key mashing. */
            if (kright == BTN_SHORT) {
                /* Only when there is a different picture to show: two-plus in
                 * the pool, or one that arrived while the embedded default was
                 * up. Otherwise the rebuild would fade out and back in on the
                 * same image, which reads as a glitch rather than a shuffle. */
                int have = __builtin_popcount(s_wall_have);
                if (have > 1 || (have == 1 && s_wall_slot < 0)) {
                    ESP_LOGI(TAG, "RIGHT tap on lock -> next wallpaper");
                    app_open(APP_LOCK);
                } else {
                    ESP_LOGI(TAG, "RIGHT tap on lock ignored: %d wallpaper(s)",
                             have);
                }
            }
            /* Second exception, and an opt-in the rule above did not anticipate
             * rather than a hole in it: the middle key does nothing at all here
             * otherwise, so it opens whichever app CONTROL points it at. Off by
             * default, and it is the user asking for the shortcut rather than a
             * stray press finding one. Scoped to this branch — everywhere else
             * PWR stays plain Home. */
            /* On the LIFT, not the press. The arc and its glyph are already up
             * by then — you see what the key is about to do, and taking your
             * finger off is what commits it. Holding shows the shortcut
             * without firing it, which is the whole point of the affordance. */
            if ((pwr_edge & 2) && app_enabled(s_lock_key_app)) {
                ESP_LOGI(TAG, "MID lift on lock -> %s", lock_key_name(s_lock_key_app));
                app_request(s_lock_key_app);
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

            /* Desk-clock dim. Its own branch, deliberately outside the if/else
             * above: those two decide whether to CHANGE state (lock, sleep) and
             * are mutually exclusive, while this one describes the panel during
             * the state that always-on exists to hold still. It is one
             * expression re-evaluated every pass rather than a pair of
             * enter/exit rules, because that makes "every exit path restores"
             * structural instead of a list somebody has to keep complete —
             * always-on switched off, the panel asleep, FOCUS opened, a finger
             * on the glass, the feature switched off in CONTROL: each of them
             * just makes this false on the next 20 ms pass.
             *
             * s_app != APP_POMO because FOCUS already owns a dim and two owners
             * of one output fight (pitfall #23's shape, applied to brightness).
             * It is NOT enough that FOCUS pins activity while a session runs:
             * POMO_IDLE does not, so on a cube parked on an unstarted FOCUS
             * both timers run down on the same screen and every pass would
             * write the other's level to 0x51, forever. FOCUS wins there
             * because its dim is the one that knows the cube has been picked
             * up — it watches the accelerometer, this only watches idle.
             *
             * Runs before pomo_poll() and before the s_req_bright_apply write
             * further down this loop, so a touch restores in the same pass that
             * observed it rather than one pass later. */
#if CFG_DIM_AB
            /* On the main loop, not an lv_timer: telemetry_row() writes FATFS,
             * and SD writes belong to this task. Phase boundaries go into the
             * existing power log as events, so segmenting the run afterwards is
             * a grep rather than a guess at where an hour started. */
            {
                static int64_t ab_at;
                static int ab_phase;
                /* Re-asserted every pass, not set once at boot. The boot
                 * assignment was silently undone: the saved lock prefs load
                 * AFTER app_main's arming block, so s_ao_dim_s came back as the
                 * user's 600 s and a "dimmed" phase would have spent all 30
                 * minutes bright. The gate log is what named it — five inputs,
                 * one wrong, indistinguishable from the status line.
                 * s_ao_dim_on is deliberately NOT re-asserted: it is the
                 * variable the experiment flips. */
                s_always_on = true;
                s_ao_dim_s  = 30;
                if (!ab_at) { ab_at = t; telemetry_row("ab-dim-on"); }
                /* Name every term of the gate rather than reasoning about which
                 * one is false. The dim not engaging looks identical from the
                 * status line whichever input is wrong, and there are five. */
                static int64_t ab_dbg;
                if (t - ab_dbg > 15000) {
                    ab_dbg = t;
                    ESP_LOGW(TAG, "A/B gate: dim_on=%d always=%d scr_on=%d "
                                  "app=%d idle=%lld delay=%d dimmed=%d",
                             (int)s_ao_dim_on, (int)s_always_on,
                             (int)s_screen_on, (int)s_app,
                             (long long)idle, s_ao_dim_s, (int)s_ao_dimmed);
                }
                if (t - ab_at >= (int64_t)AB_PHASE_MIN * 60 * 1000) {
                    ab_at = t;
                    s_ao_dim_on = !s_ao_dim_on;
                    ab_phase++;
                    telemetry_row(s_ao_dim_on ? "ab-dim-on" : "ab-dim-off");
                    ESP_LOGW(TAG, "dim A/B: phase %d -> dim %s", ab_phase,
                             s_ao_dim_on ? "ON" : "OFF");
                }
            }
#endif
#if CFG_DIM_SNAP
            if (!s_dim_snap_busy)
#endif
            ao_dim_set(s_ao_dim_on && s_always_on && s_screen_on &&
                       s_app != APP_POMO &&
                       idle > (int64_t)s_ao_dim_s * 1000);
        }

        if (s_req_wake) {                 /* touch-to-wake, still locked */
            s_req_wake = false;
            /* Pocket lock refuses the wake AND the idle stamp. Stamping anyway
             * would be the quiet half of the bug: a cube riding against fabric
             * raises this flag continuously, so s_last_btn would be pinned to
             * now forever and the sleep timer could never run down again if the
             * mode were switched off with the panel lit. The flag is still
             * consumed rather than left set — it is a request that was answered
             * with no, not one still waiting. */
            if (s_pocket_lock) {
                if (!s_screen_on) ESP_LOGD(TAG, "touch ignored (pocket lock)");
            } else {
                s_last_btn = t;
                if (!s_screen_on) {
                    ESP_LOGI(TAG, "touch wake (stays locked)");
                    screen_toggle_power();
                }
            }
        }

        if (s_req_sntp) {
            s_req_sntp = false;
            time_sync_start();
        }

        /* Wallpaper downloads run on the network task. Decoding the next cached
         * image uses LVGL, so do it here only after navigation and touch settle. */

        if (s_req_app != APP_NONE) {
            int want = s_req_app;
            s_req_app = APP_NONE;
            if (want != s_app) app_open(want);
        }

        if (s_req_wall_prime && t >= s_wall_prime_after &&
            lv_display_get_inactive_time(NULL) >= 500 &&
            !s_lock_pointer_down &&
            s_dl_state != DL_QUERY && s_dl_state != DL_IMAGE) {
            s_req_wall_prime = false;
            wall_cache_prime();
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
        bool sp_awake     = (s_screen_on && !s_doze);
        bool sp_poll_lock = (s_app == APP_LOCK && sp_awake);
        int64_t sp_due = sp_poll_lock ? (SP_POLL_MS * 2) : SP_POLL_MS;
        /* MUSIC used to poll regardless of the screen, which broke 7b's own rule
         * and had a real cost: the cover is 139 KB, and a download still in flight
         * when the panel dozed was cut off mid-body — Wi-Fi drops to
         * WIFI_PS_MAX_MODEM at the same moment. The length check caught the partial
         * and refused it, so nothing was ever drawn wrong, but it cost a wasted
         * transfer and a 30 s backoff on a cover nobody could see. */
        if (((s_app == APP_MUSIC && sp_awake) || sp_poll_lock) && s_wifi_up &&
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
        if (s_req_bright_apply && s_screen_on && !s_pomo_dimmed && !s_ao_dimmed) {
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

        if (s_req_clock_save && !ble_prov_nvs_blocked()) {
            s_req_clock_save = false;
            clock_pref_save();
        }

        if (s_req_lock_pref_save && !ble_prov_nvs_blocked()) {
            s_req_lock_pref_save = false;
            lock_pref_save();
        }

        if (s_req_chg_save && !ble_prov_nvs_blocked()) {
            s_req_chg_save = false;
            chg_save();
            chg_apply();        /* don't make the user wait for the next poll */
            telemetry_row("chgmode");
        }

        /* The network task fetched DAYS but must not touch flash from a PSRAM
         * stack. Clear before snapshotting: a fetch landing mid-save re-raises
         * the flag and is written on the next pass rather than being lost. */
        if (__atomic_load_n(&s_req_days_save, __ATOMIC_ACQUIRE) &&
            !ble_prov_nvs_blocked()) {
            __atomic_store_n(&s_req_days_save, false, __ATOMIC_RELEASE);
            days_blob_t snap;
            days_snapshot(&snap, NULL);
            store_save("days", &snap, sizeof(snap));
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
            /* Cleared on both outcomes: the wait is over either way, and the
             * CONTROL tick decides what the card says from here. */
            s_cfg_ble_starting = false;
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
                    s_wifi_reason = 0;    /* stale: our own pre-scan disconnect */
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

        pet_engine_service();          /* self-paced to ~1 Hz internally */
        if (s_req_pet_rebuild) {
            s_req_pet_rebuild = false;
            /* Straight to app_open, NOT app_request: the request consumer
             * filters same-app requests (want != s_app), which silently ate
             * every mid-app rebuild — a design applied while the user stood
             * in the pet app changed the data and never the screen. app_open
             * handles a same-app rebuild through all the teardown rules. */
            if (s_app == APP_PET) app_open(APP_PET);
        }
        /* NVS commits erase flash, which stalls a BLE controller running
         * from flash. The dirty flag stays set, so the write lands on the
         * first pass after teardown rather than being dropped. */
        if (s_pet_dirty && !ble_prov_nvs_blocked() && (t - last_pet_save) >= 60000) {
            last_pet_save = t;
            pet_save();
        }
        /* An hourly heartbeat keeps the dashboard honest even when nobody
         * opens the app; event pushes (evolution, departure) are immediate. */
        static int64_t last_pet_push;
        if (t - last_pet_push >= 60 * 60 * 1000LL) {
            last_pet_push = t;
            pet_report_publish();
        }

        /* PET reads the IMU as a joystick and a shake detector; 10 Hz
         * undersamples a hand shake (one sample per half-oscillation), so
         * the cadence rises to 50 Hz only while PET is the active app. The
         * read is one 6-byte I2C burst, ~2% bus time at 50 Hz. */
        if (!s_doze && t - last_imu >= (s_app == APP_PET ? 20 : 100)) {
            last_imu = t;
            imu_poll();
            pomo_poll(t);                         /* needs fresh accelerometer */
            if (s_app == APP_PET) pet_motion_poll();
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
            static bool was_bypass;
            battery_poll();
            if (s_batt_mv > 0 && !s_batt_t_first) {
                s_batt_mv_first = s_batt_mv;
                s_batt_t_first = now_ms();
            }
            /* A one-shot ends when the charge it asked for actually finishes,
             * not when the cube is unplugged — so an interrupted top-up resumes
             * on the next connection instead of being silently dropped.
             *
             * "Finished" has to mean a cycle we watched run. Testing s_bypass
             * alone cancelled the request 0.8 s after arming it: the cap had
             * just been raised, but reg 0x01 still carried the *previous*
             * cycle's "done" and that was enough to look complete. So wait for
             * current to actually flow. If it never does, the cell was already
             * full and the request is satisfied anyway — but only conclude that
             * after a few polls, never from the first stale byte. */
            if (s_chg_once) {
                if (s_batt_charging) { s_chg_once_seen = true; s_chg_once_idle = 0; }
                else if (s_bypass)   { s_chg_once_idle++; }

                if (s_chg_once_seen ? s_bypass : s_chg_once_idle >= 5) {
                    s_chg_once = false;
                    s_chg_once_seen = false;
                    s_chg_once_idle = 0;
                    s_req_chg_save = true;
                    ESP_LOGI(TAG, "one-shot full charge %s, cap back to %d mV",
                             s_batt_mv >= 4150 ? "done" : "not needed",
                             chg_cv_mv(chg_cv_code()));
                }
            }
            if (s_bypass != was_bypass) {
                was_bypass = s_bypass;
                telemetry_row(s_bypass ? "bypass" : "charge");
            }
        }

        if (t - last_perf >= PERF_PERIOD_MS) {
            last_perf = t;
            render_perf_report(s_app);
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
            /* Pocket lock earns a place here because its whole symptom is an
             * absence — "the touchscreen stopped working" — with nothing on the
             * glass to contradict that. One token turns a debug session into a
             * grep. It prints only when on, so a healthy line is unchanged.
             *
             * The desk-clock dim is here for exactly the same reason: from the
             * glass "the screen went dim on its own" is indistinguishable from
             * a brightness bug, and this token separates them without a flash. */
            ESP_LOGI(TAG, "uptime=%llds clock=%s scr=%d%s%s idle=%lu/%llds wifi=%s%d screen=%s batt=%d%% %dmV%s%s cap=%dmV%s fps=%u.%u "
                          "rot=%d sd=%lu pet[%s f%d %d/%d m%d]%s",
                     (long long)(t / 1000), clock, (int)s_app,
                     s_pocket_lock ? " POCKET" : "",
                     s_ao_dimmed ? " AODIM" : "",
                     (unsigned long)lv_display_get_inactive_time(NULL),
                     (long long)((t - s_last_btn) / 1000),
                     s_wifi_up ? "up" : "down", wifi_rssi(),
                     s_screen_on ? "on" : "off",
                     (int)s_batt_pct, (int)s_batt_mv,
                     s_batt_charging ? " CHG" : "",
                     s_bypass ? " BYP" : (s_vbus ? " PLUG" : ""),
                     chg_cv_mv(chg_cv_code()), s_chg_once ? " ONCE" : "",
                     (unsigned)(s_last_fps_x10 / 10), (unsigned)(s_last_fps_x10 % 10),
                     s_rot * 90, (unsigned long)s_tele_rows, pet_stage_word(),
                     (int)s_pet.form, (int)s_pet.hunger, (int)s_pet.happy,
                     (int)s_pet.mistakes,
                     s_stack_fallback ? " STACK-FALLBACK!" : "");
            log_mem("periodic");
        }
    }
}
