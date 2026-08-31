/**
 * @file xprsble.h
 * @brief XPRS over BLE5 extended advertising — the off-grid bearer.
 *
 * Connectionless: no pairing, no association, no access point. One extended
 * advertising AD carries the whole packet, so an XPRS wire is aired verbatim
 * and nothing is ever fragmented. `link:ble` is an assigned bearer word
 * (`docs/XPRS.md` §10.6.1) and `docs/ble5.md` is this bearer's page.
 *
 * ── Which chips ─────────────────────────────────────────────────────────────
 *
 * **S3 / C3 class only.** Extended advertising is what makes an AD of up to
 * 254 bytes possible; the original ESP32 has legacy 31-byte advertising and can
 * never carry an XPRS packet, whose beacons alone run to 112–173 bytes
 * (`docs/esp32.md`, "Radio capability per chip"). A board without
 * `CONFIG_SOC_BLE_50_SUPPORTED` must not declare this bearer.
 *
 * ── Framing ─────────────────────────────────────────────────────────────────
 *
 * Manufacturer-specific data under company id `0xFFFF`, marker `0x3E`, then a
 * one-byte subtype (`docs/ble5.md` §2). XPRS text is `0x58`. The other subtypes
 * on that air — Reticulum `0x55`, the compact APRS frame `0x41`, the route
 * beacon `0x4D` — belong to other software on the same radio, so the receive
 * hook reports the subtype and lets the caller decide rather than assuming
 * every frame is ours.
 *
 * ── Where the work happens ──────────────────────────────────────────────────
 *
 * Scan results arrive on the NimBLE **host task**, beside the controller, on
 * the processor `docs/esp32.md` spends its length defending. Deriving a §5
 * identifier is a SHA-256 and writing the archive is an SD transaction; neither
 * may happen there. The receive hook is therefore expected to copy and return,
 * exactly as the ESP-NOW bearer's does.
 */

#ifndef XPRS_BEARER_BLE_H
#define XPRS_BEARER_BLE_H

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One extended-advertising AD, less the six bytes of envelope. */
#define XPRSBLE_WIRE_MAX   248

/** Subtypes on this air (docs/ble5.md §2). */
#define XPRSBLE_SUB_RNS    0x55
#define XPRSBLE_SUB_APRS   0x41
#define XPRSBLE_SUB_MESH   0x4D
#define XPRSBLE_SUB_XPRS   0x58

/**
 * @brief One frame heard, after the company id and marker matched.
 * @param subtype the byte after the marker; see XPRSBLE_SUB_*
 * @param payload valid only during the call
 * @param rssi    dBm, as the radio measured it
 */
typedef void (*xprsble_rx_cb_t)(uint8_t subtype, const uint8_t *payload,
                                int len, int rssi);

/**
 * @brief Bring the radio up: controller, NimBLE host, scan, ext-adv instance 0.
 *
 * Returns once the host task is started. The radio is not yet usable at that
 * point — the controller syncs asynchronously and [xprsble_is_active] turns
 * true when it has. Airing before then is refused rather than queued, because a
 * frame aired into an unsynced host is lost silently and that is the failure
 * mode this whole file exists to make visible.
 *
 * @param callsign this station. Copied; used only for logging here.
 */
esp_err_t xprsble_start(const char *callsign);

/** True once the controller has synced and instance 0 may be driven. */
bool xprsble_is_active(void);

/** Register the receive hook. Called on the NimBLE host task: copy and return. */
void xprsble_set_rx_cb(xprsble_rx_cb_t cb);

/**
 * @brief Put one XPRS wire on air under subtype `0x58`.
 *
 * Replaces whatever instance 0 was advertising. There is one advertising set
 * and it is time-shared, so a caller with several frames to air rotates them
 * itself — `docs/ble5.md` §1 explains why the radio is a window and not a
 * state.
 *
 * @return false when the radio is not up, or the frame does not fit one AD.
 */
bool xprsble_send(const char *wire, int len);

/**
 * Offer a packet heard elsewhere for re-airing ON BLUETOOTH (§13.1: a relay
 * "repeats a packet on the medium it heard it, within the hop budget,
 * appending itself to `via:`").
 *
 * Queued with the §13.2.1 random wait and dropped if the packet is heard from
 * somebody else meanwhile; `via:` and the hop budget are xprs_codec's decision,
 * not this bearer's. Without this a Bluetooth-only station can be reached from
 * the wired bearers but two of them out of each other's range cannot reach one
 * another, however many stations sit in between.
 *
 * A wire that no longer fits an advert once `via:` is appended is not aired:
 * XPRS packets run to 250 bytes and an advert holds XPRSBLE_WIRE_MAX.
 */
void xprsble_digipeat(const char *wire, int len);

/* Air a packet we already aired, verbatim: the idle-channel echo
 * (xb_echo). */
void xprsble_echo(const char *wire, int len);

/* Milliseconds since anything was heard or aired here. */
uint32_t xprsble_idle_ms(uint32_t now_ms);

/**
 * Offer a packet heard on ANOTHER bearer to this one: the bridge leg.
 *
 * Same queue, jitter and cancel as every other bearer's offer, and the same
 * refusals -- already in via:, hop budget spent, heard here already. It is
 * the return half of the BLE<->LoRa bridge: without it a packet that came a
 * kilometre over LoRa reached the LAN and ESP-NOW but never the phones
 * standing beside the station.
 */
void xprsble_offer(const char *wire, int len);

/** As [xprsble_send], for a caller that owns a subtype of its own. */
bool xprsble_send_sub(const uint8_t *payload, int len, uint8_t subtype);

/** Adverts seen since boot, before any filtering: "deaf" told from "alone". */
uint32_t xprsble_scan_results(void);

/** Seconds since the last scan result, or -1 if there has never been one. */
int xprsble_silent_for(void);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_BEARER_BLE_H */
