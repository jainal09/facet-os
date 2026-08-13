// The "days until" countdown: one date, one line of text, set from a phone or
// laptop at /days and read by the cube at /countdown.
//
// The web page is inert markup, unauthenticated for the same reason /provision
// is — it holds no secret. Every read or write of the actual state goes through
// the device bearer, which the page asks for once and keeps in localStorage.
// The state itself is one tiny JSON file in the cache directory, written with
// the same temp-and-rename discipline as the art cache, so a killed process
// cannot leave a half-written file that later parses as garbage.
package main

import (
	_ "embed"
	"encoding/json"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

//go:embed static/days.html
var daysPage []byte

// countdown is the stored form. Set is stamped server-side on every save and is
// what lets the cube draw a progress bar: elapsed/(set..target) needs a start.
type countdown struct {
	Date string `json:"date"` // target, YYYY-MM-DD
	Text string `json:"text"`
	Set  string `json:"set"` // when it was last saved, YYYY-MM-DD
}

func (b *broker) countdownPath() string {
	return filepath.Join(b.cfg.cacheDir, "countdown.json")
}

// asciiFold reduces the text to the printable ASCII the cube's fonts can draw.
// The device would render anything else as empty boxes (its fonts are subset to
// 0x20-0x7E), so the sanitising has to happen here, where there is a full
// unicode library and one place to tune, rather than on the device.
func asciiFold(s string, max int) string {
	var out strings.Builder
	for _, r := range s {
		switch {
		case r >= 0x20 && r <= 0x7E:
			out.WriteRune(r)
		case r == '–' || r == '—': // en/em dash
			out.WriteByte('-')
		case r == '‘' || r == '’': // curly single quotes
			out.WriteByte('\'')
		case r == '“' || r == '”': // curly double quotes
			out.WriteByte('"')
		}
	}
	folded := strings.Join(strings.Fields(out.String()), " ")
	if len(folded) > max {
		folded = strings.TrimSpace(folded[:max])
	}
	return folded
}

// handleCountdown serves the state to the cube (GET) and accepts a new one from
// the page (POST). One-letter keys on the wire out, for the same reason
// queue.go uses them: every byte of key name is internal SRAM during the parse.
func (b *broker) handleCountdown(w http.ResponseWriter, r *http.Request) {
	if !b.authed(r) {
		http.Error(w, "unauthorised", http.StatusUnauthorized)
		return
	}

	switch r.Method {
	case http.MethodGet:
		var c countdown
		if data, err := os.ReadFile(b.countdownPath()); err == nil {
			_ = json.Unmarshal(data, &c) // a corrupt file reads as unset, not an error
		}
		w.Header().Set("Cache-Control", "no-store")
		writeJSON(w, http.StatusOK, map[string]string{"d": c.Date, "t": c.Text, "s": c.Set})

	case http.MethodPost, http.MethodPut:
		var in struct {
			Date string `json:"d"`
			Text string `json:"t"`
		}
		if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&in); err != nil {
			http.Error(w, "bad json", http.StatusBadRequest)
			return
		}
		if _, err := time.Parse("2006-01-02", in.Date); err != nil {
			http.Error(w, "date must be YYYY-MM-DD", http.StatusBadRequest)
			return
		}
		c := countdown{
			Date: in.Date,
			Text: asciiFold(in.Text, 48),
			Set:  time.Now().UTC().Format("2006-01-02"),
		}
		data, _ := json.Marshal(c)
		path := b.countdownPath()
		tmp, err := os.CreateTemp(filepath.Dir(path), ".countdown-*.part")
		if err != nil {
			http.Error(w, "store", http.StatusInternalServerError)
			return
		}
		tmpName := tmp.Name()
		defer os.Remove(tmpName)
		if _, err = tmp.Write(data); err == nil {
			err = tmp.Chmod(0o644)
		}
		if closeErr := tmp.Close(); err == nil {
			err = closeErr
		}
		if err == nil {
			err = os.Rename(tmpName, path)
		}
		if err != nil {
			http.Error(w, "store", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Cache-Control", "no-store")
		writeJSON(w, http.StatusOK, map[string]string{"d": c.Date, "t": c.Text, "s": c.Set})

	default:
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
	}
}

func (b *broker) handleDaysPage(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	if r.Method == http.MethodHead {
		return
	}
	_, _ = w.Write(daysPage)
}
