/**
 * @file rns_serve.h
 * @brief Requests over Reticulum links to this station: the responder side.
 *
 * What lets a station answer its visitor API over Reticulum (spec/API-HTTP.md
 * "The same API over a Reticulum link"): it accepts links to one of its own
 * destinations, takes RNS requests on them and sends the responses back,
 * mirrored from reticulum-dart's RnsRequestServer (rns_request.dart) so the
 * two agree byte for byte:
 *
 *   LINKREQUEST -> a responder link (rns_link_respond), its LRPROOF sent;
 *   LRRTT       -> the link is active; KEEPALIVE answered; LINKCLOSE ends it.
 *   REQUEST     -> msgpack([time, path_hash(16), data]); its id is the
 *                  truncated hash of the packed request. A request too large
 *                  for one packet arrives as a Resource and is gathered here
 *                  up to a cap; past it the host is told, with the id, so it
 *                  can answer 413.
 *   RESPONSE    <- msgpack([request_id, data]) in one packet when it fits 380
 *                  bytes, else as a Resource this file sends: advertised,
 *                  parts served as the receiver asks for them, the hashmap in
 *                  batches, the proof checked, one segment of up to 1 MB at a
 *                  time. One Resource each way per link; the rest wait. The
 *                  advertisement goes again every 5 s while nothing asks
 *                  (4 times), and an answer whose receiver goes quiet for
 *                  30 s is dropped, so none is held until the link idles.
 *
 * The sender holds no copy of what it sends. A response is a source the host
 * can read at any offset (RAM, or a file on a board without the room), and
 * the ciphertext is made again for each part asked for, from the CBC block
 * before it: per part, 4 bytes of map hash and 16 of chaining are kept, about
 * 2.9 KB for a 64 KB answer.
 *
 * Pure: no tasks, no clock, no sockets. The host feeds packets, calls tick
 * (every few seconds while links are open),
 * sends what comes back through cfg.send, runs requests where it may block,
 * and answers with rns_serve_respond. All calls must be serialised (one lock).
 */
#pragma once
#include "rns_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The largest request or response that travels as one packet. */
#define RNS_SERVE_INLINE_MAX 380
/** The most links any host may ask for. */
#define RNS_SERVE_LINKS_MAX  8

/** Something a response is read from, at any offset, any number of times. */
typedef struct rns_src {
    size_t len;
    bool (*read)(struct rns_src *s, size_t off, uint8_t *buf, size_t n);
    /** Done with it: free what it holds (called exactly once). */
    void (*release)(struct rns_src *s);
    void *ctx;
} rns_src_t;

typedef struct {
    /** One packed packet onto the uplink. */
    void (*send)(const uint8_t *pkt, size_t len, void *ctx);
    /**
     * A whole request arrived on link [slot] (generation [gen]): [packed] is
     * the msgpack request and [rid] its id, both valid only for the call.
     * [too_large]: it came as a Resource past [upload_max]; [packed] is
     * NULL and only the id is known. Answer later with rns_serve_respond.
     */
    void (*request)(int slot, uint32_t gen, const uint8_t link_id[RNS_HASH_LEN],
                    const uint8_t rid[RNS_HASH_LEN], const uint8_t *packed,
                    size_t len, bool too_large, void *ctx);
    void *ctx;
    const rns_identity_t *id;          /* ours, with the private half */
    uint8_t dest[RNS_HASH_LEN];        /* the destination links are opened to */
    int max_links;                     /* more are refused silently */
    uint32_t idle_s;                   /* a link quiet this long is closed */
    size_t upload_max;                 /* the largest request taken as a Resource */
    uint16_t mtu_cap;                  /* what the uplink carries: 500 */
} rns_serve_cfg_t;

/** Start (again): every link forgotten. [cfg] is copied. */
void rns_serve_init(const rns_serve_cfg_t *cfg);

/** One packet off the uplink. True when it was this server's (a link
 *  request to our destination, or anything on one of our links). */
bool rns_serve_feed(const rns_packet_t *p, uint32_t now_s);

/** Close the links that have been quiet past the idle time. */
void rns_serve_tick(uint32_t now_s);

/**
 * Answer request [rid] on link [slot]/[gen] with [data] (the msgpack of the
 * response's data; the server wraps it as [rid, data]). [data] is taken
 * whatever happens: released when sent, or at once when the link is gone.
 */
void rns_serve_respond(int slot, uint32_t gen, const uint8_t rid[RNS_HASH_LEN],
                       rns_src_t *data);

/** Links open (pending or active). */
int rns_serve_links(void);

/** Bytes this server holds for link [slot] now (state, an upload, a sender's
 *  tables): what a log line reports. 0 for a free slot. */
size_t rns_serve_link_bytes(int slot);

#ifdef __cplusplus
}
#endif
