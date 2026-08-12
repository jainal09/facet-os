# Apps as separate firmware images — deferred, with the homework done

**Status: deferred, deliberately.** The design below was researched, spiked and
costed on 2026-08-12. It was **not** rejected — the problem that justified it
mostly disappeared, so it is not worth building *yet*. The trigger for revisiting
it is written down at the bottom.

This file exists so nobody repeats the measurements. Everything here was run on
real hardware; where a number is a projection it says so.

## What the idea is

Each app — SPOTI-CONTROL, FOCUS, PET, and whatever comes next — becomes its own
firmware image in an OTA slot, built from a shared base OS with its own
`sdkconfig`. A **MORE APPS** picker switches between them: a reboot (~2.7 s) when
the image is already in a slot, or a write from the SD card (~10–30 s) when it is
not. The point is **isolation and per-app resource budgets**, which adding rows to
`s_apps[]` cannot provide.

## Why it was deferred: the memory problem dissolved

The motivation was that one firmware could not give each app enough internal SRAM.
That was true, and then it wasn't:

```
CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=n      one line, no code changed
    DIRAM used       250,915 (73%)  ->  146,035 (43%)
    heap free           ~41,000     ->  145,059
    minimum watermark   ~29,000     ->  112,836
    largest block        31,744     ->   77,824
    cost                             ~7%  (40 fps vs 43, same screen)
```

MUSIC's floor on the morning of 2026-08-11 was **8,212 bytes**. It is now
112,836. The single firmware has room, so the case for splitting is now
**isolation**, not memory — a narrower and more honest justification.

See [HARDWARE.md §4](HARDWARE.md) for the finding itself and the measurement trap
that nearly buried it.

## Measured evidence, so it need not be re-derived

All from the full build immediately before the LVGL change:

| | bytes | note |
|---|---|---|
| `liblvgl` `.text` in DIRAM | 104,575 | the lever above; **config**, not app complexity |
| `libpp`+`libnet80211`+`libphy` | 55,903 | radio stack; corroborates `esp_wifi_deinit()`'s 53.5 KB |
| DIRAM `.text` total | 201,635 | of 250,915 — code, not data, dominated |
| `facet.bin` | 2,311,264 | |
| app code: MUSIC | 13,567 | |
| app code: drawer+CONTROL | 19,056 | |
| app code: PET | 4,377 | |
| app code: FOCUS | 1,966 | |

**All three apps together are ~20 KB, 0.9% of the image.** The other 99% is LVGL,
mbedTLS, the cert bundle, Wi-Fi, the BSP, fonts and sounds — which every image
would carry a copy of. That is the cost side of the trade.

**Important nuance:** LVGL's DIRAM cost is *not* proportional to how many screens
an app has. It is hot drawing code copied to internal RAM by a config flag. A
one-screen app still links the blend and draw paths. Only the flag removes it.

## The partition table, validated

Today: `nvs` 0x9000/24K, `phy_init` 0xf000/4K, `factory` 0x10000/4M — a single app
slot and **no `otadata`**, so multi-image is structurally impossible, not merely
unconfigured. `app_update` is also absent from `main/CMakeLists.txt` REQUIRES.

This table was checked with `gen_esp32part.py` and reported **VALID**:

```
nvs       data nvs     0x9000    0x6000
otadata   data ota     0xf000    0x2000
phy_init  data phy     0x11000   0x1000
ota_0     app  ota_0   0x20000   4M
ota_1     app  ota_1   0x420000  4M
                        -> ends at 8.12 MB of 16; 7.88 MB spare
```

`nvs` keeps its offset **and** size, so Wi-Fi credentials and the Spotify pairing
survive the migration — `idf.py flash` does not erase what it does not write.

Slot sizing was left open on purpose: it depends on how big a *stripped* image is,
and that spike was never run. Today's 2.31 MB would be 92% of a 2.5 MB slot, so
three slots is only safe if stripping genuinely shrinks the image.

## What one USB flash buys

| flow | needs a laptop? |
|---|---|
| Switch between two flashed slots | no — `esp_ota_set_boot_partition()` + reboot |
| Install an image from SD into the idle slot | no — the device writes its own flash |
| Get a new `.bin` onto the card | no — download it from the broker |
| **Repartitioning, once** | **yes** — an app cannot rewrite the table it executes from |

## Design, if it is picked up

- `main/base/` — BSP and display init, power/doze, PMU, IMU, store, sfx, Wi-Fi
  (guarded), lock screen, drawer, CONTROL, MORE APPS, OTA install.
  `main/apps/<name>.c` — one app compiled per image, selected by `-DFACET_APP=`.
- **Capability flags** (`FACET_NET`, `FACET_BT`, `FACET_AUDIO`) compile the shell
  down: with `FACET_NET=0` the wallpaper service and SNTP disappear, the clock
  comes from the PCF85063 alone, and CONTROL hides its Wi-Fi panel.
- `tools/build-all.sh` builds every app and emits a manifest
  (`name, version, size, sha256`).
- Broker gains `/images` and `/image/<name>.bin`, following `broker/queue.go` as
  the pattern; the device downloads to the card through the existing
  `asset_fetch_mem` path.

## Traps found while designing it — read before starting

1. **`--gc-sections` already discards unreferenced code, and it does not give you
   RAM.** Pitfall #18 exists because BLE appeared to cost 672 bytes. The savings
   are `sdkconfig`-driven, so the mechanism is **N config files and a build
   matrix**, not C-level inheritance.
2. **A build matrix multiplies pitfall #1.** Each generated config can silently
   produce a board that neither boots nor answers esptool. The pipeline must
   *assert* `SPIRAM_MODE_OCT`, `SPIRAM_SPEED_40M`, `FLASHSIZE_16MB` present and
   `SPIRAM_FETCH_INSTRUCTIONS`/`RODATA`/`XIP_FROM_PSRAM`/`PM_DFS_INIT_AUTO`
   absent — and fail the build rather than ship a `.bin`.
3. **The power state machine assumes `APP_LOCK` exists.** It is load-bearing in
   the idle handler, the wake path and key dispatch. An app that owns the whole
   screen needs those transitions redesigned, not omitted. This is design work,
   not a flag, and it is the largest unknown in the plan.
4. **`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE` is unset.** Without it a bad image
   from the card costs a BOOT-hold power cycle instead of a reboot. Each image
   must call `esp_ota_mark_app_valid_cancel_rollback()` once it is up.
5. **NVS needs a version byte per blob.** Not because apps read differently —
   they share the code — but because *build vintages* differ. An image sits on the
   card for two months while a struct gains a field; `store_load` only rejects a
   **partial** read, so same-size-different-layout passes as garbage.
6. **Shared assets are a feature and a hazard.** Credentials persisting across
   images is the point; a silently reinterpreted state blob is not.

## Spikes still unrun

- **Strip one app and measure it.** `ESP_WIFI_ENABLED=n`, `BT_ENABLED=n`, FOCUS
  only. Wanted: `.bin` size (sizes the slots), DIRAM, boot heap. Everything above
  about the radio lever is inferred from component sizes, **not** from a stripped
  build.
- **The OTA round trip.** Repartition, flash two images, prove switching works and
  that a deliberately corrupt image rolls back rather than bricking.

## When to revisit

When the app count makes isolation worth a reboot — the user's own trigger was
around **ten experiences**. Also revisit if any single app needs more than the
current ~145 KB of heap, or needs the CPU to itself.

Until then, adding an app is a row in `s_apps[]` and ~2 KB, and it switches in
1–26 ms instead of 2.7 s.
