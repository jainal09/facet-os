# Facet broker

The small amount of server the cube cannot be. It handles OAuth, album-art
transcoding, and tiny phone-friendly editors, while staying out of the
interactive path for everything else.

```
   ┌─────────┐   control + state, direct, 6 ms warm    ┌──────────────┐
   │  CUBE   │ ─────────────────────────────────────▶  │ api.spotify  │
   │         │ ◀─────────────────────────────────────  └──────────────┘
   │         │
   │         │   pairing, art, and DAYS state          ┌──────────────┐
   │         │ ─────────────────────────────────────▶  │   broker     │
   └─────────┘                                         └──────────────┘
```

**If this service is down, playback control still works and DAYS keeps its last
saved countdown.** The cube holds its own Spotify refresh token and talks
straight to Spotify. The broker is not in either app's interactive path.

## Why it exists

**Pairing.** Spotify's OAuth must redirect to an HTTPS URL, and Spotify does not
offer the device-code flow that would let a screen with no keyboard pair on its
own. Something reachable over HTTPS has to catch the callback.

**Album art.** Measured: Spotify already serves **baseline** JPEG, so the device
*can* decode it unaided — this endpoint is an optimisation, not a rescue. It
centre-crops and scales to exactly the size the panel draws, which turns a
27 KB 300x300 into 10 KB, and it keeps the guarantee explicit rather than
depending on Spotify never switching to progressive. The firmware's own decoder
handles baseline only, and its baseline path never populates LVGL's image cache
(see [`docs/HARDWARE.md` §7c](../docs/HARDWARE.md)), so having one predictable
format arriving at one known size is worth keeping.

**Wi-Fi setup — served here, but no longer depended on.** `/provision` is inert
markup that speaks Web Bluetooth to the cube and makes no request of any server,
so the only thing it ever wanted from this service was an HTTPS certificate.
`.github/workflows/pages.yml` publishes the same file to GitHub Pages, and the
cube's setup QR points there. Wi-Fi setup therefore keeps working when this box
does not — which matters, because a cube with no credentials cannot reach
anything to fix itself, and the failure would arrive exactly when the broker is
already down. The copy served here stays as a fallback and the file under
`static/` remains the single source of truth, since `go:embed` cannot reach
outside this module.

**DAYS.** A phone or laptop serves as the comfortable date picker and keyboard.
`/days` is inert public markup; the page asks for the device bearer before it can
read or change `/countdown`. The compact state is atomically persisted under the
cache volume, while the cube keeps its own copy for an instant, offline-first
open.

## Endpoints

| Route | Auth | Purpose |
|---|---|---|
| `GET /healthz` | — | Liveness plus cache counters |
| `GET /pair?c=CODE` | — | Mints a PKCE verifier, redirects the phone to Spotify |
| `GET /callback` | — | Registered redirect URI; exchanges the code for tokens |
| `GET /token?c=CODE` | Bearer | Cube polls this; returns the refresh token **once**, then forgets it |
| `GET /art?u=URL&s=240` | Bearer | Fetch, centre-crop, scale, re-encode as baseline JPEG, cache |
| `GET /art.bin?u=URL&s=240` | Bearer | Same, but returns a pre-decoded RGB565 bitmap in LVGL's binary format — **the device does no decoding at all** |
| `GET /days` | — | Phone-friendly date/message editor; contains no countdown state or secret |
| `GET /provision` | — | Wi-Fi setup page. **Mirrored to GitHub Pages, which is where the cube's QR points** — see below |
| `GET /countdown` | Bearer | Compact cube payload: target date, message, and date set |
| `POST /countdown` | Bearer | Validate and atomically replace the DAYS state |

## Security

This sits behind Tailscale Funnel, which means **it is on the public internet**.
That shapes every decision here:

- `/token` and `/art` require a bearer matching `BROKER_TOKEN`, compared in
  constant time.
- `/countdown` uses the same bearer. `/days` is only inert markup; possessing its
  URL does not reveal or grant access to the saved countdown.
- `/art` takes an **allowlist** of Spotify image hosts, not a URL filter. A
  service that fetches arbitrary URLs on request is an open proxy — usable to
  reach link-local metadata endpoints or to launder traffic. Scheme must be
  `https` too, so `http://169.254.169.254/...` is refused twice over.
- Pair codes are `[A-Z0-9]{4,16}`, single-use, and expire after five minutes.
  The refresh token is deleted the moment the cube collects it, so a leaked pair
  code is worthless after pairing completes.
- Request logging records the path only, never the query — that carries pair
  codes and image URLs.
- Secrets come from an `EnvironmentFile`, not the command line, because
  `/proc/PID/cmdline` is world-readable.

Remaining known gap: the allowlist is by hostname, so an attacker who could
control DNS for `i.scdn.co` could redirect the fetch. Not worth solving for this
threat model, but worth knowing.

## Run it

```sh
cp broker.env.example broker.env      # fill in, then chmod 600
docker compose up -d --build
curl -s localhost:8080/healthz
```

The image is **8.7 MB**: a multi-stage build producing a single static binary on
`distroless/static`. No shell, no package manager, no Go toolchain in the
runtime — `docker exec <c> id` fails, which is the point.

`go vet` and the tests run **inside the build stage**, so a regression in the
baseline-JPEG guarantee fails the image build rather than shipping a cover the
cube renders as nothing.

Compose runs it unprivileged: `nonroot`, read-only root filesystem, all
capabilities dropped, `no-new-privileges`, and bound to `127.0.0.1:8080` only —
Funnel is what publishes it, and binding `0.0.0.0` would additionally expose it
to the LAN.

## Expose it with Tailscale Funnel

```sh
sudo tailscale funnel --bg --https=443 http://127.0.0.1:8080
sudo tailscale funnel status
```

Funnel must first be enabled for the tailnet — the CLI prints a
`login.tailscale.com/f/funnel?node=...` link and blocks until an admin approves
it. Note the CLI syntax: `tailscale funnel 8080 on` is the **old** form and now
errors with "the CLI for serve and funnel has changed".

Then add `https://<host>.<tailnet>.ts.net/callback` to the Spotify app's Redirect
URIs. Spotify matches it character for character.

## Measured against the real API

Verified end to end through Funnel from the public internet:

| | |
|---|---|
| `GET /v1/me/player` | 3,190 B |
| `GET /v1/me/player/devices` | 815 B |
| Spotify cover art, 300x300 | 27,469 B, **SOF0 — baseline** |
| Same through `/art?s=240` | 10,532 B (38%), baseline, ~185 ms |
| Same through `/art.bin?s=240` | 115,212 B raw RGB565, ~209 ms (49 ms cached) |
| Refresh token -> access token | 379 B, `expires_in=3600` |

Two findings that changed the firmware plan:

- **Spotify's cover art is already baseline JPEG**, so the device can decode it
  directly. The transcode is now an *optimisation* — 62% fewer bytes at exactly
  the size the panel draws — rather than a requirement.
- **The refresh token does not rotate.** Spotify returns no new `refresh_token`
  on refresh for this flow, so the firmware stores it once and never has to
  re-persist it.

## Testing

`go test ./...` covers the part that carries real risk. In particular it asserts
on the **JPEG SOF marker** of the output rather than trusting that `image/jpeg`
keeps writing baseline — if that ever regressed, the cube would show nothing and
fail silently, which is the worst kind of bug to inherit.

`findSOF` in `main_test.go` is the same check that was run against a real
`i.scdn.co` URL to settle whether the transcode was required. Answer: Spotify
returns `0xC0` (baseline), so it is not — it is a 62% bandwidth saving and a
format guarantee. Re-run it if covers ever stop rendering on the device; `0xC2`
would mean Spotify switched to progressive.
