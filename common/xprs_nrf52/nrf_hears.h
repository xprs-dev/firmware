/*
 * Who an nRF52 station hears directly, on which link, and what Bluetooth may
 * hand to LoRa. Shared by models/sensecap-p1-pro and models/sensecap-t1000e:
 * lifted out of the T1000-E's main.cpp (2026-11-25) when the P1-Pro, on
 * Meshtastic's LongFast with only lr_worth to go by, was found spending most
 * of its hour of airtime re-airing other stations' history commands.
 *
 * The hears: list of a beacon (XPRS.md 15.6.3) names stations heard with no
 * via:, a neighbour one hop away. The ESP32 boards keep this in
 * xprs_station, which leans on FreeRTOS and is not ported.
 */
#ifndef XPRS_NRF_HEARS_H
#define XPRS_NRF_HEARS_H

#include <stdbool.h>
#include <stdint.h>

#include "xb_qos.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NHR_MAX      16
#define NHR_FRESH_MS 600000UL       /* "heard" means within ten minutes */

/* A peer id for the bearer's table, from the callsign (FNV-1a, never 0);
 * [direct] (may be NULL) says whether the packet came with no via:. */
uint64_t nhr_peer_of(const char *wire, int len, bool *direct);

/* Note a packet heard on Bluetooth ([ble]) or LoRa. Only a direct one
 * (no via:) from somebody else counts. */
void nhr_touch(const char *wire, int len, bool ble);

/* Heard directly on that link within NHR_FRESH_MS. */
bool nhr_on(const char *call, bool ble);

/* Heard directly, on any link, within [ms]. */
bool nhr_heard_directly(const char *call, uint32_t ms);

int  nhr_count(void);

/* The first station heard directly on Bluetooth within [ms] that [want]
 * accepts (an owner's phone, say), or NULL. */
const char *nhr_find_ble(uint32_t ms, bool (*want)(const char *call));

/* " hears:A,B,..." most recent first, so a truncated list keeps the useful
 * half; 0 and nothing written when nobody is fresh. */
int  nhr_render(char *out, int cap);

/*
 * What Bluetooth (or a LoRa digipeat) may offer a shared LoRa channel: what
 * is worth LoRa (lr_worth) and not scope:local. The rest -- reached on
 * Bluetooth already, a transaction for nobody on LoRa, repeats, old news,
 * twice in half an hour, the hour's budget -- is the LoRa bearer's QoS.md
 * gate (xb_qos.h), fed by nhr_qos_hooks.
 */
bool nhr_to_lora(const char *wire, int len);

/* The station's answers for the LoRa bearer's QoS gate: heard directly on
 * LoRa (heard_here), on Bluetooth (reached_cheaper), and the clock. */
void nhr_qos_hooks(xq_hooks_t *h);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_NRF_HEARS_H */
