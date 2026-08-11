# Facet architecture

Everything here follows from one number: after Wi-Fi and the display driver have
taken their share, roughly **25 KB of internal SRAM** is left. Flash (11.9 MB
unallocated), PSRAM (7–8 MB free) and the card (32 GB) are effectively
unlimited by comparison. So the design question is never "will it fit on the
device" — it is "will it fit in internal SRAM *at the same time as everything
else*".

Board-level facts live in [HARDWARE.md](HARDWARE.md).

## The app model

An app is a build function, an optional save function, and a stable id:

```c
typedef struct {
    const char *name;                 /* shown in the drawer */
    const char *id;                   /* stable storage key   */
    const char *icon;                 /* glyph from app_icons_64 */
    uint32_t    color;                /* accent */
    void      (*build)(lv_obj_t *scr);
    void      (*save)(void);          /* flushed when the app closes */
} app_def_t;
```

Apps never coexist. Opening one tears the previous one down completely:

1. Call the outgoing app's `save()` while its widgets still exist.
2. Delete its `lv_timer` and null every widget pointer that any callback reads.
3. Load a **blank** screen, delete the old one, *then* build into the blank.
4. Call the incoming app's `build()`.

Step 3 is not a style preference. `lv_screen_load_anim(..., auto_del)` keeps
both screens alive through the cross-fade, and that peak once drove internal
heap to **16 bytes**, which wedged the display flush path and starved the idle
task. Free first, build second.

Switching is always deferred to the main loop through a request flag
(`app_request()`), because tearing a screen down from inside one of its own
touch callbacks frees the object mid-event.

Sentinels must not collide with real ids. `APP_DRAWER` is `-1`, `APP_LOCK` is
`-2`, and "nothing pending" is `APP_NONE = -100` — it used to be `-2`, which
silently swallowed every lock request.

Measured cost of a switch: **1–26 ms**. A reboot is ~2.7 s. In-firmware
switching beats OTA-partition app switching by about 100×, so OTA stays for
genuinely separate firmwares and the card is for content, not code.

## The state store

Apps keep nothing resident. State goes to the card on close and comes back on
open.

```
/sdcard/apps/<id>.bin      per-app state blob (opaque to the store)
/sdcard/logs/pwrlog.csv    power telemetry
/sdcard/assets/            wallpapers, fonts, downloaded artwork
```

```c
bool store_save(const char *id, const void *data, size_t len);
bool store_load(const char *id, void *data, size_t len);
```

Both fall back to an NVS blob under the same key when no card is mounted, so a
cardless board still works — it just has less room. Keep the blob a plain
fixed-size struct; a partial read is treated as no state at all, which makes a
truncated write fail safe.

Two constraints from FATFS worth knowing before you name a file: 8.3 filenames
are the default (`telemetry.csv`, a 9-character stem, silently fails to
create — hence `pwrlog.csv`), and long filenames need `CONFIG_FATFS_LFN_HEAP=y`.

## Writing a new app

1. Add an enum member before `APP_COUNT`.
2. Write `build_<name>_app(lv_obj_t *scr)`. Build into `scr`; do not keep a
   global screen pointer beyond what teardown nulls.
3. Create at most one `lv_timer` and assign it to `s_app_timer` — teardown
   deletes exactly that one.
4. Add a row to `s_apps[]` with an icon glyph from `app_icons_64`.
5. If the app has state, define a packed struct and call `store_save`/
   `store_load` with a stable `id`.
6. Add a case to `app_action()` for the long-press verb, and to `app_back()` if
   the app has a sub-scene to pop.

Two LVGL traps specific to this panel:

- `lv_obj_create()` objects are **clickable by default**. A decorative shape on
  a full-screen scene will silently swallow taps unless you
  `lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE)`.
- The display has heavily rounded corners and curved cover glass. Content within
  ~55 px of an edge is clipped or unreadable at an angle. Use `CONTENT_W`,
  `TOP_MARGIN` and `BOTTOM_MARGIN`, and keep circles at 430 px or less.

## The button contract

Global, identical in every app, so nothing is per-app knowledge:

| Key | Short | Long |
|---|---|---|
| Left (BOOT/minus, GPIO0) | Lock; again while locked, sleep | — |
| Middle (PWR, via AXP2101) | Home | — |
| Right (plus, GPIO18) | `app_back()`, else Home | `app_action()` |

While the screen is off, **any** key only wakes — it never also fires that key's
action. That has to test the pin *level* rather than a debounced edge: the main
loop runs at 120 ms while dozing and cannot see two consistent samples of a
~160 ms tap. After waking, the release is swallowed.

The lock screen is a deliberate partial exception, and the shape of it is worth
copying rather than re-deriving. Home and tap-back are disabled there — the
touchscreen is how you unlock, and a key must not bypass that — but the **hold**
still reaches `app_action()`, where `case APP_LOCK` toggles desk-clock mode.

Getting that wrong is easy: the dispatch was originally one
`else if (s_app != APP_LOCK)` around the whole key block, which silently made
the `APP_LOCK` branch of `app_action()` unreachable. The handler existed, the
enum matched, it compiled clean, and nothing happened when the key was pressed.
If a key action does nothing, check that the dispatcher can *reach* the handler
before debugging the handler.

Waking from sleep always lands on the lock screen, never straight into an app.
Tapping the lock screen goes **Home**, not back to wherever you locked from.

## Networking: Funnel, not a VPN

An on-device WireGuard/ts2021 stack was built and proven working on this board —
it registered on the tailnet and held a 15-minute soak with zero flaps. It was
then removed, because coexisting with LVGL left about **350 bytes** of internal
SRAM free and capped the UI at 10.8 fps.

Tailscale Funnel puts the private service behind a public HTTPS `ts.net`
endpoint with a bearer token instead. The device makes ordinary HTTPS calls, so
TLS costs ~3 KB of internal heap transiently per call rather than ~42 KB of
permanent task stacks. With `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y` the ~40 KB
session goes to PSRAM.

Result: **66 fps and ~25–35 KB internal free**, against 10.8 fps and 350 bytes.
For a device making a handful of outbound calls to a couple of services, the
mesh bought nothing that justified that.

## Assets

Fetch on demand in the background, cache to the card, decode from the card when
shown. Nothing is preloaded, and no image ever exists whole in RAM.

The wallpaper pipeline, which is the template for any future asset:

1. One Unsplash API call for a random photo, written straight to the card.
2. Parse that JSON from **PSRAM**, so a ~10 KB response never competes for
   internal heap.
3. Request the image at exactly 480×480 and stream it through the HTTP event
   callback into `<name>.part`, renaming on success. A failed download therefore
   cannot leave a corrupt file behind.
4. Verify the PNG signature before using a cached file — a fetch that only runs
   "if the file is missing" would otherwise keep a bad file forever.
5. Call `lv_image_cache_drop()` after replacing a file at a path LVGL has
   already decoded, or it keeps serving the old bitmap.

Refresh is every 6 hours, plus on demand from STATUS's action key. Four API
calls a day sits comfortably inside Unsplash's 50/hour demo limit.

PNG, not JPEG: imgix and Unsplash serve **progressive** JPEG by default and
TJpgD cannot decode it — it fails silently and yields a 0×0 image.
`urls.raw` also carries an `auto=format` that overrides an appended `&fm=png`,
so the URL has to be cut at `?` before adding your own transform.

## Power

Two states.

**ACTIVE** — 240 MHz ceiling, `WIFI_PS_MIN_MODEM`, panel on, IMU polled at
10 Hz, HTTPS every 45 s.

**DOZE** — entered when the screen turns off:

- `esp_lcd_panel_disp_on_off(panel, false)`. Backlight-off alone only writes
  brightness 0 and leaves the CO5300 scanning.
- `WIFI_PS_MAX_MODEM`. Still associated and reachable; HTTPS latency rises from
  ~1.5 s to ~4 s, which is fine for background polling.
- CPU pinned at 80 MHz. Never below — under 80 MHz the APB clock can no longer
  run USB-Serial/JTAG and the console dies, which looks exactly like a brick.
- UI timers and IMU polling stop. There is no point rendering or autorotating
  what nobody can see.

Desk-clock mode suppresses the auto-sleep entirely, so the panel stays lit.

Telemetry lands in `/sdcard/logs/pwrlog.csv` once a minute and on every state
change — the device cannot log to USB while on battery, which is exactly when
the numbers matter. Track **mV per hour**; percentage moves far too coarsely to
show an improvement over an hour.

## Layout

```
main/
  main.c              everything: drivers, apps, the main loop
  credentials.h.in    template; CMake fills it from .env
  hud_fonts.h         font + icon glyph declarations
  hud_clock_76.c      Orbitron, digits and colon only (89 KB)
  hud_text_18.c       Orbitron, ASCII (57 KB)
  app_icons_64.c      four Material Icons glyphs (34 KB)
components/
  esp32_s3_touch_amoled_2_16/   forked BSP, five fixes — see HARDWARE.md §9
sdkconfig.defaults    the config that works; read HARDWARE.md §3 before editing
partitions.csv        4 MB app slot (the 1 MB default is too small)
```

Fonts are subset with `lv_font_conv` — a full ASCII Orbitron at 76 px would be
several hundred KB of flash for ten glyphs that are actually drawn.

`main.c` is one large file on purpose for now. It is worth splitting when a
second contributor appears or it passes ~4000 lines; until then, having every
ordering constraint visible in one place has been worth more than the tidiness.
