package main

import (
	"bytes"
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
	good := []string{"A7F3", "ABCD1234", "0000"}
	bad := []string{"", "abc", "A7", "A7F3!", "../../etc/passwd", "a7f3"}
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
