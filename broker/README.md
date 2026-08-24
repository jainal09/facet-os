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

**If this service is down, playback control keeps working until the cube's
current Spotify access token expires, and DAYS keeps its last saved countdown.**
Refresh tokens stay encrypted in transit and are stored in a mode `0600` file on
the broker volume; the cube receives only short-lived access tokens and still
sends interactive calls straight to Spotify.

## Why it exists

**Pairing and token custody.** Spotify's OAuth must redirect to an HTTPS URL.
Each cube authenticates with a unique broker bearer, which selects one isolated
credential record. A missing or revoked refresh token makes `/spotify/token`
return a one-time authorization URL; MUSIC shows it as a QR, and Spotify's page
handles login, consent, and any 2FA. The callback persists the refresh token
under the broker's cache volume and returns only access tokens to firmware.

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
Tapping DAYS asks the authenticated broker for a five-minute, single-use QR
link. The browser immediately exchanges that code for a random 30-minute bearer
that works only on this user's `/countdown`; the permanent cube bearer never
enters the URL or browser. The compact state is atomically persisted per user
under the cache volume, while the cube keeps its own copy for an instant,
offline-first open and refreshes it on every boot/app open and daily.

## Endpoints

| Route | Auth | Purpose |
|---|---|---|
| `GET /healthz` | — | Liveness plus cache counters |
| `GET /spotify/token` | Bearer | Returns this user’s short-lived Spotify access token, or `428` with a one-time authorization URL |
| `GET /spotify/token?force=1` | Bearer | Bypasses the access-token cache after Spotify rejected a token |
| `GET /pair?c=CODE` | — | Consumes a broker-minted, user-bound pair code and redirects the phone to Spotify |
| `GET /callback` | — | Registered redirect URI; exchanges the code for tokens |
| `GET /art?u=URL&s=240` | Bearer | Fetch, centre-crop, scale, re-encode as baseline JPEG, cache |
| `GET /art.bin?u=URL&s=240` | Bearer | Same, but returns a pre-decoded RGB565 bitmap in LVGL's binary format — **the device does no decoding at all** |
| `GET /days` | — | Phone-friendly date/message editor; contains no countdown state or secret |
| `GET /days/link` | Cube bearer | Mint or reuse this user’s five-minute, one-time editor QR link |
| `POST /days/session` | One-time QR code | Consume the code and return a 30-minute, countdown-only browser bearer |
| `GET /provision` | — | Wi-Fi setup page. **Mirrored to GitHub Pages, which is where the cube's QR points** — see below |
| `GET /countdown` | Cube or DAYS bearer | Compact user-scoped payload: target date, message, and date set |
| `POST /countdown` | Cube or DAYS bearer | Validate and atomically replace that user’s DAYS state |

## Security

This sits behind Tailscale Funnel, which means **it is on the public internet**.
That shapes every decision here:

- `/spotify/token`, `/art`, `/queue`, and `/countdown` require a bearer from
  `BROKER_USERS`, compared in constant time. The bearer itself selects the user;
  no caller-supplied user ID can cross that boundary.
- Spotify refresh tokens are atomically persisted as mode `0600` in the broker
  volume and are never returned to firmware. This is credential storage, so back
  up and protect that Docker volume accordingly. It stores OAuth refresh tokens,
  never Spotify passwords or 2FA secrets.
- `/countdown` is stored per authenticated user. A QR code has 144 random bits,
  is user-bound, single-use, and expires after five minutes. Its exchanged
  browser bearer has 256 random bits, expires after 30 minutes, is stored only
  as a SHA-256 hash by the broker, and cannot access Spotify, art, queues, or
  mint another link.
- `/art` takes an **allowlist** of Spotify image hosts, not a URL filter. A
  service that fetches arbitrary URLs on request is an open proxy — usable to
  reach link-local metadata endpoints or to launder traffic. Scheme must be
  `https`; every redirect is checked against the same exact-host allowlist, and
  encoded size, decoded dimensions, pixel count, and concurrent decoders are
  bounded.
- Pair codes are 128-bit random, 32-character uppercase hexadecimal values,
  user-bound, single-use, and
  expire after five minutes. `/pair` refuses codes that were not minted by an
  authenticated `/spotify/token` request.
- Request logging records the path only, never the query — that carries pair
  codes and image URLs.
- Per-user and per-client token buckets protect sensitive routes, while body,
  header, server timeout, process, memory, CPU, and PID limits cap resource use.
- HTML uses a hash-based Content Security Policy and safe DOM construction.
  The DAYS page removes its one-time code from browser history immediately; its
  temporary bearer stays only in page memory and disappears when the tab closes.
- `BROKER_PUBLIC_URL` must be a bare HTTPS origin. Generated QR links cannot be
  redirected to an insecure scheme or a credential-bearing URL by bad config.
- Secrets come from an `EnvironmentFile`, not the command line, because
  `/proc/PID/cmdline` is world-readable.

## Run it

```sh
cp broker.env.example broker.env      # fill in, then chmod 600
docker compose up -d --build
curl -s localhost:8080/healthz
```

Give every cube its own generated bearer. For example:

```dotenv
BROKER_USERS=alice=TOKEN_FOR_ALICE_CUBE,bob=TOKEN_FOR_BOB_CUBE
```

Put only `TOKEN_FOR_ALICE_CUBE` in Alice's firmware `.env`, and only Bob's token
in Bob's. User labels are local broker identifiers, not Spotify usernames. A
legacy single-user `BROKER_TOKEN` still maps to a user named `default`; do not
set it on a new multi-user deployment.

The image is **8.7 MB**: a multi-stage build producing a single static binary on
`distroless/static`. No shell, no package manager, no Go toolchain in the
runtime — `docker exec <c> id` fails, which is the point.

`go vet`, the tests, and `govulncheck` run **inside the build stage**, so a known
reachable Go vulnerability or a regression in the baseline-JPEG guarantee fails
the image build rather than shipping.

Compose runs it unprivileged: `nonroot`, read-only root filesystem, all
capabilities dropped, `no-new-privileges`, bounded to 256 MB / one CPU / 128
PIDs, and bound to `127.0.0.1:8080` only — Funnel is what publishes it, and
binding `0.0.0.0` would additionally expose it to the LAN.

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
- **The observed refresh token did not rotate**, but the broker still persists a
  replacement if Spotify supplies one. Firmware never receives either value.

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
