package main

import (
	"regexp"
	"strings"
	"testing"
)

// The provisioning page wires itself up at load time with a run of
// `$("id").onclick = ...` statements. A reference to an element that does not
// exist throws *during that run*, so every assignment after it silently never
// happens — the page renders, the first button works, and the rest are dead.
//
// This shipped once: an edit added the JavaScript for a "Scan again" button but
// not the button, and the page reached a phone half-wired. Parsing the script in
// isolation cannot catch it, because the script is perfectly valid on its own;
// the missing half is in the markup. Checking the two against each other is the
// only thing that does, and it is nearly free.
//
// This runs in the Docker build via `go test ./...`, so a page that references a
// non-existent element fails the image build rather than reaching a phone.
func TestProvisionPageElementsExist(t *testing.T) {
	page, err := provisionFS.ReadFile("static/provision.html")
	if err != nil {
		t.Fatalf("embedded page unreadable: %v", err)
	}
	html := string(page)

	defined := map[string]bool{}
	for _, m := range regexp.MustCompile(`id="([A-Za-z][\w-]*)"`).FindAllStringSubmatch(html, -1) {
		defined[m[1]] = true
	}
	if len(defined) == 0 {
		t.Fatal("no id attributes found — the extraction is broken, not the page")
	}

	referenced := regexp.MustCompile(`\$\("([A-Za-z][\w-]*)"\)`).FindAllStringSubmatch(html, -1)
	if len(referenced) == 0 {
		t.Fatal(`no $("...") lookups found — the extraction is broken, not the page`)
	}

	for _, m := range referenced {
		if !defined[m[1]] {
			t.Errorf(`page calls $(%q) but no element has id=%q — `+
				`everything wired after that line will never run`, m[1], m[1])
		}
	}
}

// The UUIDs are the contract with main/ble_prov.c. If they drift, pairing fails
// with a device that simply never appears in the browser's chooser, which is
// indistinguishable from the cube being off.
func TestProvisionPageKeepsServiceUUID(t *testing.T) {
	page, _ := provisionFS.ReadFile("static/provision.html")
	const svc = "f9a30001-0b45-4f7e-9c2a-6d1e8b3c7a51"
	if !regexp.MustCompile(regexp.QuoteMeta(svc)).Match(page) {
		t.Errorf("service UUID %s missing — must match FACET_UUID(0x01) in ble_prov.c", svc)
	}
}

// The QR on the cube encodes the pairing code as a URL fragment, and the page
// is the half of that contract that reads it. Lose codeFromHash() — in a
// refactor, a merge, a minifier — and nothing breaks loudly: the page still
// loads, still pairs, and simply asks the user to type six digits that were
// already in the URL. The feature quietly stops existing.
//
// The fragment must stay a fragment. A query string would work identically for
// the user and would additionally hand the pairing code to the access log of
// whatever static host serves this page, which is the one thing the fragment
// was chosen to avoid.
func TestProvisionPageReadsCodeFromFragment(t *testing.T) {
	page, err := provisionFS.ReadFile("static/provision.html")
	if err != nil {
		t.Fatalf("embedded page unreadable: %v", err)
	}
	html := string(page)

	for _, want := range []string{
		"function codeFromHash()", // the parser itself
		"location.hash",           // reading the fragment, not location.search
		"prefillCode",             // and actually using what it parsed
	} {
		if !strings.Contains(html, want) {
			t.Errorf("page no longer contains %q — the QR's #c=NNNNNN prefill is dead, "+
				"and the only symptom is users typing a code they should not have to", want)
		}
	}

	if strings.Contains(html, "location.search") {
		t.Error("page reads location.search — the pairing code must travel in the " +
			"fragment so it never reaches the static host's logs")
	}
}

// Static hosting is only sound because this page talks to the cube over GATT
// and to no server whatsoever. That is a load-bearing architectural property,
// not an incidental one: the moment anything here fetches, the copy published
// to GitHub Pages starts depending on an origin that is not GitHub Pages, and
// Wi-Fi setup quietly re-acquires the dependency on the broker that moving it
// off the broker was meant to remove. It would still pass every other test
// here, and would fail only in the field, only when the broker was down --
// which is exactly when someone is most likely to be re-provisioning a cube.
func TestProvisionPageCallsNoBackend(t *testing.T) {
	page, err := provisionFS.ReadFile("static/provision.html")
	if err != nil {
		t.Fatalf("embedded page unreadable: %v", err)
	}
	html := string(page)

	// The App Store link is a link the user taps, not a request the page makes.
	html = strings.ReplaceAll(html, "https://apps.apple.com/app/id1492822055", "")

	for _, banned := range []string{
		"fetch(", "XMLHttpRequest", "WebSocket", "EventSource",
		"http://", "https://",
	} {
		if strings.Contains(html, banned) {
			t.Errorf("page contains %q — it must reach nothing but the cube over "+
				"Bluetooth, or hosting it as a static file stops being valid", banned)
		}
	}
}
