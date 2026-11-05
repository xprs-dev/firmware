/**
 * @file rns_resource.h
 * @brief An RNS Resource, receiver side, streamed (wire-compatible, 1.3.5).
 *
 * The bytes behind a `cmd:file` (XPRS.md 11.2) arrive over a link as a
 * resource: one or more SEGMENTS of up to 1 MB, each advertised, pulled in
 * PARTS under a window the receiver controls, verified by hash, and proved
 * back. Mirrored from reticulum-dart's rns_resource_receiver.dart, with one
 * difference that is the whole reason this file exists: nothing here holds
 * a segment. A phone assembles the resource in memory; a station with a
 * few kilobytes free cannot, so the stream is decrypted and hashed AS IT
 * ARRIVES and handed to a sink, the OTA slot or a file on the card.
 *
 * How that is possible: a segment is one RNS token, iv(16) + AES-CBC
 * ciphertext + HMAC(32), split into parts in order. The advertisement says
 * the stream's length, so the HMAC's position is known before a byte comes;
 * CBC decrypts block by block; the HMAC, the segment hash and the proof are
 * all running digests. Parts may arrive out of order within the window, so
 * a small cache holds the ones ahead of the frontier, and the window is
 * never wider than the cache. A failed check is reported at the END of the
 * segment, so the sink must be able to throw away what it took: an OTA slot
 * that is not yet committed, a file not yet renamed.
 *
 * Pure: no radio, no clock. The caller feeds packets addressed to the link,
 * sends what comes back, calls rns_res_retry() on a stall, and watches
 * rns_res_done().
 */
#pragma once
#include "rns_link.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef RNS_RES_CACHE_PARTS
/* Parts held ahead of the frontier (and the widest window). 16 parts of a
 * 464-byte SDU is about 7.5 KB; a board without PSRAM builds with 8 and
 * still streams faster than its hub path delivers. */
#define RNS_RES_CACHE_PARTS 16
#endif
#define RNS_RES_SDU_MAX   464
#define RNS_RES_BATCH      74          /* map hashes per request / HMU */

typedef struct rns_res rns_res_t;

typedef struct {
    /** Plaintext of the resource, in order. Return false to abort. */
    bool (*write)(const uint8_t *data, size_t len, void *ctx);
    /** The transfer finished: ok with the full payload delivered, or not
     *  (the sink discards what it took). [why] names the failure. */
    void (*done)(bool ok, const char *why, void *ctx);
    void *ctx;
} rns_res_sink_t;

/** Opaque receiver state. Allocate with rns_res_new(), free with
 *  rns_res_free(): one block of about 2 KB (a ring of RNS_RES_RING part
 *  hashes: the map is never held whole, only the hashes between the
 *  frontier and the known horizon matter), plus up to RNS_RES_CACHE_PARTS
 *  part slots of 472 bytes each, taken one at a time as parts arrive out
 *  of order and kept until the free. Small blocks, so it fits the
 *  fragmented heap of a board without PSRAM; a slot the heap refuses only
 *  narrows the window. */
rns_res_t *rns_res_new(const rns_link_t *link, const rns_res_sink_t *sink);
void rns_res_free(rns_res_t *r);

/**
 * Feed one packet addressed to the link (resource contexts; others are
 * ignored). Packets to send back are produced through [send], one at a
 * time, each a packed packet. Returns false when the receiver has failed
 * (the sink's done() has been called).
 */
bool rns_res_feed(rns_res_t *r, const rns_packet_t *p,
                  void (*send)(const uint8_t *pkt, size_t len, void *ctx),
                  void *send_ctx);

/** A stall: shrink the window and ask again for what is missing. */
void rns_res_retry(rns_res_t *r,
                   void (*send)(const uint8_t *pkt, size_t len, void *ctx),
                   void *send_ctx);

bool rns_res_done(const rns_res_t *r);
bool rns_res_failed(const rns_res_t *r);
/** Total payload size from the first advertisement (0 before it). */
uint32_t rns_res_total(const rns_res_t *r);
/** Bytes handed to the sink so far. */
uint32_t rns_res_received(const rns_res_t *r);
/** A line for a log: segment, parts, window. */
const char *rns_res_state(const rns_res_t *r, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
