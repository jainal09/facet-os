// Queue lookahead, so the cube can prefetch what is coming next and make a swipe
// feel instant instead of "nudge, wait, art appears".
//
// This exists on the server for one measured reason. Spotify's /me/player/queue
// answers with the next twenty tracks in full: 55,569 bytes on a real account.
// Raising the firmware's response buffer is trivial — it is PSRAM — but *parsing*
// that is not. cJSON allocates roughly one 64-byte node per value, and the
// firmware sets SPIRAM_MALLOC_ALWAYSINTERNAL=128, which sends every allocation
// under 128 bytes to internal SRAM. Twenty track objects is on the order of 800
// nodes, ~50 KB, against the 27-35 KB of internal SRAM the device actually has.
// It would not be slow, it would fail — as an allocation storm, during a swipe.
//
// So the 55 KB is decoded here, where Go ignores unknown fields for free, and the
// device receives a few hundred bytes containing only what it will draw.
package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
)

const (
	queueMaxItems = 8
	queueDefault  = 3
)

// spotifyQueue is deliberately a partial view. Everything not named here is
// discarded by the decoder, which is the whole point of doing this server-side.
type spotifyQueue struct {
	Queue []struct {
		ID      string `json:"id"`
		Name    string `json:"name"`
		Artists []struct {
			Name string `json:"name"`
		} `json:"artists"`
		Album struct {
			Images []struct {
				URL   string `json:"url"`
				Width int    `json:"width"`
			} `json:"images"`
		} `json:"album"`
	} `json:"queue"`
}

// queueItem uses one-letter keys. The device parses this with cJSON, and every
// byte of key name is a byte of internal SRAM during the parse.
type queueItem struct {
	ID     string `json:"i"`
	Name   string `json:"n"`
	Artist string `json:"a"`
	Art    string `json:"u"`
}

// handleQueue proxies the caller's own Spotify token; the broker holds no token
// of its own. Pairing deliberately hands the refresh token to the device and
// forgets it, and that stays true — this borrows an access token for the length
// of one request, over TLS, behind the same bearer that guards /art.
func (b *broker) handleQueue(w http.ResponseWriter, r *http.Request) {
	if !b.authed(r) {
		http.Error(w, "unauthorised", http.StatusUnauthorized)
		return
	}
	tok := r.Header.Get("X-Spotify-Token")
	if tok == "" {
		http.Error(w, "X-Spotify-Token required", http.StatusBadRequest)
		return
	}

	n := queueDefault
	if v, err := strconv.Atoi(r.URL.Query().Get("n")); err == nil && v > 0 {
		n = v
	}
	if n > queueMaxItems {
		n = queueMaxItems
	}
	// Which image to pick, matching what the panel will draw.
	size := artMinSize
	if v, err := strconv.Atoi(r.URL.Query().Get("s")); err == nil && v >= artMinSize && v <= artMaxSize {
		size = v
	}

	req, err := http.NewRequestWithContext(r.Context(), http.MethodGet,
		"https://api.spotify.com/v1/me/player/queue", nil)
	if err != nil {
		http.Error(w, "request", http.StatusInternalServerError)
		return
	}
	req.Header.Set("Authorization", "Bearer "+tok)

	resp, err := b.http.Do(req)
	if err != nil {
		http.Error(w, "upstream", http.StatusBadGateway)
		return
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNoContent {
		writeJSON(w, http.StatusOK, map[string]any{"q": []queueItem{}})
		return
	}
	if resp.StatusCode != http.StatusOK {
		// Pass the status through rather than flattening it: the device needs to
		// tell "your token expired" apart from "the queue is empty".
		http.Error(w, "spotify "+strconv.Itoa(resp.StatusCode), resp.StatusCode)
		return
	}

	var sq spotifyQueue
	if err := json.NewDecoder(resp.Body).Decode(&sq); err != nil {
		http.Error(w, "decode", http.StatusBadGateway)
		return
	}

	out := make([]queueItem, 0, n)
	for _, it := range sq.Queue {
		if len(out) >= n || it.ID == "" {
			continue
		}
		qi := queueItem{ID: it.ID, Name: it.Name, Art: pickImage(it.Album.Images, size)}
		if len(it.Artists) > 0 {
			qi.Artist = it.Artists[0].Name
		}
		out = append(out, qi)
	}
	writeJSON(w, http.StatusOK, map[string]any{"q": out})
}

// pickImage returns the URL of the image nearest the requested size, so the
// device asks /art.bin to rescale as little as possible.
func pickImage(imgs []struct {
	URL   string `json:"url"`
	Width int    `json:"width"`
}, size int) string {
	best, bestD := "", 1<<30
	for _, im := range imgs {
		if im.URL == "" {
			continue
		}
		d := im.Width - size
		if d < 0 {
			d = -d
		}
		if d < bestD {
			best, bestD = im.URL, d
		}
	}
	return best
}

var _ = fmt.Sprintf // keep fmt available for future error detail
