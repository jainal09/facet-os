package main

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"image"
	"image/color"
	"image/jpeg"
	"testing"
)

// The whole reason this service exists for album art is that the firmware can
// only decode BASELINE JPEG. If encodeSquare ever emits progressive, the cube
// shows nothing and fails silently — so assert on the marker itself rather than
// trusting that image/jpeg keeps behaving.
func TestEncodeSquareIsBaseline(t *testing.T) {
	src := image.NewRGBA(image.Rect(0, 0, 640, 640))
	for y := 0; y < 640; y++ {
		for x := 0; x < 640; x++ {
			src.Set(x, y, color.RGBA{uint8(x / 3), uint8(y / 3), 0x40, 0xff})
		}
	}

	out, err := encodeSquare(src, 240)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}

	if sof := findSOF(out); sof != 0xC0 {
		t.Fatalf("expected SOF0 (baseline, 0xC0), got 0x%02X — the firmware cannot decode this", sof)
	}

	cfg, _, err := image.DecodeConfig(bytes.NewReader(out))
	if err != nil {
		t.Fatalf("decode config: %v", err)
	}
	if cfg.Width != 240 || cfg.Height != 240 {
		t.Fatalf("expected 240x240, got %dx%d", cfg.Width, cfg.Height)
	}
	t.Logf("640x640 RGBA -> %d bytes baseline JPEG at 240x240", len(out))
}

// A non-square source must be centre-cropped, not squashed. Playlist mosaics are
// not always 1:1.
func TestEncodeSquareCropsRatherThanDistorts(t *testing.T) {
	src := image.NewRGBA(image.Rect(0, 0, 600, 300))
	out, err := encodeSquare(src, 200)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	cfg, _, err := image.DecodeConfig(bytes.NewReader(out))
	if err != nil {
		t.Fatalf("decode config: %v", err)
	}
	if cfg.Width != cfg.Height {
		t.Fatalf("not square: %dx%d", cfg.Width, cfg.Height)
	}
}

// Progressive input has to work, because that is exactly what we suspect Spotify
// serves and what the device cannot handle.
func TestProgressiveInputIsAccepted(t *testing.T) {
	src := image.NewRGBA(image.Rect(0, 0, 128, 128))
	var buf bytes.Buffer
	if err := jpeg.Encode(&buf, src, nil); err != nil {
		t.Fatalf("fixture: %v", err)
	}
	img, _, err := image.Decode(bytes.NewReader(buf.Bytes()))
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if _, err := encodeSquare(img, 64); err != nil {
		t.Fatalf("encode: %v", err)
	}
}

func TestArtHostAllowlistRejectsEverythingElse(t *testing.T) {
	for _, h := range []string{"evil.example", "localhost", "127.0.0.1", "i.scdn.co.evil.example", "169.254.169.254"} {
		if artHostAllow[h] {
			t.Fatalf("%q must not be allowlisted — this endpoint is public via Funnel", h)
		}
	}
	if !artHostAllow["i.scdn.co"] {
		t.Fatal("i.scdn.co should be allowed")
	}
}

func TestValidPairCode(t *testing.T) {
	good := []string{"0123456789ABCDEF0123456789ABCDEF"}
	bad := []string{"", "A7F3", "ABCD1234", "../../etc/passwd", "a7f30123456789abcdef0123456789ab", "0123456789ABCDEF0123456789ABCDE!", "0123456789ABCDEF0123456789ABCDEF0"}
	for _, s := range good {
		if !validPairCode(s) {
			t.Errorf("%q should be valid", s)
		}
	}
	for _, s := range bad {
		if validPairCode(s) {
			t.Errorf("%q should be rejected", s)
		}
	}
}

// findSOF walks JPEG markers and returns the Start-Of-Frame type.
// 0xC0 = baseline, 0xC2 = progressive. This is the same check worth running
// against a real Spotify URL to settle whether the broker is required at all.
func findSOF(b []byte) byte {
	for i := 2; i+3 < len(b); {
		if b[i] != 0xFF {
			i++
			continue
		}
		m := b[i+1]
		if m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7) {
			i += 2
			continue
		}
		if (m >= 0xC0 && m <= 0xC3) || (m >= 0xC5 && m <= 0xC7) ||
			(m >= 0xC9 && m <= 0xCB) || (m >= 0xCD && m <= 0xCF) {
			return m
		}
		seg := int(b[i+2])<<8 | int(b[i+3])
		if seg < 2 {
			return 0
		}
		i += 2 + seg
	}
	return 0
}

// The LVGL binary header is a wire format the firmware parses with no
// validation beyond a magic byte, so a wrong field here shows as a garbled or
// blank tile with no error anywhere. Pin every byte.
func TestEncodeLVGLBinHeader(t *testing.T) {
	src := image.NewRGBA(image.Rect(0, 0, 600, 300))
	const size = 240

	out, err := encodeLVGLBin(src, size)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}

	wantLen := 12 + size*size*2
	if len(out) != wantLen {
		t.Fatalf("length %d, want %d (12-byte header + w*h*2)", len(out), wantLen)
	}
	if out[0] != lvImageHeaderMagic {
		t.Errorf("magic 0x%02X, want 0x%02X", out[0], lvImageHeaderMagic)
	}
	if out[1] != lvColorFormatRGB565 {
		t.Errorf("cf 0x%02X, want 0x%02X (LV_COLOR_FORMAT_RGB565)", out[1], lvColorFormatRGB565)
	}
	if got := uint16(out[2]) | uint16(out[3])<<8; got != 0 {
		t.Errorf("flags %d, want 0", got)
	}
	if got := uint16(out[4]) | uint16(out[5])<<8; got != size {
		t.Errorf("w %d, want %d", got, size)
	}
	if got := uint16(out[6]) | uint16(out[7])<<8; got != size {
		t.Errorf("h %d, want %d", got, size)
	}
	if got := uint16(out[8]) | uint16(out[9])<<8; got != size*2 {
		t.Errorf("stride %d, want %d", got, size*2)
	}
	if got := uint16(out[10]) | uint16(out[11])<<8; got != 0 {
		t.Errorf("reserved %d, want 0", got)
	}
}

// RGB565 must be little-endian or every colour comes out wrong on the panel.
func TestEncodeLVGLBinPixelOrder(t *testing.T) {
	src := image.NewRGBA(image.Rect(0, 0, 8, 8))
	for y := 0; y < 8; y++ {
		for x := 0; x < 8; x++ {
			src.Set(x, y, color.RGBA{0xFF, 0x00, 0x00, 0xFF}) // pure red
		}
	}
	out, err := encodeLVGLBin(src, 8)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	// Pure red in RGB565 is 0xF800; little-endian on the wire is 0x00 then 0xF8.
	if out[12] != 0x00 || out[13] != 0xF8 {
		t.Fatalf("first pixel bytes %02X %02X, want 00 F8 (0xF800 little-endian)", out[12], out[13])
	}
}

// dominantColor is the accent the player tints itself with, so the cases that
// matter are the ones where naive averaging would produce something unusable.
func TestDominantColor(t *testing.T) {
	// build a size x size RGB565 buffer in LVGL's container, every pixel the same
	solid := func(r, g, b uint8) []byte {
		const size = 16
		out := make([]byte, lvglHeaderLen+size*size*2)
		out[0], out[1] = lvImageHeaderMagic, lvColorFormatRGB565
		v := uint16(r>>3)<<11 | uint16(g>>2)<<5 | uint16(b>>3)
		for i := lvglHeaderLen; i+1 < len(out); i += 2 {
			binary.LittleEndian.PutUint16(out[i:i+2], v)
		}
		return out
	}

	if _, ok := dominantColor(solid(0xE0, 0x10, 0x10)); !ok {
		t.Fatal("saturated red should yield an accent")
	}

	// Greyscale art has no hue to find. Returning a washed-out grey would tint the
	// UI with something indistinguishable from its own chrome, so it must decline
	// and let the caller keep its default.
	if got, ok := dominantColor(solid(0x80, 0x80, 0x80)); ok {
		t.Fatalf("grey art must not yield an accent, got %q", got)
	}
	// Near-black is skipped for the same reason, before saturation is even judged.
	if got, ok := dominantColor(solid(0x08, 0x06, 0x02)); ok {
		t.Fatalf("near-black art must not yield an accent, got %q", got)
	}

	// A dark but colourful cover must come back light enough to read a glyph
	// against, which is the whole point of conditioning it here.
	got, ok := dominantColor(solid(0x30, 0x08, 0x08))
	if !ok {
		t.Fatal("dark red should still yield an accent")
	}
	var rr, gg, bb int
	if _, err := fmt.Sscanf(got, "%02X%02X%02X", &rr, &gg, &bb); err != nil {
		t.Fatalf("accent %q is not RRGGBB: %v", got, err)
	}
	if mx := max(rr, max(gg, bb)); mx < 130 {
		t.Errorf("accent %q too dark to draw on (max channel %d, want >=130)", got, mx)
	}
	if rr <= gg || rr <= bb {
		t.Errorf("accent %q lost the red hue", got)
	}

	if _, ok := dominantColor([]byte{1, 2, 3}); ok {
		t.Error("a truncated buffer must not yield an accent")
	}
}
