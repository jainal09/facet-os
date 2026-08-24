package main

import (
	"bytes"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"os"
	"strings"
	"testing"
)

func petBroker(t *testing.T) *broker {
	t.Helper()
	return &broker{cfg: config{
		deviceToken: "secret",
		cacheDir:    t.TempDir(),
		publicURL:   "https://broker.example",
	}}
}

func petRequest(t *testing.T, b *broker, handler http.HandlerFunc,
	method, target, body, token string) *httptest.ResponseRecorder {
	t.Helper()
	r := httptest.NewRequest(method, target, bytes.NewBufferString(body))
	if token != "" {
		r.Header.Set("Authorization", "Bearer "+token)
	}
	if body != "" {
		r.Header.Set("Content-Type", "application/json")
	}
	w := httptest.NewRecorder()
	handler(w, r)
	return w
}

func TestPetCfgDefaultsAndCompactWireShape(t *testing.T) {
	b := petBroker(t)
	w := petRequest(t, b, b.handlePetCfg, http.MethodGet, "/pet/cfg", "", "secret")
	if w.Code != http.StatusOK {
		t.Fatalf("GET /pet/cfg status %d: %s", w.Code, w.Body.String())
	}
	var got map[string]any
	if err := json.Unmarshal(w.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got["n"] != "PIP" || got["v"].(float64) != 1 ||
		got["ss"].(float64) != 1320 || got["se"].(float64) != 420 {
		t.Fatalf("unexpected defaults: %#v", got)
	}
	if _, hasSheet := got["sn"]; hasSheet {
		t.Fatalf("vector species advertised a sprite sheet: %#v", got)
	}
	if w.Body.Len() > 300 {
		t.Fatalf("device payload is %d bytes, want <=300", w.Body.Len())
	}
}

func TestPetDesignRoundTripAndVersionGate(t *testing.T) {
	b := petBroker(t)
	design := `{"name":"Möö the pet!","species":1,"world":3,"theme":2,"hat":1,` +
		`"sleep_start":1380,"sleep_end":390,"bday":"07-14"}`
	w := petRequest(t, b, b.handlePetDesign, http.MethodPut, "/pet/design", design, "secret")
	if w.Code != http.StatusOK {
		t.Fatalf("PUT status %d: %s", w.Code, w.Body.String())
	}
	var out struct {
		Ver    uint32    `json:"cfg_ver"`
		Design petDesign `json:"design"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &out); err != nil {
		t.Fatal(err)
	}
	if out.Ver != 2 {
		t.Fatalf("first change should bump cfg_ver to 2, got %d", out.Ver)
	}
	if out.Design.Name != "M the pet!" { // ascii fold drops the umlauts
		t.Fatalf("name not folded: %q", out.Design.Name)
	}

	// The identical save must not bump the version: the version is the cube's
	// whole re-apply gate and a no-op save would rebuild its scene for nothing.
	same := `{"name":"M the pet!","species":1,"world":3,"theme":2,"hat":1,` +
		`"sleep_start":1380,"sleep_end":390,"bday":"07-14"}`
	w = petRequest(t, b, b.handlePetDesign, http.MethodPut, "/pet/design", same, "secret")
	if err := json.Unmarshal(w.Body.Bytes(), &out); err != nil {
		t.Fatal(err)
	}
	if out.Ver != 2 {
		t.Fatalf("no-op save bumped cfg_ver to %d", out.Ver)
	}

	// The cube's view reflects it, one-letter keys, sprite sheet advertised.
	w = petRequest(t, b, b.handlePetCfg, http.MethodGet, "/pet/cfg", "", "secret")
	var cfg map[string]any
	if err := json.Unmarshal(w.Body.Bytes(), &cfg); err != nil {
		t.Fatal(err)
	}
	if cfg["v"].(float64) != 2 || cfg["s"].(float64) != 1 || cfg["t"].(float64) != 2 ||
		cfg["b"] != "07-14" || cfg["sn"] != "bit" {
		t.Fatalf("cfg does not reflect the design: %#v", cfg)
	}

	if got := petRequest(t, b, b.handlePetDesign, http.MethodPut, "/pet/design",
		`{"bday":"13-40"}`, "secret").Code; got != http.StatusBadRequest {
		t.Fatalf("bad bday accepted: %d", got)
	}
}

func TestPetStateReportAndCoaxConsumption(t *testing.T) {
	b := petBroker(t)
	// Arm the coax-back ritual, as the designer page would.
	if w := petRequest(t, b, b.handlePetDesign, http.MethodPut, "/pet/design",
		`{"coax":true}`, "secret"); w.Code != http.StatusOK {
		t.Fatalf("coax PUT: %d %s", w.Code, w.Body.String())
	}

	// A still-away report keeps the ritual armed.
	report := `{"g":5,"f":0,"hu":0,"ha":0,"m":9,"sd":120,"st":40,"sk":0,"pp":0,"aw":1,"hz":1756000000,"e":42}`
	if w := petRequest(t, b, b.handlePetState, http.MethodPost, "/pet/st", report, "secret"); w.Code != http.StatusOK {
		t.Fatalf("POST /pet/st: %d %s", w.Code, w.Body.String())
	}
	if rec := b.petLoad("default"); !rec.Design.Coax || rec.State.Away != 1 {
		t.Fatalf("away report cleared coax early: %+v", rec)
	}

	// The pet coming home consumes the ritual without a version bump.
	home := `{"g":4,"f":1,"hu":60,"ha":60,"m":9,"sd":120,"st":40,"sk":0,"pp":0,"aw":0,"hz":1756000000,"e":42}`
	w := petRequest(t, b, b.handlePetState, http.MethodPost, "/pet/st", home, "secret")
	var resp map[string]uint32
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	rec := b.petLoad("default")
	if rec.Design.Coax {
		t.Fatal("home report did not consume the coax flag")
	}
	if resp["v"] != rec.Ver {
		t.Fatalf("state POST returned version %d, record has %d", resp["v"], rec.Ver)
	}
	if rec.State.Stage != 4 || rec.State.Seed != 42 || rec.State.Updated == 0 {
		t.Fatalf("state not stored: %+v", rec.State)
	}
}

func TestPetStateIsScopedToAuthenticatedUser(t *testing.T) {
	b := &broker{cfg: config{
		users: map[string]string{
			"alice": "alice-secret",
			"bob":   "bob-secret",
		},
		cacheDir: t.TempDir(),
	}}
	if w := petRequest(t, b, b.handlePetDesign, http.MethodPut, "/pet/design",
		`{"name":"ALICEPET"}`, "alice-secret"); w.Code != http.StatusOK {
		t.Fatalf("alice PUT: %d", w.Code)
	}
	if w := petRequest(t, b, b.handlePetDesign, http.MethodPut, "/pet/design",
		`{"name":"BOBPET"}`, "bob-secret"); w.Code != http.StatusOK {
		t.Fatalf("bob PUT: %d", w.Code)
	}
	for token, want := range map[string]string{"alice-secret": "ALICEPET", "bob-secret": "BOBPET"} {
		w := petRequest(t, b, b.handlePetCfg, http.MethodGet, "/pet/cfg", "", token)
		var cfg map[string]any
		if err := json.Unmarshal(w.Body.Bytes(), &cfg); err != nil {
			t.Fatal(err)
		}
		if cfg["n"] != want {
			t.Errorf("bearer %q read pet %q, want %q", token, cfg["n"], want)
		}
	}
}

func TestPetSessionFlowAndCapabilityBoundaries(t *testing.T) {
	b := petBroker(t)

	// Mint a link as the cube, twice — retries must not change the QR.
	w := petRequest(t, b, b.handlePetLink, http.MethodGet, "/pet/link", "", "secret")
	if w.Code != http.StatusOK {
		t.Fatalf("GET /pet/link: %d %s", w.Code, w.Body.String())
	}
	var link struct {
		URL string `json:"authorization_url"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &link); err != nil {
		t.Fatal(err)
	}
	w = petRequest(t, b, b.handlePetLink, http.MethodGet, "/pet/link", "", "secret")
	var link2 struct {
		URL string `json:"authorization_url"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &link2); err != nil {
		t.Fatal(err)
	}
	if link.URL != link2.URL {
		t.Fatal("pending link changed between retries")
	}
	code := link.URL[strings.LastIndex(link.URL, "c=")+2:]

	// Exchange it for a session token; the code is single-use.
	w = petRequest(t, b, b.handlePetSession, http.MethodPost, "/pet/session",
		fmt.Sprintf(`{"code":%q}`, code), "")
	if w.Code != http.StatusOK {
		t.Fatalf("POST /pet/session: %d %s", w.Code, w.Body.String())
	}
	var session struct {
		Token string `json:"access_token"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &session); err != nil {
		t.Fatal(err)
	}
	if w = petRequest(t, b, b.handlePetSession, http.MethodPost, "/pet/session",
		fmt.Sprintf(`{"code":%q}`, code), ""); w.Code != http.StatusBadRequest {
		t.Fatalf("consumed code re-issued a session: %d", w.Code)
	}

	// The session reads and writes pet state...
	if w = petRequest(t, b, b.handlePetData, http.MethodGet, "/pet/data", "", session.Token); w.Code != http.StatusOK {
		t.Fatalf("session GET /pet/data: %d", w.Code)
	}
	if w = petRequest(t, b, b.handlePetDesign, http.MethodPut, "/pet/design",
		`{"theme":3}`, session.Token); w.Code != http.StatusOK {
		t.Fatalf("session PUT /pet/design: %d", w.Code)
	}
	// ...and nothing else: not the cube-only pet endpoints, not DAYS.
	if w = petRequest(t, b, b.handlePetCfg, http.MethodGet, "/pet/cfg", "", session.Token); w.Code != http.StatusUnauthorized {
		t.Fatalf("pet session reached /pet/cfg: %d", w.Code)
	}
	if w = petRequest(t, b, b.handlePetState, http.MethodPost, "/pet/st",
		`{"g":1}`, session.Token); w.Code != http.StatusUnauthorized {
		t.Fatalf("pet session reached /pet/st: %d", w.Code)
	}
	if w = countdownRequest(t, b, http.MethodGet, "", session.Token); w.Code != http.StatusUnauthorized {
		t.Fatalf("pet session reached /countdown: %d", w.Code)
	}

	// A DAYS session cannot reach the pet endpoints either.
	daysToken := mintDaysSession(t, b)
	if w = petRequest(t, b, b.handlePetData, http.MethodGet, "/pet/data", "", daysToken); w.Code != http.StatusUnauthorized {
		t.Fatalf("days session reached /pet/data: %d", w.Code)
	}
}

func mintDaysSession(t *testing.T, b *broker) string {
	t.Helper()
	w := petRequest(t, b, b.handleDaysLink, http.MethodGet, "/days/link", "", "secret")
	if w.Code != http.StatusOK {
		t.Fatalf("GET /days/link: %d %s", w.Code, w.Body.String())
	}
	var link struct {
		URL string `json:"authorization_url"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &link); err != nil {
		t.Fatal(err)
	}
	code := link.URL[strings.LastIndex(link.URL, "c=")+2:]
	w = petRequest(t, b, b.handleDaysSession, http.MethodPost, "/days/session",
		fmt.Sprintf(`{"code":%q}`, code), "")
	if w.Code != http.StatusOK {
		t.Fatalf("POST /days/session: %d %s", w.Code, w.Body.String())
	}
	var session struct {
		Token string `json:"access_token"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &session); err != nil {
		t.Fatal(err)
	}
	return session.Token
}

func TestPetSheetHeaderAndFrameGeometry(t *testing.T) {
	b := petBroker(t)
	w := petRequest(t, b, b.handlePetSheet, http.MethodGet, "/pet/sheet?n=bit&t=1&s=96", "", "secret")
	if w.Code != http.StatusOK {
		t.Fatalf("GET /pet/sheet: %d %s", w.Code, w.Body.String())
	}
	data := w.Body.Bytes()
	if data[0] != lvImageHeaderMagic || data[1] != lvColorFormatRGB565 {
		t.Fatalf("bad .bin header: % x", data[:12])
	}
	width := int(binary.LittleEndian.Uint16(data[4:]))
	height := int(binary.LittleEndian.Uint16(data[6:]))
	stride := int(binary.LittleEndian.Uint16(data[8:]))
	frames := len(petSpriteBit)
	if width != 96 || height != frames*96 || stride != 192 {
		t.Fatalf("geometry w=%d h=%d stride=%d, want 96/%d/192", width, height, stride, frames*96)
	}
	if len(data) != lvglHeaderLen+stride*height {
		t.Fatalf("length %d does not match header geometry %d", len(data), lvglHeaderLen+stride*height)
	}
	// Every frame must fit the cube's one-flush budget; it is what makes a
	// frame swap band-proof on a panel with no tear signal.
	if width*width > 15360 {
		t.Fatalf("frame %dx%d exceeds the one-flush budget", width, width)
	}

	for _, target := range []string{
		"/pet/sheet?n=bit&t=1&s=144", // over the flush budget
		"/pet/sheet?n=bit&t=1&s=100", // not a multiple of the sprite grid
		"/pet/sheet?n=bit&t=9&s=96",  // no such theme
		"/pet/sheet?n=nope&t=1&s=96", // no such sprite
	} {
		if got := petRequest(t, b, b.handlePetSheet, http.MethodGet, target, "", "secret").Code; got == http.StatusOK {
			t.Errorf("%s accepted", target)
		}
	}
	if got := petRequest(t, b, b.handlePetSheet, http.MethodGet,
		"/pet/sheet?n=bit&t=1&s=96", "", "").Code; got != http.StatusUnauthorized {
		t.Fatalf("unauthenticated sheet fetch: %d", got)
	}
}

func TestPetSpriteGridsAreWellFormed(t *testing.T) {
	// A short row would panic the encoder at request time; catch it here.
	for name, frames := range petSprites {
		for f, frame := range frames {
			for r, row := range frame {
				if len(row) != petSpriteGrid {
					t.Errorf("sprite %q frame %d row %d is %d cells, want %d",
						name, f, r, len(row), petSpriteGrid)
				}
			}
		}
		for theme := 0; theme <= petThemeMax; theme++ {
			for _, size := range []int{48, 120} {
				if _, ok := encodePetSheet(name, theme, size); !ok {
					t.Errorf("sprite %q theme %d size %d failed to encode", name, theme, size)
				}
			}
		}
	}
}

func TestPetPageIsPublicButDataIsNot(t *testing.T) {
	b := petBroker(t)
	r := httptest.NewRequest(http.MethodGet, "/pet", nil)
	w := httptest.NewRecorder()
	b.handlePetPage(w, r)
	if w.Code != http.StatusOK || !bytes.Contains(w.Body.Bytes(), []byte("Pet Designer")) {
		t.Fatalf("GET /pet: status=%d", w.Code)
	}
	if csp := w.Header().Get("Content-Security-Policy"); !strings.Contains(csp, "connect-src 'self'") ||
		strings.Contains(csp, "unsafe-inline") {
		t.Fatalf("pet page CSP is missing or weak: %q", csp)
	}
	if got := petRequest(t, b, b.handlePetData, http.MethodGet, "/pet/data", "", "").Code; got != http.StatusUnauthorized {
		t.Fatalf("unauthenticated /pet/data: %d", got)
	}
	if got := petRequest(t, b, b.handlePetCfg, http.MethodGet, "/pet/cfg", "", "wrong").Code; got != http.StatusUnauthorized {
		t.Fatalf("wrong bearer /pet/cfg: %d", got)
	}
}

func TestPetCorruptStoreReadsAsDefaultPet(t *testing.T) {
	b := petBroker(t)
	if err := os.WriteFile(b.petPath("default"), []byte("{not json"), 0o644); err != nil {
		t.Fatal(err)
	}
	rec := b.petLoad("default")
	if rec.Design.Name != "PIP" || rec.Ver != 1 {
		t.Fatalf("corrupt store did not read as default: %+v", rec)
	}
}
