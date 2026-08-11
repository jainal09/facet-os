# Facet broker

The small amount of server the cube cannot be. Two jobs, and it stays out of the
interactive path for everything else.

```
   ┌─────────┐   control + state, direct, 6 ms warm    ┌──────────────┐
   │  CUBE   │ ─────────────────────────────────────▶  │ api.spotify  │
   │         │ ◀─────────────────────────────────────  └──────────────┘
   │         │
   │         │   one-time pairing, and album art       ┌──────────────┐
   │         │ ─────────────────────────────────────▶  │   broker     │
   └─────────┘                                         └──────────────┘
```

**If this service is down, playback control still works.** The cube holds its own
refresh token and talks straight to Spotify. Only album art and first-time
pairing depend on the broker, so it is not a dependency of daily use.

## Why it exists

**Pairing.** Spotify's OAuth must redirect to an HTTPS URL, and Spotify does not
offer the device-code flow that would let a screen with no keyboard pair on its
own. Something reachable over HTTPS has to catch the callback.

**Album art.** The firmware can only decode **baseline** JPEG, and its baseline
decoder never populates LVGL's image cache — so a stock Spotify cover is either
undecodable or re-decoded fifteen times per frame (see
[`docs/HARDWARE.md` §7c](../docs/HARDWARE.md)). Go decodes progressive happily
and only ever *writes* baseline, so transcoding here removes the entire risk
class. It also centre-crops and scales to exactly the size the panel draws,
turning a ~40 KB 640×640 into a few KB.

## Endpoints

| Route | Auth | Purpose |
|---|---|---|
| `GET /healthz` | — | Liveness plus cache counters |
| `GET /pair?c=CODE` | — | Mints a PKCE verifier, redirects the phone to Spotify |
| `GET /callback` | — | Registered redirect URI; exchanges the code for tokens |
| `GET /token?c=CODE` | Bearer | Cube polls this; returns the refresh token **once**, then forgets it |
| `GET /art?u=URL&s=240` | Bearer | Fetch, centre-crop, scale, re-encode as baseline JPEG, cache |

## Security

This sits behind Tailscale Funnel, which means **it is on the public internet**.
That shapes every decision here:

- `/token` and `/art` require a bearer matching `BROKER_TOKEN`, compared in
  constant time.
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
go test ./...
go build -o facet-broker .

BROKER_TOKEN=$(openssl rand -hex 32) \
SPOTIFY_CLIENT_ID=... \
SPOTIFY_REDIRECT_URI=https://your-host.tsXXXX.ts.net/callback \
  ./facet-broker -addr 127.0.0.1:8080
```

## Deploy to the Pi

```sh
GOOS=linux GOARCH=arm64 go build -o facet-broker .
scp facet-broker pi:/tmp/ && ssh pi 'sudo mv /tmp/facet-broker /usr/local/bin/'

# on the Pi
sudo install -m 600 broker.env /etc/facet-broker.env     # from broker.env.example
sudo cp facet-broker.service /etc/systemd/system/
sudo systemctl enable --now facet-broker
sudo tailscale funnel --bg 8080
```

Then add the resulting `https://<host>.ts.net/callback` to the Spotify app's
Redirect URIs. Spotify matches it character for character.

The unit runs under `DynamicUser` with `ProtectSystem=strict` and only
`AF_INET`/`AF_INET6` — it fetches images and writes one cache directory, and
should not be able to do anything else.

## Testing

`go test ./...` covers the part that carries real risk. In particular it asserts
on the **JPEG SOF marker** of the output rather than trusting that `image/jpeg`
keeps writing baseline — if that ever regressed, the cube would show nothing and
fail silently, which is the worst kind of bug to inherit.

`findSOF` in `main_test.go` is also the exact check worth running against a real
`i.scdn.co` URL to settle whether the transcode is strictly required or merely a
nice optimisation. `0xC0` is baseline, `0xC2` is progressive.
