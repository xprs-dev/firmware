/*
 * xprs_gossip -- who has been heard where, and by whom (XPRS.md 36.9.4).
 *
 * A port of the Flutter station's XprsGossip (xprs_gossip.dart), deliberately
 * the same SHAPE rather than a second design: the two implementations answer
 * the same question for the same network, and a board that ranked sightings
 * differently from a phone would route mail differently from it.
 *
 * Three layers, of which this component owns two:
 *
 *   L1  the recipient's own declaration -- not here. It lives in the archive
 *       (a t:mailbox record) and outranks everything below.
 *   L2  VISITS: a callsign was heard by a gateway, on a short-range radio.
 *       Never expires. Radio truth only: the internet bearer never writes
 *       this layer, because "X was heard via a hub" says nothing about where
 *       X is.
 *   L3  LIVE sightings: the same pairing with a TTL, including the lanes L2
 *       will not take. Fresh, and allowed to be wrong tomorrow.
 *
 * And three walls, which are the whole reason gossip can be believed:
 *
 *   signer-credited  an unsigned or unverifiable hears: claim feeds nothing.
 *                    The one exception is this station's OWN radio, which is
 *                    its own witness and needs nobody's signature.
 *   per-signer quota one observer's gossip is admitted at the rate its own
 *                    adverts arrive, so a chatty neighbour cannot flood the
 *                    table with its view of the world.
 *   byte budget      the store is bounded, and L3 pays before L2 does: the
 *                    visit history is the layer allowed to live forever.
 *
 * WHERE IT LIVES. On the card, in buckets: a callsign hashes to one file, so
 * a lookup reads a fraction of the store instead of all of it. RAM holds only
 * the quota meters -- this runs on boards whose free heap is measured in
 * kilobytes, and a table sized for a super would not fit in it.
 *
 * THREADING. xgossip_note_* are safe to call from a receive path: they do a
 * RAM check and queue. Nothing touches the card until xgossip_pump() is
 * called, which the owner task does on its own tick (docs/esp32.md: never
 * write from the task that heard the packet).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One place a callsign was seen. */
typedef struct {
    char     call[10];
    char     gw[10];
    char     bearer[8];
    uint32_t ts;                 /* epoch seconds, or uptime when unsynced */
    uint8_t  layer;              /* 2 = visit, 3 = live */
} xgossip_sighting_t;

/* Reference constants of 36.9.4, matching xprs_gossip.dart where the board
 * can afford to. K is the one that differs: the phone keeps 100 gateways per
 * callsign in sqlite, and an ordinary board keeping that many would spend its
 * card on one busy neighbour. */
#define XGOSSIP_LIVE_G          8        /* gateways per callsign, L3 */
#define XGOSSIP_VISIT_K         8        /* gateways per callsign, L2 */
/* What a super keeps. The phone keeps 100 in sqlite; a board keeps 32,
 * because every row a callsign can have has to fit in the working array the
 * store uses instead of loading a bucket into RAM. Four times an ordinary
 * station is still the difference the role is about. */
#define XGOSSIP_VISIT_K_SUPER  32
#define XGOSSIP_LIVE_TTL_SEC  (24 * 3600)
#define XGOSSIP_SIGNER_SEC     30        /* the fastest beacon cadence */
#define XGOSSIP_DIRECT_SEC     60        /* debounce for our own hearings */
#define XGOSSIP_MAX_BYTES     (256u * 1024u)
#define XGOSSIP_MAX_BYTES_SUPER (16u * 1024u * 1024u)

typedef struct xgossip_s xgossip_t;

/** Open (or create) the store under @p dir. NULL if the card will not take it. */
xgossip_t *xgossip_open(const char *dir);
void       xgossip_close(xgossip_t *g);

/**
 * @brief Claim the super-archiver's gossip duties (36.9.4).
 *
 * Two things change. The byte budget and the per-callsign visit ring grow --
 * a super is asked about callsigns nobody else remembers. And the need-to-know
 * admission goes away: an ordinary station keeps gossip in proportion to its
 * duties, which means callsigns it has heard itself or already knows of, while
 * a super keeps "every active callsign it can learn of" because that is what
 * the humble stations are going to ask it about.
 */
void xgossip_set_super(xgossip_t *g, bool super);

/**
 * @brief This station heard @p call itself, direct, no `via:`.
 *
 * The one feed that needs no signature: our own radio is its own witness.
 * Debounced in RAM per (callsign, bearer) -- every beacon from every
 * neighbour arrives here, and the answer does not change by the beacon.
 */
void xgossip_note_direct(xgossip_t *g, const char *call, const char *self,
                         const char *bearer, uint32_t now_s);

/**
 * @brief Would a hears: claim from @p observer be admitted right now?
 *
 * One RAM lookup, no mutation, no card. Call it BEFORE paying for the
 * signature verify: a station beacons its observation every few seconds, the
 * quota admits one per interval anyway, and verifying the ones the quota is
 * about to refuse is a curve operation bought for nothing.
 */
bool xgossip_would_accept(xgossip_t *g, const char *observer, uint32_t now_s);

/**
 * @brief A verified observation from @p observer listing what it hears.
 *
 * @p verified is the packet's signature verdict. False feeds nothing: an
 * unsigned claim about who is where is exactly what an attacker would send.
 * @p link is the bearer the observer says it heard them on (10.6.1); only a
 * short-range one writes L2.
 */
void xgossip_note_hears(xgossip_t *g, const char *observer,
                        const char *const *hears, int n_hears,
                        const char *link, bool verified, uint32_t now_s);

/** Do the queued card work. Owner task only; call it on an existing tick. */
void xgossip_pump(xgossip_t *g);

/**
 * @brief Where is @p call? Freshest first, L3 before L2.
 * @return sightings written (<= @p max).
 */
int xgossip_where_is(xgossip_t *g, const char *call,
                     xgossip_sighting_t *out, int max);

/**
 * @brief Gateways worth naming in a 404's `m:try` (36.9). Never names @p self.
 * @return bytes written to @p out, 0 when gossip knows nobody.
 */
int xgossip_try_candidates(xgossip_t *g, const char *call, const char *self,
                           char *out, int cap);

typedef struct {
    uint32_t accepted;
    uint32_t refused_unsigned;
    uint32_t refused_quota;
    uint32_t refused_need;       /* need-to-know: not a callsign we serve */
    uint32_t rows;
    uint32_t bytes;
    uint32_t queued;
    uint32_t dropped;
} xgossip_stats_t;

void xgossip_stats(xgossip_t *g, xgossip_stats_t *out);

#ifdef __cplusplus
}
#endif
