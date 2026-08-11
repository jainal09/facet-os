/* BLE Wi-Fi provisioning — pair the cube with a phone to join a network.
 *
 * Typing a WPA2 password on a 480x480 panel with an on-screen keyboard is the
 * worst interaction in this device. This module lets a phone do it instead: a
 * web page speaks Web Bluetooth to the cube, renders the scan list with a real
 * keyboard underneath it, and hands the credentials back over an encrypted
 * GATT link.
 *
 * Two rules shape everything here, both from docs/HARDWARE.md:
 *
 *  - BLE is built on start and freed on stop, like an app. The NimBLE host and
 *    the controller's dynamic allocations only exist while a session is open.
 *    What cannot be reclaimed is the ~3.7 KB of linked .text/.data/.bss, and
 *    that is only affordable because CONFIG_BT_CTRL_RUN_IN_FLASH_ONLY moves the
 *    controller out of IRAM — without it the static cost is ~22.6 KB, measured.
 *
 *  - No flash writes while the radio is up. With the controller running from
 *    flash, a flash *erase* stalls it, and SPI_FLASH_AUTO_SUSPEND is not
 *    available to us (IDF disqualifies this board's XMC-C part). NVS commits
 *    are not confined to the join path — pet_save() alone fires every 60 s
 *    unprompted — so main.c must consult ble_prov_nvs_blocked() before every
 *    commit and drain the deferred ones after teardown.
 *
 * Threading: every GATT callback runs on the NimBLE host task and does nothing
 * but copy bytes and raise a flag. All real work happens in ble_prov_poll(),
 * which the main loop calls. Nothing in this file touches LVGL.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Session lifecycle, as shown on the CONTROL card and pushed to the phone.
 * Numbering is wire-visible — the web page indexes a label table by it — so
 * append rather than reorder. ERR is terminal until the next start(). */
typedef enum {
    BLE_PROV_OFF = 0,      /* radio down, nothing allocated                  */
    BLE_PROV_ADV,          /* advertising, waiting for a phone               */
    BLE_PROV_LINKED,       /* phone connected, key exchange not finished     */
    BLE_PROV_AUTHED,       /* the 6-digit code checked out; channel is sealed */
    BLE_PROV_HANDOFF,      /* credentials taken; closing so Wi-Fi can start  */
    BLE_PROV_DONE,         /* handed off cleanly; radio going down           */
    BLE_PROV_ERR,          /* wrong code, too many attempts, or stack failure */
} ble_prov_state_t;

/* One scan result as the phone needs it. Deliberately mirrors main.c's
 * ap_entry_t rather than sharing it — this module must not reach into main.c's
 * statics, and a copy of 35 bytes is cheaper than the coupling. */
typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secure;
    bool   saved;    /* we hold a password for this one; offer to reuse it */
} ble_prov_ap_t;

/* ---- provided by ble_prov.c ------------------------------------------- */

/* Bring up the controller and host and start advertising. Generates a fresh
 * key pair and a fresh 6-digit code. Returns false if the stack would not come
 * up, in which case nothing is left allocated. Call from the main loop, never
 * from an LVGL callback.
 *
 * The scan list is passed in as a SNAPSHOT rather than fetched on demand,
 * because BLE and an initialised Wi-Fi cannot coexist on this board (§7g): the
 * caller scans, deinitialises Wi-Fi, then starts a session. There is no live
 * rescan — the list is whatever was on the air when pairing began. `current` is
 * the SSID the device was joined to, or "" / NULL. */
bool ble_prov_start(const ble_prov_ap_t *aps, int n, const char *current);

/* Disconnect any phone, stop advertising, and tear the stack down. Safe to
 * call when not running. Blocks briefly waiting for the host task to exit. */
void ble_prov_stop(void);

bool             ble_prov_active(void);
ble_prov_state_t ble_prov_state(void);

/* The 6-digit code to display. Valid while a session is open; "------" when
 * not. ASCII digits only — the on-screen fonts carry nothing else. */
const char *ble_prov_code(void);

/* True while a flash erase would be unsafe. main.c must gate every NVS commit
 * on this and retry once it clears. */
bool ble_prov_nvs_blocked(void);

/* Main-loop service: timeouts, deferred crypto, outbound notifications. Cheap
 * when idle. Must be called even while BLE is down so the post-session drain
 * and the advertising timeout still run. */
void ble_prov_poll(int64_t now_ms);

/* Bring BLE up and down three times, logging internal heap free before, during
 * and after each cycle. This is the gate on the whole feature: if the stack
 * does not give its memory back, every session costs a few KB permanently.
 * Blocks for ~8 s, so call it once at boot and never from a running session. */
void ble_prov_mem_probe(void);

/* ---- provided by main.c ------------------------------------------------
 * One callback, deliberately. Everything else the module needs is handed to it
 * at start(), so it never reaches into main.c's state.
 *
 * Called once from ble_prov_poll(), immediately before the radio is torn down.
 * The caller must re-initialise Wi-Fi and join only AFTER ble_prov_active()
 * reads false — the two radios cannot both be up (§7g). An empty ssid means
 * "stay off": the user asked to disconnect rather than to join something.
 *
 * The raw SSID bytes pass through untouched. They are arbitrary octets; only
 * the display path needs ASCII sanitising.
 *
 * `pass == NULL` means "join this SSID with the credentials already stored".
 * The device knows the password for the network it last joined — that is how it
 * rejoins after a reboot — so making the user retype it into a phone to get back
 * onto a network it already knows is pure friction. */
void ble_prov_submit(const char *ssid, const char *pass);

/* Discard the stored password for one network. Called mid-session, so it only
 * marks the intent — the erase is an NVS write and must wait until the radio is
 * down. Exists so a wrong saved password is escapable: without it the phone
 * offers "reconnect", the reconnect fails, and there is no route back to a
 * password field. */
void ble_prov_forget(const char *ssid);

/* Force a fresh Wi-Fi scan mid-session and refill `out`. Returns the number of
 * networks, or -1 if it was refused.
 *
 * Refusal is the normal answer on this board today, and is not an error: Wi-Fi
 * is deinitialised for the duration of a session precisely because its pools
 * (~53.5 KB) do not fit beside a running BLE controller, and a live session has
 * only ~41 KB free. The caller checks the real headroom at the moment of asking
 * rather than assuming, so this starts succeeding on its own if the Wi-Fi
 * buffer counts are ever trimmed — no code change needed to switch it on. */
int ble_prov_rescan(ble_prov_ap_t *out, int max);
