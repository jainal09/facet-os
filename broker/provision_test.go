package main

import (
	"regexp"
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
