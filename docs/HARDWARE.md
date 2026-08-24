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

**The three keys run along the TOP edge of the cube**, in that order, which in
LVGL space at `s_rot == 0` is edge 0 (`BTN_EDGE_NATIVE` in `main.c`). This was
not recorded anywhere until the bezel pop-out needed it, and it was guessed
wrong first — left, on the reasoning that the strip is held vertically. Look at
the device.

Anything anchored to a key must be carried through the panel rotation by hand,
and **the correction is a plain `s_rot`, not its inverse.** The inverse is the
intuitive guess (the panel turns the content, so a case-fixed feature should
travel the other way) and it is wrong, because `s_rot` already counts turns of
the panel relative to the case. The symptom of getting it backwards is the one
§6 warns about: 0° and 180° look perfect while 90° and 270° emerge from opposite
sides. **Test all four orientations; two of them will lie to you.**

Reading PWR: enable the short-press IRQ (reg `0x41` bit 3), then poll status reg
`0x49` bit 3 and **write 1 to clear**. Leave long-press alone — the PMU may act
on it itself.

**PWR is not short-press-only, and believing it was cost a feature its whole
design.** The PMU identifies four PWRKEY states, and two of them are edges:

| reg `0x41` | reg `0x49` | meaning | default |
|---|---|---|---|
| bit 3 | bit 3 | short press (completed) | **enabled** |
| bit 2 | bit 2 | long press | enabled — leave alone |
| bit 1 | bit 1 | **negative edge — key went down** | **disabled** |
| bit 0 | bit 0 | **positive edge — key came up** | **disabled** |

Enable bits 1 and 0 and the middle key gains a real press-down and release, the
same as the two GPIO keys. They are simply off at power-up, so a firmware that
only ever enabled bit 3 sees a completed press and concludes — as this one did,
in a commit message and in ARCHITECTURE.md — that press-down is impossible on
this key. It is not. Status bits are RW1C like the rest.

This is worth more than the register: the bezel pop-out was built with a 380 ms
timer to paper over the "missing" press-down, and the timer existed only because
of the wrong belief. Reading the IRQ table instead of the one bit already in use
deleted the workaround.

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
CONFIG_LV_OS_NONE=y                      # threaded dispatch cost ~12% (pitfall #30)
CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1        # inline unit; the old "1 unit stalls"
                                         # applied only WITH LV_USE_OS
CONFIG_LV_USE_PERF_MONITOR=n             # otherwise LVGL paints its own grey box
CONFIG_LV_CACHE_DEF_SIZE=5242880         # defaults to 0 — see §7c

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
| Internal SRAM free | **~29 KB** idle, ~24 KB with the heaviest app open |
| Floor across an app-switch sweep | **~21.8 KB** |
| PSRAM free | ~7.3 MB of 8 MB |
| App binary | ~1.5 MB in a 4 MB partition |
| Unallocated flash | **~11.9 MB** — room for OTA slots or a filesystem |

Rules of thumb: an LVGL screen costs ~5 KB internal; a VPN stack costs ~42 KB of
permanent task stacks; a TLS session costs ~40 KB but goes to PSRAM if
`MBEDTLS_EXTERNAL_MEM_ALLOC` is on.

**Measure one pool, consistently.** `esp_get_free_internal_heap_size()` reports
`8BIT|DMA|INTERNAL`, but `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`
watermarks a *superset* that also counts 32-bit-only IRAM. Logging one against
the other produces rows where the minimum exceeds the current free — impossible
for one pool — and quietly overstates the floor by however much IRAM-ish memory
happens to be free. `8BIT|DMA|INTERNAL` is the pool that matters, because it is
what Wi-Fi buffers, DMA descriptors and BLE need. Log free, minimum **and
largest block** from identical caps: exhaustion and fragmentation are
indistinguishable in a free-size column and need opposite fixes.

**The largest single consumer of internal SRAM was LVGL's code, not anyone's
data.** `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` copies LVGL's hot drawing
functions into internal RAM for speed, and on this build that was **104,880
bytes — 40% of the entire DIRAM pool**, four times what the Wi-Fi TX-buffer trim
won. Turning it off moves that code to the flash cache:

| | before | after |
|---|---|---|
| DIRAM used | 250,915 (73%) | **146,035 (43%)** |
| heap free, running | ~41,000 | **145,059** |
| minimum watermark | ~29,000 | **112,836** |
| largest block | 31,744 | **77,824** |

**The cost is ~7%, and the obvious way to measure it is wrong.** 40 fps against
43, same screen, same interaction. A first reading of "4.3 fps" came from a
*static* screen and looked like a tenfold collapse — that is Pitfalls #8, where
near-zero fps is the correct answer rather than a slow one. Measure this under a
drag, never at idle.

Worth generalising: `.text` was **201,635 of 250,915** DIRAM bytes here. On this
board, ask what *code* is in internal RAM before hunting for buffers.

**Task stacks default to internal SRAM, and they are expensive.** A single
`xTaskCreate(..., 8192, ...)` costs 8 KB of the scarcest pool on the board.
Anything that is not time-critical and never runs with the cache disabled should
use `xTaskCreateWithCaps(..., MALLOC_CAP_SPIRAM)` instead — moving one network
task's stack raised the measured floor from 13.6 KB to 21.8 KB, the largest
single win *realised* on this board so far.

**The Wi-Fi driver holds ~53.5 KB and never lets go.** Measured:
`esp_wifi_deinit()` returns **53,560 bytes** of `8BIT|DMA|INTERNAL`
(24,919 → 78,479). It is by far the largest consumer of the scarce pool —
bigger than LVGL, the codec and everything else together — and
`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y` is already on, so that 53.5 KB is what
*cannot* be pushed to PSRAM.

**`esp_wifi_stop()` frees 96 bytes.** The pools are allocated at
`esp_wifi_init()`, not at start, so "stop Wi-Fi while idle to save memory" does
not work — it has to be a full deinit. Worth knowing before designing anything
around it.

### Memory numbers that were wrong, and how

Four separate wrong answers were produced about this one pool in a single day,
by two people who were both being careful. They are listed individually because
the pattern is only visible with all of them: **every one came from measuring
something adjacent to the question and assuming it answered the question.**

1. **Comparing two different pools.** `esp_get_free_internal_heap_size()` is
   `8BIT|DMA|INTERNAL`; `heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)`
   watermarks a superset including 32-bit-only IRAM. Logged side by side they
   produced rows where the minimum exceeded the current free — impossible for one
   pool, and the tell that they were not one pool. Fix: one cap set everywhere,
   which is what `MEM_CAPS` above exists for.

2. **What a subsystem frees is not what it needs to start.**
   `esp_wifi_deinit()` returns 53.5 KB, but that is an upper bound including
   everything accreted while associated. `esp_wifi_init()` needs the static pools
   plus the driver task — about 45 KB at the time. Budgeting against the teardown
   figure overstated a 4 KB shortfall as 12 KB, a 3× error, and nearly cancelled
   a feature that in fact fitted.

3. **Guessing the overhead around a figure you did measure.** Static pools were
   known exactly (count × 1600). The rest — driver task stack, management short
   buffers, dynamic RX, descriptors — was guessed at 8 KB. Measured, it is
   **14.7 KB**. The gate built on that guess passed with 1.5 KB to spare and
   reported success; the true margin was inside the noise.

4. **Applying a saving to a state where it does not exist.** Trimming
   `STATIC_TX_BUFFER_NUM` returned 13,800 bytes and was read as extra headroom
   during a BLE session — but Wi-Fi is *deinitialised* for a session, so those
   buffers were never allocated then. It raised free-during-session by ~1.5 KB.
   What it actually did was **lower the cost of restarting Wi-Fi** by 12,800,
   which is what made a mid-session rescan possible. Same outcome, different
   mechanism — and the mechanism is what tells you further trims pay at full
   value against the requirement rather than against the free figure.

The habit that catches all four: **say which pool, at which instant, in which
state, and check the number against something independent** — the sanity check in
§7g, where the controller having started at all bounds what `largest` can have
been, is the cheapest example.

Most of it was buffer counts nobody chose: `sdkconfig.defaults` pinned only the
RX counts and the AMPDU flags, leaving IDF's default of **16 static TX buffers at
~1.6 KB each = 25.6 KB** on a device whose largest transmission is an HTTP GET
header. **Trimming `ESP_WIFI_STATIC_TX_BUFFER_NUM` 16 → 8 returned 13,800 bytes**
— slightly more than the 12,800 the arithmetic predicts, presumably descriptor
overhead going with it:

| `STATIC_TX_BUFFER_NUM` | 16 (IDF default) | 8 | 4 |
|---|---|---|---|
| drawer free | 21,503 | 35,303 | — |
| drawer min | 20,004 | 33,804 | — |
| MUSIC + art, free | — | — | 38,319 |
| MUSIC + art, **min** | **8,212** | 23,844 | **29,320** |

Inbound throughput is unaffected at 4: three 43 KB covers downloaded clean in
95 s, no retries, no `esp_tls` noise, fps 32–35 while interacting. Sustained
*upload* is untested because nothing here uploads.

That follows from what this device does: every heavy transfer is **inbound**, and
TX buffers do not serve those. Keep the buffers *static* (Kconfig: "If PSRAM is
enabled, Static should be selected"); the count is the lever, not the allocation
strategy.

A further ~27 KB sits in `ESP_WIFI_IRAM_OPT` and `ESP_WIFI_RX_IRAM_OPT`,
documented as ">10 Kbytes" and ">17 Kbytes" — and IDF itself defaults both to `n`
when `BT_ENABLED && SPIRAM`. Untouched so far, deliberately: they move code out
of IRAM, so changing them in the same pass as a buffer count would confound any
throughput regression between two causes.

**A subsystem that silently accepts short allocations looks like it works.**
NimBLE's runtime cost measured **23.3 KB** beside a running Wi-Fi and **26.5 KB**
once Wi-Fi was deinitialised and there was room. Same build, same config: given
less, it took less, advertised successfully, and reported no error. That is the
failure class that survives the bench and fails in the field, and it is not
specific to BLE — treat "it fits, just" as unproven rather than as a result.

## 5. Display and LVGL

- **Draw buffers must live in INTERNAL SRAM.** `esp_lvgl_adapter` bounce-copies
  PSRAM draw buffers through internal DMA staging on *every* flush, which
  collapses into an `ESP_ERR_NO_MEM` storm and 2–5 fps. 32-row double buffers
  (2 × 30,720 B) is a good default.
- Set the SPI bus `max_transfer_sz` to **one draw buffer**, not a full frame.
  The BSP defaults to 480×480×2 = 460,800 B and over-allocates DMA descriptors.
- **An animation is band-proof exactly when its dirty rect fits one draw
  buffer: `width × height ≤ 15,360 px`.** LVGL renders a slice and ships it,
  then renders the next, with no tear gate — so a region spanning several
  buffer-fulls lands on the glass strip by strip, which is the dial-up look.
  `max_row` comes from the **invalidated area's width**, not the panel's
  (`lv_refr.c` `get_max_row()`), so the budget is genuinely the product: a
  32 px-wide full-height column is one flush, and so is a 300×26 strip, while a
  full-width 480×96 band is three. Size any press/hover effect against this
  number before designing it, and confirm with `render perf:` — **`flushes /
  redraws` must be ~1**, measured under the interaction, never at idle.
- `get_max_row()` runs the rounder in a loop and forces `max_row` **even** (the
  CO5300 needs even x/y and even width/height), so it rounds *down*. Strip count
  is therefore not smooth in the width: CONTROL's 424 px column gives 36 rows and
  11 strips, but a width that lands `max_row` on an odd number drops a whole row
  per flush and the count jumps. Worth knowing before tuning a column width.
- **There is no tear gate because this board has no TE pin, and there is no way
  to add one.** The panel's init table *does* enable the tearing signal (`0x35`
  TEON, V-blank only), and `esp_lvgl_adapter` ships a complete GPIO-TE
  implementation (`display_te_sync.c`,
  `ESP_LV_ADAPTER_TEAR_AVOID_MODE_TE_SYNC`), so both halves look present and it
  is tempting to spend a session wiring them together. **The signal never
  reaches the MCU.** In the schematic (`ws-amoled-216/schematic/`, page 1)
  `LCD_TE` runs to display FPC **J4 pin 3** and stops: in the net-to-GPIO table
  printed beside that connector every neighbour is assigned — `LCD_CS`→GPIO12,
  `QSPI_SCL`→GPIO38, `LCD_RESET`→GPIO39, `TP_INT`→GPIO11 — and the `LCD_TE` row
  is **blank**. So `TEAR_AVOID_MODE_NONE` is not a default anyone forgot to
  change; it is the only mode available. Note also that `TE_SYNC` forces
  `RENDER_MODE_FULL` with one 460,800 B frame buffer, which internal SRAM could
  not hold anyway.
  **Consequence for design:** a full-viewport scroll or repaint cannot be made
  coherent on this panel, only *fast*. Anything whose dirty rect exceeds one
  draw buffer will be revealed strip by strip, and the only remedies are to
  shrink the dirty rect under 15,360 px, to hide the repaint (the rotation fade
  in `main.c` blanks the panel and is the pattern to copy), or to raise the
  frame rate until the sweep stops registering.
- A shape can be far larger than its dirty rect: LVGL clips invalidation to the
  panel, so a 300 px circle parked mostly off-screen and dipped 26 px in costs
  300×26, not 300×300. That is how the bezel pop-out draws a smooth arc cheaply.
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
- **`lv_label_set_text()` has no equality short-circuit.** `set_text_internal()`
  always reallocates, re-measures and invalidates, and on a `LONG_SCROLL_CIRCULAR`
  label it re-runs the scroll setup. Rewriting unchanged text on a timer therefore
  costs a flush per tick forever. A 400 ms tick rewriting eight labels held a
  *static* screen at 2-3 fps; gating on `strcmp` took it to **0.0 fps**, which is
  the correct answer for a screen where nothing moved (Pitfalls #8). Style setters
  behave the same way — they invalidate whether or not the value changed.
- **`LV_OBJ_FLAG_GESTURE_BUBBLE` is set on every object that has a parent**
  (`lv_obj.c:593`). So a drag on a child reaches the screen's gesture handler:
  dragging a volume slider fired swipe-up-for-home and left the app. Any widget
  that owns a drag — a slider, a scrollable list — must
  `lv_obj_remove_flag(o, LV_OBJ_FLAG_GESTURE_BUBBLE)` or its drags escape.
- **`LV_EVENT_GESTURE` repeats for as long as the finger is down.** One flick
  skipped two or three tracks. `lv_indev_wait_release(indev)` at the top of the
  handler is the fix — it suppresses further events until the touch ends. A time
  cooldown alone does not do it, because the repeats arrive inside one gesture;
  the two guard different things and a fast double-flick wants both.
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
  is scrambled. That repaint is full-screen, so it lands as 15 sequential
  strips and cannot be made coherent: there is no TE pin on this board (§5).
  Blanking the panel with `bright_apply(0)` and ramping back is therefore not
  belt-and-braces, it is the **only** way to hide a full-frame repaint here.
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

### Switching autorotate off

CONTROL carries a switch for it, and three details make it work rather than
merely stop the panel turning:

- **Gate the commit, never the poll.** `imu_poll()` must keep reading the
  accelerometer and updating `s_base_rot` even when the panel is pinned, because
  FOCUS reads orientation as *input* and flat-means-pause reads `s_acc_z`. An
  early return in `imu_poll()` looks correct — the panel stops rotating, which
  is what was asked — and silently kills the FOCUS dial, motion-wake and pause
  together. The failure is invisible at the rotation level, so testing the
  switch itself will never catch it.
- **Persist the held orientation, not just the flag.** `s_rot` lives only in
  RAM and boot calls `rotation_apply()` unconditionally, so without saving it a
  reboot lands back at native 0° with the switch still reading OFF — and since
  the calibration button fades out in that state, with no way back at all.
- **Nothing needs re-arming when it goes back on.** The vote counter keeps
  running while the switch is off, so an orientation that has already settled
  commits on the next poll. Zeroing the votes in the callback would turn an
  instant snap into an 800 ms wait; calling `rotation_apply()` from the callback
  to force one would bypass the magnitude and margin gates above, which exist
  precisely because a near-45° pose has no right answer.

The main UX consequence is worth knowing before someone reports it as a bug:
**gestures are expressed in LVGL directions, which are relative to the panel.**
Held at 90°, "swipe up for home" becomes a physically sideways swipe on every
screen — including MUSIC, where the swipe is the only way home because the keys
are rebound to volume.

## 7. AXP2101 PMU

- **Charge current powers up at 25 mA** (reg `0x62` = 0x01), which looks exactly
  like "the battery never charges". Waveshare's own example sets **400 mA**
  (step 10): `reg 0x62 = (old & 0xE0) | 10`.
- **The charge target is reg `0x64` bits[2:0]:** `001` = 4.0 V, `010` = 4.1 V,
  `011` = 4.2 V (POR default), `100` = 4.35 V, `101` = 4.4 V. There is nothing
  between the steps — 4.0/4.1/4.2 is the entire granularity available for a
  charge limit, which is why Facet's battery care has three modes and not a
  percentage slider.
- **The PMU opens BATFET at charge-done, and that is real battery bypass.**
  Datasheet §6.7.3.3: at termination it "stops charging (charger enable bit is
  still 1) and turns off BATFET", so the cell is disconnected from the system
  rail and the board runs from VBUS alone. §6.7.3.4: it re-closes and resumes
  charging on its own once VBAT falls below **VRECHG, fixed at CV − 100 mV**.
  That is hardware hysteresis — a charge limit needs no polling loop and cannot
  micro-cycle. Cap the CV and the PMU does the rest.
- **You cannot command BATFET.** `0x00` bit 4 is read-only state, there is no
  ship mode, and `0x12` — which `XPowersLib` names `BATFET_CTRL` — holds only
  die-over-temperature bits. The `enableBATFET`/`disableBATFET` in that library
  belong to the SY6970, a different chip. Bypass is reached, never ordered.
- **Reset domains decide what you must re-assert.** `0x64` (CV), `0x62` (ICC),
  `0x61`, `0x68` and the ADC enables are **POR**-reset: they survive an ESP32
  reset but not a true power-on. `0x18` bit 1 (cell charge enable) and `0x63`
  bit 4 are **System Reset** — cleared far more often, and §6.7.3.4 additionally
  warns "the charger is enabled when an adapter is inserted". Driving a charge
  limit from CV rather than from the enable bit is the robust choice, and even
  CV is re-checked every poll rather than written once at boot.
- **`reg 0x01` lags a CV change — do not read charge state straight after
  writing `0x64`.** Raising the cap to start a top-up and then testing
  bits[2:0] on the next poll returned the *previous* cycle's `100` (done), which
  looked exactly like "the charge finished instantly" and cancelled the request
  0.8 s after it was armed. Wait for `charging` (bits[6:5] == 01) to actually
  appear before believing a subsequent `done`. Raising the cap does reliably
  restart charging, including from a latched done state — verified on hardware
  going 4.0 V -> 4.2 V at 4177 mV.
- **`isVbusIn()` is a trap; use `0x00` bit 5.** `XPowersLib`'s `isVbusIn()` ANDs
  VBUS-good with *not in VINDPM*, so a drooping supply reports "unplugged" while
  plugged, and its `getVbusVoltage()` then returns 0. Bit 5 alone is the honest
  presence bit. Charge state is `0x01` bits[2:0]: `010` CC, `011` CV,
  **`100` done**, `101` not charging.
- **Two AXP2101 datasheets are in circulation and they disagree.** The
  switching-charger revision documents `0x14` bits[2:0] as a 3.2–3.9 V minimum
  system voltage; the linear-charger revision documents bits[6:4] as a 4.1–4.8 V
  VSYS DPM. The charge-control registers (`0x00`, `0x01`, `0x18`, `0x62`, `0x64`)
  are identical in both. Leave `0x14`/`0x15`/`0x16` alone unless you have
  established which silicon is fitted — `0x16` already defaults to 1500 mA, which
  is what keeps the system fed from VBUS instead of dropping into supplement mode
  under an AMOLED + Wi-Fi load spike.
- The fuel gauge reads 0% until **three** separate enables: `0x18` bit 3 (gauge
  module), `0x68` bit 0 (battery detect), `0x30` bit 0 (voltage ADC). Percentage
  at `0xA4`; battery voltage at `0x34`/`0x35` as `((hi & 0x1F) << 8) | lo` mV.
  Battery-present is `0x00` bit 3; charging is `0x01` bits[6:5] == 01.
- Voltage is a good sanity check and fallback: 3.30 V ≈ 0%, 4.20 V ≈ 100%.
- **The gauge at `0xA4` latches, and it will lie confidently.** It read a flat
  **100% while the ADC said 3964 mV** — nearer 60% — and held that for a five-hour
  soak. It is a coulomb counter: it only re-learns across a full charge and
  discharge, so on a device that lives on a charger it can stay wrong indefinitely.
  **No library fixes this.** `XPowersLib` is the de-facto AXP2101 driver and its
  `getBatteryPercent()` reads this same register; the chip *is* the fuel gauge, and
  the calibration lives in silicon. Cross-check it against the ADC instead:
  a disagreement over ~25 points, **or** a claim of ≥95% while measuring under
  4.10 V — a cell at full charge does not read below that even while charging,
  when terminal voltage is at its most flattering, so that pairing is a
  contradiction no load condition explains. The wide-gap test alone is not enough:
  100% at 4013 mV is only 21 points out and slips through.
- **A charge cap makes the gauge worse, and the one-shot is the cure.** Capped at
  4.1 V the cell never reaches a full charge, so the coulomb counter never gets
  the complete cycle it needs to re-learn and the voltage cross-check above stops
  being a fallback — it becomes the primary reading. CONTROL's "CHARGE TO 100% ONCE"
  exists as much for the gauge as for the trip you are packing for.

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
`/sdcard/logs/pwrlog3.csv` every minute and on every power-state change.

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
- **An invariant enforced by a comment decays silently.** `esp_wifi_connect()`
  had a comment declaring one owner — true when written, and the reason a retry
  storm had been fixed. Adding a BLE rescan path created a third caller without
  touching either existing one, and the assertion was stale from that moment with
  nothing to notice it. The failure it produced was intermittent and presented as
  "no networks in range", i.e. nowhere near the change that caused it. **"Exactly
  one place does X" is a claim about the whole program, and a comment cannot
  enforce it** — a flag other callers must consult, or a single function they
  must route through, can. Worth checking whenever a new subsystem touches a
  shared peripheral: what else already assumes it is alone?
- **`perform()` never redials — it retries the dead socket forever.** Observed on
  hardware: the server reset a connection mid-transfer and every subsequent poll
  logged `esp_tls_conn_read error, errno=Software caused connection abort` /
  `Socket is not connected`. The device stayed up, the heap was healthy, the
  status line kept printing, and the feature was simply dead — nothing surfaced
  anywhere near the UI. Check `perform()`'s **return value**, not just
  `esp_http_client_get_status_code()`, and call `esp_http_client_close()` on a
  genuine transport error so the next call redials for one 390 ms handshake.
  **Exclude `ESP_ERR_NOT_SUPPORTED`**, which is the Bearer-challenge return on a
  401 above: treating that as a transport fault discards the connection on every
  token expiry and silently gives back the 65x reuse this section exists to
  document. The general shape is worth remembering — the point of a long-lived
  handle is that it survives, so its failure mode is *not letting go when it
  should*, and it needs an explicit path back to disconnected.

## 7g. Bluetooth LE

The S3 is **BLE 5.0 only, no BR/EDR**, which costs nothing here — Web Bluetooth
speaks GATT anyway. All figures below are from this board with NimBLE.

**Static cost is decided by one flag.** Measured with `idf.py size-components`:

| | internal SRAM, static |
|---|---|
| Stock controller (in IRAM) | **~22.6 KB** |
| `CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY=y` | **~3.7 KB** |

The flag moves the controller out of IRAM for ~123 KB more flash, against
~11.9 MB unallocated. Without it BLE is simply unaffordable. In the full firmware
the end-to-end delta including the provisioning module is **+5,032 B of DIRAM**
and +298 KB of flash.

**Runtime cost is ~23–26.5 KB and does not fit beside Wi-Fi.** See §4 for why
the two figures differ. Measured on the shipping build with BT compiled in and
Wi-Fi associated:

| | free | min | largest |
|---|---|---|---|
| drawer | 35,303 | 33,804 | 31,744 |
| lock + album art, min | — | 23,844 | — |
| live BLE session (Wi-Fi deinitialised) | ~54,800 | — | — |

Figures after the `STATIC_TX_BUFFER_NUM` trim in §4; before it, the drawer sat at
21,503 and a live session at ~41,035.

Quote figures taken *with* BT enabled: a floor measured before `BT_ENABLED`
describes a different binary and flatters the comparison — enabling it costs
roughly 6.8 KB of free and takes largest-block down by ~7.7 KB.

**Sanity-check a memory sample against whether BLE started at all.** A reading of
`largest 9,728` was once taken to describe a live session — which cannot be true,
because the controller needs ~26 KB contiguous and it had plainly come up. The
sample turned out to be from 10.3 s *before* `wifi_driver_down()`, labelled from
adjacency in the log rather than from the clock. The controller's own success
bounds the measurement, which makes this self-checking and worth doing before
recording any figure: if BLE is running, `largest` was at least ~26 KB.

So BLE and an initialised Wi-Fi cannot coexist, and the largest block is the
sharper constraint — the controller wants contiguous internal memory, so even a
free total that looked sufficient would not serve. The radios have to be
serialised: `esp_wifi_deinit()` before the session, re-init after. With Wi-Fi
down, ~52 KB stays free during a session, which is comfortable.

**Tuning the host barely helps.** Cutting `MSYS_1`/`MSYS_2`,
`TRANSPORT_ACL_FROM_LL_COUNT` and `TRANSPORT_EVT_COUNT` hard returned only
~1.4 KB, because `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y` already places host
allocations in PSRAM. The cost is the *controller*, and it is close to fixed.

**`CONFIG_BT_CTRL_BLE_MAX_ACT=1` does not work.** Advertising fails with `519`
= `BLE_HS_HCI_ERR(7)`, "memory capacity exceeded" — the controller needs one
activity for the advert and one for the connection it produces. **2 is the
floor**, and the failure is a log line, not a crash, so the stack otherwise looks
healthy.

**Teardown genuinely reclaims.** `nimble_port_stop()` then `nimble_port_deinit()`
(the order used by IDF's `blecent`) returns everything: over three cycles,
**112–120 bytes once on first init, then 0 and 0**. So an on-demand session
model is sound. Never call `esp_bt_mem_release()` — it is irreversible and
blocks re-init until reboot.

**No flash writes while the radio is up.** With the controller executing from
flash, a flash *erase* stalls it. `SPI_FLASH_AUTO_SUSPEND` is not available to
us: IDF disqualifies XMC-C parts ("tRS >= 1ms restriction… DO NOT enable"), and
this board's flash is XMC. NVS commits are not confined to the obvious places —
`pet_save()` alone fires off a 60 s timer with no user action — so gate every
commit while a session is open and drain them afterwards. Note also that
`esp_wifi_set_config()` writes NVS by default; `esp_wifi_set_storage(WIFI_STORAGE_RAM)`
removes that, and nothing here reads esp_wifi's mirror back.

**The NimBLE host task stack is internal and cannot move.** The port creates it
with a plain `xTaskCreatePinnedToCore` (`nimble_port_freertos.c`), so the PSRAM
stack trick does not apply; trim `CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE` instead
(default 4096, IDF's own minimal profile uses 3152).

Enabling BT does **not** disturb the silent-hang surfaces — verified by
regenerating `sdkconfig` from scratch and diffing: `SPIRAM_SPEED_40M` held, and
`SPIRAM_FETCH_INSTRUCTIONS` / `RODATA` / `XIP_FROM_PSRAM` / `PM_DFS_INIT_AUTO`
all stayed unset. It does auto-select `ESP_COEX_SW_COEXIST_ENABLE`.

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
`waveshare/esp32_s3_touch_amoled_2_16` v2.0.1 with seven changes. Anyone forking
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
7. **Brightness is hardcoded to 100% in three places**, which makes a
   user-settable level impossible to honour at boot. The init-command table
   writes `0x51 = 0xFF`, and `0x29` (display on) two rows later carries a 600 ms
   delay — so the panel is lit at maximum for well over half a second before
   `app_main` gets control. `bsp_display_brightness_init()` then writes 100
   again, and `bsp_display_backlight_on()` writes 100 on every wake. Changed the
   table entry to `0x00` (dark is the correct direction to fail in, and
   `bsp_display_lcd_init` has exactly one caller, which lights the panel
   immediately afterwards), and added `bsp_display_brightness_set_boot(int)`
   that both `brightness_init()` and `backlight_on()` now read. Leaving
   `backlight_on()` at a hardcoded 100 would have left a loaded gun: the
   invariant "brightness only ever comes from `s_bright`" would have been
   enforced by nothing but the absence of callers.

A second fork of the touch driver was attempted and **failed on hardware** —
it is parked out of the tree, and the registry `waveshare/esp_lcd_touch_cst9217`
is what ships. The change (single I2C transaction per register access,
`lcd_cmd_bits = 16`, no inter-phase delay — modelled on SensorLib's
`i2c_master_transmit_receive`) looked right from source and passed every
synthetic test, because **no synthetic test moves a finger**: with the fork the
touch init failed silently — `BSP_ERROR_CHECK=n` swallowed the error, no indev
was registered, and the boot log simply had no touch lines at all. The glass
was dead until a human tried it. Two lessons: this chip apparently does need
the stop-then-delay between address write and data read, whatever SensorLib's
CST92xx path implies; and verify touch by grepping for the SUCCESS line
("Touch input device registered"), not for the absence of errors (#28). The
parked fork's intent, if anyone retries with the chip's datasheet in hand:

1. **Every touch sample parked the LVGL task for 2-3 ms.** `cst9217_read_reg()`
   wrote the 16-bit register address, `vTaskDelay(pdMS_TO_TICKS(2))`, then read
   — inside LVGL's indev timer, on the LVGL task, at `FREERTOS_HZ=1000`. The
   chip does not need the gap: Waveshare's own SensorLib reads the same report
   with one `i2c_master_transmit_receive` (`TouchDrvCST92xx.cpp:128`). Set
   `lcd_cmd_bits = 16` so `esp_lcd_panel_io_rx_param(io, reg, buf, len)` issues
   register + repeated-start + read as a single transaction (IDF serialises the
   command big-endian, `esp_lcd_panel_io_i2c_v2.c:149`), and drop the delay.
   `write_reg` gets the same treatment (SensorLib writes reg+data as one buffer,
   `:137`). The retry back-off stays on the failure path only.

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
   per-component. **The rule is not "copies between fixed buffers warn"** — it
   is *copies gcc can range-analyse right up to the boundary* warn, which is why
   `snprintf(dst, sizeof dst, "%s", src)` is fatal between two `char[33]`s and
   silent a few lines away between two `char[160]`s whose source came through a
   pointer gcc cannot range. The same-looking line being fine in one place and
   fatal in another is the confusing part; the difference is what the optimiser
   can prove, not what you wrote.
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

    **`bsp_display_backlight_on/off()` are panel IO too, and they were the ones
    that got missed.** They are not a GPIO — they write panel command `0x51`
    (§7b) over the same QSPI device. `power_set_doze()` was given the lock and
    `screen_toggle_power()`'s backlight calls, three lines away, were not. The
    board deadlocked: the LVGL task blocked *forever* in
    `spi_device_acquire_bus(portMAX_DELAY)` inside `panel_io_spi_tx_param()`
    while holding the LVGL lock, so every subsequent `ui_lock()` timed out.

    Symptom to grep for: **the UI is frozen but the device is clearly alive** —
    the 15 s status line keeps printing, `wifi=up`, heap healthy, `fps=0.0`, and
    the log repeats `Failed to acquire LVGL lock` / `LVGL lock timed out`. It is
    not a crash, a watchdog, or a memory problem, and nothing panics. Pitfall #14
    is why the rest of the system survives to tell you.

    Diagnosed with #15 in one gdb session: `thread apply all bt` put the `lvgl`
    thread in `dev_wait` → `spi_bus_lock_acquire_start` → `panel_io_spi_tx_param`,
    which names the resource and the caller outright. Worth reaching for that
    immediately rather than guessing — it took one attach.
14. **`bsp_display_lock(-1)` in the main loop converts any LVGL stall into a
    total freeze.** Use a bounded timeout and log loudly on failure — a dropped
    UI frame is recoverable, an infinite wait is not.
15. **Debugging a live hang:** the ESP32-S3's USB-Serial/JTAG does CDC and JTAG
    at once, so `openocd -f board/esp32s3-builtin.cfg` plus
    `xtensa-esp32s3-elf-gdb -ex "target extended-remote :3333"` can attach to a
    frozen board without reflashing. Get `info threads` **and** the backtraces
    in a *single* gdb session — thread IDs are re-enumerated on every attach,
    and `monitor halt` leaves the CPUs stopped, so finish with `reset run`.
16. **A 403 on one endpoint while a sibling endpoint returns 200 on the same
    token is a deprecation, not a scope problem.** `GET /me/tracks/contains` and
    `PUT /me/tracks` both answered `403 Forbidden` with `user-library-read` and
    `user-library-modify` present in the token's own `scope` field — while
    `GET /me/tracks?limit=1` and `GET /me/albums?limit=1` returned 200 on that
    same token. Every symptom pointed at scopes, so the pairing flow was rebuilt
    and re-authorised twice for nothing. Spotify's February 2026 Dev Mode changes
    deprecated the per-type library endpoints in favour of `/me/library`, which is
    keyed on Spotify **URIs** rather than bare ids:
    `GET /me/library/contains?uris=spotify%3Atrack%3A<id>` and
    `PUT`/`DELETE /me/library?uris=…`, all verified returning 200.
    **The diagnostic that would have saved the time:** when one call fails, try a
    *sibling* call needing the identical scope. Two endpoints disagreeing on the
    same token rules out the token, and points at the endpoint.
17. **A vote counter is not hysteresis.** Autorotate flipped back and forth every
    few seconds on a stationary desk despite requiring eight consecutive
    agreeing samples — near 45° the input itself is genuinely ambiguous, so the
    counter just confirms whichever wrong answer arrived first. Debounce fixes
    *noise*; it does not fix an ambiguous *decision*. Add a margin the winner
    must beat, and hold the current state when nothing wins (§6).
18. **Enabling a component in sdkconfig does not measure it.** Turning on
    `BT_ENABLED` and rebuilding showed BLE costing 672 bytes of DIRAM — a lovely
    number and completely false. Nothing *referenced* NimBLE, so `--gc-sections`
    discarded the whole stack. The tell was flash growing 4.5 KB when
    `libbtdm_app_flash.a` alone carries ~195 KB of `.text`. To size a component,
    link something that calls into it — an opaque reference the compiler cannot
    fold, e.g. `if (esp_random() == 0xFFFFFFFFu) thing();` — then confirm with
    `nm` on the ELF that its symbols are actually present before believing any
    figure. Real answer for BLE: **+5,032 B**, not 672.

19. **One buffer shared by two writers is not a buffer, it is a race — and it
    fails silently.** This board hit the same shape three times in one feature.
    A GATT write callback runs on the NimBLE host task; the main loop reads what
    it left every 20 ms. Any phone that writes twice inside that window
    overwrites the first frame, and the second is then judged in its place. The
    symptoms were never "a frame was lost": they were *"wrong code" for a
    correct code*, and a list that silently never appeared. Fixes, in the order
    they were needed: give each characteristic its own storage; then a ring
    queue for repeat writes to the SAME characteristic; then publish the payload
    before the index that exposes it (`__ATOMIC_RELEASE`/`ACQUIRE`), since only
    the index was declared volatile. Also double-buffer anything a GATT read
    serves — NimBLE re-invokes the access callback for *every* Read Blob Request
    and slices by offset, so a value rewritten mid-read hands the peer the head
    of one payload and the tail of another.
20. **A GATT read returns a DataView, and `dv.buffer` is not the value.** It is
    the whole underlying buffer, which equals the value only if the
    implementation happened to allocate it that way. Chrome does; Bluefy is a
    separate WebKit implementation and nothing in the spec requires it. Always
    `new Uint8Array(dv.buffer, dv.byteOffset, dv.byteLength)`. The failure is
    silent and total — every read decrypts to garbage — and it will not
    reproduce in desktop Chrome.
21. **Two agents flashing from different snapshots of one tree looks exactly
    like a regression.** A working feature "disappeared" because the other
    session built from its commits while the work was still uncommitted in the
    shared working tree. The boot log settles it in one line: the app version
    string and any log line the new code adds. Commit before handing the board
    over, and check `git status` before believing a regression report.
22. **The display lock IS recursive, and two comments in this file said it was
    not.** `esp_lv_adapter.c:601` creates it with
    `xSemaphoreCreateRecursiveMutex()` and `esp_lv_adapter_lock()` takes it with
    `xSemaphoreTakeRecursive()`; `bsp_display_lock()` is a one-to-one wrapper.
    So calling `ui_lock()` from an LVGL event callback succeeds immediately by
    bumping the count rather than deadlocking, and there is exactly one thing to
    be careful about: **an early `return` between the take and the unlock owns
    the mutex forever.** The single `unlock` in `lvgl_worker` drops the count to
    1, never 0, and every other task's `ui_lock()` then times out — the same
    symptom as #13, from the opposite cause.

    The reason to take the lock was never recursion. It is that two **tasks**
    must not drive the QSPI device at once: `num_trans_inflight` and
    `trans_pool[]` in `esp_lcd_panel_io_spi.c` are unlocked plain fields mutated
    by both `tx_param` and `tx_color`, and a foreign task's `tx_param` also
    consumes completions the LVGL task is accounting for. An LVGL event callback
    is exempt only because it *is* the LVGL task. Nothing else is.
23. **A "what I last wrote" cache needs exactly one writer, or the next wake is
    a black screen.** Brightness writes are change-gated against
    `s_bright_applied` so a drag does not issue a 0x51 per touch sample. The
    sleep path used `bsp_display_backlight_off()`, which writes 0 to the panel
    *without* going through the gate — so the cache read 60 while the panel sat
    at 0, and the next wake compared 60 against 60, skipped the write, and left
    the display dark with every flag saying the screen was on. Route every
    writer through the one function that owns the cache, including the ones that
    look too trivial to bother with.
24. **A 44 px control does not reliably register on this touchscreen.** The
    glass is curved and the controller is noisy near the edges; sliders read
    drags as taps and taps as nothing. 76 px works. This is the second time the
    lesson was learned here — the first MUSIC layout shipped 46 px transport
    buttons that ghost-touched and were rebuilt at 76/88/76.

    Two things that are *not* the fix: padding a slider knob proud of its track
    (a 66 px ball on a 46 px bar reads as a rendering bug, and buys nothing
    because an `lv_slider` already jumps to wherever you press on the track),
    and scaling controls without scaling type — 14 px labels beside 76 px
    controls look like a desktop dialog enlarged wrong. `lv_font_montserrat_20`
    is already compiled in and carries the same `LV_SYMBOL_*` glyphs as the
    14 px default, so it costs no flash. Use `lv_obj_set_ext_click_area()` to
    widen the hit test without disturbing the layout.
25. **A gesture handler on the screen steals drags from every widget under it.**
    Symptom: dragging a slider navigates away; scrolling a list closes the panel
    it is in. Not a touch bug — LVGL bubbles gestures up from any child with a
    parent, by default (§5). Clear `LV_OBJ_FLAG_GESTURE_BUBBLE` on anything that
    owns a drag. Related symptom, different cause: **one swipe performing the
    action two or three times** is `LV_EVENT_GESTURE` repeating while the finger
    stays down, and wants `lv_indev_wait_release()`.
26. **A truncated HTTPS body with a healthy server is a link event, and the way
    to find that out is to eliminate the far end rather than reason about it.** A
    139 KB download stopped at exactly 14,587 bytes, twice — an identical byte
    count that looks deterministic and invites a search for an off-by-something
    that does not exist. What settled it, cheaply, was four measurements: the
    server's own logs showed it served the object in 4-157 ms; eight fetches of
    the same object from a laptop came back complete; a reader deliberately
    trickling 1 KB every 3 s was still being served after 75 s, so nothing
    upstream cuts off slow consumers; and the link measured -40 dBm. That leaves
    the local side, with `errno 11` (EAGAIN) meaning the socket read simply timed
    out waiting.
    Do not guess a fix from there. Make the failure cheap — a partial body earns
    **one** immediate retry, since the far end demonstrably has not given up —
    and make the next occurrence self-documenting by logging the byte count and
    the RSSI at the moment it happens. Put RSSI on the periodic status line
    generally: a stalled bulk transfer is a link symptom and diagnosing it blind
    wastes a session.
27. **An LVGL image that paints nothing, with no error anywhere, is a decoder
    rejection — and with `LV_USE_LOG` off it is silent by construction.** A
    cover with valid JPEG bytes stopped rendering: the object was unhidden,
    `lv_image_decoder_get_info()` returned OK (it merely echoes the header *you*
    wrote into the dsc — it proves nothing about decodability), pools were
    healthy, and the panel showed bare background. Enabling `CONFIG_LV_USE_LOG`
    at WARN named it in one run: `lv_draw_sw_blend_image_to_rgb565: Not
    supported source color format` — no decoder claimed the source (its
    `data_size` was 0), so the RAW-format dsc fell through to the software
    blender, which cannot blend an undecoded JPEG and skips it. The log is now
    pinned on at WARN in `sdkconfig.defaults`: a healthy build prints nothing,
    and the failure class stops being invisible. Related trap in the same hunt:
    a widget's *absence* has two causes — hidden flag or failed draw — and they
    need different tools; check the flag from code, and let LVGL's own log rule
    the draw in or out, rather than reasoning about either from a photo.

28. **A filter that cannot fail looks exactly like a clean run.** Two of them bit
    two sessions in one evening, and neither made a sound. A build-log filter of
    `grep -E "warning: .*main\.c"` can never match a warning in `main.c`, because
    gcc prints the *filename before* `warning:` — so every "zero warnings" it
    reported was unverified by construction. Use `^/Users.*(warning|error):`. And
    a serial capture that whitelists `render perf` / `uptime=` / `rst:` faithfully
    recorded a reboot and threw away the panic and backtrace that preceded it.
    Log every line raw to a file and only narrow what is *printed*. The common
    shape: the tool is structurally incapable of the failing case, so its silence
    proves nothing — check that a filter can produce the bad output before
    believing its clean output.
29. **`lsof` the serial port before flashing.** A second Claude session, or a
    forgotten pyserial capture from one, holds `/dev/cu.usbmodem*` invisibly and
    esptool fails with "device reports readiness to read but returned no data" —
    which reads as a dead board. `lsof /dev/cu.usbmodem*` names the holder in one
    line. Worse than the failed flash is the *successful* one: flashing resets
    the board and destroys whatever repro the other session was capturing.
    Ask before taking the port, and commit before handing the tree over (#21).
    Note that a *reconnecting* monitor closes and reopens the tty on every
    read error, so `lsof` can show nothing while a capture is live — and
    macOS lets two readers open one tty, so both then see garbage. Neither
    tool failing loudly is the trap; only the other session's word frees the
    board.
30. **Fast scrolling looks torn, like a game with vsync off.** The analogy is
    literally correct and there is no vsync to turn on (§5: no TE pin). A
    full-height list invalidates its whole viewport per step, and CONTROL's
    424 px-wide column lands as `ceil(372 / (15360/424 = 36)) = 11` sequential
    strips per frame — measured 10.9-11.0 flushes/redraw at ~157k px. At
    ~17 fps that is a ~100 px jump per frame, revealed top to bottom. Measured
    where the frame goes (`render perf:` under a drag): **94% software render,
    QSPI ~10%, `wait=0.0`** — the bus never stalls, so raising pclk is worth
    ~5% and was dropped. Render is a straight line through the origin at
    **2.77 Mpx/s = 87 CPU cycles/px** with no fixed per-frame cost; do not
    compare against another scene's numbers to find one — that measures the
    content difference, not a constant. Rounded corners and the translucent
    card border cost nothing (2.75 vs 2.77 with both removed). The per-flush
    byte swap is ~6%. **The lever turned out to be LVGL's draw-task dispatch,
    not the pixels**: on the identical synthetic scroll (CLAUDE.md, Autonomous
    hardware verification), `LV_OS_NONE` + one inline draw unit took render
    from 48.5 to 43.5 ms — **~12%** — while the vendored ESP32-S3 PIE blenders
    measured a flat zero in BOTH dispatch regimes and were removed. Two
    method lessons paid for twice here: an earlier "+31%" came from comparing
    a synthetic scroll against a human one (the touch pipeline inflates the
    human baseline — only paired workloads compare), and "SIMD must help an
    87-cycle pixel" was worth one measurement, not a belief — the fills the
    assembly accelerates were never where the time went. Separately, every touch
    sample used to park the LVGL task 2-3 ms (§9 fork 2), and a slider under
    the finger commits on touch-down because `lv_slider` jumps to the press
    point — `cfg_bright_cb` applied brightness above its RELEASED guard; the
    guards in `cfg_slider()` / `cfg_scroll_guard_cb()` are the fix.
31. **A LoadProhibited in ROM strlen under vfprintf is `printf("%s", NULL)`,
    and tonight's ten "heap ghost" crashes were one compiled-out app.** The
    register dump names it before any backtrace: PC looping in ROM with
    0xff/0xff00/0xff0000 masks loaded, EXCVADDR = 0. `s_apps[]` keeps a zeroed
    hole for an app compiled out (`FACET_APP_PET 0`) and `app_open()` was the
    one table consumer that never called `app_enabled()` — its "opened %s" log
    then strlen'd the hole's NULL name. No gesture can request a hole (the
    drawer draws no tile), so the device never crashed for a person; the
    self-test harness requested it by enum and produced an intermittent-looking
    string of panics that mimicked heap corruption, then a clean 11 s crash
    loop once the request came first. Guard added at the top of `app_open()`.
    Two lessons: read the register dump before theorising — EXCVADDR 0 in a
    ROM string loop is a five-second diagnosis; and a harness that drives the
    firmware by enum must skip what the build compiled out.
32. **Tilt-as-input read from the autorotate base detector is dead on
    arrival, and it dies silently.** `s_base_rot` only flips once an axis
    DOMINATES: 45 degrees plus `QMI_TILT_MARGIN`, so "tilt to steer" built on
    it ignores every tilt short of tipping the cube onto its edge — on the
    glass it reads as "motion controls don't work at all", with zero errors
    anywhere. Steering needs the raw gravity COMPONENT along the screen edge
    (see `pet_lean_signal()`), with the direction-to-axis mapping still routed
    through `rot_from_base()` so all 8 mounting calibrations keep working.
    Related: that mapping has exactly ONE free handedness bit ("does screen-
    left correspond to rotation +1 or +3"), it cannot be settled from the
    bezel edge arithmetic on paper — the derivation produced the wrong bit
    with full confidence — and one hardware test settles it instantly. Budget
    the test, not the proof.
33. **An event reaction that is overwritten in the same frame looks like the
    event never fired.** Shake detection worked from day one — the log said
    so — while the user reported "shaking does nothing": the tilt handler ran
    right after the shake handler and its `walk_to()` replaced the jump
    animation before a single frame drew. When an input's visible effect is
    an activity/animation, every OTHER input path must yield until it
    finishes; a log line proving the event fired says nothing about whether
    its effect survived to the panel. Same family as pitfall #23 (one writer
    per output), applied to animations.

34. **A rounded rect with a huge radius is a crash, not a slow draw.** Ground
    circles of radius 1500-2400 px (flat-world horizons) sent LVGL's software
    corner mask (`lv_draw_sw_mask_radius_init` → `circ_calc_aa4`) into a
    repeating "Guru Meditation (Cache error)" panic on full redraws, while the
    long-serving 270 px planet circle was always fine. The panic does not point
    anywhere near the cause — it looks like a memory-system fault. Two
    lessons: keep `lv_obj` corner radii in the low hundreds and draw a "flat"
    horizon as a radius-0 slab (the walking-surface math can stay curved —
    nobody can see a 5 px sag); and `xtensa-esp32s3-elf-addr2line -pfiaC -e
    build/facet.elf <backtrace addrs>` turns a raw Guru backtrace into named
    frames in seconds — the fastest diagnosis in this file, provided the ELF
    still matches the flashed image.

## 11. Debugging method that worked

- Log a one-line periodic status with everything at once: uptime, wall clock,
  active screen, idle timers, Wi-Fi, battery mV and %, fps, rotation, heap free
  / min / largest block, PSRAM free. Nearly every bug here was diagnosed from
  that single line.
- Watch the *minimum* internal heap watermark, not the current free.
- Capture serial to a file with a small pyserial script that reconnects across
  resets, rather than an interactive monitor; then grep it.
  `tools/capture.py` is that script; `tools/snap_rx.py` rebuilds the
  screenshots the self-test harness dumps (see CLAUDE.md, Autonomous hardware
  verification).
- When a symptom is geometric (rotation, touch mapping), find the vendor or
  reference implementation and copy its table. Deriving it from first principles
  took several wrong guesses; `esp_lvgl_port` had the answer.
