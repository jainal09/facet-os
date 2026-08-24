package main

import (
	"crypto/rand"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"
)

const spotifyCredentialFile = "spotify-credentials.json"

var errSpotifyCredentialMissing = errors.New("Spotify credential missing")

type spotifyTokenError struct {
	Status int
	Code   string
	Body   string
}

func (e *spotifyTokenError) Error() string {
	// Spotify's raw error body is retained for programmatic diagnostics but must
	// never be interpolated into public-service logs: upstream responses can
	// contain request-correlated detail and are outside our trust boundary.
	if e.Code != "" {
		return fmt.Sprintf("spotify token endpoint returned HTTP %d (%s)", e.Status, e.Code)
	}
	return fmt.Sprintf("spotify token endpoint returned HTTP %d", e.Status)
}

type spotifyCredentialDisk struct {
	Version int               `json:"version"`
	Users   map[string]string `json:"users"`
}

// BROKER_USERS is deliberately small and boring to configure in an env file:
// alice=long-random-token,bob=another-long-random-token. The labels are local
// broker identities, not Spotify usernames, and never travel over the wire.
func parseBrokerUsers(raw, legacy string) (map[string]string, error) {
	users := map[string]string{}
	if legacy != "" {
		if !validBrokerBearer(legacy) {
			return nil, errors.New("legacy BROKER_TOKEN must be 32-256 visible ASCII characters")
		}
		users["default"] = legacy
	}
	for _, entry := range strings.Split(raw, ",") {
		entry = strings.TrimSpace(entry)
		if entry == "" {
			continue
		}
		parts := strings.SplitN(entry, "=", 2)
		if len(parts) != 2 {
			return nil, fmt.Errorf("%q must be user=bearer", entry)
		}
		user, token := strings.TrimSpace(parts[0]), strings.TrimSpace(parts[1])
		if !validUserName(user) || !validBrokerBearer(token) {
			return nil, fmt.Errorf("invalid user or bearer in %q", entry)
		}
		if _, exists := users[user]; exists {
			return nil, fmt.Errorf("duplicate user %q", user)
		}
		for other, existing := range users {
			if existing == token {
				return nil, fmt.Errorf("users %q and %q share a bearer", other, user)
			}
		}
		users[user] = token
	}
	return users, nil
}

func validBrokerBearer(s string) bool {
	if len(s) < 32 || len(s) > 256 {
		return false
	}
	for _, c := range []byte(s) {
		if c < 0x21 || c > 0x7E || c == ',' {
			return false
		}
	}
	return true
}

func validUserName(s string) bool {
	if len(s) < 1 || len(s) > 64 {
		return false
	}
	for _, c := range s {
		if !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') &&
			!(c >= '0' && c <= '9') && c != '-' && c != '_' && c != '.' {
			return false
		}
	}
	return true
}

func originOf(raw string) string {
	u, err := url.Parse(raw)
	if err != nil || u.Scheme == "" || u.Host == "" {
		return ""
	}
	return u.Scheme + "://" + u.Host
}

func canonicalPublicURL(raw string) (string, error) {
	u, err := url.Parse(raw)
	if err != nil || u.Scheme != "https" || u.Host == "" || u.User != nil ||
		u.RawQuery != "" || u.Fragment != "" {
		return "", errors.New("must be an https origin without credentials, query, or fragment")
	}
	if u.Path != "" && u.Path != "/" {
		return "", errors.New("must be an origin without a path")
	}
	return "https://" + u.Host, nil
}

func (b *broker) spotifyCredentialPath() string {
	return filepath.Join(b.cfg.cacheDir, spotifyCredentialFile)
}

func (b *broker) ensureSpotifyMapsLocked() {
	if b.pairs == nil {
		b.pairs = map[string]*pending{}
	}
	if b.credentials == nil {
		b.credentials = map[string]string{}
	}
	if b.access == nil {
		b.access = map[string]cachedAccess{}
	}
	if b.refreshMu == nil {
		b.refreshMu = map[string]*sync.Mutex{}
	}
}

func (b *broker) spotifyRefreshLock(user string) *sync.Mutex {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.ensureSpotifyMapsLocked()
	lock := b.refreshMu[user]
	if lock == nil {
		lock = &sync.Mutex{}
		b.refreshMu[user] = lock
	}
	return lock
}

func (b *broker) loadSpotifyCredentials() error {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.ensureSpotifyMapsLocked()
	data, err := os.ReadFile(b.spotifyCredentialPath())
	if errors.Is(err, os.ErrNotExist) {
		return nil
	}
	if err != nil {
		return err
	}
	var disk spotifyCredentialDisk
	if err := json.Unmarshal(data, &disk); err != nil {
		return err
	}
	if disk.Version != 1 || disk.Users == nil {
		return fmt.Errorf("unsupported credential store version %d", disk.Version)
	}
	b.credentials = disk.Users
	return nil
}

// storeSpotifyCredential holds the mutex through the tiny atomic write. Pairing
// is rare, and serialising it guarantees two callbacks cannot each persist a
// snapshot that drops the other user's new token.
func (b *broker) storeSpotifyCredential(user, refresh string) error {
	if refresh == "" {
		return errors.New("empty refresh token")
	}
	b.mu.Lock()
	defer b.mu.Unlock()
	b.ensureSpotifyMapsLocked()
	old, hadOld := b.credentials[user]
	b.credentials[user] = refresh
	if err := b.writeSpotifyCredentialsLocked(); err != nil {
		if hadOld {
			b.credentials[user] = old
		} else {
			delete(b.credentials, user)
		}
		return err
	}
	return nil
}

func (b *broker) clearSpotifyCredential(user string) error {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.ensureSpotifyMapsLocked()
	old, ok := b.credentials[user]
	delete(b.credentials, user)
	delete(b.access, user)
	if err := b.writeSpotifyCredentialsLocked(); err != nil {
		if ok {
			b.credentials[user] = old
		}
		return err
	}
	return nil
}

func (b *broker) writeSpotifyCredentialsLocked() error {
	data, err := json.Marshal(spotifyCredentialDisk{Version: 1, Users: b.credentials})
	if err != nil {
		return err
	}
	path := b.spotifyCredentialPath()
	tmp, err := os.CreateTemp(filepath.Dir(path), ".spotify-credentials-*.part")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if _, err = tmp.Write(data); err == nil {
		err = tmp.Chmod(0o600)
	}
	if closeErr := tmp.Close(); err == nil {
		err = closeErr
	}
	if err == nil {
		err = os.Rename(name, path)
	}
	return err
}

// handleSpotifyToken returns only a short-lived access token. The refresh token
// remains in the broker's per-user store, so one firmware image no longer bakes
// one Spotify account into every cube.
func (b *broker) handleSpotifyToken(w http.ResponseWriter, r *http.Request) {
	user, ok := b.authenticate(r)
	if !ok {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	force := r.URL.Query().Get("force") == "1"
	tok, expires, err := b.spotifyAccessToken(user, force)
	if err == nil {
		w.Header().Set("Cache-Control", "no-store")
		writeJSON(w, http.StatusOK, map[string]any{
			"access_token": tok,
			"expires_in":   max(1, int(time.Until(expires).Seconds())),
		})
		return
	}

	var tokenErr *spotifyTokenError
	if errors.As(err, &tokenErr) && tokenErr.Code == "invalid_grant" {
		if clearErr := b.clearSpotifyCredential(user); clearErr != nil {
			logCredentialStoreError(user, clearErr)
			http.Error(w, "credential store", http.StatusInternalServerError)
			return
		}
		err = errSpotifyCredentialMissing
	}
	if errors.Is(err, errSpotifyCredentialMissing) {
		pairURL, pairErr := b.ensurePairURL(user)
		if pairErr != nil {
			http.Error(w, "pairing unavailable", http.StatusServiceUnavailable)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		writeJSON(w, http.StatusPreconditionRequired, map[string]string{
			"status":            "authorization_required",
			"authorization_url": pairURL,
		})
		return
	}

	// A timeout or Spotify outage is not evidence that a user's refresh token is
	// bad. Keep it and let the cube retry instead of forcing needless login/2FA.
	http.Error(w, "spotify unavailable", http.StatusBadGateway)
}

func logCredentialStoreError(user string, err error) {
	// User labels are configuration, never request input; refresh tokens are not
	// included in either this error or any caller's log line.
	fmt.Fprintf(os.Stderr, "Spotify credential store for %q: %v\n", user, err)
}

func (b *broker) spotifyAccessToken(user string, force bool) (string, time.Time, error) {
	// Spotify may rotate refresh tokens. Serialising per user prevents two
	// simultaneous requests from both spending the old token and a late
	// invalid_grant response from clearing the credential stored by the winner.
	refreshLock := b.spotifyRefreshLock(user)
	refreshLock.Lock()
	defer refreshLock.Unlock()

	now := time.Now()
	b.mu.Lock()
	b.ensureSpotifyMapsLocked()
	if cached := b.access[user]; !force && cached.token != "" && cached.expires.After(now.Add(2*time.Minute)) {
		b.mu.Unlock()
		return cached.token, cached.expires, nil
	}
	refresh := b.credentials[user]
	b.mu.Unlock()
	if refresh == "" {
		return "", time.Time{}, errSpotifyCredentialMissing
	}

	form := url.Values{
		"grant_type":    {"refresh_token"},
		"refresh_token": {refresh},
		"client_id":     {b.cfg.clientID},
	}
	tok, err := b.tokenRequest(form)
	if err != nil {
		return "", time.Time{}, err
	}
	if tok.AccessToken == "" {
		return "", time.Time{}, errors.New("Spotify returned no access token")
	}
	if tok.ExpiresIn <= 0 {
		tok.ExpiresIn = 3600
	}
	expires := now.Add(time.Duration(tok.ExpiresIn) * time.Second)
	if tok.RefreshToken != "" && tok.RefreshToken != refresh {
		if err := b.storeSpotifyCredential(user, tok.RefreshToken); err != nil {
			return "", time.Time{}, err
		}
	}
	b.mu.Lock()
	b.ensureSpotifyMapsLocked()
	b.access[user] = cachedAccess{token: tok.AccessToken, expires: expires}
	b.mu.Unlock()
	return tok.AccessToken, expires, nil
}

func (b *broker) ensurePairURL(user string) (string, error) {
	if b.cfg.clientID == "" || b.cfg.redirectURI == "" || b.cfg.publicURL == "" {
		return "", errors.New("broker OAuth configuration incomplete")
	}
	now := time.Now()
	b.mu.Lock()
	b.ensureSpotifyMapsLocked()
	for code, p := range b.pairs {
		if p.user == user && p.created.After(now.Add(-pairTTL)) {
			b.mu.Unlock()
			return strings.TrimRight(b.cfg.publicURL, "/") + "/pair?c=" + url.QueryEscape(code), nil
		}
	}
	b.mu.Unlock()

	for attempts := 0; attempts < 8; attempts++ {
		raw := make([]byte, 16)
		if _, err := rand.Read(raw); err != nil {
			return "", err
		}
		code := strings.ToUpper(hex.EncodeToString(raw))
		b.mu.Lock()
		b.ensureSpotifyMapsLocked()
		if _, exists := b.pairs[code]; !exists {
			b.pairs[code] = &pending{user: user, created: now}
			b.mu.Unlock()
			return strings.TrimRight(b.cfg.publicURL, "/") + "/pair?c=" + url.QueryEscape(code), nil
		}
		b.mu.Unlock()
	}
	return "", errors.New("could not allocate pair code")
}
