// PET designer sessions: the same single-use QR -> short-lived browser bearer
// shape as DAYS (days_auth.go), duplicated rather than generalised so the two
// capabilities can never accidentally widen each other. A pet session can touch
// only the pet endpoints; the permanent cube bearer never enters the browser.
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
	petLinkTTL       = 5 * time.Minute
	petSessionTTL    = 30 * time.Minute
	petLinkCodeBytes = 18 // 144 bits; 24 base64url characters in the QR URL
	petTokenBytes    = 32 // 256 bits; returned once and stored only as SHA-256
	maxPetSessions   = 4096
)

type petLink struct {
	user    string
	created time.Time
}

type petSession struct {
	user    string
	expires time.Time
}

func (b *broker) ensurePetMapsLocked() {
	if b.petLinks == nil {
		b.petLinks = map[string]petLink{}
	}
	if b.petSessions == nil {
		b.petSessions = map[[sha256.Size]byte]petSession{}
	}
}

func (b *broker) reapPetLocked(now time.Time) {
	b.ensurePetMapsLocked()
	for code, link := range b.petLinks {
		if !now.Before(link.created.Add(petLinkTTL)) {
			delete(b.petLinks, code)
		}
	}
	for hash, session := range b.petSessions {
		if !now.Before(session.expires) {
			delete(b.petSessions, hash)
		}
	}
}

func validPetLinkCode(code string) bool {
	if len(code) != base64.RawURLEncoding.EncodedLen(petLinkCodeBytes) {
		return false
	}
	raw, err := base64.RawURLEncoding.DecodeString(code)
	return err == nil && len(raw) == petLinkCodeBytes
}

func validPetSessionToken(token string) bool {
	if len(token) != base64.RawURLEncoding.EncodedLen(petTokenBytes) {
		return false
	}
	raw, err := base64.RawURLEncoding.DecodeString(token)
	return err == nil && len(raw) == petTokenBytes
}

// authenticatePetSession accepts only a temporary browser bearer, kept separate
// from authenticate for the same reason as authenticateDaysSession: this
// capability must never grow access to Spotify tokens, art, or other cubes.
func (b *broker) authenticatePetSession(r *http.Request) (string, bool) {
	token, ok := bearerToken(r)
	if !ok || !validPetSessionToken(token) {
		return "", false
	}
	hash := sha256.Sum256([]byte(token))
	now := time.Now()
	b.mu.Lock()
	b.ensurePetMapsLocked()
	session, found := b.petSessions[hash]
	if found && !now.Before(session.expires) {
		delete(b.petSessions, hash)
		found = false
	}
	b.mu.Unlock()
	return session.user, found
}

// authenticatePet is the union the shared pet endpoints accept: the cube's own
// bearer, or a designer session minted from its QR.
func (b *broker) authenticatePet(r *http.Request) (string, bool) {
	if user, ok := b.authenticate(r); ok {
		return user, true
	}
	return b.authenticatePetSession(r)
}

// handlePetLink lets authenticated firmware mint a user-bound QR link. A
// pending link is stable until consumed or expired, so a retry never changes
// the QR under a phone camera mid-scan.
func (b *broker) handlePetLink(w http.ResponseWriter, r *http.Request) {
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

	candidate, err := randomURLSafe(petLinkCodeBytes)
	if err != nil {
		http.Error(w, "entropy", http.StatusInternalServerError)
		return
	}
	now := time.Now()
	code := ""
	b.mu.Lock()
	b.reapPetLocked(now)
	for existing, link := range b.petLinks {
		if link.user == user {
			code = existing
			break
		}
	}
	if code == "" {
		if _, collision := b.petLinks[candidate]; collision {
			b.mu.Unlock()
			http.Error(w, "entropy collision", http.StatusInternalServerError)
			return
		}
		code = candidate
		b.petLinks[code] = petLink{user: user, created: now}
	}
	b.mu.Unlock()

	authorizationURL := strings.TrimRight(b.cfg.publicURL, "/") +
		"/pet?c=" + url.QueryEscape(code)
	writeJSON(w, http.StatusOK, map[string]any{
		"authorization_url": authorizationURL,
		"expires_in":        int(petLinkTTL.Seconds()),
	})
}

// handlePetSession atomically consumes a QR code and returns a temporary,
// pet-only browser bearer. The raw bearer is never retained server-side.
func (b *broker) handlePetSession(w http.ResponseWriter, r *http.Request) {
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
	if !validPetLinkCode(in.Code) {
		http.Error(w, "invalid or expired link", http.StatusBadRequest)
		return
	}

	token, err := randomURLSafe(petTokenBytes)
	if err != nil {
		http.Error(w, "entropy", http.StatusInternalServerError)
		return
	}
	hash := sha256.Sum256([]byte(token))
	now := time.Now()
	b.mu.Lock()
	b.reapPetLocked(now)
	link, found := b.petLinks[in.Code]
	if found {
		// Consume before returning anything. Concurrent scans can never mint two
		// sessions, even if the successful response is lost on the network.
		delete(b.petLinks, in.Code)
	}
	if found && len(b.petSessions) < maxPetSessions {
		b.petSessions[hash] = petSession{user: link.user, expires: now.Add(petSessionTTL)}
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
		"expires_in":   int(petSessionTTL.Seconds()),
	})
}
