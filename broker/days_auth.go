package main

import (
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"io"
	"mime"
	"net/http"
	"net/url"
	"strings"
	"time"
)

const (
	daysLinkTTL       = 5 * time.Minute
	daysSessionTTL    = 30 * time.Minute
	daysLinkCodeBytes = 18 // 144 bits; 24 base64url characters in the QR URL
	daysTokenBytes    = 32 // 256 bits; returned once and stored only as SHA-256
	maxDaysSessions   = 4096
)

type daysLink struct {
	user    string
	created time.Time
}

type daysSession struct {
	user    string
	expires time.Time
}

func (b *broker) ensureDaysMapsLocked() {
	if b.daysLinks == nil {
		b.daysLinks = map[string]daysLink{}
	}
	if b.daysSessions == nil {
		b.daysSessions = map[[sha256.Size]byte]daysSession{}
	}
}

func (b *broker) reapDaysLocked(now time.Time) {
	b.ensureDaysMapsLocked()
	for code, link := range b.daysLinks {
		if !now.Before(link.created.Add(daysLinkTTL)) {
			delete(b.daysLinks, code)
		}
	}
	for hash, session := range b.daysSessions {
		if !now.Before(session.expires) {
			delete(b.daysSessions, hash)
		}
	}
}

func validDaysLinkCode(code string) bool {
	if len(code) != base64.RawURLEncoding.EncodedLen(daysLinkCodeBytes) {
		return false
	}
	raw, err := base64.RawURLEncoding.DecodeString(code)
	return err == nil && len(raw) == daysLinkCodeBytes
}

func bearerToken(r *http.Request) (string, bool) {
	header := r.Header.Get("Authorization")
	if !strings.HasPrefix(header, "Bearer ") {
		return "", false
	}
	token := strings.TrimPrefix(header, "Bearer ")
	if token == "" || strings.ContainsAny(token, " \t\r\n") {
		return "", false
	}
	return token, true
}

func validDaysSessionToken(token string) bool {
	if len(token) != base64.RawURLEncoding.EncodedLen(daysTokenBytes) {
		return false
	}
	raw, err := base64.RawURLEncoding.DecodeString(token)
	return err == nil && len(raw) == daysTokenBytes
}

// authenticateDaysSession accepts only a temporary browser bearer. It is kept
// separate from authenticate so this capability cannot accidentally gain
// access to Spotify tokens, artwork, queues, or future cube endpoints.
func (b *broker) authenticateDaysSession(r *http.Request) (string, bool) {
	token, ok := bearerToken(r)
	if !ok || !validDaysSessionToken(token) {
		return "", false
	}
	hash := sha256.Sum256([]byte(token))
	now := time.Now()
	b.mu.Lock()
	b.ensureDaysMapsLocked()
	session, found := b.daysSessions[hash]
	if found && !now.Before(session.expires) {
		delete(b.daysSessions, hash)
		found = false
	}
	b.mu.Unlock()
	return session.user, found
}

func (b *broker) authenticateCountdown(r *http.Request) (string, bool) {
	if user, ok := b.authenticate(r); ok {
		return user, true
	}
	return b.authenticateDaysSession(r)
}

// handleDaysLink lets authenticated firmware mint a user-bound QR link. A
// pending link is stable until it is consumed or expires, preventing every
// network retry from changing the QR underneath a phone camera.
func (b *broker) handleDaysLink(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	user, ok := b.authenticate(r)
	if !ok {
		http.Error(w, "unauthorized", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if b.cfg.publicURL == "" {
		http.Error(w, "public broker URL unavailable", http.StatusServiceUnavailable)
		return
	}

	candidate, err := randomURLSafe(daysLinkCodeBytes)
	if err != nil {
		http.Error(w, "entropy", http.StatusInternalServerError)
		return
	}
	now := time.Now()
	code := ""
	b.mu.Lock()
	b.reapDaysLocked(now)
	for existing, link := range b.daysLinks {
		if link.user == user {
			code = existing
			break
		}
	}
	if code == "" {
		if _, collision := b.daysLinks[candidate]; collision {
			b.mu.Unlock()
			http.Error(w, "entropy collision", http.StatusInternalServerError)
			return
		}
		code = candidate
		b.daysLinks[code] = daysLink{user: user, created: now}
	}
	b.mu.Unlock()

	authorizationURL := strings.TrimRight(b.cfg.publicURL, "/") +
		"/days?c=" + url.QueryEscape(code)
	writeJSON(w, http.StatusOK, map[string]any{
		"authorization_url": authorizationURL,
		"expires_in":        int(daysLinkTTL.Seconds()),
	})
}

// handleDaysSession atomically consumes a QR code and returns a temporary,
// countdown-only browser bearer. The raw bearer is never retained server-side.
func (b *broker) handleDaysSession(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	mediaType, _, err := mime.ParseMediaType(r.Header.Get("Content-Type"))
	if err != nil || mediaType != "application/json" {
		http.Error(w, "content type must be application/json", http.StatusUnsupportedMediaType)
		return
	}
	var in struct {
		Code string `json:"code"`
	}
	decoder := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&in); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}
	if !validDaysLinkCode(in.Code) {
		http.Error(w, "invalid or expired link", http.StatusBadRequest)
		return
	}

	token, err := randomURLSafe(daysTokenBytes)
	if err != nil {
		http.Error(w, "entropy", http.StatusInternalServerError)
		return
	}
	hash := sha256.Sum256([]byte(token))
	now := time.Now()
	b.mu.Lock()
	b.reapDaysLocked(now)
	link, found := b.daysLinks[in.Code]
	if found {
		// Consume before returning anything. Concurrent scans can never mint two
		// sessions, even if the successful response is lost on the network.
		delete(b.daysLinks, in.Code)
	}
	if found && len(b.daysSessions) < maxDaysSessions {
		b.daysSessions[hash] = daysSession{user: link.user, expires: now.Add(daysSessionTTL)}
	} else if found {
		found = false
	}
	b.mu.Unlock()
	if !found {
		http.Error(w, "invalid or expired link", http.StatusBadRequest)
		return
	}

	writeJSON(w, http.StatusOK, map[string]any{
		"access_token": token,
		"expires_in":   int(daysSessionTTL.Seconds()),
	})
}
