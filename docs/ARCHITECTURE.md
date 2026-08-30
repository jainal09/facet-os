# Facet architecture

Everything here follows from one number: after Wi-Fi and the display driver have
taken their share, roughly **25 KB of internal SRAM** is left. Flash (11.9 MB
unallocated), PSRAM (7–8 MB free) and the card (32 GB) are effectively
unlimited by comparison. So the design question is never "will it fit on the
device" — it is "will it fit in internal SRAM *at the same time as everything
else*".

Board-level facts live in [HARDWARE.md](HARDWARE.md). The researched-and-deferred
plan to make each app its own firmware image is in
[MULTI-IMAGE.md](MULTI-IMAGE.md) — read it before proposing that again.

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
/sdcard/logs/pwrlog3.csv    power telemetry
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
create — hence the short `pwrlog3` stem, which still has room to grow), and long
filenames need `CONFIG_FATFS_LFN_HEAP=y`.

The trailing digit is a schema version. The CSV header is written only for a
file that does not exist yet, so adding a column has to come with a new name —
otherwise old rows sit under a header that no longer describes them.

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
still reaches `app_action()`, where `case APP_LOCK` toggles desk-clock mode, and
a **quick press** of the same right key shuffles the wallpaper. The two verbs
cannot collide by construction: `BTN_SHORT` only fires on a release that beat
`LONG_PRESS_MS`, and `BTN_LONG` fires while the finger is still down, so lifting
quickly can never toggle desk clock and holding through 800 ms can never
shuffle. The shuffle is a plain lock-screen rebuild — it consumes the primed
next slot, re-arms the primer, and rides the nav fade that hides any full-frame
repaint — and it is skipped when the pool has nothing different to show, because
a fade-out-and-back to the same image reads as a glitch rather than a shuffle.

A tap can also be **faster than the debouncer**: down on one 20 ms poll, up by
the next, so no debounced "pressed" sample ever exists and the release completes
nothing. `btn_poll()` treats the pair of opposite raw edges as a completed
`BTN_SHORT` — real contact bounce settles in under ~10 ms, so a high sample a
full poll after a low one means the finger genuinely lifted. Without that, the
fastest taps (exactly the ones a wallpaper-shuffle key invites) silently did
nothing.

Getting that wrong is easy: the dispatch was originally one
`else if (s_app != APP_LOCK)` around the whole key block, which silently made
the `APP_LOCK` branch of `app_action()` unreachable. The handler existed, the
enum matched, it compiled clean, and nothing happened when the key was pressed.
If a key action does nothing, check that the dispatcher can *reach* the handler
before debugging the handler.

**FOCUS consumes the right key's tap and spends its hold on rotation lock.**
Both go through the sanctioned hooks rather than through a special case in the
dispatcher — `app_back()` returns true for `APP_POMO` and does nothing, and
`app_action()` toggles the lock — so the contract above still describes the
dispatch exactly. The tap is consumed *to be inert*: its fallthrough was Home,
which tore the screen out from under a running countdown on the key a hand
finds while turning the cube. FOCUS is still left by the middle key or the
swipe up it already carries. The hold used to cancel the session; nothing binds
cancel now, and a session is ended by letting it finish or by leaving it paused.

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

### The power state machine, written down

It had never been written out, and that is how a bug got in: always-on suppressed
the *sleep* branch but not the *auto-lock* branch, so a cube parked on MUSIC kept
its panel lit and then tore the app down anyway. Two idle behaviours, written at
different times, one of which learned about a later flag.

| from | trigger | to |
|---|---|---|
| any app, screen on | idle > `AUTO_LOCK_MS`, **not** always-on | lock screen |
| lock screen, screen on | idle > `LOCK_SLEEP_MS`, **not** always-on | panel off + doze |
| any app | left key | lock screen |
| lock screen | left key | panel off + doze |
| panel off | any key **or** touch | panel on, still locked — the press is swallowed |
| lock screen, screen on | **swipe up, from anywhere** | drawer (home), never back to the previous app |
| lock screen, screen on | tap | nothing but reset idle — which is what lifts the dim |
| lock screen | right hold | toggle always-on (desk clock) |
| lock screen | right quick press | next wallpaper (skipped when nothing different to show) |
| FOCUS | right hold | toggle rotation lock (the cube stops picking a duration) |
| FOCUS | right tap | nothing — consumed so it cannot fall through to Home |
| FOCUS, session running | `lv_display_trigger_activity()` each tick | never idles out; dims via brightness instead |
| FOCUS, session finished | — | stays put: DONE holds the screen on (always-on or not) until a tap starts the next session or the user navigates away |
| lock screen, always-on holding it lit | idle > the CONTROL delay, dim enabled | panel to 12% of the user's level **and** the wallpaper hidden |
| any other screen, always-on holding it lit (not FOCUS) | idle > the CONTROL delay, dim enabled | panel to 12% of the user's level; content untouched |
| always-on, dimmed | touch, any key, or anything that resets idle | full level, wallpaper back — same pass, not the next one |
| always-on, dimmed | always-on switched off, FOCUS opened, feature switched off in CONTROL | full level, wallpaper back |
| always-on, dimmed | left key (sleep) | panel off + doze; the dim is dropped on the way down, so the next wake is not black-with-no-wallpaper |
| FOCUS, always-on | — | FOCUS's own dim owns the panel there; the desk-clock dim never runs on `APP_POMO` |

**The invariant everything rests on: the panel only ever sleeps from the lock
screen.** Nothing enforces it. It holds because auto-lock always moves to
`APP_LOCK` before the sleep timer can fire, so no app is ever on screen when the
panel goes dark — and every consumer depends on that. The wake path lights the
panel and nothing else, which is only correct if the lock screen is already behind
it.

`screen_toggle_power()` now logs a warning if that is ever violated, rather than
asserting. A wrong screen after wake is miserable to diagnose from the symptom;
one log line names it. It is also the reason an app that owns the whole screen is a
redesign and not a flag — see [MULTI-IMAGE.md](MULTI-IMAGE.md).

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
   callback into `<name>.part`, renaming on success. "Success" includes the byte
   count: `perform()` can return `ESP_OK` on a body that stopped short of the
   advertised Content-Length (a clean FIN mid-transfer), and renaming that file
   into place was how a partial image entered the pool as a permanent citizen.
4. Verify the PNG at **both ends** before using it — the 8-byte signature and
   the trailing IEND chunk. A fetch that only runs "if the file is missing"
   would otherwise keep a bad file forever, and a truncated download has a
   perfect signature and a missing tail, so the head alone proves nothing. The
   same test runs on every downloaded file before its slot is marked usable,
   and on every cached file at boot, which also purges partials left by older
   firmware.
5. Call `lv_image_cache_drop()` after replacing a file at a path LVGL has
   already decoded, or it keeps serving the old bitmap.

**The lock screen dims the wallpaper adaptively, not by a flat opacity.** The
original flat opa 110 (~43%) kept the clock legible over bright photos — and
erased dark ones: with themes like "deep space nebula" in `UNSPLASH_QUERY`, an
on-device audit found slots at mean luminance 29, 44 and 51 out of 255, all
decoding perfectly, all rendering as "black wallpaper, no image" under the flat
dim. That report is indistinguishable from a missing-file bug from the glass,
which is what it was mistaken for. `build_lock_screen()` now measures the mean
luminance of the chosen image (`wall_src_lum()` — it goes through the ordinary
decoder, so on a primed slot it is a cache hit and on a miss it IS the draw's
cache warm-up, never an extra decode) and dims toward `WALL_DIM_TARGET`
effective brightness: bright photos keep the old 110 floor, photos already at
or below the target draw at full opacity. One log line per lock build names the
slot, luminance and opacity so the next "black wallpaper" report is a grep, not
a debug flash. `CFG_WALL_POOL_AUDIT` in `main.c` (keep 0 in commits) decodes
and measures every slot at boot when the whole pool needs naming at once.

When the pool has nothing usable at all — no card, a fresh card, or everything
failing validation — the lock screen falls back to a wallpaper **embedded in
the app image** (`assets/wall_default.png`, neon-lit palms by Andre Tan /
Unsplash, ~75 KB) instead of bare black. It is palette-quantised to 64 colours
and denoised offline to embed small, and served as a C-array PNG through
LodePNG's `LV_IMAGE_SRC_VARIABLE` path — staged once into an aligned PSRAM copy,
because `EMBED_FILES` guarantees no alignment and the decoder reads the PNG
header through a `uint32_t` cast. `CFG_WALL_DEFAULT_TEST` in `main.c` forces
that branch for one flashed run (keep it 0 in commits), since a full pool never
exercises it naturally.

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
   relative to your eye and turning the cube would change nothing. The selected
   edge is computed relative to that frozen panel rotation, so entering FOCUS in
   any autorotated orientation starts with 60 minutes at the top.
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
disturbed by the cube then being set down.

**Rotation lock turns the input off without turning the app off.** Hold the
right key and the cube stops selecting a duration: the session survives being
picked up, knocked, or carried to another desk. It is the exception that proves
the app's own rule — orientation is input here, and this is the switch for that
input, so it is not a mode the rest of the firmware has any equivalent of.

Four things it does *not* do, each a decision rather than an omission:

- **It does not pin the panel.** The readout still counter-rotates, because it
  is fixed to the reader and always was. Only the *selection* is frozen. Two
  things read `pomo_top_edge()` for two different questions, and separating
  them is the whole implementation: `wr` still means "which way is up" and
  drives every rotation, while `pomo_pick_edge()` means "which duration the
  user chose" and is what a tap starts from. The dial highlight moves to the
  second, or a locked cube turned 90° lights a duration it will not run.
- **It does not stop flat-means-pause.** That is a different gesture with a
  different verb, and pausing costs nothing — it is the *restart* that loses a
  session. The name the feature was asked for settles it: laying a cube down is
  not rotating it.
- **It does not clear itself.** Not at DONE, not on cancel, not on leaving the
  app. Re-arming it for every session is the cost the feature exists to remove.
  It lives only in RAM and never in the blob, so a reboot always comes back
  unlocked and the padlock on the glass is the only thing to believe.
- **It does not let a swallowed turn accumulate.** `s_pomo_last_rot` is banked
  even while locked. Leaving it stale would fire every turn made during the
  lock the instant it came off — the release, rather than any press, restarting
  the session, which is precisely what the mode exists to prevent.

The indicator is **drawn, not typed**: LVGL's symbol set carries no padlock and
the hud fonts are ASCII-only, so any character that looks like one renders as an
empty box. It is an `lv_arc` shackle over a rounded slab in a 16×22 container
that carries the transform, which rotates the pair as a unit for a transient
layer small enough not to matter. It sits 95 px world-*above* the digits, in the
band between the top dial label and the readout, and it is amber in every state
— the word below owns run/pause/done, this owns the mode, and colouring them
alike would merge two independent readouts into one.

The banner confirming the toggle counter-rotates like everything else here, and
measures itself with `lv_obj_update_layout()` before setting its pivot: the
pivot is in box coordinates, the box is `LV_SIZE_CONTENT`, and hardcoding a
width both strings would have to agree on is the kind of constant that goes
stale silently.

**The finish screen is persistent.** It used to retire to the lock screen after
7 s, which meant the glance that mattered — "did it finish?" — usually found a
clock. Now DONE shows a green check where the digits were, and after a short
grace reveals "TAP TO START ANOTHER"; a tap begins a new session with whatever
duration is at the top, exactly like idle. The grace period exists because a tap
meant to *pause* can land just as the timer hits zero, and it doubles as the
hint's reveal delay so the screen never invites a tap it would swallow. The
panel stays on regardless of the always-on setting — but only while FOCUS is the
active app, since `pomo_poll()` returns early everywhere else — and the
inactivity dim still applies, which is what makes indefinitely-on affordable on
an AMOLED.

## DAYS: remote editing, local-first display

The editor lives at the broker's `/days` page because a native date picker and
real keyboard are better tools than a 480 px touch panel. Tapping the DAYS
screen requests `/days/link` with the cube's permanent bearer and draws the
returned HTTPS URL as a full-screen QR. The URL carries only a 144-bit,
user-bound, single-use code with a hard five-minute TTL. The browser removes the
code from its visible URL/history, exchanges it at `/days/session`, and keeps
the resulting 256-bit, 30-minute bearer only in tab memory. That bearer is
accepted only by `/countdown`; it cannot access Spotify, art, queues, or mint
another QR. The permanent cube bearer never enters the URL or browser.

The page sends the authenticated date and 48-character message to `/countdown`;
the server stores one small per-user JSON document with temp-file-plus-rename
replacement. Closing the QR scene queues an immediate cube refresh. The public
page contains no saved state, and opening `/days` directly instructs the user to
start from their cube.

The cube loads its own fixed-size DAYS blob from the app store at boot, so
opening the tile never waits for the network. A dedicated HTTP client in the
existing network task refreshes that blob on app open, from either the DAYS
screen or its CONTROL card, and once every 24 hours (with a 15-minute retry
after failure). It is deliberately
separate from Spotify's client because `esp_http_client` handles are mutable and
must stay task-owned.

The screen recomputes the remaining whole calendar days locally at midnight.
Civil-date arithmetic avoids the 23/25-hour discontinuity at daylight-saving
transitions. Progress runs from the date the countdown was saved to the target;
its bar and headline interpolate from cyan through violet and amber to coral as
the target approaches. Today, local time, the target, and the saved message all
remain useful when the broker is unavailable.

## PET: a life the cube simulates and the phone dresses

The pet is two things with a hard boundary between them: an **engine** — file-
scope state ticked ~1 Hz from the main loop, alive on every screen — and a
**view** (`build_pet_app`), which renders whatever the engine says and never
owns truth. The FOCUS pattern taken further: the session there outlives the
screen; the pet outlives *power*, because every duration in it is anchored to
wall-clock time the RTC makes trustworthy from early boot.

**Authority is split by who is good at what.** The broker owns what the owner
DESIGNS — name, species, world, theme, hat, sleep window, birthday, weather
city — behind a `cfg_ver` the cube uses as its whole re-apply gate. The cube
owns the life actually LIVED — stage, meters, care mistakes, stardust — and
reports it up (`POST /pet/st`, hourly, on app close, and on events) so the
phone page shows a live pet. The report's response carries the current
`cfg_ver`, so a state push doubles as a drift check and a design saved on the
phone is noticed in minutes, not on the 2 h fetch cycle. All of it rides the
DAYS keep-alive handle on the net task; the net task never touches the blob —
it publishes a parsed config into a staging struct the engine consumes on its
own tick (the same publish/snapshot discipline DAYS uses, and the same
main-loop-only store_save rule the DAYS PSRAM-stack crash taught).

**Care is the Tamagotchi Uni model with the pressure where it is fair.** Two
coupled meters (hunger, happiness) decay per stage; a need raises a cue, a cue
ignored for 15 minutes becomes one care mistake, and mistakes pick the adult
form while childhood happiness (an EWMA) picks the teen. The window only
counts down while the pet is awake AND the screen is on — mistakes never
accrue where nobody could have seen the ask, which is both the no-guilt rule
from the research and what makes offline time safe to credit. Crediting is
capped at 4 days of decay (a cube found in a drawer resumes hungry-but-alive)
but promotion uses wall time uncapped, so a pet hatches and grows while away
exactly like the 1996 toy. Sustained adult neglect is departure, not death;
the way home is a coax-back ritual on the designer page, and the apology text
deliberately never leaves the phone — only the completed gesture travels.

**The cube is the joystick.** Tilt walks the pet downhill — the gravity
component along the screen edge, read through `rot_from_base()` so all eight
mounting calibrations work, analog so steeper is faster; shake hops it; the
panel rotation pins while PET is open (FOCUS gates the same commit) so the
ground cannot rotate out from under a walk. The IMU polls at 50 Hz only
inside PET; see HARDWARE.md pitfalls #32 and #33 for the two ways this went
wrong first.

**The designer QR lives under the pet's name.** Same single-use-code flow as
DAYS (`/pet/link` → `/pet/session`); the page shows every option as a picture
— species, worlds, themes and hats are drawn, not named — because a dropdown
reading "WORLD 2" asks the user to already know the answer. Closing the QR on
the cube forces the fetch, and the panel says so, because a sync with no
visible acknowledgement reads as a design that was lost.

The blob is v2 (~100 B, natural alignment, no packing — int64s at odd offsets
are not worth a smaller file); v1 blobs migrate in place, mapping age onto the
stage table so the original astronaut kept its life. Species can be vector
(parameterised builders) or pixel sprites the broker renders from character
grids into LVGL RGB565 `.bin` sheets — frames stacked vertically, each frame
sized under the one-flush budget by construction.

## MUSIC: a remote for whatever is already playing

The cube never plays the audio. It drives whichever Spotify endpoint is active —
phone, laptop, speaker — which is why it can be useful without a decent speaker
of its own.

**Direct to Spotify for everything interactive; user-scoped tokens from the
broker.** Control, state and the device list go straight to `api.spotify.com` at
6 ms warm (see [HARDWARE.md §7f](HARDWARE.md)). Each cube has a unique broker
bearer that selects one isolated user record. The broker persists that user's
Spotify refresh token and returns only a short-lived access token; it is also
used for pairing, album art, queue compaction, and renewing that token roughly
once an hour.

| Broker down | Result |
|---|---|
| Controls, state, device list | Work until the current access token expires |
| Album art | Last cached art, or the placeholder |
| Pairing or token renewal | Blocked until the broker returns |

**Reauthorisation is a recoverable UI state.** If the broker volume is fresh,
its credential was erased, or Spotify rejects the refresh grant, authenticated
`GET /spotify/token` returns `428` with a random, five-minute authorization URL.
MUSIC covers the player with a high-contrast QR. The phone opens the ordinary
Spotify authorization flow, so Spotify owns password entry, consent, and any
2FA; the callback atomically stores the new refresh token under that bearer.
The cube polls again, receives an access token, and removes the QR. A transient
Spotify or broker error does not erase a valid stored credential or force login.

The broker's `BROKER_USERS` map is the tenancy boundary: labels exist only in
server configuration and unique bearer tokens identify cubes on the wire. No
request accepts a user ID that could be changed to read another account. DAYS
uses the same identity to keep its persisted countdown per user. The legacy
single `BROKER_TOKEN` remains as a one-user migration mode named `default`.

**The Spotify app is global; only the account and the bearer are per cube.**
`b.cfg.clientID` is one value read from `SPOTIFY_CLIENT_ID`, while credentials are
stored per user by `storeSpotifyCredential(user, refresh)`. So N cubes are one
Spotify app, N `BROKER_USERS` entries, and up to N Spotify accounts — never N
apps. The failure this prevents is worth stating plainly, because it does not
look like a configuration error: give a second cube an existing cube's
`BROKER_TOKEN` and the two collapse into one user, so the new cube opens MUSIC
playing the first cube's account and shows the first cube's countdown. Nothing
errors; the bearer *is* the identity, and it was valid.

**A Spotify app in Development mode admits five hand-added accounts**, and the
account each cube authorises with must be one of them, on the app whose Client ID
matches `SPOTIFY_CLIENT_ID`. Match that app by Client ID rather than by name. The
failure is deferred and misleading: the QR flow completes, `/callback` stores a
refresh token, `/spotify/token` returns 200 with a valid access token, and only
Spotify refuses — `403 The user is not registered for this application`, on every
endpoint including `/v1/me`. Sibling endpoints all failing is the tell that
separates it from a scope problem, where some still answer 200; an authorised but
idle account answers 204.

**One task, one HTTP handle, one command queue.** `esp_http_client` handles carry
no lock and mutate in place, so the handle is confined to the `spotify` task
(stack in PSRAM) and touch callbacks only ever enqueue. Taps are optimistic: the
icon flips immediately and the next poll confirms rather than discovers.

**Album art is decoded by the broker and rendered from PSRAM.** The broker returns
LVGL's RGB565 `.bin` format, so the device runs no image decoder at all — which
sidesteps TJpgD never populating the image cache. Ask for **exactly the size that
gets drawn**: the cover is 148 px, so `SP_ART_PX` is 148, and that is 43 KB rather
than 115 KB.

It used to land on the card, where `lv_bin_decoder` streams rows per draw chunk.
That is cheap for a full-size image and stops being cheap twice over: it is ~15
card reads per frame, and the streaming path collapses the moment the image must be
**scaled**, which the lock screen does drawing a 148 px cover into a 100 px slot.
43,824 bytes against 8 MB of free PSRAM removes the card from the write path and
the render path both, and an in-memory `lv_image_dsc_t` is the fastest form LVGL
has — no decoder, no file I/O, a direct blit.

Two things that buys, beyond speed. The fetch **swaps the image in itself** rather
than leaving it ready for the next 400 ms UI tick, which was up to 400 ms of dead
wait; it already held the lock to drop the cache. And because nothing decodes the
bytes on the way through, the header is **validated before use** — magic, colour
format and both dimensions — since a short body or a broker answering with
something else would otherwise be blitted straight to the panel as garbage.

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

### Laying out a square screen with round corners

The cover is **264 px**, up from 148, and the change that bought it was not
horizontal. It was never limited by width — it was pinned in the band between the
top button row and the track title. Moving shuffle, like and devices into a column
down the left edge, and volume into a permanent slider down the right, vacated
that band. Track and artist then moved **onto** the cover on a flat chip, because
with them in their own row the vertical stack still capped the art at ~228 no
matter how much width the edges gave back.

Two things worth copying:

- **Positions are checked against the corner arcs before they are written.** The
  panel is a 480×480 rounded rect with r=110, so a point is invalid only if it is
  inside a corner's bounding box *and* further than 110 from that corner's centre.
  Ten elements were verified against that rule in a throwaway script before any C
  was written; nothing had to be nudged afterwards.
- **The cover centres on the gap it lives in, not on the screen.** The column ends
  at x102 and the slider starts at x410, so the midpoint is x256. Screen-centring
  left a 6 px gutter on one side and 38 on the other and read as a mistake. A
  single `SP_ART_DX` offset carries the cover and everything stacked on it.

Touch targets follow [HARDWARE.md pitfall #24](HARDWARE.md#10-pitfalls-index):
76 px minimum, and the clock yields rather than the buttons shrinking.

### Lookahead: making a swipe instant

The next three tracks' covers, names and accents are fetched before they are
asked for, so a swipe draws immediately instead of waiting on a poll plus a 139 KB
download.

**The queue is summarised by the broker, and that is a memory decision rather than
a bandwidth one.** Spotify's `/me/player/queue` returns the next twenty tracks in
full — 55,569 bytes measured. cJSON allocates roughly one 64-byte node per value,
and `SPIRAM_MALLOC_ALWAYSINTERNAL=128` sends every allocation *under* 128 bytes to
internal SRAM, so ~800 nodes is ~50 KB against the 27–35 KB the device has.
Parsing it on the cube would not have been slow, it would have failed — as an
allocation storm, during a swipe. The broker returns **408 bytes** of exactly what
gets drawn, with one-letter keys because every byte of key name is a byte of
internal SRAM during the parse.

**Two properties make the cache safe rather than merely fast:**

1. The list is re-asked whenever what-comes-next may have changed: any new track
   id — including a different playlist started from the phone, which arrives here
   as nothing more than a new id — and a shuffle toggle, which reorders the queue
   without changing the current track.
2. **Promotion requires an exact art-URL match.** A stale entry cannot show the
   wrong cover; it can only cost a download. That is what makes the window between
   a change and the next refresh harmless. Matching on URL rather than track id
   also means two tracks off one album share a single fetch.

Promotion swaps buffer pointers rather than copying 139 KB. Buffers are PSRAM, not
the card — the card would hold them happily, but reading 139 KB back off FATFS
during the swipe puts the latency straight back into the moment this exists to
remove.

**Scheduling it took three attempts, and the two failures generalise.** Fetching
all three covers synchronously put 3–5 seconds of background work in front of
interactive commands on the single worker task, and the app felt broken —
*background work does not get to hold the worker that user commands run on*.
Yielding on "is the command queue non-empty" then starved it completely, because a
poll is enqueued every 3 s and the work takes longer than that, so the queue is
essentially never empty — *yield to the user's commands, not to your own
housekeeping*. It now yields to a flag raised only by user actions, and keeps
"the list is stale" separate from "slots still need covers", because conflating
those meant four broker round trips to fill three slots.

The window is six tracks deep and fills in a **burst**: on a skip the queue is
re-asked immediately and every empty slot is filled in one pass, with two guards
that are the whole design. The burst yields to `s_sp_urgent` between covers, so
the most a user command ever waits is the single download already in flight; and
it stops on the first fill that reports no progress, because a failed download
leaves its slot pending and retrying it in the same burst is an infinite loop —
one that pins the only worker task and presents as Spotify going dead at random,
nowhere near the prefetcher. Steady state is still one ~420 byte list fetch plus
**one** cover per track change — the same download rate as having no lookahead at
all.

Two bugs here cost a full evening each, and both generalise:

- **A buffer's metadata must travel with the buffer.** The queue-refresh
  carry-over moved `buf`, `accent` and `ready` into the rebuilt slot table and
  forgot `len` — so a carried cover was ready-with-0-bytes, and promoting it
  handed LVGL an empty source. The decoder refused it, the RAW dsc fell through
  to the software blender, and the panel showed nothing, with no error, on every
  cover after it (HARDWARE.md pitfall #27). Promotion now also refuses `len == 0`
  outright, whatever produced it.
- **Success may only be claimed with the work actually done.** `sp_art_show()`
  stamped "shown" even when its `ui_lock()` timed out, so the UI tick believed
  the cover was up while the object stayed hidden and nothing ever retried — an
  infinite loading ring over correct bytes sitting in RAM. The stamps moved
  inside the lock; a timed-out show now heals on the next poll, because the
  already-on-hand branch finds `ready` still false and shows again. That branch
  also re-stamps the track id: its bytes belong to *whatever is playing now*, and
  the id is what the tick compares.

The loading ring over the placeholder has **one writer** — the MUSIC tick — after
a hide from the fetch-failure path was silently undone by the tick's own gate one
frame later. It keys on the art URL: no artwork means the note sits alone, and a
failed fetch keeps the ring through its cooldown because a retry genuinely is
coming.

Not covered: *previous*. Spotify exposes no history endpoint, so back-swipes stay
as slow as they were — which is also why heavy back-and-forth swiping shows a
lower cache hit rate than forward listening.

### Now playing on the lock screen

The lock screen grows a transport panel when Spotify has an active device, and is
untouched when it does not. Three gates make it affordable:

0. **Polling stops with the screen, in MUSIC too.** This was originally gated for
   the lock screen only, and MUSIC kept polling with the panel dark — which broke
   §7b's own rule and had a real cost: a 139 KB cover still in flight when the
   panel dozed was cut off mid-body, because Wi-Fi drops to `WIFI_PS_MAX_MODEM` at
   the same moment.
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

**Dismissal survives lock and home.** Swiping the transport panel away keeps it
hidden across later lock-screen builds, track changes, and playback restarts.
Entering MUSIC is the sole reset: it expresses renewed Spotify intent, so the
panel may appear the next time the cube is locked. The flag is session state in
RAM, not a saved preference.

## Wi-Fi setup from a phone

Typing a WPA2 password on a 480x480 panel with an on-screen keyboard was the
worst interaction this device had. It is now done from a phone over Web
Bluetooth: the phone renders the scan list with a real keyboard under it and
hands credentials back over an encrypted GATT link. The old WI-FI app and its
on-screen keyboard are gone entirely — 278 lines and thirteen widgets — which is
also what returned the drawer to a clean 2x2.

**The radios take turns.** BLE needs ~26 KB of internal SRAM while running and a
session leaves ~42 KB free *only because Wi-Fi is fully deinitialised first*
(HARDWARE.md §7g). So a pairing session is: scan -> `esp_wifi_deinit()` -> BLE ->
hand off -> re-init -> join. `wifi_init()` is split for this: `wifi_init_once()`
holds everything that must happen exactly once per boot (netif, event loop,
handlers) and `wifi_driver_up()`/`wifi_driver_down()` cycle around it. Creating a
second netif leaks the first and then asserts, so that split is not cosmetic.

**Wi-Fi is restored when a session ends for ANY reason** — handed off, timed out,
stopped, phone walked away, or the stack refusing to start — through a single
block that every exit route funnels into. Restoring only after a successful
hand-off would strand the cube offline until reboot every time someone opened
pairing and wandered off, which is the likeliest way this gets used wrong.

**Known networks.** The device used to remember exactly one, so "saved" could
only ever mean the network it was last on. It now keeps eight `{ssid, pass}`
pairs in NVS — not on the card, which is removable and these are plaintext —
seeded at boot from the boot credential and updated on every confirmed join.
Eviction is least-recently-joined; a plain insertion order made the network you
use daily the first casualty of one hotel. The scan list carries a per-AP flag
bit so the phone can offer to reuse a password without asking for it, and a
Forget that removes exactly one entry.

**No flash writes while the radio is up.** The BT controller executes from flash
and an erase stalls it; `SPI_FLASH_AUTO_SUSPEND` is unavailable on this board's
XMC part. Every NVS writer therefore consults `ble_prov_nvs_blocked()` and its
dirty flag survives to the next pass. Credentials are held *pending* and
committed on GOT_IP rather than on submission, so a wrong password cannot
overwrite a working one.

**Threading.** GATT callbacks run on the NimBLE host task and do nothing but
copy bytes and raise a flag; all real work happens in `ble_prov_poll()` on the
main task. Frames arrive in a **ring queue**, not a single slot: the main loop
only drains every 20 ms, and a phone that writes twice in that window — which the
page does, HELLO then SCAN — silently lost the first frame and judged the second
in its place. That one cost several rounds of debugging and presented as "wrong
code" for a correct code.

**Authentication** is ECDH P-256 plus a six-digit code shown on the panel, mixed
into HKDF so a phone that never saw the screen cannot complete the exchange;
everything after is AES-GCM. Five wrong attempts ends the session and stops
advertising — the limit is what makes six digits worth anything, so it must
close the session rather than merely refuse the frame.

**The cube shows a QR code, because knowing the URL was a prerequisite nobody
had.** Tapping "Set up / change Wi-Fi" used to display six digits and leave the
user to discover an address that appears nowhere on the device. The panel now
draws a QR beside the code, encoding the setup page with the pairing code in its
**fragment** (`.../setup/v1/#c=123456`) so the phone arrives already unlocked. A
fragment rather than a query string: fragments are never sent to a server, so the
code stays out of the static host's access log. It is no more secret than the
digits printed next to the QR, but there is no reason to hand it to a third party.

**The page is static, and it is not served by the broker any more.** It talks to
the cube over GATT and to no server at all, so the only thing it ever needed from
the broker was an HTTPS certificate for Web Bluetooth's secure-context rule.
GitHub Pages provides one, so `SETUP_URL` points there and
`.github/workflows/pages.yml` publishes it. That matters because Wi-Fi setup was
otherwise a hostage to the home server: when the broker's disk filled,
re-provisioning a cube would have been down with it, and a cube that cannot be
given Wi-Fi credentials cannot reach anything to fix itself. The broker keeps
serving its own embedded copy at `/provision` as a fallback, and
`broker/static/provision.html` stays the single source of truth — it must live
under `broker/` because `go:embed` cannot reach outside the Go module.

**The `/vN/` in the path is the GATT protocol version**, and it exists because
Pages will not let us set headers. The broker sends `Cache-Control: no-store` so a
phone cannot hold a cached page that speaks an older protocol than the cube in
front of it; on Pages that header is not ours to send, so the *path* carries the
version instead. A stale cache can then only be old-and-unused, never wrong.

**The page branches on capability, never on user-agent.** `navigator.bluetooth`
being undefined is the actual question — it is true on iOS, where Safari does not
implement Web Bluetooth and every iOS browser is Safari underneath, and equally
true in Firefox on Android, in every in-app webview, and on a desktop. Sniffing
the UA string gets all of those wrong. Where it is missing the page swaps the
Connect card for instructions to install Bluefy and copy the link, rather than
presenting a disabled button with no explanation. Note what is deliberately *not*
done: no `bluefy://` scheme goes into the QR itself. iOS Camera handles non-HTTPS
schemes unreliably, and a scan that dead-ends when the app is absent is worse than
one that lands somewhere able to explain itself.

The page is inert markup holding no secret, so it is deliberately
unauthenticated: reaching a cube needs radio range *and* the code on its screen.

## CONTROL: sized for a fingertip

CONTROL is an unbounded scrolling column of cards, each pairing a live readout
with the control that acts on it. Height is therefore free, and that turns out to
be the whole design constraint: **every control is sized for a fingertip, not for
a cursor.** 44 px sliders and switches did not reliably register on this panel —
curved glass, a noisy controller near the edges, drags read as taps and taps read
as nothing. `CFG_TOUCH_H` is 76 px, the same number the MUSIC transport arrived at
after its 46 px buttons ghost-touched, and `lv_obj_set_ext_click_area()` widens
the hit test further without disturbing the layout.

Scaling controls without scaling type looks wrong — 14 px labels beside 76 px
controls read as a desktop dialog enlarged badly — so readouts and button labels
use `lv_font_montserrat_20`, which is already compiled in and carries the same
`LV_SYMBOL_*` glyphs as the default.

**Every setting here shares one shape:** the LVGL callback
records a value and raises a flag, and the main loop does the work. That is not
ceremony. Writing NVS from a `LV_EVENT_VALUE_CHANGED` handler erases flash on
every pixel of a drag, and rewriting a value label mid-drag re-lays out the
`LV_SIZE_CONTENT` card, which moves the slider out from under the finger — a
crash the volume slider found first, which is why labels only update on
`LV_EVENT_RELEASED`.

**Brightness** has one writer, `bright_apply()`, and it is the only thing in the
firmware that issues panel command `0x51`. Everything routes through it: boot,
wake, the FOCUS dim and its restore, and the sleep path — including the sleep
path, because it change-gates against a cached level and a second writer would
desynchronise the cache and leave the next wake black (HARDWARE.md pitfall #23).
It returns `false` on a lock timeout so the wake path can retry rather than
strand the panel dark with every flag claiming the screen is on.

The floor matters more than the ceiling. `bsp_display_brightness_set(0)` blanks
the panel, and the only control that could undo that is a slider you can no
longer see — there is no other way into this device. Hence `BRIGHT_MIN`. The dim
that FOCUS applies is a *percentage* of the user's level rather than a flat 12%:
flat would make "dimming" brighter for anyone below it, and clamping instead
would dim by nothing at all at the floor, quietly retiring the feature for
exactly the people who chose a dark panel.

Honouring a chosen level at boot needed a BSP change, not an app one — the panel
is lit and holding a 600 ms delay partway through `bsp_display_start_with_config()`,
so `app_main` never gets a say in time. See HARDWARE.md §9 change 7.

**Autorotate** gates the commit in `imu_poll()`, never the poll, because FOCUS
reads orientation as input and would die silently otherwise; the held orientation
is persisted, or a reboot strands you at native with the switch still off. Both
traps are written up in HARDWARE.md §6.

**Controls that cannot act are faded, not hidden** — dimmed and non-clickable,
the same treatment MUSIC gives a transport control the endpoint refuses. A
control that vanishes makes people think something broke and go looking for it.
That covers the rotation-calibration button while autorotate is off (it applies a
rotation immediately, which would contradict the switch) and the autorotate
switch itself on a board with no IMU. `LV_STATE_DISABLED` alone is not enough:
LVGL's indev does honour it, but local styles beat the theme's grey, so the fade
has to be explicit. `IMU_FORCE_ABSENT` exists to make that branch testable in one
build cycle rather than never.

**That fade must not be applied to a slider, and the desk-clock delay slider is
where that surfaced.** It would have been the obvious treatment for a control
whose feature switch is off, but `cfg_scroll_guard_cb()` owns
`LV_OBJ_FLAG_CLICKABLE` on every *registered* slider and re-asserts it on
`SCROLL_END` — so `cfg_button_live()`'s fade comes undone the first time the
column moves, leaving a control that looks dead and is not. The delay slider
therefore stays live whatever its switch says; choosing a delay before enabling
the feature is harmless and it is remembered.

Two smaller traps that the same card re-found: any button placed **under** a
slider needs an extra `margin_top` (`cfg_slider` adds 18 px of invisible hit
area below its track, `cfg_button` 6 px above itself, and the card's gutter is
14) — the lock-screen middle-key button needed it once the delay slider landed
above it. And `CFG_SLIDER_MAX` overflowing is a **silent** downgrade rather than
an error: `cfg_slider()` just skips registering, and the unregistered slider
then commits a value whenever a finger lands on it to arrest a scroll. It is
kept at 6 against four sliders for that reason.

## The bezel pop-out

Press a side key and the black bezel beside it swells into the screen, the way
iOS 18 deforms the iPhone's edge. Until this, no key press produced any visual
acknowledgement anywhere in the firmware.

**It is a circle, not a rectangle.** Each of the three keys owns a 300 px circle
parked exactly tangent to its screen edge, entirely off-panel at rest. Pressing
translates it 26 px inward, so what shows is a shallow circular segment — a
~168 px swell that tapers to nothing at both ends. A rounded rectangle was tried
first and read as a widget sliding in; the arc reads as the edge itself moving.

Three properties make it cheap enough to feel instant, and none is optional:

- **Flat black, never a gradient.** RGB565 cannot ramp a dark colour smoothly
  (HARDWARE.md §5), so a soft vignette would band no matter how it was flushed.
  Flat black is also literally unlit pixels on this panel, which is why it reads
  as bezel rather than as paint.
- **Translated, never resized or shadowed.** Same rule MUSIC's nudge follows:
  translate composes with the existing layout and allocates no transient layer.
  A `shadow_width` re-blurred per frame is the known route to 20 fps.
- **One flush.** LVGL clips invalidation to the panel, so the circle costs
  300×26 = 7,800 px, not 300×300 — inside the one-buffer budget, so it cannot
  band. See the rule in HARDWARE.md §5.

**Driven from the raw pin edge, not the debounced event.** `btn_poll()` needs
50 ms of stable level before it will classify anything, and the loop ticks every
20 ms, so a `BTN_SHORT` is 50–70 ms late — enough to feel like lag rather than
cause. `btn_t.raw_edge` reports the first poll on which the pin changed, ≤20 ms.
A contact bounce can flash a lobe once; it is self-clearing and worth it.

**PWR was thought to be asymmetric, and it is not.** This section previously
said the AXP2101 hands us only a *completed* short press, that there is no
press-down bit, and that "no register would change that". All three were wrong,
and the error had a cost: the lock-screen shortcut was built with a 380 ms timer
to compensate for feedback that arrived after the finger had already lifted.

The PMU identifies four PWRKEY states. `reg 0x41` bit 1 is the negative edge —
the key going down — and bit 0 is the positive edge. Both ship **disabled**,
which is why only the completed press was ever visible to a firmware that
enabled bit 3 alone. Enabling them gives the middle key the same press-down and
release as the two GPIO keys, so the lobe rises as the finger lands and the
shortcut fires on the lift. The timer is gone. See HARDWARE.md §1.

The lobes live on `lv_layer_top()`, which belongs to the display rather than to
a screen, so they survive every app teardown and lock cycle. That also makes
them the one set of widgets nothing else nulls, and any object there **must**
clear `CLICKABLE` — the top layer is above every screen, so a clickable child
would swallow the tap that unlocks the device, on all of them.

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

### The desk-clock dim: fewer lit pixels, not just dimmer ones

**It fades, and the reason is interaction rather than looks.** A panel that
drops in one frame has already happened by the time you notice it, so the only
thing left to do is wonder whether the cube is broken. A 2.4 s ramp is an
announcement with a window inside it, and a touch anywhere in that window calls
the whole thing off. The ramp is quantised to 24 steps rather than run off the
20 ms loop tick because every step is a `0x51` write taking the LVGL lock from
the main task while the lock screen's own 16 ms timer runs — pitfall #13's
contended case. Free-running would be ~88 lock acquisitions in 2.4 s. Restoring
is deliberately *not* faded: a fade out is the device announcing something, a
fade in would be the device answering your finger slowly.

**Tap-to-unlock had to go, and the dim is what killed it.** A dimmed panel
invites exactly one thing — touch it to see it properly — and that same touch
was also the gesture that threw you into the drawer. One is a glance, the other
is a decision, and they cannot share an input. Unlocking is now a **swipe up
from anywhere**; a tap does nothing but be a touch, which is enough, because
LVGL stamps `last_activity_time` on the press and that is one of the two terms
in the main loop's `idle`. The dim lifts on the next 20 ms pass without
`lock_tap_cb` knowing the dim exists. The bottom-edge rule `gesture_home_cb`
applies everywhere else is deliberately not applied here: on an app screen the
edge separates "go home" from a drag inside content, and the lock screen has no
content to confuse it with.

**Burn-in is the AMOLED-specific risk, and the blackout made it worse before it
made it better.** A desk clock holds the same digits in the same pixels for
hours, which is the textbook OLED case; the wallpaper at least used to reshuffle
everything underneath on each lock build, and hiding it leaves nothing but
static elements. So while dimmed the content walks a slow eight-point ring at
±4 px, one step a minute. 4 px because burn-in is driven by the *edges* of a
static luminance step, and moving a glyph by a third of its stroke width is what
stops one column of pixels carrying the whole duty cycle.

That drift is applied to the screen's **children, never to the screen** — see
[HARDWARE.md pitfall #36](HARDWARE.md#10-pitfalls-index). A screen has no
parent, and `lv_obj_refr_pos()` returns before it ever reads `translate_x/y`, so
the obvious one-line version is a silent no-op that no hardware test could have
caught. Walking the children keeps what made a whole-screen shift attractive:
everything moves together so nothing drifts out of alignment, and a widget added
later joins in without knowing.

**The clock also goes amber while dimmed** (`0xE8FBFF` → `0xF59E0B`). AMOLED
power is per-subpixel; blue is both the least efficient primary and the fastest
to age, and near-white drives all three near full. Total subpixel drive falls
738 → 414 for the same legibility. It is also already what desk-clock mode means
on this device — `s_lock_ao_ring` is the same amber. The normal colour is
recorded into `s_lock_time_col` at build time rather than duplicated as a
constant, so retuning the clock cannot leave it coming back the wrong shade.

**Open defect: the now-playing transport renders malformed while dimmed.**
Captured with `CFG_DIM_SNAP`, two frames from one run seconds apart on the same
screen: undimmed the transport is clean — a correct pause glyph, three round
buttons on the card's translucent panel; dimmed, the play glyph is distorted,
a second green shape appears beside it, and the buttons carry dark notches. It
is not capture corruption — all 1200 chunks of that frame arrived, only the
final partial one was padded — and it is not pre-existing, because the undimmed
frame from the same run is clean. The dim touches only two things on this
screen, the wallpaper's HIDDEN flag and the clock's colour, so the likely
mechanism is the card's translucent backgrounds compositing over the screen's
own black rather than over the wallpaper. Seen once and not yet reduced; the
shots are the evidence to start from.

**None of this is measured.** The saving is argued from how the panel works, not
from evidence, and HARDWARE.md §7b is deliberately untouched because it opens
"All measured on hardware". The number that would settle it is mV/hour from
`pwrlog3.csv`: one hour always-on dimmed against one hour always-on bright.



Always-on is the only mode that can hold this panel lit for hours, and on an
AMOLED that bill has two lines on it: emission per lit pixel, and how many
pixels are lit. Dimming alone only pays the first. So after a configurable idle
delay the dim does both — it drops `0x51` to **12% of the user's brightness**,
and on the lock screen it **hides the wallpaper**, leaving the screen's own
`0x000000` background. Those pixels are then genuinely off, not "a photo drawn
darker", and what is left emitting is the clock, the date, the battery text and
four hairline arcs. Dropping 230,400 photo pixels to a few thousand glyph pixels
is the larger of the two savings and it is the one only this panel technology
offers.

The blackout is lock-screen-only, and it is scoped by construction rather than
by an `if`: `s_lock_wall` is NULL on every other screen, so a cube parked on
MUSIC by always-on gets the brightness half only. That is the right answer
anyway — its content is the thing you asked to keep looking at. The now-playing
card is likewise left alone: the wallpaper is decoration, the card is
information, and a desk clock that hides what is playing is a different feature.

Four things about it are worth not re-deriving:

- **The depth is a percentage of the user's level**, not a flat constant and not
  a `min()`, for the reasons written out at the FOCUS dim. The constants are the
  same pair (12 / 3) because that is what was actually checked through this
  cover glass, not because the two features were assumed identical.
- **The delay is a stops table** — 10, 20, 30, 60 s, 2, 5, 10 min — persisted as
  **seconds**, with the slider index derived at the UI boundary. A seconds
  slider that can read "37" is worse than seven detents, and storing the index
  would silently redefine every saved setting the day a stop is added.
  Default 60 s, which is `AUTO_LOCK_MS` — the threshold this firmware already
  treats as "nobody is here". FOCUS waits 120 s because you are sitting in front
  of it; a desk clock is finished with you the moment you look away.
- **Default ON.** The mode's only switch is a right-key hold on the lock screen,
  so the person who turns always-on on has no reason to open CONTROL, and a
  battery feature nobody finds is not a feature.
- **The restore is one expression, not a set of exit rules.** The main loop
  re-evaluates `dim enabled && always-on && screen on && not FOCUS && idle >
  delay` every 20 ms pass, so "every exit path restores" is structural instead
  of a list somebody has to keep complete. `screen_toggle_power()` and
  `app_open()` clear it explicitly on top of that, because both destroy
  something the dim was holding.

**FOCUS wins on `APP_POMO`, and the exclusion is explicit.** It is not enough
that a running session pins activity: `POMO_IDLE` does not, so on a cube parked
on an unstarted FOCUS both timers run down on the same screen and each pass
would write the other's level to `0x51` forever — pitfall #23's shape with
brightness as the contended output. FOCUS keeps that screen because its dim is
the one watching the accelerometer; this one only watches idle.

Ordering inside the transition is load-bearing and looks wrong: brightness goes
**down first, then** the wallpaper is hidden, and on the way back the wallpaper
is repainted **while still dim** and only then is the level raised. Hiding or
showing a full-screen image invalidates the whole viewport, which lands as
fifteen sequential strips with no tear gate (HARDWARE.md §5); doing it under a
3-12% panel is what makes the sweep invisible. It is the same trick
`rotation_apply()` uses, and it calls `lv_refr_now()` for the same reason —
an asynchronous repaint would land on the wrong side of the brightness write.

Telemetry lands in `/sdcard/logs/pwrlog3.csv` once a minute and on every state
change — the device cannot log to USB while on battery, which is exactly when
the numbers matter. Track **mV per hour**; percentage moves far too coarsely to
show an improvement over an hour.

### Battery care

A cube on a desk is a cell held at 4.2 V forever, which is the fastest way to
wear one out — roughly 300-500 cycles there against 1200-2000 at 4.0 V. CONTROL
offers three charge targets, stated to the user as **100%**, **90%** (the
default) and **80%** — internally 4.2 V, 4.1 V and 4.0 V.

**Those percentages are derived from the volts, not written down beside them.**
`chg_mode_pct()` computes them from the mode's own CV target on the firmware's
voltage scale (HARDWARE.md §7: 3.30 V ~ 0%, 4.20 V ~ 100%), rounded to the
nearest five. They used to be a hand-written table reading 75/85/100 while a
comment two lines above it admitted 4.10 V reads ~87% — the label disagreed with
the hardware by four points, in writing, and nobody noticed until a user asked
how a cube capped at "85%" could be showing 86%.

That error was not cosmetic; the charge countdown inherited it. Counting down to
the *label* meant `remain = 85 - gauge` went negative while the charger was
still working, so the caption withdrew several minutes early — and made one of
its own tests unobservable, since "the caption disappears when charging
terminates" could never be seen when the caption was already gone. The countdown
now measures the distance in **millivolts against the CV target** and converts
at 9 mV/point, which is what the PMU actually terminates on. Two independent
fixes for one root cause: a percentage that was a second copy of a fact the
volts already carried.

The control reads **"charge limit  90%"** with a one-line reason under it
("stops early, less battery wear"). Three labelling attempts got there, and the
failures are the useful part:

- **"balanced 4.1V"** — asks the reader to learn a mapping before the setting
  means anything. Volts are an implementation detail; keep them in the log line.
- **"charge to 90%"** — states a number without saying it is a *ceiling* or why
  anyone would want one. A setting that needs a manual is mislabelled.
- The slider originally ran Full→Lifespan, so **dragging right lowered the
  target**. Right-means-more is not negotiable on a control sitting under a
  percentage; `chg_mode_to_slider()` inverts at the UI boundary rather than
  renumbering the enum, which would change the meaning of a stored `chgmode`.

Also: `cfg_slider` adds 18 px of invisible hit area below its track and
`cfg_button` adds 6 px above itself, which is more than the card's 14 px gutter —
finishing a drag near the bottom of the slider fired the button underneath. Any
button placed under a slider in a card needs an extra `margin_top`.

**The PMU does all of it.** Facet writes one register — the CV target, `0x64` —
and the AXP2101 runs the cycle itself: charge, terminate, open BATFET so the
cube runs on USB with the cell disconnected, and resume unaided once VBAT drifts
100 mV below the target. There is no charge-inhibit bit, no state machine and no
threshold polling; the hysteresis is in silicon. See HARDWARE.md §7.

Three consequences worth knowing:

- **"Charging" and "plugged in" stopped being the same question.** With a cap in
  force the charger finishes hours before the cube leaves the dock, so the UI
  reads `s_vbus` (reg `0x00` bit 5) for presence and shows charging / on USB -
  bypass / plugged as three distinct states. The lock-screen bolt means *on the
  charger*, not *current flowing*.
- **The cap is re-asserted every poll, not written at boot.** `0x64` survives an
  ESP32 reset but not a PMU power-on reset, and the datasheet warns the charger
  is re-enabled on adapter insertion. One extra register read every two seconds
  on a bus already in use buys a setting that cannot silently lapse.
- **Lowering the cap does not drain a full cell** — the AXP2101 has no force
  discharge, and under bypass the only drain is self-discharge. The setting takes
  effect from the next charge onward.

"CHARGE TO 100% ONCE" arms a single 4.2 V cycle that reverts itself at charge-done
and survives a reboot. It is also the only way to re-calibrate the fuel gauge,
which needs a complete cycle it otherwise never gets.

**The charge countdown counts to the limit, and that is the whole feature.** The
AXP2101 reports no time-to-full, so `chg_eta_track()` builds one from two
different measurements, and keeping them straight is the thing to understand
here. **Rate** comes from the gauge: milliseconds per `s_batt_pct` point,
smoothed, because that is the signal that moves in usable steps. **Distance**
comes from the ADC: millivolts to the CV target, at the 9 mV per point the
firmware's own voltage scale implies. Rate is a gauge quantity, distance is a
voltage one, and they are deliberately not the same number.

That split is why the caption and the ring **can** disagree — an earlier version
of this paragraph claimed they never could, which was true only while distance
was also measured in gauge points. Counting in gauge points made the countdown
vanish when the gauge crossed the label rather than when charging actually
stopped, because the two cross at different moments; counting in millivolts ends
it where the charger ends. The visible cost is that a cell can read 87% under a
caption saying "TO 90%", which is the gauge and the voltage scale disagreeing by
a couple of points near the top of the curve, not a bug in either. It reads
"1H 05M TO 90%" on the lock screen and takes
over the CONTROL drain line while charging, where the drain figure was not merely
uninteresting but unmeasurable and read "measuring..." for the whole of every
charge.

Naming the target in the string is not decoration. A capped cube stops at 90%,
so a bare "1h 05m" under a 62% ring reads as time to *full* — a moment that never
arrives, which is precisely what the cap exists to prevent. The target comes from
`chg_target_pct()`, one definition shared with the CONTROL card, and the one-shot
overrides it exactly as `chg_cv_code()` resolves the same question in volts.

Four properties keep it honest, and each answers a way the naive version lies:

- **Each 1-point step is one rate sample, smoothed rather than averaged over the
  session.** A session average cannot see the CV taper and would still promise
  "6 min" half an hour after the current started falling.
- **Only a single-point step counts as charge.** Polling is every 2 s and no real
  rate covers two points in that, so a bigger jump is the gauge correcting itself
  — dividing it out would fabricate a rate several times the true one. The
  correction re-anchors and keeps the rate already learned.
- **The first interval is discarded.** It starts wherever the cube happened to be
  inside a point, so it is short by an unknown amount and would seed the whole
  estimate optimistic. Nothing is shown until a second step lands, which is why a
  fresh charge displays nothing for the first couple of points.
- **A point in progress is charged for the time it has already taken**, and once
  that exceeds any rate we would have learned the estimate is *withdrawn* rather
  than inflated. Without the first half the number parks and counts nothing down;
  without the second, a stalled charge grows an ever-larger figure nobody should
  plan around.

It is computed on the main task and published as one plain `int`
(`s_chg_eta_mins`, -1 = no honest answer). The UI reads it from the LVGL task,
and a 64-bit timestamp shared across the two can tear on a 32-bit core and paint
a garbage frame.

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
