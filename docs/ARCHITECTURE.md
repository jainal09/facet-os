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
| lock screen, screen on | tap | drawer (home), never back to the previous app |
| lock screen | right hold | toggle always-on (desk clock) |
| FOCUS, session running | `lv_display_trigger_activity()` each tick | never idles out; dims via brightness instead |
| FOCUS, session finished | — | stays put: DONE holds the screen on (always-on or not) until a tap starts the next session or the user navigates away |

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
real keyboard are better tools than a 480 px touch panel. The page sends an
authenticated date and 48-character message to `/countdown`; the server stores
one small JSON document with temp-file-plus-rename replacement. The public page
contains no saved state and every state read or write still requires the device
bearer.

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

The phone page is served by the broker at `/provision`, because Web Bluetooth
requires a secure context and that service already has a real certificate behind
Tailscale Funnel. It is inert markup holding no secret, so it is deliberately
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

**PWR is asymmetric and cannot be fixed.** The AXP2101 hands us a *completed*
short press over I2C — there is no level register and no press-down bit — so the
middle lobe can only pop and retract after the key is already back up. It reads
a beat behind the other two. No register would change that.

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

Telemetry lands in `/sdcard/logs/pwrlog3.csv` once a minute and on every state
change — the device cannot log to USB while on battery, which is exactly when
the numbers matter. Track **mV per hour**; percentage moves far too coarsely to
show an improvement over an hour.

### Battery care

A cube on a desk is a cell held at 4.2 V forever, which is the fastest way to
wear one out — roughly 300-500 cycles there against 1200-2000 at 4.0 V. CONTROL
offers three charge targets, stated to the user as **100%**, **85%** (the
default) and **75%** — internally 4.2 V, 4.1 V and 4.0 V.

The control reads **"charge limit  85%"** with a one-line reason under it
("stops early, less battery wear"). Three labelling attempts got there, and the
failures are the useful part:

- **"balanced 4.1V"** — asks the reader to learn a mapping before the setting
  means anything. Volts are an implementation detail; keep them in the log line.
- **"charge to 85%"** — states a number without saying it is a *ceiling* or why
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
