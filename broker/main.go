// Facet broker — the small amount of server the cube cannot be.
//
// It exists for exactly two jobs, and deliberately stays out of the interactive
// path for everything else:
//
//  1. Pairing. Spotify's OAuth needs an HTTPS redirect target, and it does not
//     offer the device-code flow that would let a screen with no keyboard pair
//     on its own. So something reachable over HTTPS has to catch the callback.
//     Once paired, the cube holds a refresh token and talks straight to
//     api.spotify.com — this service can be down and playback control still
//     works.
//
//  2. Album art. The firmware can only decode *baseline* JPEG, and its baseline
//     decoder never populates LVGL's image cache, so a stock Spotify image is
//     either undecodable or re-decoded fifteen times per frame. Transcoding here
//     removes the whole risk class and drops a 640x640 cover to a few KB at
//     exactly the size the panel draws.
//
// Deployed behind Tailscale Funnel, which means it is on the public internet.
// Everything below is written with that in mind: no open image proxy, no
// unauthenticated token store, single-use pair codes with a short TTL.
package main

import (
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"image"
	"image/jpeg"
	"io"
	"log"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	"golang.org/x/image/draw"
)

const (
	spotifyAuthURL  = "https://accounts.spotify.com/authorize"
	spotifyTokenURL = "https://accounts.spotify.com/api/token"

	// Only what a remote control needs. Asking for less makes the consent
	// screen honest and limits the damage if a refresh token ever leaks.
	spotifyScopes = "user-read-playback-state user-modify-playback-state"

	pairTTL     = 5 * time.Minute
	artMaxSize  = 640
	artMinSize  = 64
	artMaxBytes = 8 << 20 // refuse absurd upstream images
)

// artHostAllow is an allowlist, not a filter. Funnel makes this endpoint public,
// and a service that fetches arbitrary URLs on request is an open proxy — usable
// to probe private networks or to launder traffic. Spotify serves all cover art
// from i.scdn.co, so nothing else is ever legitimate.
var artHostAllow = map[string]bool{
	"i.scdn.co":                   true,
	"mosaic.scdn.co":              true, // playlist mosaics
	"image-cdn-ak.spotifycdn.com": true,
	"image-cdn-fa.spotifycdn.com": true,
}

type config struct {
	addr         string
	clientID     string
	clientSecret string // optional; PKCE works without it
	redirectURI  string
	deviceToken  string // shared secret the cube presents
	cacheDir     string
}

// pending is one in-flight pairing. Held in memory on purpose: it lives for at
// most pairTTL, and losing them all on restart is the correct behaviour.
type pending struct {
	verifier  string
	created   time.Time
	refresh   string // filled in once the callback completes
	completed bool
}

type broker struct {
	cfg config

	mu      sync.Mutex
	pairs   map[string]*pending
	artHits int
	artMiss int

	http *http.Client
}

func main() {
	var cfg config
	flag.StringVar(&cfg.addr, "addr", envOr("BROKER_ADDR", ":8080"), "listen address")
	flag.StringVar(&cfg.clientID, "client-id", os.Getenv("SPOTIFY_CLIENT_ID"), "Spotify client ID")
	flag.StringVar(&cfg.clientSecret, "client-secret", os.Getenv("SPOTIFY_CLIENT_SECRET"), "Spotify client secret (optional, PKCE works without)")
	flag.StringVar(&cfg.redirectURI, "redirect-uri", os.Getenv("SPOTIFY_REDIRECT_URI"), "must match the app registration exactly")
	flag.StringVar(&cfg.deviceToken, "device-token", os.Getenv("BROKER_TOKEN"), "shared secret the cube presents")
	flag.StringVar(&cfg.cacheDir, "cache-dir", envOr("BROKER_CACHE", "./cache"), "transcoded art cache")
	flag.Parse()

	if cfg.deviceToken == "" {
		log.Fatal("BROKER_TOKEN is required: /token and /art are public once Funnel is on")
	}
	if cfg.clientID == "" || cfg.redirectURI == "" {
		log.Println("warning: SPOTIFY_CLIENT_ID or SPOTIFY_REDIRECT_URI unset — /pair will refuse, /art still works")
	}
	if err := os.MkdirAll(cfg.cacheDir, 0o755); err != nil {
		log.Fatalf("cache dir: %v", err)
	}

	b := &broker{
		cfg:   cfg,
		pairs: map[string]*pending{},
		http:  &http.Client{Timeout: 20 * time.Second},
	}
	go b.reapPairs()

	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", b.handleHealth)
	mux.HandleFunc("/pair", b.handlePair)
	mux.HandleFunc("/callback", b.handleCallback)
	mux.HandleFunc("/token", b.handleToken)
	mux.HandleFunc("/art", b.handleArt)
	mux.HandleFunc("/art.bin", b.handleArt)

	srv := &http.Server{
		Addr:              cfg.addr,
		Handler:           logging(mux),
		ReadHeaderTimeout: 10 * time.Second,
	}
	log.Printf("facet broker listening on %s (cache %s)", cfg.addr, cfg.cacheDir)
	if err := srv.ListenAndServe(); err != nil && !errors.Is(err, http.ErrServerClosed) {
		log.Fatal(err)
	}
}

func envOr(k, def string) string {
	if v := os.Getenv(k); v != "" {
		return v
	}
	return def
}

func logging(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		next.ServeHTTP(w, r)
		// Deliberately logs the path only, never the query: it carries pair
		// codes and image URLs.
		log.Printf("%s %s %s", r.Method, r.URL.Path, time.Since(start).Round(time.Millisecond))
	})
}

// authed guards the endpoints the cube calls. Constant-time compare because this
// is reachable from the internet and a timing oracle on a shared secret is free
// to exploit.
func (b *broker) authed(r *http.Request) bool {
	got := strings.TrimPrefix(r.Header.Get("Authorization"), "Bearer ")
	return subtle.ConstantTimeCompare([]byte(got), []byte(b.cfg.deviceToken)) == 1
}

func (b *broker) handleHealth(w http.ResponseWriter, r *http.Request) {
	b.mu.Lock()
	pairs, hits, miss := len(b.pairs), b.artHits, b.artMiss
	b.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]any{
		"ok": true, "pending_pairs": pairs, "art_cache_hits": hits, "art_cache_misses": miss,
	})
}

// ---------------------------------------------------------------- pairing

// handlePair is what the QR code on the cube points at. The cube generates the
// code itself and encodes it in the QR, so it already knows what to poll for;
// this only has to mint a PKCE verifier and bounce the phone to Spotify.
func (b *broker) handlePair(w http.ResponseWriter, r *http.Request) {
	if b.cfg.clientID == "" || b.cfg.redirectURI == "" {
		http.Error(w, "broker not configured for pairing", http.StatusServiceUnavailable)
		return
	}
	code := r.URL.Query().Get("c")
	if !validPairCode(code) {
		http.Error(w, "bad pair code", http.StatusBadRequest)
		return
	}

	verifier, err := randomURLSafe(64)
	if err != nil {
		http.Error(w, "entropy", http.StatusInternalServerError)
		return
	}
	sum := sha256.Sum256([]byte(verifier))
	challenge := base64.RawURLEncoding.EncodeToString(sum[:])

	b.mu.Lock()
	b.pairs[code] = &pending{verifier: verifier, created: time.Now()}
	b.mu.Unlock()

	q := url.Values{
		"client_id":             {b.cfg.clientID},
		"response_type":         {"code"},
		"redirect_uri":          {b.cfg.redirectURI},
		"scope":                 {spotifyScopes},
		"code_challenge_method": {"S256"},
		"code_challenge":        {challenge},
		"state":                 {code},
	}
	http.Redirect(w, r, spotifyAuthURL+"?"+q.Encode(), http.StatusFound)
}

// handleCallback is the registered Spotify redirect URI. A human's phone lands
// here, so it answers in HTML rather than JSON.
func (b *broker) handleCallback(w http.ResponseWriter, r *http.Request) {
	q := r.URL.Query()
	if e := q.Get("error"); e != "" {
		page(w, http.StatusOK, "Denied", "Spotify returned: "+e)
		return
	}
	code, state := q.Get("code"), q.Get("state")

	b.mu.Lock()
	p := b.pairs[state]
	b.mu.Unlock()
	if p == nil {
		page(w, http.StatusBadRequest, "Expired", "That pairing link has expired. Start again from the cube.")
		return
	}

	form := url.Values{
		"grant_type":    {"authorization_code"},
		"code":          {code},
		"redirect_uri":  {b.cfg.redirectURI},
		"client_id":     {b.cfg.clientID},
		"code_verifier": {p.verifier},
	}
	tok, err := b.tokenRequest(form)
	if err != nil {
		log.Printf("token exchange failed: %v", err)
		page(w, http.StatusBadGateway, "Failed", "Could not complete pairing. Try again.")
		return
	}
	if tok.RefreshToken == "" {
		page(w, http.StatusBadGateway, "Failed", "Spotify did not return a refresh token.")
		return
	}

	b.mu.Lock()
	p.refresh = tok.RefreshToken
	p.completed = true
	b.mu.Unlock()

	page(w, http.StatusOK, "Paired", "You can put your phone down — the cube is picking this up now.")
}

// handleToken is polled by the cube. The token is handed over exactly once and
// then erased, so a leaked pair code is worthless the moment pairing completes.
func (b *broker) handleToken(w http.ResponseWriter, r *http.Request) {
	if !b.authed(r) {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	code := r.URL.Query().Get("c")
	if !validPairCode(code) {
		http.Error(w, "bad pair code", http.StatusBadRequest)
		return
	}

	b.mu.Lock()
	p := b.pairs[code]
	var refresh string
	if p != nil && p.completed {
		refresh = p.refresh
		delete(b.pairs, code)
	}
	b.mu.Unlock()

	if p == nil {
		http.Error(w, "unknown or expired", http.StatusNotFound)
		return
	}
	if refresh == "" {
		// Not an error — the human simply has not finished logging in.
		writeJSON(w, http.StatusAccepted, map[string]any{"status": "waiting"})
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"refresh_token": refresh})
}

type tokenResp struct {
	AccessToken  string `json:"access_token"`
	RefreshToken string `json:"refresh_token"`
	ExpiresIn    int    `json:"expires_in"`
	Scope        string `json:"scope"`
}

func (b *broker) tokenRequest(form url.Values) (*tokenResp, error) {
	req, err := http.NewRequest("POST", spotifyTokenURL, strings.NewReader(form.Encode()))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
	// A confidential client may also send basic auth; PKCE does not need it, and
	// omitting the secret is what lets the cube refresh on its own afterwards.
	if b.cfg.clientSecret != "" {
		req.SetBasicAuth(b.cfg.clientID, b.cfg.clientSecret)
	}
	res, err := b.http.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(res.Body, 1<<20))
	if res.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("spotify %d: %s", res.StatusCode, strings.TrimSpace(string(body)))
	}
	var t tokenResp
	if err := json.Unmarshal(body, &t); err != nil {
		return nil, err
	}
	return &t, nil
}

func (b *broker) reapPairs() {
	for range time.Tick(time.Minute) {
		cutoff := time.Now().Add(-pairTTL)
		b.mu.Lock()
		for k, p := range b.pairs {
			if p.created.Before(cutoff) {
				delete(b.pairs, k)
			}
		}
		b.mu.Unlock()
	}
}

// ---------------------------------------------------------------- album art

// handleArt fetches a Spotify cover, scales it to exactly what the panel draws,
// and re-encodes it as baseline JPEG.
//
// Every part of that matters on the device: Go decodes progressive JPEG happily
// and never writes it, which the firmware's decoder cannot do; scaling here turns
// a ~40 KB 640x640 into a few KB, so the download is quick and the decode is
// small; and a stable, predictable output means the firmware never has to guess.
func (b *broker) handleArt(w http.ResponseWriter, r *http.Request) {
	if !b.authed(r) {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	raw := r.URL.Query().Get("u")
	size, err := strconv.Atoi(r.URL.Query().Get("s"))
	if err != nil || size < artMinSize || size > artMaxSize {
		http.Error(w, "s must be "+strconv.Itoa(artMinSize)+".."+strconv.Itoa(artMaxSize), http.StatusBadRequest)
		return
	}
	u, err := url.Parse(raw)
	if err != nil || u.Scheme != "https" || !artHostAllow[u.Host] {
		http.Error(w, "u must be an https Spotify image URL", http.StatusForbidden)
		return
	}

	// .bin means a pre-decoded RGB565 bitmap in LVGL's own container: the device
	// then does no decoding at all, and LVGL streams rows off the card per draw
	// chunk rather than holding a decoded frame.
	rawOut := strings.HasSuffix(r.URL.Path, ".bin")
	ext := ".jpg"
	if rawOut {
		ext = ".bin"
	}

	key := sha256.Sum256([]byte(raw + "|" + strconv.Itoa(size) + "|" + ext))
	path := filepath.Join(b.cfg.cacheDir, hex.EncodeToString(key[:])+ext)

	if data, err := os.ReadFile(path); err == nil {
		b.mu.Lock()
		b.artHits++
		b.mu.Unlock()
		serveArt(w, data, rawOut)
		return
	}

	data, err := b.transcode(raw, size, rawOut)
	if err != nil {
		log.Printf("art: %v", err)
		http.Error(w, "upstream", http.StatusBadGateway)
		return
	}
	b.mu.Lock()
	b.artMiss++
	b.mu.Unlock()

	// Write via a temp file and rename, the same discipline the firmware uses for
	// downloads: a killed process must not leave a half-written cache entry that
	// later reads as a valid image.
	tmp := path + ".part"
	if err := os.WriteFile(tmp, data, 0o644); err == nil {
		_ = os.Rename(tmp, path)
	}
	serveArt(w, data, rawOut)
}

func (b *broker) transcode(src string, size int, rawOut bool) ([]byte, error) {
	res, err := b.http.Get(src)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("upstream %d", res.StatusCode)
	}

	img, _, err := image.Decode(io.LimitReader(res.Body, artMaxBytes))
	if err != nil {
		return nil, fmt.Errorf("decode: %w", err)
	}
	if rawOut {
		return encodeLVGLBin(img, size)
	}
	return encodeSquare(img, size)
}

// encodeSquare centre-crops to a square, scales to size, and writes baseline
// JPEG. Split out from transcode so it is testable without a network.
func encodeSquare(img image.Image, size int) ([]byte, error) {
	// Centre-crop to a square first, so scaling cannot distort a non-square
	// source (playlist mosaics are not always 1:1).
	bnds := img.Bounds()
	side := bnds.Dx()
	if bnds.Dy() < side {
		side = bnds.Dy()
	}
	crop := image.Rect(0, 0, side, side).
		Add(image.Pt(bnds.Min.X+(bnds.Dx()-side)/2, bnds.Min.Y+(bnds.Dy()-side)/2))

	dst := image.NewRGBA(image.Rect(0, 0, size, size))
	// CatmullRom rather than ApproxBiLinear: this runs once per track on a Pi,
	// where a few extra milliseconds are free, and album art downscaled 640->240
	// looks visibly softer with a cheaper kernel.
	draw.CatmullRom.Scale(dst, dst.Bounds(), img, crop, draw.Over, nil)

	var out strings.Builder
	// Quality 82 is where the file stops shrinking meaningfully and artefacts are
	// still invisible at this size. image/jpeg only ever writes baseline, which
	// is the whole point of doing this here.
	if err := jpeg.Encode(&stringWriter{&out}, dst, &jpeg.Options{Quality: 82}); err != nil {
		return nil, err
	}
	return []byte(out.String()), nil
}

type stringWriter struct{ b *strings.Builder }

func (s *stringWriter) Write(p []byte) (int, error) { return s.b.Write(p) }

// LVGL 9 binary image format. Verified against the vendored source:
// lv_image_dsc.h gives a 12-byte little-endian header, and lv_bin_decoder.c
// registers get_area for RGB565 — so LVGL reads only the rows it needs for each
// draw chunk directly off the SD card.
const (
	lvImageHeaderMagic  = 0x19 // LV_IMAGE_HEADER_MAGIC
	lvColorFormatRGB565 = 0x12 // LV_COLOR_FORMAT_RGB565
)

// encodeLVGLBin produces a pre-decoded RGB565 bitmap in LVGL's own container.
//
// This is the endpoint worth using. A JPEG saves bandwidth but costs the device a
// full decode — and worse, the firmware's baseline decoder never populates LVGL's
// image cache, so it re-decodes once per draw chunk, fifteen times a frame,
// forever. Handing over raw pixels moves that work to a machine that has CPU to
// spare and leaves the cube doing nothing but reading bytes.
//
// Costs 11x the bytes of the JPEG. That is the right trade here: Wi-Fi is
// abundant on this device and internal SRAM and CPU are not.
func encodeLVGLBin(img image.Image, size int) ([]byte, error) {
	scaled := image.NewRGBA(image.Rect(0, 0, size, size))
	bnds := img.Bounds()
	side := bnds.Dx()
	if bnds.Dy() < side {
		side = bnds.Dy()
	}
	crop := image.Rect(0, 0, side, side).
		Add(image.Pt(bnds.Min.X+(bnds.Dx()-side)/2, bnds.Min.Y+(bnds.Dy()-side)/2))
	draw.CatmullRom.Scale(scaled, scaled.Bounds(), img, crop, draw.Over, nil)

	stride := size * 2
	out := make([]byte, 12+stride*size)

	out[0] = lvImageHeaderMagic
	out[1] = lvColorFormatRGB565
	// flags [2:4] and reserved [10:12] stay zero
	putU16(out[4:], uint16(size))   // w
	putU16(out[6:], uint16(size))   // h
	putU16(out[8:], uint16(stride)) // bytes per row

	o := 12
	for y := 0; y < size; y++ {
		for x := 0; x < size; x++ {
			r, g, b, _ := scaled.At(x, y).RGBA() // 16-bit per channel
			// RGB565, little-endian, which is what LVGL expects on ESP32.
			v := uint16(r>>11)<<11 | uint16(g>>10)<<5 | uint16(b>>11)
			out[o] = byte(v)
			out[o+1] = byte(v >> 8)
			o += 2
		}
	}
	return out, nil
}

func putU16(b []byte, v uint16) {
	b[0] = byte(v)
	b[1] = byte(v >> 8)
}

func serveArt(w http.ResponseWriter, data []byte, rawOut bool) {
	if rawOut {
		w.Header().Set("Content-Type", "application/octet-stream")
	} else {
		w.Header().Set("Content-Type", "image/jpeg")
	}
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	// Art for a given track never changes, so let anything in between keep it.
	w.Header().Set("Cache-Control", "public, max-age=604800, immutable")
	_, _ = w.Write(data)
}

// ---------------------------------------------------------------- helpers

func validPairCode(s string) bool {
	if len(s) < 4 || len(s) > 16 {
		return false
	}
	for _, c := range s {
		if !(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') {
			return false
		}
	}
	return true
}

func randomURLSafe(n int) (string, error) {
	buf := make([]byte, n)
	if _, err := rand.Read(buf); err != nil {
		return "", err
	}
	return base64.RawURLEncoding.EncodeToString(buf), nil
}

func writeJSON(w http.ResponseWriter, code int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(code)
	_ = json.NewEncoder(w).Encode(v)
}

func page(w http.ResponseWriter, code int, title, body string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.WriteHeader(code)
	fmt.Fprintf(w, `<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Facet — %s</title>
<style>body{background:#05070b;color:#e8fbff;font:16px/1.6 system-ui,sans-serif;
display:grid;place-items:center;min-height:100vh;margin:0;text-align:center;padding:2rem}
h1{font-weight:600;letter-spacing:.02em;margin:0 0 .5rem}p{color:#94a3b8;margin:0}</style>
<div><h1>%s</h1><p>%s</p></div>`, title, title, body)
}
