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
 * Ordering matters only in that ERR is terminal until the next start(). */
typedef enum {
    BLE_PROV_OFF = 0,      /* radio down, nothing allocated                  */
    BLE_PROV_ADV,          /* advertising, waiting for a phone               */
    BLE_PROV_LINKED,       /* phone connected, key exchange not finished     */
    BLE_PROV_AUTHED,       /* the 6-digit code checked out; channel is sealed */
    BLE_PROV_JOINING,      /* credentials handed to Wi-Fi, awaiting result   */
    BLE_PROV_DONE,         /* joined; session winding down                   */
    BLE_PROV_ERR,          /* wrong code, too many attempts, or stack failure */
} ble_prov_state_t;

/* One scan result as the phone needs it. Deliberately mirrors main.c's
 * ap_entry_t rather than sharing it — this module must not reach into main.c's
 * statics, and a copy of 35 bytes is cheaper than the coupling. */
typedef struct {
    char   ssid[33];
    int8_t rssi;
    bool   secure;
} ble_prov_ap_t;

/* ---- provided by ble_prov.c ------------------------------------------- */

/* Bring up the controller and host and start advertising. Generates a fresh
 * key pair and a fresh 6-digit code. Returns false if the stack would not
 * come up, in which case nothing is left allocated. Call from the main loop,
 * never from an LVGL callback. */
bool ble_prov_start(void);

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

/* main.c reports the outcome of the join it was asked to perform. */
void ble_prov_join_result(bool joined);

/* main.c reports that a scan requested through ble_prov_request_scan() has
 * finished and s_aps[] is fresh. */
void ble_prov_scan_ready(void);

/* ---- provided by main.c ------------------------------------------------ */

/* Copy up to max entries from the live scan list; returns how many. */
int  ble_prov_get_aps(ble_prov_ap_t *out, int max);

/* Ask the main loop for a fresh scan. Asynchronous — ble_prov_scan_ready()
 * comes back when it lands. */
void ble_prov_request_scan(void);

/* Hand credentials to the main loop to join with. The raw SSID bytes are
 * passed through untouched: they are arbitrary octets and only the *display*
 * path needs ASCII sanitising. */
void ble_prov_submit(const char *ssid, const char *pass);

/* Ask the main loop to drop the current association. */
void ble_prov_request_wifi_off(void);

/* The currently joined SSID, or "" when not associated. */
const char *ble_prov_current_ssid(void);
