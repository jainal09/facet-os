package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

// The device parses this with cJSON on an internal heap of 27-35 KB, so the two
// properties that matter are: it must be small, and it must not carry keys the
// device does not draw.
func TestQueueShapeIsMinimal(t *testing.T) {
	items := []queueItem{
		{ID: "3KDgOdm8Ir4GmoVarAQ7Zc", Name: "Timepiece", Artist: "Jenevieve",
			Art: "https://i.scdn.co/image/ab67616d00004851a90dc4218ebadb23869813dd"},
		{ID: "0vnz6lN4xKHZX7gtFMplMY", Name: "All This Time", Artist: "Someone",
			Art: "https://i.scdn.co/image/ab67616d000048510000000000000000deadbeef"},
		{ID: "7tFiyTwD0nx5a1eklYtX2J", Name: "Bohemian Rhapsody", Artist: "Queen",
			Art: "https://i.scdn.co/image/ab67616d00004851ffffffffffffffffcafebabe"},
	}
	b, err := json.Marshal(map[string]any{"q": items})
	if err != nil {
		t.Fatal(err)
	}
	// Spotify's own answer for the same three tracks is ~8 KB; the point of this
	// endpoint is that the device never sees that.
	if len(b) > 900 {
		t.Errorf("three items serialised to %d bytes, want <900: %s", len(b), b)
	}
	for _, k := range []string{`"i":`, `"n":`, `"a":`, `"u":`} {
		if !contains(string(b), k) {
			t.Errorf("missing key %s in %s", k, b)
		}
	}
	for _, k := range []string{"available_markets", "duration_ms", "external_urls"} {
		if contains(string(b), k) {
			t.Errorf("passthrough field %q leaked into the device payload", k)
		}
	}
}

func TestQueueRequiresBothCredentials(t *testing.T) {
	b := &broker{cfg: config{deviceToken: "secret"}, http: http.DefaultClient}

	// no broker bearer at all
	r := httptest.NewRequest(http.MethodGet, "/queue", nil)
	w := httptest.NewRecorder()
	b.handleQueue(w, r)
	if w.Code != http.StatusUnauthorized {
		t.Errorf("no bearer: got %d, want 401", w.Code)
	}

	// correct bearer but no Spotify token: must not reach Spotify unauthenticated
	r = httptest.NewRequest(http.MethodGet, "/queue", nil)
	r.Header.Set("Authorization", "Bearer secret")
	w = httptest.NewRecorder()
	b.handleQueue(w, r)
	if w.Code != http.StatusBadRequest {
		t.Errorf("no spotify token: got %d, want 400", w.Code)
	}
}

func TestPickImageChoosesNearest(t *testing.T) {
	type img = struct {
		URL   string `json:"url"`
		Width int    `json:"width"`
	}
	imgs := []img{{"big", 640}, {"mid", 300}, {"small", 64}}
	if got := pickImage(imgs, 264); got != "mid" {
		t.Errorf("for 264 px got %q, want mid(300)", got)
	}
	if got := pickImage(nil, 264); got != "" {
		t.Errorf("no images should give empty, got %q", got)
	}
}

func contains(s, sub string) bool {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return true
		}
	}
	return false
}
