package main

// Wi-Fi provisioning UI.
//
// The cube has a 480x480 panel, an on-screen keyboard and three side keys, so
// typing a WPA2 password on it is the worst interaction it has. This serves the
// page that replaces that: the phone talks to the cube directly over Web
// Bluetooth, renders the scan list with a real keyboard underneath, and hands
// the credentials back over an encrypted GATT link.
//
// Why it lives here rather than on any static host: **Web Bluetooth requires a
// secure context**, so the page has to come over HTTPS. This service is already
// behind Tailscale Funnel with a real certificate, so it is the one HTTPS origin
// this project already owns.
//
// Deliberately unauthenticated, unlike /token and /art. The page is inert
// markup — it holds no secret and talks to no Spotify endpoint. Reaching a cube
// with it needs BLE radio range *and* the six digits shown on that cube's
// screen, neither of which a bearer token here would add to. Requiring one would
// only mean typing a token into a phone browser before you could set up Wi-Fi.

import (
	"bytes"
	"embed"
	"net/http"
	"time"
)

//go:embed static/provision.html
var provisionFS embed.FS

// Baked into the binary at build time rather than read from disk, so the
// distroless image needs no filesystem layout and the running container cannot
// serve a file that was swapped underneath it.
func (b *broker) handleProvision(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	data, err := provisionFS.ReadFile("static/provision.html")
	if err != nil {
		http.Error(w, "not found", http.StatusNotFound)
		return
	}

	// No-store: the page is small and the pairing flow changes with the
	// firmware. A stale cached copy on a phone would speak an older GATT
	// protocol than the cube in front of it, and the failure would look like a
	// pairing bug rather than a caching one.
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Content-Type-Options", "nosniff")
	// It is served over Funnel, i.e. the public internet. Nothing here should
	// ever be framed by another origin.
	w.Header().Set("X-Frame-Options", "DENY")

	http.ServeContent(w, r, "provision.html", time.Time{}, bytes.NewReader(data))
}
