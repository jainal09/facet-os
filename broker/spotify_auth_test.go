package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

type roundTripFunc func(*http.Request) (*http.Response, error)

func (f roundTripFunc) RoundTrip(r *http.Request) (*http.Response, error) { return f(r) }

func spotifyBroker(t *testing.T) *broker {
	t.Helper()
	return &broker{
		cfg: config{
			clientID:    "client-id",
			redirectURI: "https://broker.example/callback",
			publicURL:   "https://broker.example",
			users: map[string]string{
				"alice": "alice-secret",
				"bob":   "bob-secret",
			},
			cacheDir: t.TempDir(),
		},
		pairs:       map[string]*pending{},
		credentials: map[string]string{},
		access:      map[string]cachedAccess{},
		http:        http.DefaultClient,
	}
}

func spotifyTokenRequest(b *broker, token, target string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(http.MethodGet, target, nil)
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	w := httptest.NewRecorder()
	b.handleSpotifyToken(w, r)
	return w
}

func TestParseBrokerUsersRejectsAmbiguousIdentity(t *testing.T) {
	one := strings.Repeat("1", 64)
	two := strings.Repeat("2", 64)
	users, err := parseBrokerUsers("alice="+one+",bob="+two, "")
	if err != nil || users["alice"] != one || users["bob"] != two {
		t.Fatalf("parse: users=%#v err=%v", users, err)
	}
	for _, raw := range []string{
		"alice=" + one + ",bob=" + one,
		"alice=" + one + ",alice=" + two,
		"bad user=" + one,
		"missing-separator",
		"alice=short",
	} {
		if _, err := parseBrokerUsers(raw, ""); err == nil {
			t.Errorf("%q should be rejected", raw)
		}
	}
}

func TestAuthenticateSelectsUserByBearer(t *testing.T) {
	b := spotifyBroker(t)
	for token, wantUser := range map[string]string{
		"alice-secret": "alice",
		"bob-secret":   "bob",
	} {
		r := httptest.NewRequest(http.MethodGet, "/spotify/token", nil)
		r.Header.Set("Authorization", "Bearer "+token)
		user, ok := b.authenticate(r)
		if !ok || user != wantUser {
			t.Errorf("bearer %q selected user=%q ok=%v, want %q", token, user, ok, wantUser)
		}
	}
	r := httptest.NewRequest(http.MethodGet, "/spotify/token", nil)
	r.Header.Set("Authorization", "Bearer alice-secret-extra")
	if user, ok := b.authenticate(r); ok {
		t.Errorf("wrong bearer authenticated as %q", user)
	}
	r = httptest.NewRequest(http.MethodGet, "/spotify/token", nil)
	r.Header.Set("Authorization", "alice-secret")
	if user, ok := b.authenticate(r); ok {
		t.Errorf("raw token without Bearer scheme authenticated as %q", user)
	}
}

func TestSpotifyCredentialsPersistAndRemainIsolated(t *testing.T) {
	b := spotifyBroker(t)
	if err := b.storeSpotifyCredential("alice", "alice-refresh"); err != nil {
		t.Fatal(err)
	}
	if err := b.storeSpotifyCredential("bob", "bob-refresh"); err != nil {
		t.Fatal(err)
	}

	info, err := os.Stat(filepath.Join(b.cfg.cacheDir, spotifyCredentialFile))
	if err != nil {
		t.Fatal(err)
	}
	if got := info.Mode().Perm(); got != 0o600 {
		t.Fatalf("credential mode %o, want 600", got)
	}

	restarted := &broker{cfg: b.cfg}
	if err := restarted.loadSpotifyCredentials(); err != nil {
		t.Fatal(err)
	}
	if restarted.credentials["alice"] != "alice-refresh" || restarted.credentials["bob"] != "bob-refresh" {
		t.Fatalf("credentials crossed or disappeared after restart: %#v", restarted.credentials)
	}
}

func TestMissingCredentialsReturnStableUserScopedPairURLs(t *testing.T) {
	b := spotifyBroker(t)
	alice1 := spotifyTokenRequest(b, "alice-secret", "/spotify/token")
	alice2 := spotifyTokenRequest(b, "alice-secret", "/spotify/token")
	bob := spotifyTokenRequest(b, "bob-secret", "/spotify/token")
	if alice1.Code != http.StatusPreconditionRequired || alice2.Code != http.StatusPreconditionRequired ||
		bob.Code != http.StatusPreconditionRequired {
		t.Fatalf("statuses alice1=%d alice2=%d bob=%d", alice1.Code, alice2.Code, bob.Code)
	}
	decodeURL := func(w *httptest.ResponseRecorder) string {
		var body map[string]string
		if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
			t.Fatal(err)
		}
		return body["authorization_url"]
	}
	a1, a2, bu := decodeURL(alice1), decodeURL(alice2), decodeURL(bob)
	if a1 == "" || a1 != a2 {
		t.Fatalf("one user's pending URL should be stable: %q then %q", a1, a2)
	}
	if a1 == bu {
		t.Fatalf("different users received the same pairing session: %q", a1)
	}

	// /pair no longer accepts a caller-chosen code that was not minted for an
	// authenticated user by /spotify/token.
	r := httptest.NewRequest(http.MethodGet, "/pair?c=ATTACKER", nil)
	w := httptest.NewRecorder()
	b.handlePair(w, r)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("unissued pair code: got %d, want 400", w.Code)
	}
}

func TestSpotifyTokenUsesOnlyAuthenticatedUsersRefreshToken(t *testing.T) {
	b := spotifyBroker(t)
	if err := b.storeSpotifyCredential("alice", "alice-refresh"); err != nil {
		t.Fatal(err)
	}
	if err := b.storeSpotifyCredential("bob", "bob-refresh"); err != nil {
		t.Fatal(err)
	}
	var seen []string
	b.http = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		body, _ := io.ReadAll(r.Body)
		form, _ := url.ParseQuery(string(body))
		seen = append(seen, form.Get("refresh_token"))
		access := strings.TrimSuffix(form.Get("refresh_token"), "-refresh") + "-access"
		return &http.Response{
			StatusCode: http.StatusOK,
			Header:     make(http.Header),
			Body:       io.NopCloser(strings.NewReader(`{"access_token":"` + access + `","expires_in":3600}`)),
		}, nil
	})}

	alice := spotifyTokenRequest(b, "alice-secret", "/spotify/token")
	bob := spotifyTokenRequest(b, "bob-secret", "/spotify/token")
	if alice.Code != http.StatusOK || bob.Code != http.StatusOK {
		t.Fatalf("statuses alice=%d bob=%d", alice.Code, bob.Code)
	}
	if !strings.Contains(alice.Body.String(), "alice-access") || strings.Contains(alice.Body.String(), "bob-access") {
		t.Fatalf("alice response crossed users: %s", alice.Body.String())
	}
	if !strings.Contains(bob.Body.String(), "bob-access") || strings.Contains(bob.Body.String(), "alice-access") {
		t.Fatalf("bob response crossed users: %s", bob.Body.String())
	}
	if strings.Join(seen, ",") != "alice-refresh,bob-refresh" {
		t.Fatalf("refreshes sent upstream in wrong scope: %#v", seen)
	}
	if strings.Contains(alice.Body.String(), "refresh") || strings.Contains(bob.Body.String(), "refresh") {
		t.Fatal("a refresh token leaked to firmware")
	}
}

func TestOAuthCallbackStoresCredentialForPairingUser(t *testing.T) {
	b := spotifyBroker(t)
	start := spotifyTokenRequest(b, "bob-secret", "/spotify/token")
	var startBody map[string]string
	if err := json.Unmarshal(start.Body.Bytes(), &startBody); err != nil {
		t.Fatal(err)
	}
	pairURL, err := url.Parse(startBody["authorization_url"])
	if err != nil {
		t.Fatal(err)
	}
	state := pairURL.Query().Get("c")

	// Scanning the QR binds a PKCE verifier to the already-authenticated user.
	pairReq := httptest.NewRequest(http.MethodGet, pairURL.RequestURI(), nil)
	pairRes := httptest.NewRecorder()
	b.handlePair(pairRes, pairReq)
	if pairRes.Code != http.StatusFound {
		t.Fatalf("pair status=%d body=%s", pairRes.Code, pairRes.Body.String())
	}
	spotifyRedirect, err := url.Parse(pairRes.Header().Get("Location"))
	if err != nil {
		t.Fatal(err)
	}
	if spotifyRedirect.Query().Get("state") != state || spotifyRedirect.Query().Get("code_challenge") == "" {
		t.Fatalf("bad Spotify redirect: %s", spotifyRedirect)
	}

	b.http = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		body, _ := io.ReadAll(r.Body)
		form, _ := url.ParseQuery(string(body))
		if form.Get("code") != "spotify-code" || form.Get("code_verifier") == "" {
			t.Errorf("callback exchange form: %s", form.Encode())
		}
		return &http.Response{StatusCode: http.StatusOK, Header: make(http.Header),
			Body: io.NopCloser(strings.NewReader(
				`{"access_token":"bob-access","refresh_token":"bob-refresh","expires_in":3600}`))}, nil
	})}
	callbackReq := httptest.NewRequest(http.MethodGet,
		"/callback?code=spotify-code&state="+url.QueryEscape(state), nil)
	callbackRes := httptest.NewRecorder()
	b.handleCallback(callbackRes, callbackReq)
	if callbackRes.Code != http.StatusOK {
		t.Fatalf("callback status=%d body=%s", callbackRes.Code, callbackRes.Body.String())
	}
	if b.credentials["bob"] != "bob-refresh" {
		t.Fatalf("callback stored credentials under wrong user: %#v", b.credentials)
	}
	if _, exists := b.credentials["alice"]; exists {
		t.Fatalf("bob callback changed alice: %#v", b.credentials)
	}
	if _, exists := b.pairs[state]; exists {
		t.Fatal("completed pair session was not consumed")
	}
}

func TestPairCodeHasHardExpiryAndCallbackConsumesItOnFailure(t *testing.T) {
	b := spotifyBroker(t)
	state := "0123456789ABCDEF0123456789ABCDEF"
	b.pairs[state] = &pending{user: "alice", created: time.Now().Add(-pairTTL - time.Second)}
	w := httptest.NewRecorder()
	b.handlePair(w, httptest.NewRequest(http.MethodGet, "/pair?c="+state, nil))
	if w.Code != http.StatusBadRequest {
		t.Fatalf("expired pair status=%d, want 400", w.Code)
	}
	if _, ok := b.pairs[state]; ok {
		t.Fatal("expired pair was not removed")
	}

	b.pairs[state] = &pending{user: "alice", verifier: "verifier", created: time.Now()}
	b.http = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		return &http.Response{StatusCode: http.StatusBadGateway, Header: make(http.Header),
			Body: io.NopCloser(strings.NewReader(`{"error":"temporary"}`))}, nil
	})}
	w = httptest.NewRecorder()
	b.handleCallback(w, httptest.NewRequest(http.MethodGet, "/callback?code=bad&state="+state, nil))
	if w.Code != http.StatusBadGateway {
		t.Fatalf("failed callback status=%d, want 502", w.Code)
	}
	if _, ok := b.pairs[state]; ok {
		t.Fatal("callback did not consume pair state after token-exchange failure")
	}
}

func TestInvalidGrantClearsOnlyThatUserAndRequestsReauthorization(t *testing.T) {
	b := spotifyBroker(t)
	if err := b.storeSpotifyCredential("alice", "revoked"); err != nil {
		t.Fatal(err)
	}
	if err := b.storeSpotifyCredential("bob", "still-good"); err != nil {
		t.Fatal(err)
	}
	b.http = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		return &http.Response{
			StatusCode: http.StatusBadRequest,
			Header:     make(http.Header),
			Body: io.NopCloser(strings.NewReader(
				`{"error":"invalid_grant","error_description":"Refresh token revoked"}`)),
		}, nil
	})}

	w := spotifyTokenRequest(b, "alice-secret", "/spotify/token?force=1")
	if w.Code != http.StatusPreconditionRequired || !strings.Contains(w.Body.String(), "authorization_url") {
		t.Fatalf("revoked token response: status=%d body=%s", w.Code, w.Body.String())
	}
	if _, ok := b.credentials["alice"]; ok {
		t.Fatal("revoked credential was not cleared")
	}
	if b.credentials["bob"] != "still-good" {
		t.Fatal("clearing alice changed bob's credential")
	}

	// Verify the on-disk copy has the same isolation after a restart.
	restarted := &broker{cfg: b.cfg}
	if err := restarted.loadSpotifyCredentials(); err != nil {
		t.Fatal(err)
	}
	if _, ok := restarted.credentials["alice"]; ok || restarted.credentials["bob"] != "still-good" {
		t.Fatalf("unexpected restarted credentials: %#v", restarted.credentials)
	}
}

func TestCachedSpotifyAccessCanBeForcedFresh(t *testing.T) {
	b := spotifyBroker(t)
	b.credentials["alice"] = "refresh"
	b.access["alice"] = cachedAccess{token: "stale", expires: time.Now().Add(time.Hour)}
	requests := 0
	b.http = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		requests++
		return &http.Response{StatusCode: http.StatusOK, Header: make(http.Header),
			Body: io.NopCloser(strings.NewReader(`{"access_token":"fresh","expires_in":3600}`))}, nil
	})}

	if w := spotifyTokenRequest(b, "alice-secret", "/spotify/token"); !strings.Contains(w.Body.String(), "stale") {
		t.Fatalf("ordinary request should use cache: %s", w.Body.String())
	}
	if w := spotifyTokenRequest(b, "alice-secret", "/spotify/token?force=1"); !strings.Contains(w.Body.String(), "fresh") {
		t.Fatalf("forced request should refresh: %s", w.Body.String())
	}
	if requests != 1 {
		t.Fatalf("upstream requests=%d, want 1", requests)
	}
}

func TestSpotifyRefreshesAreSerializedPerUser(t *testing.T) {
	b := spotifyBroker(t)
	b.credentials["alice"] = "original-refresh"
	var mu sync.Mutex
	active, maxActive := 0, 0
	var seen []string
	b.http = &http.Client{Transport: roundTripFunc(func(r *http.Request) (*http.Response, error) {
		body, _ := io.ReadAll(r.Body)
		form, _ := url.ParseQuery(string(body))
		mu.Lock()
		active++
		if active > maxActive {
			maxActive = active
		}
		seen = append(seen, form.Get("refresh_token"))
		sequence := len(seen)
		mu.Unlock()

		time.Sleep(20 * time.Millisecond)
		mu.Lock()
		active--
		mu.Unlock()
		payload := `{"access_token":"fresh","refresh_token":"rotated-` +
			strconv.Itoa(sequence) + `","expires_in":3600}`
		return &http.Response{StatusCode: http.StatusOK, Header: make(http.Header),
			Body: io.NopCloser(strings.NewReader(payload))}, nil
	})}

	var wg sync.WaitGroup
	statuses := make(chan int, 2)
	for range 2 {
		wg.Add(1)
		go func() {
			defer wg.Done()
			statuses <- spotifyTokenRequest(b, "alice-secret", "/spotify/token?force=1").Code
		}()
	}
	wg.Wait()
	close(statuses)
	for status := range statuses {
		if status != http.StatusOK {
			t.Errorf("concurrent refresh status=%d, want 200", status)
		}
	}
	mu.Lock()
	defer mu.Unlock()
	if maxActive != 1 {
		t.Fatalf("same-user refresh concurrency=%d, want 1", maxActive)
	}
	if strings.Join(seen, ",") != "original-refresh,rotated-1" {
		t.Fatalf("rotating refresh sequence=%#v", seen)
	}
}
