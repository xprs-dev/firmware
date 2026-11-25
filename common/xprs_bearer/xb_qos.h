/**
 * @file xb_qos.h
 * @brief Sharing a constrained bearer's airtime (spec QoS.md).
 *
 * ../../../spec/QoS.md is the rulebook and xb_qos.c implements it; where the
 * two disagree the document wins and the code is the bug. Section numbers
 * here are QoS.md's unless they say XPRS.md. The Dart twin is
 * core/lib/src/services/xprs/xprs_qos.dart, and the two are kept rule for
 * rule: the same classes, the same order, the same refusals, checked by the
 * same replay of the same bench trace (test_xbqos_replay_host.sh here,
 * test/xprs_qos_replay_test.dart there).
 *
 *   4.1  one budget per bearer, a rolling hour, every transmission charged
 *        (the bearer's own duty ledger, xb_duty_t: Meshtastic frames and
 *        XPRS packets spend the same hour);
 *   4.2  a tenth reserved: third parties stop at 90%, the owner at 98%,
 *        sos/warning may spend all of it;
 *   4.3  airing order: sos/warning, owner, first:, urg:, oldest ts:;
 *   4.4  admission by class, and the refusals: scope, reached on a cheaper
 *        bearer, a transaction for nobody heard here, aired in the last 30
 *        minutes, old news without spare capacity;
 *   4.5  a periodic packet no more often than the bearer's repeat interval,
 *        per author and type (5 min short bearers, 30 LoRa, 60 crowded);
 *   4.6  max-min fair share among third-party authors;
 *   4.7  at most a tenth of the budget in any six minutes for L2-L6, half
 *        of that for keys, presence and bulk;
 *   4.8  expiry per class, eviction lowest class first.
 *
 * ── Shape ───────────────────────────────────────────────────────────────────
 *
 * Decisions on injected facts: no radio, no clock of its own, no heap. The
 * bearer (xprsbearer.c) asks it at the doors -- offer, digipeat, echo, its
 * own send, xb_spend -- and its pump airs in 4.3 order. Every table is a
 * fixed array; an xq_t is about 1.7 KB, caller-owned like xb_duty_t, and
 * attached only to the bearers that need it (a LoRa radio, a VHF port).
 *
 * Where the Dart side keeps exact lists, this side keeps buckets and rounds
 * the safe way: the six-minute pace and the per-author hour count a whole
 * bucket that has only partly left the window, so a limit can bind a few
 * seconds early and never late.
 */
#ifndef XB_QOS_H
#define XB_QOS_H

#include <stdbool.h>
#include <stdint.h>

#include "xprs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The admission classes of 4.4, highest first; the value is the eviction
 *  order (a higher value goes first). */
typedef enum {
    XQ_LIFE = 0,     /* L0 t:sos, t:warning */
    XQ_OWNER,        /* L1 this station's own */
    XQ_CONV,         /* L2 message, status, receipt, reaction, DM */
    XQ_TXN,          /* L3 command, result, request */
    XQ_KEYS,         /* L4 identity */
    XQ_PRESENCE,     /* L5 observation, service, track */
    XQ_BULK,         /* L6 mailbox, old ts: */
    XQ_CLASS_N
} xq_class_t;

/** Why a packet was not carried (section 5), or XQ_OK. */
typedef enum {
    XQ_OK = 0,
    XQ_SCOPE,        /* scope:local, or a scope word nobody knows (9.11) */
    XQ_REPEAT,       /* a periodic packet sooner than the interval (4.5) */
    XQ_REACHED,      /* for somebody heard on a cheaper bearer (4.4) */
    XQ_NOT_HERE,     /* a transaction for somebody not heard here (4.4) */
    XQ_OLD_NEWS,     /* old ts: and no spare capacity (4.4, 4.7) */
    XQ_AIRED,        /* aired here in the last 30 minutes (4.4) */
    XQ_FAIR,         /* its author is over the fair share (4.6) */
    XQ_BUDGET,       /* the hour, the reserve or the pace (4.2, 4.7) */
    XQ_EXPIRED,      /* waited past its class's limit (4.8) */
    XQ_EVICTED,      /* pushed out of a full queue (4.8) */
    XQ_SHED_N
} xq_shed_t;

/** The numbers for one kind of bearer (section 6). The budget itself is
 *  the bearer's duty ledger's, passed in xq_ctx_t: a region change moves it
 *  and the rules stay. Shares are per mille. */
typedef struct {
    const char *name;
    uint16_t repeat_s;          /* 4.5 ordinary */
    uint16_t repeat_crowded_s;  /* 4.5 crowded */
    uint8_t  crowded_stations;  /* heard directly in ten minutes */
    uint8_t  crowded_util_pct;  /* channel utilisation above this */
    uint16_t fresh_txn_s;       /* 4.4: older than this is old news */
    uint16_t fresh_bulk_s;
    bool     bulk_on;           /* old news ever carried */
    uint8_t  fair_min_div;      /* 4.6 */
    uint16_t reserve_pm;        /* 4.2: 100 = third parties stop at 90% */
    uint16_t life_pm;           /* 4.2: 20 = the owner stops at 98% */
    uint16_t pace_pm;           /* 4.7: 100 = a tenth in six minutes */
    uint16_t bulk_min_free_pm;  /* 4.7: bulk only while 600 is unspent */
    uint16_t background_pm;     /* 4.7: keys/presence/bulk get 500 of pace */
} xq_profile_t;

extern const xq_profile_t XQ_PROFILE_SHORT;     /* ble, lan, espnow: 5 min */
extern const xq_profile_t XQ_PROFILE_LORA;      /* 10% band: 30/60 min */
extern const xq_profile_t XQ_PROFILE_LORA_1PCT; /* 1% band: 60/120, no bulk */
extern const xq_profile_t XQ_PROFILE_APRS;      /* 1200 bd: 30/60, no bulk */

/** What the station knows, asked only when a packet needs it. Any may be
 *  NULL. heard_here NULL is "cannot say", and a transaction is then
 *  admitted rather than refused on no evidence. */
typedef struct {
    /* [call] (base, upper case) heard directly on THIS bearer lately. */
    bool (*heard_here)(const char *call);
    /* [call] heard directly on a CHEAPER bearer lately: the packet for it
     * reached it already. */
    bool (*reached_cheaper)(const char *call);
    /* UTC seconds, 0 when the station has no clock. */
    uint32_t (*epoch_s)(void);
    /* Channel utilisation in percent, everybody's, 0 when unknown. */
    uint8_t (*util_pct)(void);
} xq_hooks_t;

/** What the caller knows about one packet right now. */
typedef struct {
    uint32_t now_ms;
    uint32_t epoch_s;       /* 0 = no clock: "now" is the newest ts: heard */
    uint32_t spent_ms;      /* the bearer's hour so far */
    uint32_t budget_ms;     /* its budget; 0 = unmetered */
    bool     own;           /* signed by this station */
    bool     first;         /* its author is in the policy's first: */
    uint8_t  stations;      /* heard directly here in ten minutes */
    uint8_t  util_pct;
} xq_ctx_t;

typedef struct {
    xq_class_t cls;
    xq_shed_t  shed;
} xq_decision_t;

/* Table sizes. A LoRa channel airs a few hundred packets an hour at most,
 * and 4.4's half hour of aired identifiers is the largest of these. */
#define XQ_AIRED_N    64
#define XQ_REPEAT_N   32
#define XQ_AUTHORS_N  16
#define XQ_PACE_N     37        /* 10 s buckets: six minutes and the one filling */
#define XQ_PACE_MS    10000u
#define XQ_AUTH_N     7         /* 10 min buckets: the hour and the one filling */
#define XQ_AUTH_MS    600000u

typedef struct {
    uint32_t author;            /* FNV-1a of the base callsign, never 0 */
    uint16_t b[XQ_AUTH_N];      /* third-party ms aired, per ten minutes */
} xq_author_t;

typedef struct {
    uint32_t author;
    uint32_t k;                 /* hash of k:, identity only */
    uint32_t t_ms;
    uint8_t  type;              /* 1..4, 0 = free */
} xq_repeat_t;

typedef struct {
    uint32_t id;                /* the 5 identifier, 24 bits; 0 = free */
    uint32_t t_ms;
} xq_aired_t;

typedef struct {
    uint32_t offered, admitted, aired;
    uint16_t shed[XQ_SHED_N];
} xq_count_t;

typedef struct xq {
    const xq_profile_t *p;
    xprs_reach_t reach;         /* this bearer's 9.11 class */
    xq_hooks_t   hooks;
    uint16_t     pace[XQ_PACE_N];
    uint32_t     pace_head_ms;
    uint8_t      pace_head;
    xq_author_t  authors[XQ_AUTHORS_N];
    uint32_t     auth_head_ms;
    uint8_t      auth_head;
    xq_repeat_t  repeat[XQ_REPEAT_N];
    xq_aired_t   aired[XQ_AIRED_N];
    uint8_t      aired_pos;
    uint32_t     newest_ts, newest_ts_at_ms;
    int32_t      reserve_low_ms;    /* -1 until something is aired */
    uint32_t     frames, frames_ms; /* frames that are not XPRS packets
                                       (Meshtastic relays via xb_spend) */
    xq_count_t   count[XQ_CLASS_N];
} xq_t;

void xq_init(xq_t *q, const xq_profile_t *p, xprs_reach_t reach);
void xq_set_hooks(xq_t *q, const xq_hooks_t *h);
void xq_reset(xq_t *q);

/** Seconds since the epoch of a `YYYY-MM-DD_hh:mm:ss` value, 0 if not one. */
uint32_t xq_ts_epoch(const char *v, int vl);

/** The value of [key] on a wire, before m:; NULL when absent. */
const char *xq_field(const char *wire, int len, const char *key, int *vl);

/** FNV-1a of a callsign's base (before '-'), upper case; 0 for none. */
uint32_t xq_call_hash(const char *call, int len);

/** 4.4: the class of a wire, by its type and [c]. */
xq_class_t xq_classify(const char *wire, int len, const xq_ctx_t *c);

/** 9.5 rank for 4.3, lower first; a stranger's urgent counts as high. */
uint8_t xq_urg_rank(const char *wire, int len, bool stranger);

/** A heard packet is evidence of the time whether or not it is carried. */
void xq_note(xq_t *q, const char *wire, int len, uint32_t now_ms);

/** 4.4 and 4.5: may this wire enter the bearer at all? [id] is its 5
 *  identifier when the caller has it (NULL: derived here, a SHA-256).
 *  [count] false for a look that is not an offer. */
xq_decision_t xq_admit(xq_t *q, const char *wire, int len, const char *id,
                       const xq_ctx_t *c, bool count);

/** 4.2, 4.6, 4.7: may [air_ms] of [cls] by [author] (0 = nobody's) go now? */
xq_shed_t xq_may_air(xq_t *q, xq_class_t cls, uint32_t author,
                     uint32_t air_ms, const xq_ctx_t *c);

/** 4.6: [author]'s cap this hour. */
uint32_t xq_fair_cap(xq_t *q, uint32_t author, uint32_t budget_ms,
                     uint32_t now_ms);

/** Third-party airtime of [author] this hour (rounded up to buckets). */
uint32_t xq_author_spent(xq_t *q, uint32_t author, uint32_t now_ms);

/** L2-L6 airtime in the last six minutes (rounded up to buckets). */
uint32_t xq_paced_ms(xq_t *q, uint32_t now_ms);

/** Record one transmission. [wire] may be NULL for a frame that is not an
 *  XPRS packet (a Meshtastic relay): then the pace and the `frames`
 *  counter, not the class's (it was never offered to the gate).
 *  [spent_after] is the ledger including this one, for the low-water mark. */
void xq_aired(xq_t *q, const char *wire, int len, const char *id,
              xq_class_t cls, uint32_t author, uint32_t air_ms,
              uint32_t spent_after, uint32_t budget_ms, uint32_t now_ms);

/** A packet admitted a moment ago that the queue then refused. */
void xq_revoke(xq_t *q, xq_class_t cls, xq_shed_t why);
/** A shed decided after admission (expiry, eviction, the budget). */
void xq_count_shed(xq_t *q, xq_class_t cls, xq_shed_t why);

/** 4.8: how long [cls] may wait in a queue, in ms. */
uint32_t xq_limit_ms(const xq_t *q, xq_class_t cls, const xq_ctx_t *c);

/** The repeat interval now (4.5), in ms. */
uint32_t xq_repeat_ms(const xq_t *q, const xq_ctx_t *c);
bool xq_crowded(const xq_t *q, const xq_ctx_t *c);

/** 1..4 for observation, service, identity, track; 0 otherwise. */
uint8_t xq_periodic_type(const char *wire, int len);

const char *xq_class_name(xq_class_t c);
const char *xq_shed_name(xq_shed_t s);

/** `{"profile":..,"classes":{"conversation":{"offered":..,...}}}` for a
 *  status page; returns the length written. */
int xq_json(const xq_t *q, char *out, int cap);

#ifdef __cplusplus
}
#endif
#endif /* XB_QOS_H */
