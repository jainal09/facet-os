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

**MUSIC is the one app that rebinds the keys**, and it takes all three: left
raises the volume, right lowers it, middle mutes. A remote whose volume lives in
a menu is not a remote, and the three side keys are the only controls you can
find without looking at the screen.

Left-is-up **deliberately contradicts the silkscreen**, which labels the leftmost
key minus and the rightmost plus. It matches how the cube is actually held rather
than how it is printed, it was asked for explicitly, and it is the kind of thing
a later reader will try to "correct" back to the labels. Don't.

The costs of rebinding are paid explicitly:

- **Home moves to a swipe up from the bottom**, phone-style. `sp_gesture_cb`
  handles it on this screen — the other apps use `gesture_home_cb`, same
  direction. Lock is then one swipe plus one key rather than unreachable.
- **The device picker keeps the global bindings**, so the right key can still pop
  a sub-scene. `sp_keys()` returns false whenever that panel is visible, which is
  what stops volume from trapping you in it.
- Keys are offered to `sp_keys()` *before* the global chain, and it reports
  whether it consumed them — the alternative, special-casing `APP_MUSIC` inside
  each global branch, is how the lock-screen bug above happened.

Held keys repeat: `btn_poll()` emits `BTN_REPEAT` every 130 ms after the long
press. Every other consumer tests for `BTN_SHORT`/`BTN_LONG` specifically, so
repeats are ignored without those call sites needing to know they exist.

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

### The wallpaper pool

A single wallpaper file meant every unlock showed the same picture until the
next download — four images a day, and the device looked static. Instead the
card holds `WALL_SLOTS` (12) of them at `/sdcard/assets/wN.png`, and the lock
screen picks a random one **every time it is built**, avoiding an immediate
repeat. Downloads then only control how fast the pool turns over, not how much
variety you see. Twelve images is about 5 MB against a 32 GB card.

`UNSPLASH_QUERY` is a `;`-separated list of themes and one is chosen at random
per download, so the pool ends up mixed rather than all one subject. Themes are
human-written and contain spaces, so they are percent-encoded into the URL.

Which slots hold a usable image is cached in a bitmask (`s_wall_have`), rebuilt
once at mount, rather than stat-ing twelve files on every lock-screen build.
Attribution is stored per slot in `wN.txt` and CONTROL shows the credit for
whatever is currently on screen.

Fetching runs on the **network task, not the main loop**. That task already owns
an 8 KB stack and a TLS path, so the pool costs no additional internal SRAM, and
a slow download can no longer stall button handling or the app switcher — which
it did when the fetch ran inline. It also runs while dozing: the device spends
nearly all its life asleep, so skipping downloads there meant the pool never
filled. Throttling comes free from the task's own cadence (45 s awake, 10 min
dozing), so a fresh card fills in about ten minutes of use or two hours idle,
then settles to one replacement every `WALLPAPER_PERIOD_MS` (6 h).

Measured cost: **zero steady-state**, and a transient dip of internal free from
~23.7 KB to ~8.9 KB while an image is in flight, fully recovered afterwards.
That download is now the single largest transient consumer of internal SRAM in
the system — worth remembering before adding anything else that allocates
internally at the same time.

PNG, not JPEG: imgix and Unsplash serve **progressive** JPEG by default and
TJpgD cannot decode it — it fails silently and yields a 0×0 image.
`urls.raw` also carries an `auto=format` that overrides an appended `&fm=png`,
so the URL has to be cut at `?` before adding your own transform.

## FOCUS: orientation as input

The Pomodoro app is the only place the device treats its own attitude as a
control rather than as something to compensate for, and the pattern is worth
copying.

Four durations sit at fixed screen positions, each pre-rotated by `-90*i` tenths
of a degree so that turning the cube by `+90*i` cancels the pre-rotation and
whichever label reaches the top reads upright. The bottom one is genuinely
stored upside down.

Three things make it work:

1. **Autorotate is gated at the commit, not the poll.** `imu_poll()` still
   computes `s_base_rot` every 100 ms, but `rotation_apply()` is skipped while
   `s_app == APP_POMO`. If the panel counter-rotated, the labels would stay put
   relative to your eye and turning the cube would change nothing.
2. **The dial is fixed to the device; the readout is fixed to you.** The centre
   clock counter-rotates by `-90*top_edge`. Crucially, rotating a label spins it
   about its own centre only — its *offset* from the screen centre does not
   rotate, so the offsets are rotated by hand from a four-entry table. Skipping
   that put the status word on top of the digits the moment the cube turned.
3. **The session outlives the screen.** State is file-scope and ticked from the
   main loop at 1 Hz, so navigating away does not cancel a timer; the app is
   only ever a view onto it.

Flat-means-pause tests `abs(s_acc_z)` rather than a signed value, which keeps it
independent of how the IMU is mounted relative to the panel — undocumented, and
the reason autorotate needed an empirical 8-state calibration.

Staying awake uses `lv_display_trigger_activity(NULL)` from the main loop while a
session runs. `idle` is `min(LVGL inactivity, time since a key)` and a countdown
touches neither, so without it the 60 s auto-lock would tear the app down
mid-timer. Dimming is `bsp_display_brightness_set()`, with wake on touch **or**
on an accelerometer delta, since the IMU is already polling.

Wallpaper downloads are suppressed during a session — partly so a 400 KB fetch
does not compete for internal SRAM with the audio codec, partly because a radio
burst mid-focus is rude.

**Pause has two triggers and one owner.** A tap toggles a live session; laying the
cube flat pauses it and standing it up resumes. Both route through
`pomo_set_running()`, which exists because two copies of that logic would drift,
and because it holds a detail neither caller should have to remember: **resuming
must re-stamp `s_pomo_tick_ms`.** The countdown is driven by elapsed wall time
rather than by counting ticks, so without it the whole pause is charged to the
session the instant it resumes — a bug that would present as the timer
mysteriously losing minutes, nowhere near the pause that caused it.

The helper is guarded on current state, so a transition that does not apply is a
no-op. That is what lets the two triggers coexist: a session paused by tap is not
disturbed by the cube then being set down. `POMO_DONE` ignores taps entirely,
because the finish screen retires itself and a stray tap would otherwise start a
whole new session.

## MUSIC: a remote for whatever is already playing

The cube never plays the audio. It drives whichever Spotify endpoint is active —
phone, laptop, speaker — which is why it can be useful without a decent speaker
of its own.

**Direct to Spotify for everything interactive; the broker only where it earns
its place.** Control, state and the device list go straight to `api.spotify.com`
at 6 ms warm (see [HARDWARE.md §7f](HARDWARE.md)). The broker in [`broker/`](../broker)
is touched for exactly two things — one-time pairing, and album art — so it is
not a dependency of daily use:

| Broker down | Result |
|---|---|
| Controls, state, device list | Unaffected |
| Album art | Last cached art, or the placeholder |
| First-time pairing | Blocked, but that is a one-off |

**One task, one HTTP handle, one command queue.** `esp_http_client` handles carry
no lock and mutate in place, so the handle is confined to the `spotify` task
(stack in PSRAM) and touch callbacks only ever enqueue. Taps are optimistic: the
icon flips immediately and the next poll confirms rather than discovers.

**Album art is decoded by the broker, not the device.** It returns LVGL's RGB565
`.bin` format, so `lv_bin_decoder` streams rows off the card per draw chunk and
the device runs no image decoder at all — which sidesteps TJpgD never populating
the image cache. Ask for **exactly the size that gets drawn**: the cover is 148 px,
so `SP_ART_PX` is 148, and the file is 43 KB rather than 115 KB.

That number is load-bearing in a second way. The cover has to end above the track
title, and for three commits it did not — `SP_ART_PX` stayed at 240 through the
relayout that moved the labels up to y262, so the title, the artist and the top of
the play button were all drawn *over* the artwork. It was plainly visible in a
photo of the device and nobody had said anything, because the eye reads it as a
busy album cover. Geometry that depends on two constants agreeing is worth
asserting or deriving, not restating.

**Unavailable is a distinct state from off.** Spotify reports
`actions.disallows` per transport verb and `device.supports_volume` per endpoint,
and plenty of Connect speakers refuse remote volume. Controls therefore render in
three states — on, off, and unavailable-and-not-clickable — because a button that
looks live and does nothing is worse than one that looks disabled.

**Volume converges rather than queues.** `s_sp_vol` is stepped locally so the bar
tracks the key, and `s_sp_vol_sent` records what Spotify accepted; the two being
unequal *is* the "a PUT is owed" flag. Nothing is cleared before the call, so a
press landing mid-flight cannot be lost to a read-then-clear race — the trailing
check sees the level moved and queues another round. The same function runs on
every poll, which makes a dropped command self-healing within one cycle.

**Gestures, because the keys are spent.** Swipe up goes home; swipe left and
right change track, in the direction a carousel flicks. Both are skipped while the
device picker is open, where a flick belongs to the list.

**A reused connection has to be able to give up.** `esp_http_client_perform()`
does not redial after a transport failure — it retries the dead socket forever.
Seen live: the server reset a connection mid-transfer and every 3 s poll
afterwards logged `esp_tls_conn_read error / Socket is not connected` while the
app sat frozen, with nothing surfacing near the UI. So `sp_call()` checks
`perform()`'s **return value**, not just the status code, and closes the handle on
a genuine transport error so the next call redials at the cost of one 390 ms
handshake. `ESP_ERR_NOT_SUPPORTED` is excluded — that is the Bearer-challenge
path, which is a normal 401 and must not force a reconnect.

The lesson generalises past this app: the whole point of a long-lived handle is
that it survives, so the failure mode is *not* letting go when it should. Any
long-lived client needs an explicit path back to disconnected.

**Decoration must back off.** A failed art fetch only recorded the URL on
success, so a truncated download retried on every poll — a 43 KB request every
3 s, hammering the broker and pinning internal SRAM near its floor for as long as
the track played. Failures are now remembered with a 30 s cooldown. Anything
retried from a periodic poll needs a cooldown, or one failure becomes a load
generator.

**The library endpoints moved.** `/me/tracks/contains` and `/me/tracks` now answer
403 regardless of scope; use `/me/library` with Spotify URIs. This cost real
debugging time and is written up as [HARDWARE.md pitfall #16](HARDWARE.md#10-pitfalls-index).

### The backdrop follows the cover

The broker returns a dominant colour as an `X-Art-Accent` header on the art fetch,
so the tint costs **no extra request and no extra bytes** — a second endpoint would
have cost a 390 ms cold handshake per track. It is derived from the bytes being
served rather than stored beside them, so a cache hit and a cache miss cannot
disagree and no existing cache entry is invalidated.

Three things make it usable rather than merely colourful:

- **Conditioning happens on the server**, where there is float math and one place
  to tune it. Averaging album art gives mud, and a near-black or near-white tint
  makes a glyph on top unreadable, so the broker discards greys and near-blacks
  per pixel, weights the rest by how colourful they are, and forces the result
  into a saturation and lightness band. A genuinely monochrome cover makes it
  **decline** — the player keeps its default rather than tinting itself a grey
  indistinguishable from its own chrome.
- **The tint goes behind the controls, not on them**, at a quarter strength. Full
  value fights white text, and on an AMOLED every lit pixel costs power. Flat, never
  a gradient: RGB565 bands visibly on a dark ramp.
- **It is change-gated.** Setting a screen's background invalidates all 480×480 of
  it, so applied on every tick this would be a full-frame flush forever.

Black is the default and the fallback: the accent starts at 0 meaning "not known
yet", so a screen still waiting on `/me/player` stays black rather than showing a
colour that would read as chosen on purpose.

### Now playing on the lock screen

The lock screen grows a transport panel when Spotify has an active device, and is
untouched when it does not. Three gates make it affordable:

1. **Polls only while the screen is genuinely on** (`s_screen_on && !s_doze`), at
   half the MUSIC rate. Dozing runs at 80 MHz with `WIFI_PS_MAX_MODEM` where an
   HTTPS call costs ~4 s, so a 3 s cadence would never let the radio sleep — and
   desk-clock mode never sleeps at all, so an ungated poll would run for as long
   as the cube sat on the desk.
2. **Wallpaper downloads are suppressed while the panel is up.** The wallpaper
   fetch is the largest transient consumer of internal SRAM on this board, and the
   panel fetches album art; overlapping them was the real risk, not the widgets.
   Note this does *not* cover boot, when the lock screen decodes a wallpaper and
   fetches art before the panel appears — the lowest floor yet recorded (5,368
   bytes, since relieved by the Wi-Fi buffer trim) was measured in that window.
3. **Nothing repaints unless it changed**, which is what makes a panel on a screen
   that already redraws every 40 ms for its sweep arc cost approximately nothing.

**The clock yields, not the buttons.** The first attempt kept the clock centred and
fitted the transport into what was left, which produced 46 px targets — small
enough to ghost-touch, and the same mistake the first MUSIC layout made. When the
panel appears the clock drops to `hud_clock_48` and moves up, the divider hides,
and the transport gets 76/88/76 px with real padding. The clock has nothing below
it worth protecting.

## Sound

Clips are authored offline and embedded in flash via `EMBED_FILES` (~99 KB),
not fetched at runtime. See [assets/sounds/CREDITS.md](../assets/sounds/CREDITS.md)
for why they are generated rather than sampled.

Playback runs on a dedicated task whose stack is in PSRAM, so it costs no
internal SRAM. It has to be off the caller's thread: `esp_codec_dev_write()`
blocks until the DMA drains, so a 2 s bell played inline would freeze button
handling for two seconds. The codec is brought up lazily on the first sound
(~3.7 KB internal, unavoidable — I2S DMA cannot live in PSRAM) and stays open
between sounds, closing only after four seconds of quiet; closing eagerly
truncated every tail and clicked the amp on each play.

The gain trap is documented in [HARDWARE.md §7e](HARDWARE.md): the stock volume
curve stops at 0 dB while the chip reaches +32 dB.

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
  hud_clock_48.c      the same at 48 px, for the lock screen in music mode (40 KB)
  hud_text_18.c       Orbitron, ASCII (57 KB)
  app_icons_64.c      six Material Icons glyphs (34 KB)
  hud_icons_30.c      five 30 px glyphs: Connect, volume, mute, heart x2 (12 KB)
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
