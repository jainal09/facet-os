# The board: Waveshare ESP32-S3-Touch-AMOLED-2.16

Ground knowledge for this board. Everything here was verified on real hardware,
mostly by hitting the problem first. **Read [§10 Pitfalls](#10-pitfalls-index)
before debugging anything** — most "impossible" symptoms on this board are in
it, indexed by how the symptom actually appeared.

Facet's own design lives in [ARCHITECTURE.md](ARCHITECTURE.md).

> ## Keep this file hot — maintenance is expected
>
> This is a living document, not an archive. Anyone (or any agent) who touches
> this board should leave it more useful than they found it, **in the same
> session the lesson is learned**.
>
> Write here whenever you:
> - burn more than a few minutes on a symptom — record symptom → cause → fix,
>   and phrase the symptom the way it actually *appeared*, so it is greppable
> - discover a register, pin, magic value, or config flag that matters
> - find that something here is wrong or out of date — correct it in place and
>   delete the stale claim rather than layering a caveat on top
> - find a vendor or reference implementation that settles a question, so the
>   next person copies instead of re-deriving
> - make a mistake worth not repeating, including your own. Several entries
>   below are self-inflicted; those are the most valuable ones here.
>
> Prefer correcting an existing section to appending a new one. Keep the
> pitfalls index short, blunt, and searchable by symptom.

## 1. Hardware map

| Part | Detail |
|---|---|
| MCU | ESP32-S3 (QFN56) rev **v0.2**, dual core, 240 MHz |
| PSRAM | **8 MB OCTAL** (AP_3v3) — quad mode will not init |
| Flash | 16 MB quad (XMC, mfr 0x20 dev 0x4018), 3.3 V per eFuse |
| USB | On-chip **USB-Serial/JTAG** (VID 0x303A PID 0x1001) — no bridge chip, no driver install; enumerates as `/dev/cu.usbmodemXXXX` on macOS |
| Display | **CO5300** AMOLED, 480×480, QSPI |
| Touch | **CST9220** (driver component `waveshare/esp_lcd_touch_cst9217`, CST92xx family) |
| PMU | **AXP2101** (I²C 0x34) |
| IMU | **QMI8658** 6-axis (I²C 0x6B; 0x6A is the alternate SA0 address) |
| RTC | **PCF85063**, battery-backed (I²C 0x51) |
| Audio | ES8311 DAC + speaker, ES7210 ADC + two onboard mics, PA on GPIO46 — see §7e |

### Pin map (from the BSP header)

```
I2C      SCL 14   SDA 15                (shared: touch + AXP2101 + QMI8658 + RTC)
LCD      CS 12  PCLK 38  D0 4  D1 5  D2 6  D3 7   RST 39
Touch    RST 40   INT 11
SD       D0 3   CMD 1   CLK 2
I2S      SCLK 9  MCLK 42  LCLK 45  DOUT 8  DSIN 10   PA enable 46
```

### Side keys — read the label on the back, don't guess

```
leftmost  = BOOT / minus  -> GPIO0     (strap pin; ordinary pulled-up input after boot)
middle    = PWR           -> AXP2101 PWRKEY, NOT a GPIO — read it from the PMU
rightmost = plus          -> GPIO18
```

This is the opposite of the intuitive guess and cost real debugging time. GPIO0
is safe as a user key; only holding it **through a reset** enters download mode.

Reading PWR: enable the short-press IRQ (reg `0x41` bit 3), then poll status reg
`0x49` bit 3 and **write 1 to clear**. Leave long-press alone — the PMU may act
on it itself.

## 2. Toolchain

- ESP-IDF **v5.5.5** works. On macOS with Python 3.14, `install.sh` silently
  skips cmake and ninja → `python3 $IDF_PATH/tools/idf_tools.py install cmake ninja`.
- esptool v5 uses hyphenated subcommands (`chip-id`, `flash-id`, `write-flash`);
  v4 used underscores.
- Full flash backup: `esptool read-flash 0 0x1000000 backup.bin` (~95 s).
  Restore with `esptool write-flash 0 backup.bin`.

## 3. sdkconfig that actually works

The full, commented set is in [`sdkconfig.defaults`](../sdkconfig.defaults). The
entries that are load-bearing:

```ini
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y                 # REQUIRED — this board is octal
CONFIG_SPIRAM_SPEED_40M=y                # NOT 80M — see Pitfalls #1
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=128  # LVGL objects (~150-250 B) go to PSRAM
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y

CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
CONFIG_PARTITION_TABLE_CUSTOM=y          # 4 MB app slot; the 1 MB default is too small

CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y      # TLS sessions (~40 KB) into PSRAM
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_BSP_ERROR_CHECK=n                 # init failures return instead of abort

CONFIG_LV_USE_CLIB_MALLOC=y
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=2        # 1 unit + LV_USE_OS gives fps=0.0
CONFIG_LV_USE_PERF_MONITOR=n             # otherwise LVGL paints its own grey box
CONFIG_LV_CACHE_DEF_SIZE=3145728         # defaults to 0 — see §7c

CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
# CONFIG_PM_DFS_INIT_AUTO is NOT set     # see Pitfalls #2
```

**Deliberately not set**, both inherited from the vendor demo and both harmful:

- `SPIRAM_FETCH_INSTRUCTIONS` / `SPIRAM_RODATA` / `SPIRAM_XIP_FROM_PSRAM` — see
  Pitfalls #1. Disabling XIP also **returns ~1.2 MB of PSRAM**, since `.text`
  and `.rodata` no longer live there.
- `ESP32S3_INSTRUCTION_CACHE_32KB` / `DATA_CACHE_64KB` — the vendor's cache
  config costs **48 KB of internal SRAM** and sinks the whole budget.

## 4. Memory budget — internal SRAM is the only scarce resource

Typical healthy figures with display + Wi-Fi + an app running:

| | |
|---|---|
| Internal SRAM free | **~25–35 KB** idle, ~22 KB with a heavy app open |
| Min watermark during a TLS handshake | ~21 KB (was 16 **bytes** before tuning) |
| PSRAM free | ~7.3 MB of 8 MB |
| App binary | ~1.5 MB in a 4 MB partition |
| Unallocated flash | **~11.9 MB** — room for OTA slots or a filesystem |

Rules of thumb: an LVGL screen costs ~5 KB internal; a VPN stack costs ~42 KB of
permanent task stacks; a TLS session costs ~40 KB but goes to PSRAM if
`MBEDTLS_EXTERNAL_MEM_ALLOC` is on.

Watch `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)` — the *minimum* is
what kills you, not the current free.

## 5. Display and LVGL

- **Draw buffers must live in INTERNAL SRAM.** `esp_lvgl_adapter` bounce-copies
  PSRAM draw buffers through internal DMA staging on *every* flush, which
  collapses into an `ESP_ERR_NO_MEM` storm and 2–5 fps. 32-row double buffers
  (2 × 30,720 B) is a good default.
- Set the SPI bus `max_transfer_sz` to **one draw buffer**, not a full frame.
  The BSP defaults to 480×480×2 = 460,800 B and over-allocates DMA descriptors.
- `lv_obj_create()` objects are **clickable by default**. A full-screen scene
  must `lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE)` on every decorative shape
  or they silently swallow taps. Gestures still bubble.
- Reference frame rates: ~66 fps for a normal UI, ~58–64 fps for a busy animated
  scene. If a scene drops to ~20 fps, look for a large `shadow_width` being
  re-blurred every frame, and only touch widget properties when the value
  actually changed.
- **`lv_obj_set_style_transform_rotation()` does rotate label TEXT** in LVGL
  9.3, contrary to the common claim that transforms are image-only: the widget
  is snapshotted to an off-screen layer and that bitmap is rotated
  (`lv_refr.c`), so glyphs come out correctly. Units are 0.1 degrees, clockwise.
  Set `transform_pivot_x/y` to the object's centre or it spins about its corner.
  Two costs: a rotated widget allocates an un-chunked transient layer sized to
  its bounding box on every redraw, so keep rotated objects small; and rotation
  spins a widget **about its own centre only** — its offset from its parent does
  not rotate, so a stack of rotated labels needs its offsets rotated by hand or
  they collide.
- **RGB565 cannot render a smooth dark gradient.** A dark blue vertical ramp
  shows as hard bands. Use flat black — which on an AMOLED also means those
  pixels are simply off.
- **`lv_obj_fade_out()` does not delete anything.** It animates opacity to
  transparent and stops (`lv_obj_style.c`), so a "transient" toast built this
  way silently accumulates invisible objects on the screen. Pair it with
  `lv_obj_delete_delayed()`. That is safe even if the screen is torn down first:
  `lv_obj_destructor` calls `lv_anim_delete(obj, NULL)`, which cancels the
  pending delete along with the object.
- The panel has heavily rounded corners and curved cover glass. Content within
  ~55 px of an edge is clipped or unreadable at an angle; keep circles ≤ 430 px.

App-switching rules are in [ARCHITECTURE.md](ARCHITECTURE.md#the-app-model) —
in particular, free the outgoing screen *before* building the next one.

## 6. Rotation (autorotate)

The panel is square, so rotation never changes resolution and nothing needs
re-laying out.

- The CO5300 init sequence programs **MADCTL = 0xA0** (swap_xy + mirror_y). That
  is the panel's *native* state and the BSP's touch flags match it.
  **`bsp_display_rotation_set(BSP_DISPLAY_ROTATE_0)` writes `0x00`** — its enum
  is one quarter turn off from where the panel starts. Express rotation as
  **quarter turns from native**.
- Don't hand-derive the table — use Espressif's
  `lvgl_port_disp_rotation_update()` in `esp_lvgl_port` as the reference. With
  native flags (swap=1, mx=0, my=1) it gives:

  | turns | swap | mirror_x | mirror_y | MADCTL |
  |---|---|---|---|---|
  | 0 (native) | 1 | 0 | 1 | 0xA0 |
  | 90 | 0 | 1 | 1 | 0xC0 |
  | 180 | 1 | 1 | 0 | 0x60 |
  | 270 | 0 | 0 | 0 | 0x00 |

- **Panel and touch take the same flags** — both describe the LVGL-space ↔
  physical-space mapping.
- Drive the panel with `esp_lcd_panel_swap_xy()` / `esp_lcd_panel_mirror()`,
  **never raw MADCTL writes**: the driver must transpose the address window
  while the panel transposes the pixel stream. Skipping the driver half
  misplaces *partial* updates once rotated.
- After rotating, `lv_obj_invalidate(lv_screen_active())` — the existing frame
  is scrambled.
- For QSPI panels LVGL 9 software rotation is unavailable and `esp_lv_adapter`
  refuses rotation, so hardware swap/mirror is the only route.
- **Diagnostic:** "two of the four orientations are wrong" means the rotation
  *direction* is inverted, not offset. Flipping handedness leaves 0° and 180°
  correct while swapping 90° and 270°. An offset knob can never fix it.
- How the IMU is mounted relative to the panel is not documented anywhere. Make
  it a saved calibration covering offset (4) × handedness (2) = 8 states rather
  than guessing.

IMU setup: probe 0x6B then 0x6A, `WHO_AM_I` (0x00) = 0x05; `CTRL1(0x02)=0x40`
auto-increment, `CTRL2(0x03)=0x06` accel ±2 g @ 125 Hz, `CTRL7(0x08)=0x01` accel
on / gyro off; accel data at `0x35` (6 bytes LE), ±2 g = 16384 LSB/g.

Two independent gates are needed, and the second is easy to miss:

- **Magnitude:** require ~0.4 g of tilt (6500 LSB) before considering a change
  at all, or a device lying flat rotates on noise.
- **Margin:** require the dominant axis to lead the other by ~0.18 g (3000 LSB),
  and keep the current orientation when neither does. Held near 45° the two axes
  trade places on noise, and a vote counter **cannot** fix this — it will
  happily count eight consecutive samples of the wrong answer. Symptom: the
  screen flips back and forth every few seconds while the device sits still on
  a desk at an angle.

## 7. AXP2101 PMU

- **Charge current powers up at 25 mA** (reg `0x62` = 0x01), which looks exactly
  like "the battery never charges". Waveshare's own example sets **400 mA**
  (step 10): `reg 0x62 = (old & 0xE0) | 10`. CV reg `0x64` = 0x03 = 4.2 V is
  already correct.
- The fuel gauge reads 0% until **three** separate enables: `0x18` bit 3 (gauge
  module), `0x68` bit 0 (battery detect), `0x30` bit 0 (voltage ADC). Percentage
  at `0xA4`; battery voltage at `0x34`/`0x35` as `((hi & 0x1F) << 8) | lo` mV.
  Battery-present is `0x00` bit 3; charging is `0x01` bits[6:5] == 01.
- Voltage is a good sanity check and fallback: 3.30 V ≈ 0%, 4.20 V ≈ 100%.

## 7b. Power management and idle drain

Idle drain is dominated by things that never stop unless you stop them. All
measured on hardware:

- **`bsp_display_backlight_off()` does NOT turn the panel off.** It writes
  brightness 0 (cmd `0x51`), which stops emission but leaves the CO5300 driver
  IC scanning. Use `esp_lcd_panel_disp_on_off(panel, false)` (cmd `0x28`) — the
  CO5300 driver implements it.
- **`bsp_display_brightness_set(percent)` is a real 0-100 control**, not just
  on/off — it writes panel command `0x51` with `percent * 255 / 100`. Use it for
  graduated dimming; use `esp_lcd_panel_disp_on_off()` when you actually want the
  panel to stop scanning. `backlight_on`/`backlight_off` are just
  `brightness_set(100)` and `(0)`.
- **Stop LVGL redrawing what nobody can see.** A once-per-second clock label was
  causing a full render + QSPI flush every second with the screen dark. Guard
  periodic UI timers on the screen-power flag.
- **Stop sensor polling that feeds an invisible output** — don't sample the IMU
  for autorotate while the panel is off.
- **Wi-Fi:** default is `WIFI_PS_MIN_MODEM`; `WIFI_PS_NONE` (sometimes set for
  low-latency work) keeps the radio fully on and is a large drain. Use
  `WIFI_PS_MAX_MODEM` while idle — the station stays associated and reachable,
  it just sleeps between DTIM beacons. Cost: HTTPS latency rises from ~1.5 s to
  ~4 s while dozing, which is fine for background polling.
- **DFS:** `esp_pm_configure()` with `max_freq_mhz` 240 active / 80 idle, and
  **`min_freq_mhz = 80` always** (Pitfalls #2 — lower kills the USB console).
- Stretch background network polling while idle.
- Not attempted yet: automatic light sleep (would reach single-digit mA but
  drops the USB console while asleep), and disabling unused AXP2101 rails —
  risky without knowing which rail feeds the panel and I²C on this board.

Measure, don't guess: log battery mV and derive mV/hour. Percentage moves far
too coarsely to see an improvement over an hour. Facet writes a row to
`/sdcard/logs/pwrlog.csv` every minute and on every power-state change.

## 7c. Images, assets and the SD card

Pipeline that works: fetch on demand in the background (the main task, never the
UI task), cache to the card, decode from the card when shown. Nothing preloaded.

- **Stream downloads through the HTTP event callback** into a `.part` file and
  rename on success. The image then never exists whole in RAM — only the
  client's small receive buffer — and a failed download cannot leave a corrupt
  file behind. Verified: a 100 KB image with internal heap steady at ~22 KB.
- **Ask for the exact panel size** (`w=480&h=480&fit=crop`) so you never
  download pixels you cannot show.
- **Progressive JPEG is the default from imgix/Unsplash and TJpgD cannot decode
  it** — it fails *silently*, yielding a 0×0 image. Use PNG with
  `CONFIG_LV_USE_LODEPNG=y`; 400 KB is nothing against 32 GB.
- **Unsplash `urls.raw` already carries query params**, including an
  `auto=format` that silently overrides an appended `&fm=png`. Cut the URL at
  `?` and add your own transform, or you get progressive JPEG in a `.png` file.
- **`CONFIG_LV_CACHE_DEF_SIZE` defaults to 0**, meaning a file-backed image is
  re-decoded *for every partial draw strip* — 15 full decodes per frame with
  32-row buffers, so a wallpaper visibly paints band by band like dial-up. Give
  it a few MB; it lives in PSRAM.
- **Replacing a file at a path LVGL has already decoded needs
  `lv_image_cache_drop(path)`**, or it keeps serving the old bitmap and the new
  wallpaper never appears. It is declared in
  `src/misc/cache/instance/lv_image_cache.h`, which `lvgl.h` does not include.
- **TJpgD never puts anything in LVGL's image cache**, so `LV_CACHE_DEF_SIZE`
  cannot help a JPEG at all. `lv_tjpgd.c` never calls
  `lv_image_decoder_add_to_cache()` — LodePNG does. With 32-row draw buffers a
  full redraw is 15 passes, and a JPEG re-runs a **complete decode on every
  one of them, every frame, forever**. That is the dial-up wallpaper symptom
  again, except no cache size fixes it. TJpgD also decodes baseline only, outputs
  RGB888 (a further conversion to the panel's RGB565 at blit time), and has
  descaling compiled out (`JD_USE_SCALE 0`).
- **`espressif__esp_lv_decoder` is already vendored and compiled, but inert.** It
  wraps `esp_new_jpeg` (baseline JPEG, ~10 KB fixed scratch), libpng and QOI, it
  **does** register with the image cache, and it explicitly allocates its buffers
  from PSRAM. It is dead because `CONFIG_ESP_LVGL_ADAPTER_ENABLE_DECODER` is
  unset and nothing calls `esp_lv_decoder_init()` — confirmed with `nm` on the
  ELF, not just from config. Turn both on if JPEG is ever needed on screen.
  Neither decoder handles progressive JPEG, so the source still has to cooperate.
- **A server can skip the device's decoder entirely.** LVGL 9's binary image
  decoder is compiled in and always registered (`lv_bin_decoder.c`), needs a
  `.bin` extension, and — the part that matters — supports `get_area` for
  `RGB565`, so LVGL reads only the rows a draw chunk needs directly from the
  card. Hand it a 12-byte header (`lv_image_dsc.h`: magic `0x19`, cf `0x12` for
  RGB565, then w/h/stride as little-endian u16) followed by raw pixels and the
  device does no decoding at all, needs no decoder config, and never holds a
  decoded frame. Costs ~11x the bytes of an equivalent JPEG, which is the right
  trade when CPU and internal SRAM are the scarce resources and Wi-Fi is not.
- **Spotify's cover art is baseline JPEG** (`SOF0`, verified against a real
  `i.scdn.co` URL — 300x300 came back 27 KB). So it is decodable here without
  help. It still hits the no-caching problem above, so either enable
  `esp_lv_decoder` or serve it as PNG through the existing LodePNG path.
- **FATFS defaults to 8.3 filenames**, so creating `telemetry.csv` (9-char stem)
  silently fails. Enable `CONFIG_FATFS_LFN_HEAP=y` or keep stems ≤ 8 chars.
- **Validate cached assets on boot** (e.g. check the PNG signature). A fetch
  that only runs "if the file is missing" will otherwise keep a corrupt file
  forever.
- Debug probe that lies: `lv_obj_get_width()` right after `lv_image_set_src()`
  always reads 0×0 because layout has not run. Use
  `lv_image_decoder_get_info()` — and note `LV_RESULT_OK` is **1**, not 0.

## 7d. Real-time clock (PCF85063)

I²C 0x51, BCD registers from 0x04: sec (bit 7 = oscillator-stopped), min, hour,
day, weekday, month, year (00–99). Seed the system clock from it at boot and
write back once SNTP lands — otherwise every boot shows a placeholder for the
several seconds SNTP takes. Store UTC and let TZ handle DST. newlib here has no
`timegm()`, and `mktime()` would apply the local zone, so convert explicitly.

## 7e. Audio

The board has an **ES8311 DAC** (playback) and an **ES7210 ADC** (the two
onboard mics), both on the shared I²C bus, with a power amp enabled by
**GPIO46**. There is a real speaker and it works.

- `bsp_audio_codec_speaker_init()` does everything: `bsp_i2c_init()`,
  `bsp_audio_init(NULL)`, then returns an `esp_codec_dev` handle. Default format
  is **22050 Hz, 16-bit, mono** — match it and `esp_codec_dev_open()` never has
  to reconfigure the codec.
- **GPIO46 is not driven by the BSP.** The ES8311 driver's own `enable` callback
  raises it inside `esp_codec_dev_open()` and drops it on close
  (`es8311.c`, `es8311_pa_power`). So opening the codec clicks the amp on.
- **Measured speaker response (this board):** nothing below ~500 Hz, faint at
  500, usably present from there to **8 kHz**, no resonant buzz anywhere. Put
  fundamentals at **1.2–2.5 kHz** and let partials reach ~6 kHz. **High-pass
  everything at 500 Hz** — energy the driver cannot move does not vanish, it
  comes back as distortion.
- **`esp_codec_dev`'s default volume curve tops out at 0 dB** while the ES8311
  itself reaches **+32 dB**. `esp_codec_dev_set_out_vol(dev, 100)` therefore asks
  for unity gain and sounds far quieter than the hardware can manage — a factor
  of 40 in amplitude left unused. Install a curve with
  `esp_codec_dev_set_vol_curve()` reaching +32 dB. This is the single most
  misleading thing about the audio stack.
- **`esp_codec_dev_write()` returns when the data is queued, not when it has
  played.** Closing immediately afterwards disables the I2S channel mid-drain
  and truncates the tail, which sounds like a crack at the end of every clip.
  Keep the device open between sounds and only close after an idle period, with
  a ~120 ms delay first.
- **Cost: ~3.7 KB of internal SRAM**, measured, and the I2S DMA rings are
  `MALLOC_CAP_INTERNAL` only — they cannot go to PSRAM. That is with the
  capture channel skipped (§9 patch 6); leaving it in costs ~2.9 KB more for
  nothing if you only play audio. There is no deinit path through the BSP, so
  initialise lazily and expect to keep it.
- `esp_codec_dev_write()` blocks. Play from a dedicated task, and put its stack
  in PSRAM (`xTaskCreateWithCaps`, `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`)
  so it costs no internal SRAM.

## 7f. HTTPS latency — reuse the connection

Measured against `api.spotify.com` (no token, so a 401 with a 94-byte body —
which costs the same as a 200 everywhere that matters, and exercises a real
cert chain, cipher suite and edge node without needing credentials):

| | |
|---|---|
| New connection per call | **390 ms** |
| Connection reused | **6 ms** |
| Reused, called every 3 s for 24 s | **6 ms, every call** |

Almost all of the 390 ms is TCP plus the TLS handshake — chiefly asymmetric
crypto and certificate-chain verification on a 240 MHz Xtensa, not the request.
So `.keep_alive_enable = true` on a **long-lived `esp_http_client` handle** is
worth roughly **65x**, and it survives idle gaps, which is what makes it useful
for polling rather than only for bursts. Creating a fresh client per request
throws all of it away.

Two things to know when measuring this:

- **`esp_http_client_perform()` returns an error for a 401 carrying
  `WWW-Authenticate: Bearer`** — it treats it as an auth challenge it cannot
  answer. The headers and body have already arrived, so read
  `esp_http_client_get_status_code()` **unconditionally**; gating it on
  `perform() == ESP_OK` reports `HTTP 0` and makes a working request look broken.
- Don't generalise from one host. An earlier ~1.5 s figure came from
  `example.com` and was mostly that server, not our TLS cost.

A re-runnable bench lives behind `#define NET_BENCH` in `main.c`.

### Reusing one `esp_http_client` handle across an API

Verified against `esp_http_client.c` in IDF v5.5.5. All of this matters if you
want the 6 ms figure across a whole REST API rather than one URL:

- **Changing only the path keeps the connection.** `esp_http_client_set_url()`
  closes only when the **host or port** changes (`esp_http_client.c:1174-1210`);
  path and query never trigger a close. So one handle serves every endpoint on a
  host. `esp_http_client_set_method()` is likewise free to change between calls.
- **`keep_alive_enable` is TCP socket keepalive (`SO_KEEPALIVE`, `TCP_KEEPIDLE`
  …), not HTTP persistence.** HTTP/1.1 persistence is the default and the
  keep-open decision is made from the *server's* response
  (`http_should_keep_alive`). The speed-up comes from that, not from the flag —
  the flag only helps notice a dead socket. Easy to misattribute.
- **A POST body survives into the next request.** `client->post_data` and
  `post_len` are never cleared automatically, so a bodiless `POST` on a handle
  that last sent JSON **resends the old JSON**. Call
  `esp_http_client_set_post_field(c, NULL, 0)` first — it clears both and deletes
  the `Content-Type` header (`esp_http_client.c:1872-1891`). Headers persist
  across requests generally, which is what makes `Authorization` reuse work.
- `Content-Length: 0` is emitted automatically for POST/PUT with no body
  (`esp_http_client.c:1661-1672`); nothing extra is needed.
- **A 401 carrying `WWW-Authenticate: Bearer` makes `perform()` fail *and* skips
  draining the body.** It is treated as an auth challenge the client cannot
  answer, returning `ESP_ERR_NOT_SUPPORTED` before the body-read loops, so
  unread bytes are left on the socket and the connection is not closed. Reusing
  the handle then reads garbage. Any bearer-token API hits this on **every token
  expiry**, so it is a main path, not an edge case: on 401, call
  `esp_http_client_flush_response()` or close, then refresh and retry. There is
  no config flag to suppress it; the only clean alternative is the
  `open`/`fetch_headers`/`read` API, which bypasses the check entirely.
- **One handle is not thread-safe.** The struct carries no lock and every field
  is mutated in place. Confine it to one task — a command queue plus a single
  owning task is the shape that works.

## 8. Recovery when the board won't flash

- **A connected battery defeats "unplug USB to power-cycle".** The board keeps
  running on battery, so re-plugging USB is not a power-on and the BOOT strap is
  never sampled. A crash loop then looks unflashable.
- Order of escalation:
  1. Hold **BOOT (leftmost)**, unplug USB-C, plug back in while holding, hold
     ~2 s more.
  2. Long-press **PWR (middle) for ~10 s** to force a hard power cut, then power
     on while holding BOOT.
  3. Unplug USB **and** the battery's white 2-pin connector, then hold BOOT
     while re-plugging USB.
- Useful while waiting: poll
  `esptool --connect-attempts 1 --after no-reset read-mac` every few seconds and
  flash the instant it answers.
- **Prevention beats recovery:** make display and peripheral init failures
  non-fatal (`CONFIG_BSP_ERROR_CHECK=n`, and patch any bare `ESP_ERROR_CHECK` in
  the BSP's `bsp_display_new`) so an out-of-memory config degrades to
  headless-but-flashable instead of a crash loop.

## 9. BSP bugs patched in this fork

`components/esp32_s3_touch_amoled_2_16` is forked from
`waveshare/esp32_s3_touch_amoled_2_16` v2.0.1 with five changes. Anyone forking
it independently will need the same ones:

1. `bsp_display_lock()` returns `esp_lv_adapter_lock()`'s `esp_err_t`
   (0 = success) straight into a `bool` API where true = success — **inverted**,
   so the lock always appears to fail.
2. `spi_bus_initialize()` is wrapped in a bare `ESP_ERROR_CHECK` → aborts on
   `NO_MEM` → crash loop → unflashable. Return the error instead.
3. `max_transfer_sz` is a whole frame; size it to one draw buffer.
4. Add accessors: `bsp_display_panel_handle()` and `bsp_touch_handle()` (both
   are file-static upstream, and both are needed for rotation and for panel
   power-down).
5. Parameterise draw-buffer height and placement — the stock BSP hardcodes 50
   rows in PSRAM. See `include/ml_draw_buf_cfg.h`.
6. `bsp_audio_init()` always creates both I2S directions, so a playback-only app
   pays ~2.9 KB of internal SRAM for a capture channel it never reads. Added
   `bsp_audio_enable_rx(bool)`; `bsp_audio_codec_microphone_init()` forces it
   back on so the mics still work.

## 10. Pitfalls index

1. **Silent boot hang — the single most expensive trap on this board.** The
   bootloader prints `Loaded app from partition` / `Disabling RNG early entropy
   source` and then *nothing, ever*: no crash dump, no ROM messages, and esptool
   cannot sync either, so it looks bricked and needs a physical power cycle. It
   is **not** corrupt flash and it is **not** whatever you changed last — it
   recurs after trivial edits.
   **Root cause: `esp_psram_init()` hangs.** Diff a good boot against a bad one
   — the good one's next line after the RNG message is
   `octal_psram: vendor id : 0x0d (AP)`; the hung one never gets there. Octal
   PSRAM at **80 MHz is marginal on this board**; use
   `CONFIG_SPIRAM_SPEED_40M=y`. Nothing needs the bandwidth once draw buffers
   live in internal SRAM. Anything else that runs before console init compounds
   it — notably XIP-from-PSRAM (`SPIRAM_FETCH_INSTRUCTIONS` / `SPIRAM_RODATA`),
   which should also stay off.
2. **Anything that kills the USB-Serial/JTAG console before the first log makes
   the board look bricked.** The app blocks forever on its first write, so you
   get `Loaded app from partition` then silence, and esptool can't sync either.
   Known causes: XIP-from-PSRAM (#1) and **`CONFIG_PM_DFS_INIT_AUTO=y`**, which
   sets DFS `min_freq` to the XTAL (40 MHz) at startup — below 80 MHz the APB
   clock can no longer run USJ. With PM enabled, always pin `min_freq_mhz = 80`
   and configure DFS explicitly in code.
3. **Polling a GPIO key slowly misses presses entirely.** While dozing the main
   loop runs at 120 ms; a debounce needing two consistent samples cannot see a
   ~160 ms tap, so the key appears dead. Wake logic must test the pin **level**,
   not a debounced edge — then swallow the release so it does not also fire that
   key's action. PWR is immune only because the PMU latches it in hardware.
4. **Hardcoded serial ports in tooling.** The board re-enumerates between
   `/dev/cu.usbmodemX101` numbers across replugs. A capture script with a fixed
   port silently returns zero bytes and looks exactly like a dead board. Glob
   for it.
5. **Sentinel collisions.** A deferred-request sentinel of `-2` collided with a
   real id of `-2`, silently swallowing every request. Sentinels must not
   overlap valid values.
6. **Unguarded port in shell one-liners.** `esptool -p $(ls /dev/cu.usbmodem*)`
   with no device attached makes esptool auto-detect and start writing,
   producing a partial image and a wedged board. Always
   `P=$(ls ... | head -1); [ -z "$P" ] && exit 1`.
7. **`-Werror` at `-O2`.** `stringop-truncation` and `format-truncation` fire on
   perfectly intentional truncation. Bound conversions with `%.31s`, or suppress
   per-component.
8. **`fps=0.0` on a static screen is correct**, not a hang — LVGL only redraws
   on invalidation.
9. **A "working" build may be working because something failed silently.** A
   beautifully smooth UI once turned out to be smooth precisely *because* the
   VPN had failed to start. Always confirm the other subsystem is actually up.
10. **The capture harness can stall the USB-CDC console**, producing a long
    silent gap that mimics a firmware hang. Re-run before diagnosing.
11. **`uint8_t` coordinate tables silently wrap.** Star positions past 255 in a
    `static const uint8_t sx[]` piled up against the left edge. The compiler does
    warn (`-Woverflow`); don't let warnings accumulate to the point where a real
    one is invisible.
12. **The task WDT cannot see a deadlock in which everything blocks.** The
    device froze solid: display stuck, no panic, no watchdog, no reboot — but
    the console kept printing from a background task, so it was clearly still
    alive. The default task WDT only watches the **idle** tasks, and in a
    mutual-block every task sleeps, so both idle tasks run happily and the WDT
    is satisfied. Attaching over the S3's built-in USB-JTAG confirmed it:
    `IDLE0` and `IDLE1` both `Running`, every other task blocked. Subscribe the
    main loop explicitly with `esp_task_wdt_add(NULL)` + a per-iteration
    `esp_task_wdt_reset()`; then a stall panics with a backtrace instead of
    sitting there. Budget for legitimately slow work inside the loop (an HTTP
    fetch) by feeding the dog from its progress callback rather than by
    unsubscribing.
13. **Never drive the panel IO from two tasks.** `esp_lcd_panel_disp_on_off()`
    called from the main task races the LVGL task's `esp_lcd_panel_draw_bitmap()`
    on the same QSPI device; a lost completion callback leaves LVGL waiting on
    a flush that never finishes, holding the LVGL lock, which then blocks
    everything else. Take the LVGL lock around any panel command issued outside
    the LVGL task. Auto-sleep on an animated screen is the worst case, because
    its timer keeps flushes nearly continuous.
14. **`bsp_display_lock(-1)` in the main loop converts any LVGL stall into a
    total freeze.** Use a bounded timeout and log loudly on failure — a dropped
    UI frame is recoverable, an infinite wait is not.
15. **Debugging a live hang:** the ESP32-S3's USB-Serial/JTAG does CDC and JTAG
    at once, so `openocd -f board/esp32s3-builtin.cfg` plus
    `xtensa-esp32s3-elf-gdb -ex "target extended-remote :3333"` can attach to a
    frozen board without reflashing. Get `info threads` **and** the backtraces
    in a *single* gdb session — thread IDs are re-enumerated on every attach,
    and `monitor halt` leaves the CPUs stopped, so finish with `reset run`.
16. **A vote counter is not hysteresis.** Autorotate flipped back and forth every
    few seconds on a stationary desk despite requiring eight consecutive
    agreeing samples — near 45° the input itself is genuinely ambiguous, so the
    counter just confirms whichever wrong answer arrived first. Debounce fixes
    *noise*; it does not fix an ambiguous *decision*. Add a margin the winner
    must beat, and hold the current state when nothing wins (§6).

## 11. Debugging method that worked

- Log a one-line periodic status with everything at once: uptime, wall clock,
  active screen, idle timers, Wi-Fi, battery mV and %, fps, rotation, heap free
  / min / largest block, PSRAM free. Nearly every bug here was diagnosed from
  that single line.
- Watch the *minimum* internal heap watermark, not the current free.
- Capture serial to a file with a small pyserial script that reconnects across
  resets, rather than an interactive monitor; then grep it.
- When a symptom is geometric (rotation, touch mapping), find the vendor or
  reference implementation and copy its table. Deriving it from first principles
  took several wrong guesses; `esp_lvgl_port` had the answer.
