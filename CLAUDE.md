# Facet — agent context

Firmware for a cube-shaped ESP32-S3 device with a 480×480 round-cornered AMOLED
touchscreen (Waveshare ESP32-S3-Touch-AMOLED-2.16). An app-drawer OS where apps
are built on open, freed on close, and keep their state on a microSD card.

**Read these two before doing anything non-trivial:**

- @docs/HARDWARE.md — the board. Pin map, the sdkconfig that works, the memory
  budget, rotation, the PMU, recovery, and a pitfalls index written from real
  symptoms. **Check §10 before debugging anything.**
- @docs/ARCHITECTURE.md — the app model, the state store, the button contract,
  how to add an app.

## Working on this repo

**Build and flash.** ESP-IDF v5.5.5 lives at `~/esp/esp-idf`.

```sh
. ~/esp/esp-idf/export.sh
idf.py build
PORT=$(ls /dev/cu.usbmodem* | head -1); [ -z "$PORT" ] && exit 1
idf.py -p "$PORT" flash
```

Always guard the port glob. An unguarded `-p $(ls /dev/cu.usbmodem*)` with no
device attached makes esptool auto-detect and start writing, which produces a
partial image and wedges the board. This has happened.

**Reading the serial console.** `idf.py monitor` is interactive and awkward to
drive from a tool call. Prefer a bounded pyserial capture with the IDF's python:

```sh
. ~/esp/esp-idf/export.sh
PORT=$(ls /dev/cu.usbmodem* | head -1)
python -c "
import serial,time
s=serial.Serial('$PORT',115200,timeout=1); t0=time.time()
while time.time()-t0<45:
    l=s.readline().decode('utf8','replace').rstrip()
    if l: print(l, flush=True)
"
```

The firmware prints a one-line status every 15 s carrying uptime, wall clock,
active screen, idle timers, Wi-Fi, battery, fps, rotation, SD rows and heap.
Nearly every bug in this project was diagnosed from that line — read it before
adding new instrumentation.

**Secrets.** `.env` at the repo root is gitignored and is the only place real
credentials live. CMake parses it at configure time into
`build/generated/credentials.h` via `main/credentials.h.in`. Never write a
credential into a tracked file, never echo `.env` into a commit message or an
artifact, and never add `main/credentials.h` back — it would shadow the
generated header.

Only the Unsplash **Access Key** belongs on the device. The Secret Key is for
OAuth flows this project does not use.

**Editing `main/main.c`.** It is ~2800 lines and ordering matters — C needs
definitions before use, and several forward declarations near the top exist
exactly to break cycles. Use Edit with unique anchors. Do **not** splice it with
a Python script that slices between two markers: one such splice deleted ~250
lines of this file in a single call. If a large structural change is genuinely
needed, copy the file first.

**Verify on hardware.** A build that compiles proves very little here. Flash it
and read at least one status line before calling something done. Several bugs in
the pitfalls index compiled perfectly.

## Conventions

- Comments explain *why*, especially where the code looks wrong but is not —
  most of the odd-looking lines here encode a hardware constraint. Don't add
  comments that restate the next line.
- Keep the build at zero warnings. `-Woverflow` caught a real coordinate bug
  that had been invisible in the noise.
- The three side keys have one global contract (see ARCHITECTURE.md). Do not
  give a key an app-specific meaning outside `app_action()` / `app_back()`.
- The lock screen is deliberately sparse: clock, date, battery ring. No status
  text, no photo credit. Attribution goes in the CONTROL app.
- **On-screen strings must be pure ASCII.** `lv_font_montserrat_14` carries ASCII
  plus LVGL's own `LV_SYMBOL_*` glyphs and nothing else, so a typographic
  character renders as an empty box — `·` and `—` are the easy mistakes, and
  `hud_text_18` (0x20-0x7F) and `hud_clock_76` (digits and colon only) are
  narrower still. Non-ASCII in `ESP_LOG` strings is fine; that goes to serial.
- **Never give a second cube an existing cube's `BROKER_TOKEN`, and say so before
  anyone sets one up.** The bearer *is* the identity, so the two cubes become one
  broker user and silently share the Spotify account and the DAYS countdown — the
  new cube opens MUSIC already playing someone else's music, and nothing errors.
  Mint a fresh `openssl rand -hex 32` per cube and add a `BROKER_USERS` entry.
  `SPOTIFY_CLIENT_ID` is global to the broker: N cubes are one Spotify app, N
  bearers, up to N accounts. Tell the user that a Development-mode app admits only
  five hand-added accounts, that the app must be matched by Client ID rather than
  by name, and that a missing allowlist entry surfaces only as
  `403 The user is not registered for this application` on every Spotify endpoint
  — long after the login appeared to succeed. See README and ARCHITECTURE.md.

## Keep the docs hot

`docs/HARDWARE.md` carries a standing maintenance directive at the top and it is
meant literally: when you burn time on a symptom, discover a magic register
value, or find something in there that is now wrong, update it **in the same
session**, correcting in place rather than appending caveats. That file is the
reason this board is tractable at all.
