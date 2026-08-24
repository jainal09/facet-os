# Facet Internet-Facing Security Review

Date: 2026-08-24

Scope: the Go broker, its embedded JavaScript pages, the broker-facing firmware
flow, Docker packaging, and the live broker deployment at `100.79.18.100` behind
Tailscale Funnel. Secrets were checked by presence and length only and are not
reproduced in this report.

## Executive summary

The application has no known unresolved Critical or High findings after the
changes in this review. The broker now uses authenticated, server-selected user
identities; stores Spotify refresh tokens per user; isolates DAYS state per
user; and gives firmware only short-lived Spotify access tokens. Missing or
revoked Spotify authorization is handled by a short-lived, user-bound QR flow.
The DAYS editor now uses its own five-minute, single-use QR exchange and a
30-minute countdown-only browser capability, so the permanent cube bearer never
enters a phone URL or browser.

The public HTTP surface was hardened with strict methods, request and server
limits, rate limiting, security headers, safe HTML construction, an exact-host
artwork allowlist with redirect revalidation, bounded image decoding, and a
nonroot/read-only/resource-limited container. Go was upgraded from 1.25.4,
where `govulncheck` found 24 reachable standard-library vulnerabilities, to
1.25.13. The final scan found zero reachable vulnerabilities.

The shared Ubuntu host was fully upgraded from the standard configured
repositories and rebooted into kernel 7.0.0-30. One Medium entitlement issue
remains outside the application: Canonical reports 24 additional ESM Apps fixes,
but the host is not attached to Ubuntu Pro and cannot receive them without the
owner's subscription/token.

## Remediated findings

### FACET-SEC-001 — High — Single-user authorization boundary

- Location: `broker/main.go:228`, `broker/spotify_auth.go:45`,
  `broker/countdown.go:72`.
- Evidence: the prior broker used one shared bearer and one account state. The
  current code maps every unique bearer to a server-configured user, accepts no
  caller-selected user ID, and scopes Spotify credentials and countdown paths
  to that authenticated identity.
- Impact: in a naive multi-user extension, one cube could read or overwrite
  another user's Spotify credential or DAYS countdown.
- Recommended fix: use the authenticated bearer as the tenancy boundary and
  reject duplicate users, duplicate bearers, weak bearers, and ambiguous input.
- Status/mitigation: fixed. Authentication comparisons are constant-time. The
  live deployment uses `BROKER_USERS` with the named user `jaxx`; legacy
  `BROKER_TOKEN` is disabled.

### FACET-SEC-002 — High — Long-lived OAuth credential exposure and refresh race

- Location: `broker/spotify_auth.go:217`, `broker/spotify_auth.go:244`,
  `broker/spotify_auth.go:300`.
- Evidence: the old pairing route returned a Spotify refresh token to firmware.
  The current broker persists refresh tokens in an atomic mode `0600` file and
  returns only access tokens. Same-user refreshes are serialized so token
  rotation cannot race and erase a winner's newly stored credential.
- Impact: a firmware dump would have exposed a long-lived Spotify grant, and
  concurrent refreshes could have forced unnecessary reauthorization.
- Recommended fix: keep refresh tokens server-side, rotate them atomically, and
  serialize refreshes per user.
- Status/mitigation: fixed and race-tested. A revoked grant clears only its own
  user and returns HTTP 428 with a new authorization URL.

### FACET-SEC-003 — High — OAuth callback reflected XSS

- Location: `broker/main.go:905`, `broker/main.go:914`,
  `broker/security.go:135`.
- Evidence: Spotify's callback `error` value was interpolated into an HTML page.
  It is now rendered through `html/template`, and every embedded inline block is
  constrained by a generated SHA-256 Content Security Policy without
  `unsafe-inline`.
- Impact: an attacker could otherwise construct a callback URL that executed
  script in the public broker origin.
- Recommended fix: context-aware HTML templates plus a strict CSP.
- Status/mitigation: fixed. Regression tests inject script and image-handler
  payloads and assert that they render only as escaped text.

### FACET-SEC-004 — High — Artwork SSRF and decompression/resource exhaustion

- Location: `broker/main.go:482`, `broker/main.go:573`,
  `broker/main.go:580`.
- Evidence: artwork fetches now require HTTPS, no URL userinfo, port 443 or the
  default port, and an exact Spotify CDN hostname. Every redirect is rechecked.
  Encoded bytes, decoded dimensions, total pixels, redirect count, and
  concurrent decoder count are bounded.
- Impact: a public image proxy can reach link-local/internal services or exhaust
  CPU and memory with oversized compressed images.
- Recommended fix: strict destination allowlisting and limits before full image
  decode.
- Status/mitigation: fixed. Tests cover deceptive hostnames, userinfo, private
  IPs, non-HTTPS URLs, unusual ports, and redirects leaving the allowlist.

### FACET-SEC-005 — High — Permanent cube bearer exposed to the DAYS browser

- Location: `broker/days_auth.go:84`, `broker/days_auth.go:115`,
  `broker/days_auth.go:165`, `broker/countdown.go:73`,
  `broker/static/days.html:109`.
- Evidence: at the start of this phase, the page asked the user to type the
  permanent cube bearer and retained it in JavaScript memory. That bearer also
  authorizes Spotify, art, queue, and QR endpoints. The replacement QR contains
  only 144 random bits of user-bound state with a hard five-minute TTL. Its
  atomic, single-use exchange returns a 256-bit, 30-minute bearer accepted only
  by `/countdown`; the broker stores only its SHA-256 hash. The page removes the
  one-time code from browser history immediately and keeps the temporary bearer
  only in tab memory.
- Impact: compromise of the old browser session could expose a long-lived,
  broad cube credential rather than a narrow countdown editing capability.
- Recommended fix: use a server-minted, user-bound, single-use capability and a
  separate least-privilege browser session with hard expiry.
- Status/mitigation: fixed and race-tested. Twenty-four simultaneous exchanges
  produce exactly one success. Live tests confirmed the temporary bearer gets
  200 only on `/countdown`, 401 on Spotify/art/link endpoints, and the consumed
  code returns 400 on replay.

### FACET-SEC-006 — High — Vulnerable Go standard library

- Location: `broker/go.mod:3`, `broker/Dockerfile:4`,
  `broker/Dockerfile:21`.
- Evidence: `govulncheck` under Go 1.25.4 reported 24 reachable standard-library
  vulnerabilities, including HTTP, TLS, URL parsing, and template issues fixed
  in later Go patch releases. The project and builder are pinned to Go 1.25.13,
  and the image build now fails if `govulncheck` finds a reachable issue.
- Impact: an Internet-facing service could be exposed through vulnerable
  standard-library code even when its own handlers are correct.
- Recommended fix: pin a patched Go release and make vulnerability scanning a
  release gate.
- Status/mitigation: fixed. Final local and container scans report zero reachable
  vulnerabilities.

### FACET-SEC-007 — Medium — Missing public-service resource and protocol limits

- Location: `broker/main.go:190`, `broker/security.go:20`,
  `broker/security.go:95`, `broker/docker-compose.yml:20`.
- Evidence: the server now has header/read/write/idle timeouts and header/body
  limits; sensitive routes use token buckets; read-only handlers reject other
  methods; and artwork workers are separately bounded. The container is
  nonroot, read-only, capability-free, loopback-bound, and limited to 256 MB,
  one CPU, and 128 PIDs.
- Impact: slow clients, oversized requests, or parallel expensive requests
  could otherwise consume process resources and degrade every user's cube.
- Recommended fix: enforce limits at the application, HTTP server, container,
  and edge layers.
- Status/mitigation: fixed in the application and container. Tailscale Funnel
  continues to supply TLS and public routing.

### FACET-SEC-008 — Medium — Pairing lifetime and replay semantics

- Location: `broker/main.go:278`, `broker/main.go:350`,
  `broker/spotify_auth.go:343`.
- Evidence: broker-minted pair codes contain 128 random bits, are bound to one
  authenticated user, have a hard five-minute age that scans cannot extend, and
  are consumed before the callback's network exchange. PKCE verifiers are
  server-generated and never accepted from a caller.
- Impact: reusable or caller-selected state would permit pairing-session
  fixation, replay, cross-user binding, or denial of service.
- Recommended fix: high-entropy server-minted state, a hard TTL, user binding,
  PKCE, and single-use consumption.
- Status/mitigation: fixed and regression-tested.

## Open operational findings

### FACET-OPS-001 — Medium — Ubuntu Pro entitlement gates 24 ESM Apps fixes

- Location: production host `100.79.18.100` (Ubuntu 24.04.4 LTS).
- Evidence: the standard `apt` transaction is complete, `dpkg --audit` is clean,
  `apt list --upgradable` returns zero packages, and the host runs patched kernel
  7.0.0-30. `pro security-status` still reports 24 Universe/Multiverse security
  fixes available only through ESM Apps and confirms this host is not attached.
- Impact: a host-level vulnerability can bypass protections inside the broker
  container and expose its credential-bearing Docker volume.
- Recommended fix: attach the host to the owner's Ubuntu Pro subscription and
  enable ESM Apps, then install and audit the additional updates. A free personal
  subscription is sufficient for an eligible personal deployment, but account
  enrollment requires owner credentials/consent.
- Current mitigation: all updates available from configured standard
  repositories were installed, including Docker, Tailscale, AppArmor, curl,
  Kerberos, NetworkManager, browser, firmware, and kernel packages. The broker
  remains isolated in a distroless, nonroot, read-only, loopback-only container.

### FACET-OPS-002 — Low — Refresh tokens are not application-layer encrypted at rest

- Location: `broker/spotify_auth.go:217`, Docker volume mounted at `/cache`.
- Evidence: each refresh token is stored in a mode `0600` JSON file on the
  Docker volume. Transport is HTTPS, but the stored value is plaintext to the
  broker process.
- Impact: compromise of the Docker host, root account, or unencrypted volume
  backup exposes Spotify grants.
- Recommended fix: encrypt the host disk and backups, restrict backup access,
  and consider a platform secret manager if the deployment grows beyond one
  trusted host. Application encryption with a key stored beside the data does
  not materially improve this threat.
- Current mitigation: mode `0600`, atomic writes, a nonroot/read-only container,
  and no API that returns refresh tokens.

### FACET-OPS-003 — Low — Per-cube bearer is recoverable from physical firmware

- Location: generated firmware credentials consumed by `main/main.c`.
- Evidence: a cube must retain its unique broker bearer to authenticate without
  interactive entry after every reboot.
- Impact: someone with physical/debug access to a cube may extract its bearer
  and access that cube's user-scoped broker routes.
- Recommended fix: treat each cube bearer as revocable inventory; rotate and
  remove it from `BROKER_USERS` if a cube is lost. Hardware-backed key storage
  can be evaluated for a later board revision.
- Current mitigation: unique per-user/per-cube credentials limit the blast
  radius; tokens shorter than 32 characters and duplicate bearers are rejected.

## Verification evidence

- `go vet ./...` passed.
- `go test -race ./...` passed, including multi-user Spotify and DAYS isolation,
  concurrent one-time QR consumption, scoped-session expiry/replay,
  refresh-rotation concurrency, OAuth state, CSP/XSS, rate-limit, and SSRF tests.
- `govulncheck ./...` under Go 1.25.13: zero reachable vulnerabilities.
- The production Docker build passed its embedded vet, tests, and vulnerability
  scan before final image `sha256:9818c944...` replaced the running container.
- Live checks confirmed health 200, unexpected health method 405, unauthenticated
  art 401, authenticated missing Spotify credential 428, strict HTML security
  headers including HSTS/COOP/CORP, loopback-only port binding,
  nonroot/read-only execution, dropped capabilities, and configured
  memory/CPU/PID limits.
- The final live DAYS exchange returned countdown 200, Spotify/art/link 401,
  replay 400, invalid content type 415, and unauthenticated link 401. The direct
  page contains no device-key prompt. The Spotify credential store remains mode
  `0600` and 308 bytes after both deployments and the host reboot.
- The cube completed the QR authorization flow after deployment; the named
  user's token route then returned 200 and live queue/artwork requests resumed.
- Firmware built and flashed successfully. The live serial check showed the new
  image booting, Wi-Fi connected, cached DAYS available immediately, a 59-byte
  live DAYS sync, and a healthy periodic memory/status line.
- The host full upgrade completed with exit status zero, `dpkg --audit` clean,
  zero standard upgrades remaining, and no reboot required after booting kernel
  7.0.0-30. Docker, Tailscale Funnel, SSH, broker, Nextcloud frontend/Redis/
  Imaginary, and RustDesk returned. The Nextcloud database remains in its
  pre-existing `unhealthy` state; the pre-existing failed `vncserver@5` and
  `vpn-monitor` units also remain outside this broker change.
