/**
 * @file xfw.h
 * @brief Firmware from the publisher's folder, over Reticulum, unattended.
 *
 * The loop (docs/plan-firmware-folders.md), each step the format's own:
 *
 *   1. ASK      `cmd:history kind:file only:<publisher>` to an archiver
 *               (XPRS.md 12.6): the publisher's signed `t:file`
 *               descriptions come back as ordinary heard traffic.
 *   2. CHOOSE   a description signed by the key this station pins as
 *               `fwkey`, `name:<board>-<version>.bin`, `tag:<channel>`,
 *               newer than what runs, not a digest that rolled back before
 *               (common/xprs_folder decides the name and the order).
 *   3. FETCH    `cmd:file file:<ref>` to the archiver (11.2); on its 202,
 *               a link to its `xprs/files` destination, GET_FILE, and the
 *               image as a resource streamed into the OTA slot
 *               (common/xprs_rns: rns_link, rns_resource).
 *   4. INSTALL  the digest of what arrived against the description, then
 *               xprs_ota's verify and commit; the new image reports itself
 *               on its beacon (`fw:`) and 11.8's rollback stands behind it.
 *
 * Policy: `fwauto` on, `fwchan` stable or beta, never while an install or a
 * replay is under way, one attempt per digest per six hours, the first ask
 * two minutes after boot and then every six hours, jittered.
 *
 * Every outside effect is a callback, so the state machine runs on a host
 * (test_xfw_host.c) with no radio, no flash and no clock. On the device
 * xprs_app wires the callbacks to what it already has: its signer, its
 * archiver list, the Reticulum bearer, xprs_ota.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "rns.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XPRS_CALL_LEN_MAX 12

typedef struct {
    const char *board;                              /* "tdeck" */
    const char *self;                               /* this station's callsign */
    const char *(*running_version)(void);
    bool (*publisher_key)(uint8_t out[32]);         /* fwkey, x-only */
    const char *(*channel)(void);                   /* "stable" | "beta" */
    bool (*enabled)(void);                          /* fwauto */
    uint32_t (*now_s)(void);                        /* a monotonic clock */
    uint32_t (*epoch_s)(void);                      /* 0 when unknown */
    bool (*busy)(void);                             /* an install or a replay runs */
    /** An archiver this station can address over Reticulum, or false. */
    bool (*pick_archiver)(char *out, size_t cap);
    /** Sign a wire in place; returns the new length (xprs_app's sign_wire). */
    int (*sign)(char *wire, int len, int cap);
    /** Send one wire to one callsign over Reticulum. */
    bool (*send_to)(const char *call, const char *wire, int len);
    /** The peer's `xprs/files` route: destination, full public key, next hop. */
    bool (*peer_route)(const char *call, uint8_t dest[RNS_HASH_LEN],
                       uint8_t pub[RNS_PUB_LEN], uint8_t next_hop[RNS_HASH_LEN],
                       bool *have_hop);
    /** Put one raw Reticulum packet on the uplink. */
    bool (*send_packet)(const uint8_t *pkt, size_t len);
    /** The image sink: xprs_ota's push door, by digest. */
    bool (*sink_begin)(const char *version, size_t size, const uint8_t sha[32]);
    bool (*sink_write)(const uint8_t *data, size_t len);
    bool (*sink_finish)(void);                      /* verify + commit; restarts */
    void (*sink_abort)(void);
    /** Serialise the two tasks that touch this state (NULL on a host). */
    void (*lock)(void);
    void (*unlock)(void);
    void (*say)(const char *line);                  /* one log line */
} xfw_cfg_t;

void xfw_start(const xfw_cfg_t *cfg);

/** Every heard wire, from any task: cheap unless it is a `t:file` from the
 *  publisher or a `t:result` for an ask of ours, which are parked for
 *  xfw_poll(). */
void xfw_on_wire(const char *wire, int len);

/** A packet addressed to a link (dest LINK) or an LRPROOF, from the uplink's
 *  task. Ignored unless a fetch is open. */
void xfw_on_link_packet(const rns_packet_t *p);

/** The cadence: asks, timeouts, retries, the start of an install. Call every
 *  second or so from the task that may afford the signing and the flash. */
void xfw_poll(void);

/** One line for /api/status and the log. */
const char *xfw_state(char *buf, size_t cap);

/* The publisher's derived callsign for a key: X1 and four npub characters. */
void xfw_publisher_call(const uint8_t key[32], char out[XPRS_CALL_LEN_MAX]);

#ifdef __cplusplus
}
#endif
