package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/base64"
	"net"
	"net/http"
	"strings"
	"sync"
	"time"
)

const maxRequestBody = 8 << 10
const maxRateLimitKeys = 4096

// securityHeaders is the application-side baseline. Funnel supplies TLS, but
// limits and browser policy still belong here so a proxy configuration mistake
// cannot silently remove them.
func securityHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Security-Policy",
			"default-src 'none'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Strict-Transport-Security", "max-age=31536000; includeSubDomains")
		w.Header().Set("Cross-Origin-Opener-Policy", "same-origin")
		w.Header().Set("Cross-Origin-Resource-Policy", "same-origin")
		w.Header().Set("Referrer-Policy", "no-referrer")
		// Bluetooth is intentionally not disabled: /provision needs Web Bluetooth.
		w.Header().Set("Permissions-Policy",
			"camera=(), microphone=(), geolocation=(), payment=(), usb=()")

		if r.ContentLength > maxRequestBody {
			http.Error(w, "request body too large", http.StatusRequestEntityTooLarge)
			return
		}
		r.Body = http.MaxBytesReader(w, r.Body, maxRequestBody)
		next.ServeHTTP(w, r)
	})
}

type tokenBucket struct {
	tokens float64
	last   time.Time
}

type requestLimiter struct {
	mu      sync.Mutex
	buckets map[string]tokenBucket
	rate    float64
	burst   float64
}

func newRequestLimiter(rate float64, burst int) *requestLimiter {
	return &requestLimiter{buckets: map[string]tokenBucket{}, rate: rate, burst: float64(burst)}
}

func (l *requestLimiter) allow(key string) bool {
	now := time.Now()
	l.mu.Lock()
	defer l.mu.Unlock()
	b, exists := l.buckets[key]
	if !exists && len(l.buckets) >= maxRateLimitKeys {
		cutoff := now.Add(-10 * time.Minute)
		for oldKey, old := range l.buckets {
			if old.last.Before(cutoff) {
				delete(l.buckets, oldKey)
			}
		}
		if len(l.buckets) >= maxRateLimitKeys {
			return false
		}
	}
	if b.last.IsZero() {
		b.tokens = l.burst
		b.last = now
	} else {
		b.tokens += now.Sub(b.last).Seconds() * l.rate
		if b.tokens > l.burst {
			b.tokens = l.burst
		}
		b.last = now
	}
	if b.tokens < 1 {
		l.buckets[key] = b
		return false
	}
	b.tokens--
	l.buckets[key] = b
	return true
}

// rateLimit keeps unauthenticated floods and a leaked individual cube bearer
// from monopolising the broker. Authenticated requests key on the server-side
// user label; invalid tokens key on RemoteAddr, so an attacker cannot grow an
// unbounded map simply by inventing bearer strings.
func (b *broker) rateLimit(next http.Handler) http.Handler {
	global := newRequestLimiter(100, 200)
	perClient := newRequestLimiter(20, 60)
	sensitive := newRequestLimiter(4, 12)
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		key := remoteKey(r)
		if user, ok := b.authenticate(r); ok {
			key = "user:" + user
		} else if r.URL.Path == "/countdown" {
			if user, ok := b.authenticateDaysSession(r); ok {
				key = "days-user:" + user
			}
		} else if r.URL.Path == "/pet/data" || r.URL.Path == "/pet/design" {
			if user, ok := b.authenticatePetSession(r); ok {
				key = "pet-user:" + user
			}
		}
		allowed := global.allow("global") && perClient.allow(key)
		if allowed && sensitivePath(r.URL.Path) {
			allowed = sensitive.allow(key + ":" + r.URL.Path)
		}
		if !allowed {
			w.Header().Set("Cache-Control", "no-store")
			w.Header().Set("Retry-After", "1")
			http.Error(w, "too many requests", http.StatusTooManyRequests)
			return
		}
		next.ServeHTTP(w, r)
	})
}

func remoteKey(r *http.Request) string {
	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err != nil {
		host = r.RemoteAddr
	}
	if len(host) > 128 {
		host = host[:128]
	}
	return "remote:" + host
}

func sensitivePath(path string) bool {
	switch path {
	case "/spotify/token", "/pair", "/callback", "/art", "/art.bin", "/queue",
		"/countdown", "/days/link", "/days/session",
		"/pet/link", "/pet/session", "/pet/data", "/pet/design", "/pet/cfg",
		"/pet/st", "/pet/sheet":
		return true
	default:
		return false
	}
}

// setHTMLCSP permits only the exact inline blocks embedded in a page. Hashes are
// derived from the shipped bytes, so editing the UI cannot leave a stale hand-
// copied policy or force script-src 'unsafe-inline'.
func setHTMLCSP(w http.ResponseWriter, page []byte, connectSelf bool) {
	parts := []string{"default-src 'none'", "base-uri 'none'", "form-action 'none'", "frame-ancestors 'none'",
		"require-trusted-types-for 'script'", "trusted-types 'none'"}
	if hashes := inlineCSPHashes(page, "style"); len(hashes) > 0 {
		parts = append(parts, "style-src "+strings.Join(hashes, " "))
	}
	if hashes := inlineCSPHashes(page, "script"); len(hashes) > 0 {
		parts = append(parts, "script-src "+strings.Join(hashes, " "))
	}
	if connectSelf {
		parts = append(parts, "connect-src 'self'")
	}
	w.Header().Set("Content-Security-Policy", strings.Join(parts, "; "))
}

func inlineCSPHashes(page []byte, tag string) []string {
	open, close := []byte("<"+tag+">"), []byte("</"+tag+">")
	var out []string
	for rest := page; ; {
		start := bytes.Index(rest, open)
		if start < 0 {
			return out
		}
		content := rest[start+len(open):]
		end := bytes.Index(content, close)
		if end < 0 {
			return out
		}
		sum := sha256.Sum256(content[:end])
		out = append(out, "'sha256-"+base64.StdEncoding.EncodeToString(sum[:])+"'")
		rest = content[end+len(close):]
	}
}
