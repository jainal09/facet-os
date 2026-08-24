package main

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"sync"
	"testing"
	"time"
)

func daysLinkRequest(b *broker, token string) *httptest.ResponseRecorder {
	r := httptest.NewRequest(http.MethodGet, "/days/link", nil)
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	w := httptest.NewRecorder()
	b.handleDaysLink(w, r)
	return w
}

func decodeDaysLinkCode(t *testing.T, w *httptest.ResponseRecorder) string {
	t.Helper()
	var body struct {
		AuthorizationURL string `json:"authorization_url"`
		ExpiresIn        int    `json:"expires_in"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	u, err := url.Parse(body.AuthorizationURL)
	if err != nil {
		t.Fatal(err)
	}
	if u.Scheme != "https" || u.Host != "broker.example" || u.Path != "/days" ||
		body.ExpiresIn != int(daysLinkTTL.Seconds()) {
		t.Fatalf("unexpected DAYS link response: %#v", body)
	}
	return u.Query().Get("c")
}

func exchangeDaysCode(b *broker, code string) *httptest.ResponseRecorder {
	body, _ := json.Marshal(map[string]string{"code": code})
	r := httptest.NewRequest(http.MethodPost, "/days/session", bytes.NewReader(body))
	r.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	b.handleDaysSession(w, r)
	return w
}

func decodeDaysToken(t *testing.T, w *httptest.ResponseRecorder) string {
	t.Helper()
	var body struct {
		AccessToken string `json:"access_token"`
		ExpiresIn   int    `json:"expires_in"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body.AccessToken == "" || body.ExpiresIn != int(daysSessionTTL.Seconds()) {
		t.Fatalf("unexpected DAYS session response: %#v", body)
	}
	return body.AccessToken
}

func TestDaysLinkRequiresCubeBearerAndIsStablePerUser(t *testing.T) {
	b := spotifyBroker(t)
	if got := daysLinkRequest(b, "").Code; got != http.StatusUnauthorized {
		t.Fatalf("missing cube bearer status=%d, want 401", got)
	}
	alice1 := daysLinkRequest(b, "alice-secret")
	alice2 := daysLinkRequest(b, "alice-secret")
	bob := daysLinkRequest(b, "bob-secret")
	if alice1.Code != http.StatusOK || alice2.Code != http.StatusOK || bob.Code != http.StatusOK {
		t.Fatalf("link statuses alice=%d/%d bob=%d", alice1.Code, alice2.Code, bob.Code)
	}
	a1 := decodeDaysLinkCode(t, alice1)
	a2 := decodeDaysLinkCode(t, alice2)
	bc := decodeDaysLinkCode(t, bob)
	if !validDaysLinkCode(a1) || a1 != a2 {
		t.Fatalf("Alice link should be valid and stable: %q then %q", a1, a2)
	}
	if a1 == bc {
		t.Fatal("different users received the same DAYS link code")
	}
}

func TestDaysCodeIsSingleUseEvenWithConcurrentExchange(t *testing.T) {
	b := spotifyBroker(t)
	code := decodeDaysLinkCode(t, daysLinkRequest(b, "alice-secret"))

	const attempts = 24
	results := make(chan int, attempts)
	var wg sync.WaitGroup
	for i := 0; i < attempts; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			results <- exchangeDaysCode(b, code).Code
		}()
	}
	wg.Wait()
	close(results)
	ok := 0
	for status := range results {
		if status == http.StatusOK {
			ok++
		} else if status != http.StatusBadRequest {
			t.Fatalf("concurrent exchange returned unexpected status %d", status)
		}
	}
	if ok != 1 {
		t.Fatalf("successful exchanges=%d, want exactly 1", ok)
	}
}

func TestDaysSessionIsUserScopedAndCountdownOnly(t *testing.T) {
	b := spotifyBroker(t)
	aliceCode := decodeDaysLinkCode(t, daysLinkRequest(b, "alice-secret"))
	bobCode := decodeDaysLinkCode(t, daysLinkRequest(b, "bob-secret"))
	aliceExchange := exchangeDaysCode(b, aliceCode)
	bobExchange := exchangeDaysCode(b, bobCode)
	if aliceExchange.Code != http.StatusOK || bobExchange.Code != http.StatusOK {
		t.Fatalf("session statuses alice=%d bob=%d", aliceExchange.Code, bobExchange.Code)
	}
	aliceToken := decodeDaysToken(t, aliceExchange)
	bobToken := decodeDaysToken(t, bobExchange)

	if w := countdownRequest(t, b, http.MethodPost,
		`{"d":"2027-03-20","t":"Alice"}`, aliceToken); w.Code != http.StatusOK {
		t.Fatalf("Alice temporary session POST: %d %s", w.Code, w.Body.String())
	}
	if w := countdownRequest(t, b, http.MethodPost,
		`{"d":"2028-04-21","t":"Bob"}`, bobToken); w.Code != http.StatusOK {
		t.Fatalf("Bob temporary session POST: %d %s", w.Code, w.Body.String())
	}
	for token, want := range map[string]string{aliceToken: "Alice", bobToken: "Bob"} {
		w := countdownRequest(t, b, http.MethodGet, "", token)
		var got map[string]string
		if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil {
			t.Fatal(err)
		}
		if got["t"] != want {
			t.Fatalf("temporary session crossed users: got %q, want %q", got["t"], want)
		}
	}

	if got := spotifyTokenRequest(b, aliceToken, "/spotify/token").Code; got != http.StatusUnauthorized {
		t.Fatalf("DAYS session reached Spotify token endpoint: status=%d", got)
	}
	if got := daysLinkRequest(b, aliceToken).Code; got != http.StatusUnauthorized {
		t.Fatalf("DAYS session minted another link: status=%d", got)
	}
	artReq := httptest.NewRequest(http.MethodGet, "/art?url=https://i.scdn.co/image/x", nil)
	artReq.Header.Set("Authorization", "Bearer "+aliceToken)
	artRes := httptest.NewRecorder()
	b.handleArt(artRes, artReq)
	if artRes.Code != http.StatusUnauthorized {
		t.Fatalf("DAYS session reached artwork endpoint: status=%d", artRes.Code)
	}

	hash := sha256.Sum256([]byte(aliceToken))
	b.mu.Lock()
	_, storesHash := b.daysSessions[hash]
	b.mu.Unlock()
	if !storesHash {
		t.Fatal("broker did not retain the DAYS token hash")
	}
}

func TestDaysLinksAndSessionsExpireWithoutExtension(t *testing.T) {
	b := spotifyBroker(t)
	code := decodeDaysLinkCode(t, daysLinkRequest(b, "alice-secret"))
	b.mu.Lock()
	link := b.daysLinks[code]
	link.created = time.Now().Add(-daysLinkTTL - time.Second)
	b.daysLinks[code] = link
	b.mu.Unlock()
	if got := exchangeDaysCode(b, code).Code; got != http.StatusBadRequest {
		t.Fatalf("expired link exchange status=%d, want 400", got)
	}

	code = decodeDaysLinkCode(t, daysLinkRequest(b, "alice-secret"))
	exchange := exchangeDaysCode(b, code)
	token := decodeDaysToken(t, exchange)
	hash := sha256.Sum256([]byte(token))
	b.mu.Lock()
	session := b.daysSessions[hash]
	session.expires = time.Now().Add(-time.Second)
	b.daysSessions[hash] = session
	b.mu.Unlock()
	if got := countdownRequest(t, b, http.MethodGet, "", token).Code; got != http.StatusUnauthorized {
		t.Fatalf("expired session status=%d, want 401", got)
	}
}

func TestDaysSessionRejectsWrongMethodTypeAndShape(t *testing.T) {
	b := spotifyBroker(t)
	for name, req := range map[string]*http.Request{
		"method": httptest.NewRequest(http.MethodGet, "/days/session", nil),
		"type":   httptest.NewRequest(http.MethodPost, "/days/session", bytes.NewBufferString(`{"code":"x"}`)),
	} {
		w := httptest.NewRecorder()
		b.handleDaysSession(w, req)
		if name == "method" && w.Code != http.StatusMethodNotAllowed {
			t.Errorf("wrong method status=%d, want 405", w.Code)
		}
		if name == "type" && w.Code != http.StatusUnsupportedMediaType {
			t.Errorf("wrong content type status=%d, want 415", w.Code)
		}
	}

	req := httptest.NewRequest(http.MethodPost, "/days/session",
		bytes.NewBufferString(`{"code":"AAAAAAAAAAAAAAAAAAAAAAAA","extra":true}`))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	b.handleDaysSession(w, req)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("unknown JSON field status=%d, want 400", w.Code)
	}
}
