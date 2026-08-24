// Facet broker — the small amount of server the cube cannot be.
//
// It exists only where a server genuinely buys something, and deliberately
// stays out of the interactive path for everything else:
//
//  1. Pairing and token custody. Spotify's OAuth needs an HTTPS redirect target.
//     Each cube authenticates as one broker user; the broker persists that user's
//     refresh token and leases short-lived access tokens back to firmware. The
//     cube still talks straight to api.spotify.com for interactive control.
//
//  2. Album art. The firmware can only decode *baseline* JPEG, and its baseline
//     decoder never populates LVGL's image cache, so a stock Spotify image is
//     either undecodable or re-decoded fifteen times per frame. Transcoding here
//     removes the whole risk class and drops a 640x640 cover to a few KB at
//     exactly the size the panel draws.
//
//  3. Small phone UIs and shared state. Wi-Fi provisioning and DAYS use the
//     browser for interactions that are miserable on a round 480 px panel, then
//     hand the cube compact authenticated payloads.
//
// Deployed behind Tailscale Funnel, which means it is on the public internet.
// Everything below is written with that in mind: no open image proxy, no
// unauthenticated token store, user isolation, single-use pair codes with a
// short TTL.
package main

import (
	"bytes"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"html/template"
	"image"
	"image/jpeg"
	"io"
	"log"
	"math"
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
	// The library pair is for the like button: read to show whether the current
	// track is already saved, modify to toggle it.
	spotifyScopes = "user-read-playback-state user-modify-playback-state " +
		"user-library-read user-library-modify"

	pairTTL         = 5 * time.Minute
	artMaxSize      = 640
	artMinSize      = 64
	artMaxBytes     = 8 << 20 // refuse absurd upstream images
	artMaxPixels    = 16 << 20
	artMaxDimension = 4096
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
	publicURL    string
	deviceToken  string            // legacy single-user bearer
	users        map[string]string // user name -> unique cube bearer
	cacheDir     string
}

// pending is one in-flight pairing. Held in memory on purpose: it lives for at
// most pairTTL, and losing them all on restart is the correct behaviour.
type pending struct {
	user     string
	verifier string
	created  time.Time
}

type cachedAccess struct {
	token   string
	expires time.Time
}

type broker struct {
	cfg config

	mu           sync.Mutex
	pairs        map[string]*pending
	daysLinks    map[string]daysLink
	daysSessions map[[sha256.Size]byte]daysSession
	petLinks     map[string]petLink
	petSessions  map[[sha256.Size]byte]petSession
	petWx        map[string]petWeather
	credentials  map[string]string
	access       map[string]cachedAccess
	refreshMu    map[string]*sync.Mutex
	artSem       chan struct{}
	artHits      int
	artMiss      int

	http *http.Client
}

func main() {
	var cfg config
	var rawUsers string
	flag.StringVar(&cfg.addr, "addr", envOr("BROKER_ADDR", ":8080"), "listen address")
	flag.StringVar(&cfg.clientID, "client-id", os.Getenv("SPOTIFY_CLIENT_ID"), "Spotify client ID")
	flag.StringVar(&cfg.clientSecret, "client-secret", os.Getenv("SPOTIFY_CLIENT_SECRET"), "Spotify client secret (optional, PKCE works without)")
	flag.StringVar(&cfg.redirectURI, "redirect-uri", os.Getenv("SPOTIFY_REDIRECT_URI"), "must match the app registration exactly")
	flag.StringVar(&cfg.publicURL, "public-url", os.Getenv("BROKER_PUBLIC_URL"), "public broker origin encoded in pairing QR codes")
	flag.StringVar(&cfg.deviceToken, "device-token", os.Getenv("BROKER_TOKEN"), "legacy single-user cube bearer")
	flag.StringVar(&rawUsers, "users", os.Getenv("BROKER_USERS"), "comma-separated user=bearer entries")
	flag.StringVar(&cfg.cacheDir, "cache-dir", envOr("BROKER_CACHE", "./cache"), "transcoded art cache")
	flag.Parse()

	var err error
	cfg.users, err = parseBrokerUsers(rawUsers, cfg.deviceToken)
	if err != nil {
		log.Fatalf("BROKER_USERS: %v", err)
	}
	if len(cfg.users) == 0 {
		log.Fatal("BROKER_USERS or legacy BROKER_TOKEN is required: authenticated endpoints are public once Funnel is on")
	}
	if cfg.clientID == "" || cfg.redirectURI == "" {
		log.Println("warning: SPOTIFY_CLIENT_ID or SPOTIFY_REDIRECT_URI unset — /pair will refuse, /art still works")
	}
	if cfg.publicURL == "" {
		cfg.publicURL = originOf(cfg.redirectURI)
	}
	if cfg.publicURL != "" {
		cfg.publicURL, err = canonicalPublicURL(cfg.publicURL)
		if err != nil {
			log.Fatalf("BROKER_PUBLIC_URL: %v", err)
		}
	}
	if err := os.MkdirAll(cfg.cacheDir, 0o755); err != nil {
		log.Fatalf("cache dir: %v", err)
	}

	b := &broker{
		cfg:          cfg,
		pairs:        map[string]*pending{},
		daysLinks:    map[string]daysLink{},
		daysSessions: map[[sha256.Size]byte]daysSession{},
		petLinks:     map[string]petLink{},
		petSessions:  map[[sha256.Size]byte]petSession{},
		petWx:        map[string]petWeather{},
		credentials:  map[string]string{},
		access:       map[string]cachedAccess{},
		refreshMu:    map[string]*sync.Mutex{},
		artSem:       make(chan struct{}, 2),
		http:         &http.Client{Timeout: 20 * time.Second},
	}
	if err := b.loadSpotifyCredentials(); err != nil {
		log.Fatalf("Spotify credential store: %v", err)
	}
	go b.reapPairs()

	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", b.handleHealth)
	mux.HandleFunc("/pair", b.handlePair)
	mux.HandleFunc("/callback", b.handleCallback)
	mux.HandleFunc("/spotify/token", b.handleSpotifyToken)
	mux.HandleFunc("/art", b.handleArt)
	mux.HandleFunc("/art.bin", b.handleArt)
	// Queue lookahead for prefetching — see queue.go for why it is server-side.
	mux.HandleFunc("/queue", b.handleQueue)
	// Wi-Fi setup UI. Unauthenticated by design — see provision.go.
	mux.HandleFunc("/provision", b.handleProvision)
	mux.HandleFunc("/countdown", b.handleCountdown)
	mux.HandleFunc("/days", b.handleDaysPage)
	mux.HandleFunc("/days/link", b.handleDaysLink)
	mux.HandleFunc("/days/session", b.handleDaysSession)
	// The PET designer and its cube endpoints — see pet.go.
	mux.HandleFunc("/pet", b.handlePetPage)
	mux.HandleFunc("/pet/link", b.handlePetLink)
	mux.HandleFunc("/pet/session", b.handlePetSession)
	mux.HandleFunc("/pet/data", b.handlePetData)
	mux.HandleFunc("/pet/design", b.handlePetDesign)
	mux.HandleFunc("/pet/cfg", b.handlePetCfg)
	mux.HandleFunc("/pet/st", b.handlePetState)
	mux.HandleFunc("/pet/sheet", b.handlePetSheet)

	srv := &http.Server{
		Addr:              cfg.addr,
		Handler:           logging(securityHeaders(b.rateLimit(mux))),
		ReadHeaderTimeout: 10 * time.Second,
		ReadTimeout:       15 * time.Second,
		WriteTimeout:      45 * time.Second,
		IdleTimeout:       75 * time.Second,
		MaxHeaderBytes:    16 << 10,
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

// authenticate both guards a cube endpoint and identifies whose persisted state
// it may touch. Each cube gets a unique bearer, so no user-controlled ID is
// accepted from a header or query string. Constant-time comparison matters here
// because Funnel makes this reachable from the public internet.
func (b *broker) authenticate(r *http.Request) (string, bool) {
	header := r.Header.Get("Authorization")
	if !strings.HasPrefix(header, "Bearer ") {
		return "", false
	}
	got := strings.TrimPrefix(header, "Bearer ")
	matched := ""
	for user, want := range b.cfg.users {
		if subtle.ConstantTimeCompare([]byte(got), []byte(want)) == 1 {
			matched = user
		}
	}
	if matched != "" {
		return matched, true
	}
	// Tests and older in-process callers construct config directly. Keep the
	// legacy field functional without making it a second identity when main has
	// already folded it into cfg.users.
	if len(b.cfg.users) == 0 && b.cfg.deviceToken != "" &&
		subtle.ConstantTimeCompare([]byte(got), []byte(b.cfg.deviceToken)) == 1 {
		return "default", true
	}
	return "", false
}

func (b *broker) authed(r *http.Request) bool {
	_, ok := b.authenticate(r)
	return ok
}

func (b *broker) handleHealth(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	b.mu.Lock()
	pairs, hits, miss := len(b.pairs), b.artHits, b.artMiss
	b.mu.Unlock()
	writeJSON(w, http.StatusOK, map[string]any{
		"ok": true, "pending_pairs": pairs,
		"art_cache_hits": hits, "art_cache_misses": miss,
	})
}

// ---------------------------------------------------------------- pairing

// handlePair is what the QR code on the cube points at. /spotify/token creates
// the random, user-bound code first; accepting caller-chosen codes here would
// let one cube overwrite another cube's in-flight session.
func (b *broker) handlePair(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if b.cfg.clientID == "" || b.cfg.redirectURI == "" {
		http.Error(w, "broker not configured for pairing", http.StatusServiceUnavailable)
		return
	}
	code := r.URL.Query().Get("c")
	if !validPairCode(code) {
		http.Error(w, "bad pair code", http.StatusBadRequest)
		return
	}
	b.mu.Lock()
	p := b.pairs[code]
	if p != nil && time.Since(p.created) > pairTTL {
		delete(b.pairs, code)
		p = nil
	}
	verifier := ""
	if p != nil {
		verifier = p.verifier
	}
	b.mu.Unlock()
	if p == nil {
		page(w, http.StatusBadRequest, "Expired", "That pairing link has expired. Start again from the cube.")
		return
	}

	if verifier == "" {
		var err error
		verifier, err = randomURLSafe(64)
		if err != nil {
			http.Error(w, "entropy", http.StatusInternalServerError)
			return
		}
		b.mu.Lock()
		// A reaper or simultaneous second scan may have won while entropy was
		// being read. Reuse the winner's verifier so two browser tabs cannot
		// invalidate each other's callback.
		if current := b.pairs[code]; current == p {
			if p.verifier == "" {
				p.verifier = verifier
			} else {
				verifier = p.verifier
			}
		} else {
			p = nil
		}
		b.mu.Unlock()
	}
	if p == nil {
		page(w, http.StatusBadRequest, "Expired", "That pairing link has expired. Start again from the cube.")
		return
	}
	sum := sha256.Sum256([]byte(verifier))
	challenge := base64.RawURLEncoding.EncodeToString(sum[:])

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
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	q := r.URL.Query()
	if e := q.Get("error"); e != "" {
		b.mu.Lock()
		delete(b.pairs, q.Get("state"))
		b.mu.Unlock()
		page(w, http.StatusOK, "Denied", "Spotify returned: "+e)
		return
	}
	code, state := q.Get("code"), q.Get("state")
	if code == "" {
		page(w, http.StatusBadRequest, "Failed", "Spotify did not return an authorization code.")
		return
	}

	b.mu.Lock()
	p := b.pairs[state]
	user, verifier := "", ""
	if p != nil {
		user, verifier = p.user, p.verifier
		// Consume the state before any network call. A callback is a single-use
		// capability even when Spotify later rejects the supplied code.
		delete(b.pairs, state)
	}
	b.mu.Unlock()
	if p == nil || verifier == "" {
		page(w, http.StatusBadRequest, "Expired", "That pairing link has expired. Start again from the cube.")
		return
	}

	form := url.Values{
		"grant_type":    {"authorization_code"},
		"code":          {code},
		"redirect_uri":  {b.cfg.redirectURI},
		"client_id":     {b.cfg.clientID},
		"code_verifier": {verifier},
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

	if err := b.storeSpotifyCredential(user, tok.RefreshToken); err != nil {
		log.Printf("store Spotify credential for %q: %v", user, err)
		page(w, http.StatusInternalServerError, "Failed", "Could not save this account. Try again.")
		return
	}
	b.mu.Lock()
	if tok.AccessToken != "" {
		b.access[user] = cachedAccess{token: tok.AccessToken,
			expires: time.Now().Add(time.Duration(tok.ExpiresIn) * time.Second)}
	}
	b.mu.Unlock()

	page(w, http.StatusOK, "Paired", "You can put your phone down — the cube is picking this up now.")
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
		var detail struct {
			Error       string `json:"error"`
			Description string `json:"error_description"`
		}
		_ = json.Unmarshal(body, &detail)
		return nil, &spotifyTokenError{
			Status: res.StatusCode,
			Code:   detail.Error,
			Body:   strings.TrimSpace(string(body)),
		}
	}
	var t tokenResp
	if err := json.Unmarshal(body, &t); err != nil {
		return nil, err
	}
	return &t, nil
}

func (b *broker) reapPairs() {
	ticker := time.NewTicker(time.Minute)
	defer ticker.Stop()
	for now := range ticker.C {
		pairCutoff := now.Add(-pairTTL)
		b.mu.Lock()
		for k, p := range b.pairs {
			if p.created.Before(pairCutoff) {
				delete(b.pairs, k)
			}
		}
		b.reapDaysLocked(now)
		b.reapPetLocked(now)
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
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
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
	if err != nil || !validArtURL(u) {
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

	// Decoding attacker-triggered compressed images is the most CPU- and
	// memory-intensive operation in the broker. Bound it independently of HTTP
	// request concurrency so a valid but leaked cube bearer cannot exhaust the
	// small container with parallel cache misses.
	b.mu.Lock()
	if b.artSem == nil {
		b.artSem = make(chan struct{}, 2)
	}
	artSem := b.artSem
	b.mu.Unlock()
	select {
	case artSem <- struct{}{}:
		defer func() { <-artSem }()
	default:
		w.Header().Set("Retry-After", "1")
		http.Error(w, "art workers busy", http.StatusTooManyRequests)
		return
	}

	data, err := b.transcode(r.Context(), raw, size, rawOut)
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
	if tmp, err := os.CreateTemp(b.cfg.cacheDir, ".art-*.part"); err == nil {
		name := tmp.Name()
		if _, err = tmp.Write(data); err == nil {
			err = tmp.Chmod(0o644)
		}
		if closeErr := tmp.Close(); err == nil {
			err = closeErr
		}
		if err == nil {
			err = os.Rename(name, path)
		}
		if err != nil {
			_ = os.Remove(name)
		}
	}
	serveArt(w, data, rawOut)
}

func validArtURL(u *url.URL) bool {
	if u == nil || u.Scheme != "https" || u.User != nil || !artHostAllow[u.Hostname()] {
		return false
	}
	return u.Port() == "" || u.Port() == "443"
}

func (b *broker) transcode(ctx context.Context, src string, size int, rawOut bool) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, src, nil)
	if err != nil {
		return nil, err
	}
	client := *b.http
	previousRedirect := client.CheckRedirect
	client.CheckRedirect = func(req *http.Request, via []*http.Request) error {
		if len(via) >= 5 {
			return errors.New("too many art redirects")
		}
		if !validArtURL(req.URL) {
			return errors.New("art redirect left the Spotify image allowlist")
		}
		if previousRedirect != nil {
			return previousRedirect(req, via)
		}
		return nil
	}
	res, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("upstream %d", res.StatusCode)
	}

	encoded, err := io.ReadAll(io.LimitReader(res.Body, artMaxBytes+1))
	if err != nil {
		return nil, err
	}
	if len(encoded) > artMaxBytes {
		return nil, errors.New("upstream art exceeds encoded size limit")
	}
	cfg, _, err := image.DecodeConfig(bytes.NewReader(encoded))
	if err != nil {
		return nil, fmt.Errorf("decode config: %w", err)
	}
	if cfg.Width <= 0 || cfg.Height <= 0 || cfg.Width > artMaxDimension ||
		cfg.Height > artMaxDimension || int64(cfg.Width)*int64(cfg.Height) > artMaxPixels {
		return nil, fmt.Errorf("upstream art dimensions %dx%d exceed limit", cfg.Width, cfg.Height)
	}
	img, _, err := image.Decode(bytes.NewReader(encoded))
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
	// CatmullRom rather than ApproxBiLinear: this runs once per track on the server,
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
	lvColorFormatRGB565 = 0x12
	lvglHeaderLen       = 12 // LV_COLOR_FORMAT_RGB565
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
	out := make([]byte, lvglHeaderLen+stride*size)

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

// dominantColor picks a UI-usable accent from an encoded RGB565 buffer.
//
// Derived from the served bytes rather than stored beside them, so a cache hit and
// a cache miss can never disagree and no existing cache entry is invalidated.
//
// Averaging the image is not usable: album art averages to mud, and a near-black
// or near-white tint makes a glyph drawn on top unreadable. So this weights every
// sample by how colourful it is, discards the greys and the near-blacks entirely,
// and then forces the result into a saturation and lightness band the UI can
// actually use. Conditioning happens here rather than on the device because there
// is float math available and one place to tune it.
func dominantColor(bin []byte) (string, bool) {
	if len(bin) < lvglHeaderLen+2 {
		return "", false
	}
	px := bin[lvglHeaderLen:]

	var sr, sg, sb, wsum float64
	// Step 4 pixels: a 148x148 cover still yields ~5,400 samples, which is far
	// more than enough to find a dominant hue and keeps this well under a
	// millisecond.
	for i := 0; i+1 < len(px); i += 8 {
		v := binary.LittleEndian.Uint16(px[i : i+2])
		// RGB565 -> 8 bit, replicating high bits into the low ones so full-scale
		// channels reach 255 rather than 248/252.
		r := float64((v>>11&0x1F)<<3 | (v >> 13 & 0x07))
		g := float64((v>>5&0x3F)<<2 | (v >> 9 & 0x03))
		b := float64((v&0x1F)<<3 | (v >> 2 & 0x07))

		mx := math.Max(r, math.Max(g, b))
		mn := math.Min(r, math.Min(g, b))
		if mx < 40 {
			continue // near-black: contributes no usable hue
		}
		sat := (mx - mn) / mx
		if sat < 0.15 {
			continue // grey, white, sepia wash
		}
		wt := sat * mx
		sr += r * wt
		sg += g * wt
		sb += b * wt
		wsum += wt
	}
	return conditionAccent(sr, sg, sb, wsum)
}

// dominantColorJPEG is the same judgement over an encoded cover. The device now
// receives baseline JPEG rather than raw RGB565 — a 296px cover is ~22 KB instead
// of 175,244 — so the accent has to be derivable from that. Decoding here costs a
// few milliseconds on a machine that has them, and keeps the cache-hit and
// cache-miss paths identical, which is the property that made the raw version
// trustworthy in the first place.
func dominantColorJPEG(data []byte) (string, bool) {
	img, err := jpeg.Decode(bytes.NewReader(data))
	if err != nil {
		return "", false
	}
	b := img.Bounds()
	var sr, sg, sb, wsum float64
	// Same stride in pixels as the RGB565 sampler: every 4th.
	for y := b.Min.Y; y < b.Max.Y; y += 2 {
		for x := b.Min.X; x < b.Max.X; x += 2 {
			r32, g32, b32, _ := img.At(x, y).RGBA()
			r, g, bb := float64(r32>>8), float64(g32>>8), float64(b32>>8)
			mx := math.Max(r, math.Max(g, bb))
			mn := math.Min(r, math.Min(g, bb))
			if mx < 40 {
				continue
			}
			sat := (mx - mn) / mx
			if sat < 0.15 {
				continue
			}
			wt := sat * mx
			sr += r * wt
			sg += g * wt
			sb += bb * wt
			wsum += wt
		}
	}
	return conditionAccent(sr, sg, sb, wsum)
}

// conditionAccent turns weighted colour sums into something the UI can actually
// use: never grey, never so dark or so bright that a glyph on top disappears.
func conditionAccent(sr, sg, sb, wsum float64) (string, bool) {
	if wsum == 0 {
		return "", false // monochrome art; caller keeps its own default
	}

	r, g, b := sr/wsum, sg/wsum, sb/wsum

	// Force into a usable band. Below ~0.55 saturation the tint reads as grey
	// against a dark UI; above ~0.95 value it starts to fight white text.
	mx := math.Max(r, math.Max(g, b))
	mn := math.Min(r, math.Min(g, b))
	if mx == 0 {
		return "", false
	}
	sat, val := (mx-mn)/mx, mx/255
	if sat < 0.55 {
		// pull each channel away from the grey axis until saturation reaches the
		// floor, which preserves hue exactly
		for _, c := range []*float64{&r, &g, &b} {
			*c = mn + (*c-mn)*(mx-mn*(1-0.55))/math.Max(mx-mn, 1)
		}
	}
	if val < 0.55 {
		k := 0.55 / val
		r, g, b = r*k, g*k, b*k
	} else if val > 0.95 {
		k := 0.95 / val
		r, g, b = r*k, g*k, b*k
	}

	cl := func(f float64) uint8 {
		if f < 0 {
			return 0
		}
		if f > 255 {
			return 255
		}
		return uint8(f + 0.5)
	}
	return fmt.Sprintf("%02X%02X%02X", cl(r), cl(g), cl(b)), true
}

func serveArt(w http.ResponseWriter, data []byte, rawOut bool) {
	if rawOut {
		w.Header().Set("Content-Type", "application/octet-stream")
		// Ride the fetch the device already makes: a header costs no extra bytes
		// and no extra round trip, where a second endpoint would cost a ~390 ms
		// cold TLS handshake on the device.
		if accent, ok := dominantColor(data); ok {
			w.Header().Set("X-Art-Accent", accent)
		}
	} else {
		w.Header().Set("Content-Type", "image/jpeg")
		if accent, ok := dominantColorJPEG(data); ok {
			w.Header().Set("X-Art-Accent", accent)
		}
	}
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	// Art for a given track never changes, so let anything in between keep it.
	w.Header().Set("Cache-Control", "public, max-age=604800, immutable")
	_, _ = w.Write(data)
}

// ---------------------------------------------------------------- helpers

func validPairCode(s string) bool {
	if len(s) != 32 {
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

const pageStyle = `body{background:#05070b;color:#e8fbff;font:16px/1.6 system-ui,sans-serif;
display:grid;place-items:center;min-height:100vh;margin:0;text-align:center;padding:2rem}
h1{font-weight:600;letter-spacing:.02em;margin:0 0 .5rem}p{color:#94a3b8;margin:0}`

const messagePageMarkup = `<!doctype html><meta name=viewport content="width=device-width,initial-scale=1">
<title>Facet — {{.Title}}</title>
<style>` + pageStyle + `</style>
<div><h1>{{.Title}}</h1><p>{{.Body}}</p></div>`

var messagePage = template.Must(template.New("message").Parse(messagePageMarkup))

func page(w http.ResponseWriter, code int, title, body string) {
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	setHTMLCSP(w, []byte(messagePageMarkup), false)
	w.WriteHeader(code)
	_ = messagePage.Execute(w, struct {
		Title string
		Body  string
	}{Title: title, Body: body})
}
