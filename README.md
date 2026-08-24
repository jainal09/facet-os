# Facet

A small operating system for a cube-shaped ESP32-S3 device with a round-cornered
480×480 AMOLED touchscreen — the
[Waveshare ESP32-S3-Touch-AMOLED-2.16](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.16).

A cube has facets; so does this. Apps are faces of one system rather than
separate firmwares, and only the one you are looking at costs any RAM.

<table align="center"><tr>
<td align="center"><img src="docs/images/lock.png" width="300" alt="Lock screen, now playing" /><br/><b>Lock screen</b><br/><sub>with the now-playing card while Spotify is active</sub></td>
<td align="center"><img src="docs/images/lock_ring.png" width="300" alt="Lock screen" /><br/><b>Lock screen</b><br/><sub>clock, date, battery ring over the day's wallpaper</sub></td>
</tr></table>

## What it is

The ESP32-S3 has 512 KB of internal SRAM, and after Wi-Fi and a display driver
have taken their share you are left with roughly 25 KB. That number, not flash
and not PSRAM, is what decides whether a feature fits. Facet is built around it:

- **Apps are built when opened and freed when closed.** A switch costs 1–26 ms,
  against ~2.7 s for a reboot, so there is no reason to page apps through OTA
  partitions. Adding a twentieth app costs nothing while it is closed.
- **State lives on the microSD card**, not in RAM and not in flash. Each app
  writes an opaque blob to `/sdcard/apps/<id>.bin` when it closes and reads it
  back when it opens, with NVS as a fallback when no card is present.
- **Assets stream to the card and are decoded from it.** The lock-screen
  wallpaper is fetched in the background at exactly panel resolution and never
  exists whole in RAM.
- **No VPN on the device.** Reaching private services goes through a Tailscale
  Funnel HTTPS endpoint with a bearer token, so TLS costs ~3 KB transiently per
  call instead of ~42 KB of permanent task stacks. That decision is what buys
  the frame rate; see [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Built in

| App | What it does | Hold right key |
|---|---|---|
| **CONTROL** | Settings and diagnostics in one scrolling column of cards: wallpaper pool, manual DAYS refresh, rotation calibration, battery and drain rate, network, system counters | Fetch a new wallpaper |
| **MUSIC** | A direct Spotify remote with cover art, queue lookahead, device transfer and volume controls | Play / pause |
| **FOCUS** | A Pomodoro timer you drive by turning the cube. Rotate to pick 60/30/10/5 and start; lay it flat to pause | Cancel the session |
| **DAYS** | A local-first countdown with a colour-changing progress bar. Tap it for a secure, one-time QR that opens your own phone editor. | Refresh now |

Sound is authored, not sampled — see
[assets/sounds/CREDITS.md](assets/sounds/CREDITS.md).

Plus a lock screen: a HUD clock with a battery ring and a sweeping arc, backed
by SNTP and a battery-backed RTC so the time is right the instant it wakes.

### Wi-Fi setup from your phone

There is no on-screen keyboard any more. CONTROL has a **PAIR** card: press it,
the cube shows a six-digit code and turns Wi-Fi off, and a web page on your
phone talks to it over Bluetooth — network list, real keyboard, done. Networks
you have joined before are marked *saved* and reconnect without a password.

The two radios cannot run at once on this board, so pairing takes Wi-Fi down for
the duration and restores it however the session ends. See
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the shape and
[docs/HARDWARE.md §7g](docs/HARDWARE.md) for the measurements behind it.

## Screens

<table align="center"><tr>
<td align="center"><img src="docs/images/music.png" width="180" alt="MUSIC" /><br/><b>MUSIC</b><br/><sub>now playing; volume slider being raised to 100</sub></td>
<td align="center"><img src="docs/images/music_liked.png" width="180" alt="MUSIC, liked" /><br/><b>MUSIC</b><br/><sub>track liked — the heart fills red</sub></td>
<td align="center"><img src="docs/images/devices.png" width="180" alt="Device picker" /><br/><b>MUSIC · devices</b><br/><sub>pick which speaker or computer plays</sub></td>
</tr><tr>
<td align="center"><img src="docs/images/control.png" width="180" alt="CONTROL" /><br/><b>CONTROL</b><br/><sub>battery care: the charge limit and the 100%-once override</sub></td>
<td align="center"><img src="docs/images/focus.png" width="180" alt="FOCUS" /><br/><b>FOCUS</b><br/><sub>rotate the cube to pick 60/30/10/5, tap to start</sub></td>
<td align="center"><img src="docs/images/days.png" width="180" alt="DAYS" /><br/><b>DAYS</b><br/><sub>countdown with a colour-shifting progress bar</sub></td>
</tr></table>

<table align="center"><tr>
<td align="center"><img src="docs/images/drawer.png" width="300" alt="App drawer" /><br/><b>The drawer</b><br/><sub>every app is a face of the cube; only the open one costs RAM</sub></td>
<td align="center"><img src="docs/images/bezel.png" width="300" alt="Bezel pop-out" /><br/><b>Bezel pop-out</b><br/><sub>press a side key and the black bezel swells into the screen</sub></td>
</tr></table>

Every image here was taken by the device itself — hold LEFT+RIGHT and it
saves a screenshot of its own framebuffer to the card (see CLAUDE.md,
*Autonomous hardware verification*). No camera, no emulator; these are the
real pixels.

## The three keys

Every app obeys the same contract, so nothing has to be re-learned per app.

| Key | Short press | Long press |
|---|---|---|
| **Left** (BOOT / minus, GPIO0) | Lock. Again while locked: sleep. | — |
| **Middle** (PWR, via the PMU) | Home | — |
| **Right** (plus, GPIO18) | Back one scene, or Home at an app's root | The app's primary action |

On the lock screen, holding the right key toggles **desk-clock mode**: the panel
stays lit indefinitely and the sweep ring turns amber.

## Build and flash

Requires [ESP-IDF v5.5](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/get-started/)
or newer.

```sh
git clone https://github.com/jainal09/facet-os.git
cd facet-os
cp .env.example .env      # then fill it in
. $IDF_PATH/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

Editing `.env` is enough — CMake regenerates the credentials header on the next
build. Nothing secret is ever written to a tracked file.

`managed_components/` is not committed; `idf.py build` restores it from
`main/idf_component.yml` and `dependencies.lock`. `components/` **is** committed
— the board BSP is forked there with six fixes the upstream one needs
([docs/HARDWARE.md §9](docs/HARDWARE.md)).

If the board stops accepting a flash, [docs/HARDWARE.md §8](docs/HARDWARE.md)
has the recovery sequence. A connected battery is usually why unplugging USB
does not power-cycle it.

## Configuration

All of it lives in `.env`:

| Variable | Notes |
|---|---|
| `WIFI_SSID` / `WIFI_PASSWORD` | Boot credentials; the WI-FI app overrides them at runtime |
| `STT_ENDPOINT_URL` | An HTTPS endpoint to poll — a Tailscale Funnel URL in practice |
| `STT_BEARER_TOKEN` | Leave empty to send no `Authorization` header |
| `TIMEZONE` | POSIX TZ string, e.g. `EST5EDT,M3.2.0,M11.1.0` or `IST-5:30` |
| `NTP_SERVER` | Defaults to `pool.ntp.org` |
| `UNSPLASH_KEY` | Unsplash **Access Key** only. Leave empty to disable wallpapers. |
| `UNSPLASH_QUERY` | Wallpaper themes separated by `;`, one picked at random per download |
| `BROKER_URL` | Public Facet broker origin; required for multi-user Spotify login |
| `BROKER_TOKEN` | Unique bearer for this cube; must match exactly one server `BROKER_USERS` entry |
| `SPOTIFY_CLIENT_ID` / `SPOTIFY_REFRESH_TOKEN` | Legacy brokerless mode only; leave empty with a broker |

Only the Unsplash Access Key belongs on the device — public read endpoints use
Client-ID auth. The Secret Key is for OAuth flows Facet does not use, so it
never reaches the firmware.

## Documentation

- **[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)** — the app model, the state
  store, the button contract, and how to write a new app.
- **[docs/HARDWARE.md](docs/HARDWARE.md)** — the board itself: pin map, the
  sdkconfig that works, the memory budget, rotation, the PMU, recovery, and
  every pitfall found the hard way. Read the pitfalls before debugging anything.

## License

MIT — see [LICENSE](LICENSE).
