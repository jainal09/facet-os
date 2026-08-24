package main

import (
	"bytes"
	"context"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"testing"
)

func TestOAuthPageEscapesUntrustedErrorText(t *testing.T) {
	w := httptest.NewRecorder()
	page(w, http.StatusOK, `<script>alert("title")</script>`, `<img src=x onerror=alert(1)>`)
	body := w.Body.String()
	for _, raw := range []string{`<script>alert`, `<img src=x`} {
		if strings.Contains(body, raw) {
			t.Fatalf("untrusted callback text rendered as markup: %s", body)
		}
	}
	if !strings.Contains(body, "&lt;script&gt;") || !strings.Contains(body, "&lt;img") {
		t.Fatalf("escaped callback text missing: %s", body)
	}
	if csp := w.Header().Get("Content-Security-Policy"); csp == "" || strings.Contains(csp, "unsafe-inline") {
		t.Fatalf("callback CSP is missing or weak: %q", csp)
	}
}

func TestSecurityMiddlewareSetsBaselineAndRejectsLargeBodies(t *testing.T) {
	h := securityHeaders(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))
	r := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	w := httptest.NewRecorder()
	h.ServeHTTP(w, r)
	for _, name := range []string{
		"Content-Security-Policy", "X-Content-Type-Options", "X-Frame-Options",
		"Referrer-Policy", "Permissions-Policy", "Strict-Transport-Security",
		"Cross-Origin-Opener-Policy", "Cross-Origin-Resource-Policy",
	} {
		if w.Header().Get(name) == "" {
			t.Errorf("missing security header %s", name)
		}
	}

	r = httptest.NewRequest(http.MethodPost, "/countdown",
		bytes.NewReader(make([]byte, maxRequestBody+1)))
	w = httptest.NewRecorder()
	h.ServeHTTP(w, r)
	if w.Code != http.StatusRequestEntityTooLarge {
		t.Fatalf("oversized body status=%d, want 413", w.Code)
	}
}

func TestRateLimitIsUserScopedAndReturns429(t *testing.T) {
	b := spotifyBroker(t)
	h := b.rateLimit(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNoContent)
	}))
	request := func(token string) int {
		r := httptest.NewRequest(http.MethodGet, "/spotify/token", nil)
		r.Header.Set("Authorization", "Bearer "+token)
		w := httptest.NewRecorder()
		h.ServeHTTP(w, r)
		return w.Code
	}
	for i := 0; i < 12; i++ {
		if got := request("alice-secret"); got != http.StatusNoContent {
			t.Fatalf("alice request %d got %d before burst was consumed", i, got)
		}
	}
	if got := request("alice-secret"); got != http.StatusTooManyRequests {
		t.Fatalf("alice request after burst got %d, want 429", got)
	}
	if got := request("bob-secret"); got != http.StatusNoContent {
		t.Fatalf("alice rate limit spilled into bob: status %d", got)
	}
}

func TestEmbeddedPagesHaveStrictMatchingCSPAndNoStoredBearer(t *testing.T) {
	provision, err := provisionFS.ReadFile("static/provision.html")
	if err != nil {
		t.Fatal(err)
	}
	for name, data := range map[string][]byte{"days": daysPage, "provision": provision} {
		text := string(data)
		if strings.Contains(text, "unsafe-inline") {
			t.Errorf("%s page CSP permits unsafe-inline", name)
		}
		for _, tag := range []string{"style", "script"} {
			for _, hash := range inlineCSPHashes(data, tag) {
				if !strings.Contains(text, hash) {
					t.Errorf("%s meta CSP does not match its inline %s hash %s", name, tag, hash)
				}
			}
		}
		for _, sink := range []string{
			"innerHTML", "outerHTML", "insertAdjacentHTML", "document.write", "eval(", "new Function",
		} {
			if strings.Contains(text, sink) {
				t.Errorf("%s page contains dangerous DOM sink %q", name, sink)
			}
		}
	}
	if bytes.Contains(daysPage, []byte("localStorage")) || bytes.Contains(daysPage, []byte("sessionStorage")) {
		t.Fatal("DAYS persists the broker bearer in browser storage")
	}
	for _, forbidden := range [][]byte{[]byte("DEVICE KEY"), []byte("broker bearer"), []byte("id=\"k\"")} {
		if bytes.Contains(daysPage, forbidden) {
			t.Fatalf("DAYS still asks the browser for the permanent cube credential: %q", forbidden)
		}
	}
	if !bytes.Contains(daysPage, []byte("/days/session")) ||
		!bytes.Contains(daysPage, []byte("history.replaceState")) {
		t.Fatal("DAYS does not exchange and remove its one-time QR code")
	}

	w := httptest.NewRecorder()
	r := httptest.NewRequest(http.MethodGet, "/days", nil)
	(&broker{}).handleDaysPage(w, r)
	if csp := w.Header().Get("Content-Security-Policy"); !strings.Contains(csp, "connect-src 'self'") ||
		strings.Contains(csp, "unsafe-inline") {
		t.Fatalf("DAYS response CSP is missing or weak: %q", csp)
	}
}

func TestCanonicalPublicURLRequiresHTTPSOrigin(t *testing.T) {
	good := map[string]string{
		"https://broker.example":      "https://broker.example",
		"https://broker.example/":     "https://broker.example",
		"https://broker.example:8443": "https://broker.example:8443",
	}
	for raw, want := range good {
		got, err := canonicalPublicURL(raw)
		if err != nil || got != want {
			t.Errorf("canonicalPublicURL(%q) = %q, %v; want %q", raw, got, err, want)
		}
	}
	for _, raw := range []string{
		"http://broker.example", "https://user@broker.example", "https://broker.example/path",
		"https://broker.example?next=evil", "https://broker.example/#fragment", "//broker.example", "not a URL",
	} {
		if got, err := canonicalPublicURL(raw); err == nil {
			t.Errorf("canonicalPublicURL(%q) accepted as %q", raw, got)
		}
	}
}

func TestReadOnlyEndpointsRejectUnexpectedMethods(t *testing.T) {
	b := &broker{}
	for path, handler := range map[string]http.HandlerFunc{
		"/healthz":  b.handleHealth,
		"/pair":     b.handlePair,
		"/callback": b.handleCallback,
		"/art":      b.handleArt,
		"/queue":    b.handleQueue,
	} {
		r := httptest.NewRequest(http.MethodPost, path, nil)
		w := httptest.NewRecorder()
		handler(w, r)
		if w.Code != http.StatusMethodNotAllowed {
			t.Errorf("POST %s status=%d, want 405", path, w.Code)
		}
	}
}

func TestArtURLValidationCoversRedirectTargets(t *testing.T) {
	good := []string{
		"https://i.scdn.co/image/abc",
		"https://i.scdn.co:443/image/abc",
		"https://mosaic.scdn.co/640/abc",
	}
	bad := []string{
		"http://i.scdn.co/image/abc",
		"https://i.scdn.co.evil.example/image/abc",
		"https://user@i.scdn.co/image/abc",
		"https://i.scdn.co:444/image/abc",
		"https://127.0.0.1/image/abc",
	}
	for _, raw := range good {
		u, _ := url.Parse(raw)
		if !validArtURL(u) {
			t.Errorf("safe Spotify art URL rejected: %s", raw)
		}
	}
	for _, raw := range bad {
		u, _ := url.Parse(raw)
		if validArtURL(u) {
			t.Errorf("unsafe art URL accepted: %s", raw)
		}
	}
}

func TestArtRedirectCannotLeaveAllowlist(t *testing.T) {
	b := &broker{http: &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		return &http.Response{
			StatusCode: http.StatusFound,
			Header:     http.Header{"Location": []string{"http://127.0.0.1/latest/meta-data"}},
			Body:       io.NopCloser(strings.NewReader("")),
			Request:    r,
		}, nil
	})}}
	if _, err := b.transcode(context.Background(), "https://i.scdn.co/image/abc", 64, false); err == nil ||
		!strings.Contains(err.Error(), "allowlist") {
		t.Fatalf("art redirect should be blocked, got %v", err)
	}
}
