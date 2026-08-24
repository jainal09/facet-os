package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func countdownBroker(t *testing.T) *broker {
	t.Helper()
	return &broker{cfg: config{
		deviceToken: "secret",
		cacheDir:    t.TempDir(),
	}}
}

func countdownRequest(t *testing.T, b *broker, method, body, token string) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, "/countdown", bytes.NewBufferString(body))
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	w := httptest.NewRecorder()
	b.handleCountdown(w, r)
	return w
}

func TestCountdownRoundTripAndCompactWireShape(t *testing.T) {
	b := countdownBroker(t)
	w := countdownRequest(t, b, http.MethodPost,
		`{"d":"2027-03-20","t":"Launch — day!"}`, "secret")
	if w.Code != http.StatusOK {
		t.Fatalf("POST status %d: %s", w.Code, w.Body.String())
	}

	w = countdownRequest(t, b, http.MethodGet, "", "secret")
	if w.Code != http.StatusOK {
		t.Fatalf("GET status %d: %s", w.Code, w.Body.String())
	}
	var got map[string]string
	if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got["d"] != "2027-03-20" || got["t"] != "Launch - day!" || len(got["s"]) != 10 {
		t.Fatalf("unexpected payload: %#v", got)
	}
	if len(w.Body.Bytes()) > 120 {
		t.Fatalf("device payload is %d bytes, want <=120: %s", w.Body.Len(), w.Body.String())
	}
}

func TestCountdownValidationAndAuth(t *testing.T) {
	b := countdownBroker(t)
	if got := countdownRequest(t, b, http.MethodGet, "", "").Code; got != http.StatusUnauthorized {
		t.Fatalf("no bearer: got %d, want 401", got)
	}
	if got := countdownRequest(t, b, http.MethodPost,
		`{"d":"03/20/2027","t":"x"}`, "secret").Code; got != http.StatusBadRequest {
		t.Fatalf("bad date: got %d, want 400", got)
	}
	long := `{"d":"2027-03-20","t":"` + string(bytes.Repeat([]byte("x"), 60)) + `"}`
	w := countdownRequest(t, b, http.MethodPost, long, "secret")
	var got map[string]string
	if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if len(got["t"]) != 48 {
		t.Fatalf("stored text length %d, want 48", len(got["t"]))
	}
}

func TestDaysPageIsPublicButStateIsNot(t *testing.T) {
	b := countdownBroker(t)
	r := httptest.NewRequest(http.MethodGet, "/days", nil)
	w := httptest.NewRecorder()
	b.handleDaysPage(w, r)
	if w.Code != http.StatusOK || !bytes.Contains(w.Body.Bytes(), []byte("Days Until")) {
		t.Fatalf("GET /days: status=%d body=%q", w.Code, w.Body.String())
	}
}

func TestCountdownStateIsScopedToAuthenticatedUser(t *testing.T) {
	b := &broker{cfg: config{
		users: map[string]string{
			"alice": "alice-secret",
			"bob":   "bob-secret",
		},
		cacheDir: t.TempDir(),
	}}
	if w := countdownRequest(t, b, http.MethodPost,
		`{"d":"2027-03-20","t":"Alice"}`, "alice-secret"); w.Code != http.StatusOK {
		t.Fatalf("alice POST: %d %s", w.Code, w.Body.String())
	}
	if w := countdownRequest(t, b, http.MethodPost,
		`{"d":"2028-04-21","t":"Bob"}`, "bob-secret"); w.Code != http.StatusOK {
		t.Fatalf("bob POST: %d %s", w.Code, w.Body.String())
	}

	for token, wantText := range map[string]string{
		"alice-secret": "Alice",
		"bob-secret":   "Bob",
	} {
		w := countdownRequest(t, b, http.MethodGet, "", token)
		var got map[string]string
		if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil {
			t.Fatal(err)
		}
		if got["t"] != wantText {
			t.Errorf("bearer %q read %q, want %q", token, got["t"], wantText)
		}
	}
}
