// The PET store: the broker is the authoritative home of everything about a
// cube's pet that a phone designs and a dashboard reads — species, world,
// theme, name, hat, sleep window, birthday, weather location — while the cube
// remains authoritative for the life the pet actually lives (stage, meters,
// care mistakes, stardust). The cube caches the design in a small blob and
// works fully offline; this file only ever has to be right, not fast.
//
// One JSON file per user, written with the same temp-and-rename discipline as
// the countdown store. Wire format to the cube is one-letter keys for the same
// reason queue.go uses them: every byte of key name is internal SRAM during
// the parse on the device.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"time"

	_ "embed"
)

//go:embed static/pet.html
var petPage []byte

// ---------------------------------------------------------------- store

// petDesign is what the owner chooses on the web page. Everything here is
// broker-authoritative and folded into /pet/cfg for the cube.
type petDesign struct {
	Name       string  `json:"name"`
	Species    int     `json:"species"` // index into the firmware's species table
	World      int     `json:"world"`
	Theme      int     `json:"theme"`
	Hat        int     `json:"hat"`
	SleepStart int     `json:"sleep_start"` // minutes from local midnight
	SleepEnd   int     `json:"sleep_end"`
	Bday       string  `json:"bday"` // "MM-DD" or ""
	City       string  `json:"city"`
	Lat        float64 `json:"lat"`
	Lon        float64 `json:"lon"`
	Coax       bool    `json:"coax"` // coax-back ritual completed, pet may return
}

// petState is the cube's latest report, stored verbatim so the dashboard can
// show a live pet without talking to the cube.
type petState struct {
	Stage    int    `json:"stage"`
	Form     int    `json:"form"`
	Hunger   int    `json:"hunger"`
	Happy    int    `json:"happy"`
	Mistakes int    `json:"mistakes"`
	Stardust int    `json:"stardust"`
	Steps    int    `json:"steps"`
	Sick     int    `json:"sick"`
	Poop     int    `json:"poop"`
	Away     int    `json:"away"`
	Hatched  int64  `json:"hatched"`
	Seed     uint32 `json:"seed"`
	Updated  int64  `json:"updated"` // server stamp, unix seconds
}

type petRecord struct {
	Ver    uint32    `json:"cfg_ver"`
	Design petDesign `json:"design"`
	State  petState  `json:"state"`
}

func defaultPetDesign() petDesign {
	return petDesign{
		Name:       "PIP",
		SleepStart: 22 * 60, // 22:00
		SleepEnd:   7 * 60,  // 07:00
	}
}

func (b *broker) petPath(user string) string {
	return filepath.Join(b.cfg.cacheDir, "pet-"+user+".json")
}

func (b *broker) petLoad(user string) petRecord {
	rec := petRecord{Ver: 1, Design: defaultPetDesign()}
	if data, err := os.ReadFile(b.petPath(user)); err == nil {
		// A corrupt file reads as the default pet, never as an error.
		var stored petRecord
		if json.Unmarshal(data, &stored) == nil && stored.Ver != 0 {
			rec = stored
		}
	}
	return rec
}

func (b *broker) petSave(user string, rec petRecord) error {
	data, err := json.Marshal(rec)
	if err != nil {
		return err
	}
	path := b.petPath(user)
	tmp, err := os.CreateTemp(filepath.Dir(path), ".pet-*.part")
	if err != nil {
		return err
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
	return err
}

// ---------------------------------------------------------------- validation

const (
	petSpeciesMax = 1 // 0 PIP (vector astronaut), 1 BIT (pixel cat)
	petWorldMax   = 3 // space, ocean, forest, city
	petThemeMax   = 3 // modern, retro-lcd, b/w, amoled-neon
	petHatMax     = 3 // none, cap, crown, bow
)

func validMMDD(s string) bool {
	if s == "" {
		return true
	}
	t, err := time.Parse("01-02", s)
	return err == nil && t.Format("01-02") == s
}

func clampInt(v, lo, hi int) int {
	if v < lo {
		return lo
	}
	if v > hi {
		return hi
	}
	return v
}

// ---------------------------------------------------------------- weather

// Weather rides /pet/cfg so the cube pays zero extra requests for it. Fetched
// from Open-Meteo (keyless) at most every 15 minutes per user, best-effort
// with a short deadline: a slow or absent weather service must never delay a
// cube poll, so failures serve whatever is cached and stale simply ages out
// on the device (it stops drawing the layer after 6 h).
const petWeatherTTL = 15 * time.Minute

// Compact weather kinds shared with the firmware.
// 0 unknown, 1 clear, 2 partly, 3 cloudy, 4 fog, 5 rain, 6 snow, 7 storm.
func petWeatherKind(wmo int) int {
	switch {
	case wmo == 0:
		return 1
	case wmo <= 2:
		return 2
	case wmo == 3:
		return 3
	case wmo == 45 || wmo == 48:
		return 4
	case (wmo >= 51 && wmo <= 67) || (wmo >= 80 && wmo <= 82):
		return 5
	case (wmo >= 71 && wmo <= 77) || wmo == 85 || wmo == 86:
		return 6
	case wmo >= 95:
		return 7
	default:
		return 0
	}
}

type petWeather struct {
	kind    int
	temp    int
	fetched time.Time
	lat     float64
	lon     float64
}

// petWeatherFor returns the cached or freshly fetched weather for a design's
// location. Zero lat AND lon means "no location set" — a real (0,0) is open
// ocean and no one's desk.
func (b *broker) petWeatherFor(d petDesign) (petWeather, bool) {
	if d.Lat == 0 && d.Lon == 0 {
		return petWeather{}, false
	}
	now := time.Now()
	b.mu.Lock()
	if b.petWx == nil {
		b.petWx = map[string]petWeather{}
	}
	key := fmt.Sprintf("%.3f,%.3f", d.Lat, d.Lon)
	cached, ok := b.petWx[key]
	b.mu.Unlock()
	if ok && now.Sub(cached.fetched) < petWeatherTTL {
		return cached, true
	}
	if b.http == nil { // direct-constructed test brokers have no client
		return cached, ok
	}

	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	u := fmt.Sprintf(
		"https://api.open-meteo.com/v1/forecast?latitude=%.3f&longitude=%.3f&current=temperature_2m,weather_code",
		d.Lat, d.Lon)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return cached, ok
	}
	resp, err := b.http.Do(req)
	if err != nil {
		return cached, ok
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 8<<10))
	if err != nil || resp.StatusCode != http.StatusOK {
		return cached, ok
	}
	var out struct {
		Current struct {
			Temp float64 `json:"temperature_2m"`
			Code int     `json:"weather_code"`
		} `json:"current"`
	}
	if json.Unmarshal(body, &out) != nil {
		return cached, ok
	}
	fresh := petWeather{
		kind:    petWeatherKind(out.Current.Code),
		temp:    int(out.Current.Temp + 0.5),
		fetched: now,
		lat:     d.Lat,
		lon:     d.Lon,
	}
	b.mu.Lock()
	b.petWx[key] = fresh
	b.mu.Unlock()
	return fresh, true
}

// petGeocode resolves a city name once at design-save time, so the cube and
// the recurring weather fetch never pay for geocoding.
func (b *broker) petGeocode(city string) (lat, lon float64, ok bool) {
	if b.http == nil {
		return 0, 0, false
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	u := "https://geocoding-api.open-meteo.com/v1/search?count=1&name=" + url.QueryEscape(city)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return 0, 0, false
	}
	resp, err := b.http.Do(req)
	if err != nil {
		return 0, 0, false
	}
	defer resp.Body.Close()
	body, err := io.ReadAll(io.LimitReader(resp.Body, 32<<10))
	if err != nil || resp.StatusCode != http.StatusOK {
		return 0, 0, false
	}
	var out struct {
		Results []struct {
			Lat float64 `json:"latitude"`
			Lon float64 `json:"longitude"`
		} `json:"results"`
	}
	if json.Unmarshal(body, &out) != nil || len(out.Results) == 0 {
		return 0, 0, false
	}
	return out.Results[0].Lat, out.Results[0].Lon, true
}

// ---------------------------------------------------------------- handlers

func (b *broker) handlePetPage(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	setHTMLCSP(w, petPage, true)
	if r.Method == http.MethodHead {
		return
	}
	_, _ = w.Write(petPage)
}

// handlePetData serves the full record to the designer page (long keys — the
// browser has RAM to spare) and accepts nothing.
func (b *broker) handlePetData(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	user, ok := b.authenticatePet(r)
	if !ok {
		http.Error(w, "unauthorised", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	rec := b.petLoad(user)
	resp := map[string]any{
		"cfg_ver": rec.Ver,
		"design":  rec.Design,
		"state":   rec.State,
	}
	if wx, ok := b.petWeatherFor(rec.Design); ok {
		resp["weather"] = map[string]any{"kind": wx.kind, "temp": wx.temp}
	}
	writeJSON(w, http.StatusOK, resp)
}

// handlePetDesign accepts the designer page's save. Any accepted change bumps
// cfg_ver, which is the cube's whole re-apply gate, so a no-op save must not
// bump it — the cube would rebuild its scene for nothing.
func (b *broker) handlePetDesign(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	user, ok := b.authenticatePet(r)
	if !ok {
		http.Error(w, "unauthorised", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodPost && r.Method != http.MethodPut {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var in struct {
		Name       *string `json:"name"`
		Species    *int    `json:"species"`
		World      *int    `json:"world"`
		Theme      *int    `json:"theme"`
		Hat        *int    `json:"hat"`
		SleepStart *int    `json:"sleep_start"`
		SleepEnd   *int    `json:"sleep_end"`
		Bday       *string `json:"bday"`
		City       *string `json:"city"`
		Coax       *bool   `json:"coax"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 4096)).Decode(&in); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}

	rec := b.petLoad(user)
	d := rec.Design
	if in.Name != nil {
		name := asciiFold(*in.Name, 11)
		if name == "" {
			name = "PIP"
		}
		d.Name = name
	}
	if in.Species != nil {
		d.Species = clampInt(*in.Species, 0, petSpeciesMax)
	}
	if in.World != nil {
		d.World = clampInt(*in.World, 0, petWorldMax)
	}
	if in.Theme != nil {
		d.Theme = clampInt(*in.Theme, 0, petThemeMax)
	}
	if in.Hat != nil {
		d.Hat = clampInt(*in.Hat, 0, petHatMax)
	}
	if in.SleepStart != nil {
		d.SleepStart = clampInt(*in.SleepStart, 0, 1439)
	}
	if in.SleepEnd != nil {
		d.SleepEnd = clampInt(*in.SleepEnd, 0, 1439)
	}
	if in.Bday != nil {
		if !validMMDD(*in.Bday) {
			http.Error(w, "bday must be MM-DD", http.StatusBadRequest)
			return
		}
		d.Bday = *in.Bday
	}
	if in.City != nil {
		city := asciiFold(*in.City, 40)
		if city != d.City {
			d.City = city
			d.Lat, d.Lon = 0, 0
			if city != "" {
				// Best-effort: an unresolvable city keeps its name with no
				// coordinates, and the page shows weather as unavailable.
				if lat, lon, ok := b.petGeocode(city); ok {
					d.Lat, d.Lon = lat, lon
				}
			}
		}
	}
	if in.Coax != nil {
		d.Coax = *in.Coax
	}

	if !reflect.DeepEqual(d, rec.Design) {
		rec.Design = d
		rec.Ver++
		if err := b.petSave(user, rec); err != nil {
			http.Error(w, "store", http.StatusInternalServerError)
			return
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"cfg_ver": rec.Ver, "design": rec.Design})
}

// handlePetCfg is the cube's poll: the whole design plus weather in one small
// one-letter-key response, versioned so an unchanged config costs the cube a
// comparison and nothing else.
func (b *broker) handlePetCfg(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	user, ok := b.authenticate(r)
	if !ok {
		http.Error(w, "unauthorised", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	rec := b.petLoad(user)
	resp := map[string]any{
		"v":  rec.Ver,
		"n":  rec.Design.Name,
		"s":  rec.Design.Species,
		"w":  rec.Design.World,
		"t":  rec.Design.Theme,
		"h":  rec.Design.Hat,
		"ss": rec.Design.SleepStart,
		"se": rec.Design.SleepEnd,
		"b":  rec.Design.Bday,
		"cb": boolInt(rec.Design.Coax),
	}
	if name, ok := petSheetNameFor(rec.Design.Species); ok {
		resp["sn"] = name
	}
	if wx, ok := b.petWeatherFor(rec.Design); ok {
		resp["wx"] = wx.kind
		resp["wt"] = wx.temp
	}
	writeJSON(w, http.StatusOK, resp)
}

func boolInt(v bool) int {
	if v {
		return 1
	}
	return 0
}

// handlePetState accepts the cube's report. The broker never second-guesses
// the life the cube simulated; it just remembers it for the dashboard. The
// one write-back is the coax flag: a pet reporting itself home consumes the
// ritual, so a later departure needs a fresh one.
func (b *broker) handlePetState(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Cache-Control", "no-store")
	user, ok := b.authenticate(r)
	if !ok {
		http.Error(w, "unauthorised", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodPost {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	var in struct {
		Stage    int    `json:"g"`
		Form     int    `json:"f"`
		Hunger   int    `json:"hu"`
		Happy    int    `json:"ha"`
		Mistakes int    `json:"m"`
		Stardust int    `json:"sd"`
		Steps    int    `json:"st"`
		Sick     int    `json:"sk"`
		Poop     int    `json:"pp"`
		Away     int    `json:"aw"`
		Hatched  int64  `json:"hz"`
		Seed     uint32 `json:"e"`
	}
	if err := json.NewDecoder(http.MaxBytesReader(w, r.Body, 1024)).Decode(&in); err != nil {
		http.Error(w, "bad json", http.StatusBadRequest)
		return
	}

	rec := b.petLoad(user)
	rec.State = petState{
		Stage:    clampInt(in.Stage, 0, 5),
		Form:     clampInt(in.Form, 0, 3),
		Hunger:   clampInt(in.Hunger, 0, 100),
		Happy:    clampInt(in.Happy, 0, 100),
		Mistakes: clampInt(in.Mistakes, 0, 9999),
		Stardust: clampInt(in.Stardust, 0, 1<<30),
		Steps:    clampInt(in.Steps, 0, 1<<30),
		Sick:     clampInt(in.Sick, 0, 3),
		Poop:     clampInt(in.Poop, 0, 3),
		Away:     clampInt(in.Away, 0, 1),
		Hatched:  in.Hatched,
		Seed:     in.Seed,
		Updated:  time.Now().Unix(),
	}
	if rec.State.Away == 0 && rec.Design.Coax {
		rec.Design.Coax = false // ritual consumed; no Ver bump, nothing to re-apply
	}
	if err := b.petSave(user, rec); err != nil {
		http.Error(w, "store", http.StatusInternalServerError)
		return
	}
	// Return the config version so a state POST doubles as a drift check.
	writeJSON(w, http.StatusOK, map[string]any{"v": rec.Ver})
}

// handlePetSheet serves a sprite sheet as one LVGL RGB565 .bin: the standard
// 12-byte header where h = frames x w, frames stacked vertically. Sheets are
// generated from pixel maps compiled into the broker and keyed by validated
// name — nothing here fetches a URL, so the /art SSRF allowlist machinery
// deliberately does not apply.
func (b *broker) handlePetSheet(w http.ResponseWriter, r *http.Request) {
	if !b.authed(r) {
		http.Error(w, "unauthorised", http.StatusUnauthorized)
		return
	}
	if r.Method != http.MethodGet {
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	q := r.URL.Query()
	name := q.Get("n")
	theme, err := strconv.Atoi(q.Get("t"))
	if err != nil || theme < 0 || theme > petThemeMax {
		http.Error(w, "t must be 0..3", http.StatusBadRequest)
		return
	}
	size, err := strconv.Atoi(q.Get("s"))
	// Each frame is the cube's per-swap dirty rect; 120x120 = 14,400 px is the
	// largest square under the panel's one-flush budget of 15,360 px.
	if err != nil || size < 48 || size > 120 || size%petSpriteGrid != 0 {
		http.Error(w, "s must be 48..120 and a multiple of 12", http.StatusBadRequest)
		return
	}
	data, ok := encodePetSheet(name, theme, size)
	if !ok {
		http.Error(w, "unknown sprite", http.StatusNotFound)
		return
	}
	w.Header().Set("Content-Type", "application/octet-stream")
	w.Header().Set("Content-Length", strconv.Itoa(len(data)))
	// Sheets only change with a broker deploy; a day of caching is safe and
	// spares the cube's retry paths.
	w.Header().Set("Cache-Control", "public, max-age=86400")
	_, _ = w.Write(data)
}
