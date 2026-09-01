/**
 * @file xprsbearer.h
 * @brief What every XPRS bearer does, minus the radio.
 *
 * A bearer is a way of putting a 250-byte packet in front of whoever is
 * listening. The differences between them are small — a socket, a advertising
 * set, an ESP-NOW frame — and the part that is hard is identical: when to
 * re-air somebody else's packet, when to shut up because they got there first,
 * which identifiers have been seen, and when to beacon.
 *
 * That part was written twice before this file existed — once in
 * `xprs_bearer_lan` and once inline in the dongle's BLE relay, with the same
 * 200–1200 ms jitter and the same eight slots — and a third copy was about to
 * be written for ESP-NOW. This is that logic, once, with the radio behind five
 * function pointers.
 *
 * ── What a bearer supplies ──────────────────────────────────────────────────
 *
 * `air` puts bytes on the medium. `now_ms` and `random` are the clock and the
 * jitter. `lock`/`unlock` serialise transmission, because on a real station
 * several tasks air on one bearer: its own beacons, a reply from whichever task
 * heard the ask, and this file's re-air queue.
 *
 * ── What it does NOT decide ─────────────────────────────────────────────────
 *
 * Whether a packet may be relayed at all belongs to `xprs_codec`
 * (`xprs_append_via`, §13.1 hop budgets and §13.2 loop prevention). This file
 * asks and obeys; it never second-guesses.
 *
 * No ESP-IDF here, deliberately: the whole file compiles on a host, which is
 * where the timing and the cancel rule are actually tested.
 */

#ifndef XPRS_BEARER_H
#define XPRS_BEARER_H

#include <stdint.h>
#include <stdbool.h>

#include "xprs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Longest XPRS packet (docs/XPRS.md §4) — the same on every bearer. */
#define XB_WIRE_MAX      XPRS_MAX_WIRE
#define XB_ID_LEN        XPRS_ID_LEN

/**
 * How long a re-aired packet waits, and the window in which hearing it from
 * somebody else cancels it (§13.2.1). Random inside the window, because the
 * whole point is that stations hearing the same packet do not answer together.
 */
#define XB_JITTER_MIN_MS 200
#define XB_JITTER_MAX_MS 1200

#define XB_QUEUE_MAX     8     /* packets waiting to be re-aired */
#define XB_PEERS_MAX     16    /* distinct stations remembered */
/* Signal that counts as "close" and as "at the edge", for the re-air wait.
 * A station that heard a packet faintly is the one whose repeat extends the
 * network; a station that heard it loudly is standing next to the sender
 * and adds nothing. So the faint one waits the least and wins 13.2.1's
 * race. Between these two the wait slides linearly. */
#define XB_RSSI_CLOSE   (-35)
#define XB_RSSI_EDGE    (-100)

#define XB_SEEN_RING     32    /* identifiers remembered, per ring */
#define XB_SEEN_MS       60000u/* how long "already heard" lasts */

/* A queued relay that has waited this long is no longer worth its airtime:
 * the conversation moved on, and the identifier has aged out of every ring
 * that would stop it looping. Priority traffic gets ten minutes -- an sos
 * held by a spent budget is still an sos when the window rolls. */
#define XB_STALE_MS      120000u
#define XB_STALE_PRIO_MS 600000u

/* The duty ledger's rolling hour: sixty one-minute buckets. */
#define XB_DUTY_BUCKETS   60
#define XB_DUTY_BUCKET_MS 60000u

/**
 * One packet heard.
 *
 * @p peer identifies the sender on this bearer's own terms — an IPv4 address, a
 * MAC, 0 when the bearer cannot say. @p rssi is dBm, or 0 for a bearer with no
 * signal to report: a network has none, a radio does.
 */
typedef void (*xb_rx_cb_t)(const char *wire, int len, uint64_t peer, int rssi);

/**
 * Told the §5 identifier of EVERY valid packet heard, including the duplicates
 * the rx callback swallows.
 *
 * A station that queued its own copy for ANOTHER bearer has to know when
 * somebody else airs it, and "somebody else is saying it" arrives as a repeat.
 * Without this the §13.2.1 cancel only works one bearer at a time.
 */
typedef void (*xb_heard_cb_t)(const char *id, const char *wire, int len);

/** Build this station's periodic beacon (§10.6). Return its length, or 0. */
typedef int (*xb_beacon_cb_t)(char *out, int cap);

/** The radio, and the clock it runs on. */
typedef struct {
    bool     (*air)(void *ctx, const char *wire, int len);
    uint32_t (*now_ms)(void);
    uint32_t (*random)(void);
    void     (*lock)(void *ctx);      /* may be NULL on a single-task bearer */
    void     (*unlock)(void *ctx);
    /**
     * Empty whatever the receive path filled, calling xb_on_wire() for each.
     * NULL for a bearer that delivers on its own task.
     *
     * This exists because ESP-NOW hands packets to a callback running in the
     * WiFi task, on core 0, where the SHA-256 an identifier costs must never
     * happen. The callback copies and returns; this runs on the bearer task.
     */
    void     (*drain)(void *ctx);
    void      *ctx;
    const char *name;                 /* "lan", "espnow" — for logs only */
} xb_ops_t;

/* Why a queued packet has not gone out yet, for whoever asks. */
typedef enum {
    XB_WAIT_NONE = 0,
    XB_WAIT_JITTER,   /* its moment has not come (13.2.1) */
    XB_WAIT_PACE,     /* the bearer owes the inter-packet gap (31.1) */
    XB_WAIT_DUTY,     /* the hour's airtime budget is spent */
    XB_WAIT_PRIO,     /* something more urgent went instead this tick */
} xb_wait_t;

typedef struct {
    char     wire[XB_WIRE_MAX + 1];
    int      len;
    char     id[XB_ID_LEN];
    uint32_t due_ms;
    uint32_t queued_ms; /* when it entered, for the staleness drop */
    bool     used;
    bool     held;      /* pacing made it wait past due_ms (§31.1) */
    uint8_t  prio;      /* 1 = t:sos, t:warning or urg:urgent */
    uint8_t  own;       /* ours, deferred by the budget: never stales */
    uint8_t  why;       /* xb_wait_t, the last reason it sat */
} xb_queued_t;

/** How long [len] bytes take on this medium, in ms. NULL is unmetered, and
 *  that is a real answer, not a missing one -- the same sense as pace 0. */
typedef uint32_t (*xb_airtime_cb_t)(int len, void *ctx);

/**
 * The duty ledger: real transmit-milliseconds over a rolling hour.
 *
 * Caller-owned, like xb_t itself, and attached by pointer so the bearers
 * with nothing to meter (the LAN, ESP-NOW, BLE) do not carry 150 bytes of
 * ring on a board whose whole free heap is ten kilobytes.
 */
typedef struct {
    uint16_t bucket[XB_DUTY_BUCKETS]; /* ms aired in each of the last 60 min */
    uint32_t head_ms;                 /* now_ms at the start of bucket[head] */
    uint32_t spent_ms;                /* running sum: the check is O(1) */
    uint32_t budget_ms;               /* per rolling hour; 0 = ledger off */
    uint32_t reserve_ms;              /* of that, priority traffic only */
    uint32_t dwell_ms;                /* longest single transmission; 0 = any.
                                         The US/AU regime, which caps one
                                         transmission rather than the hour. */
    xb_airtime_cb_t airtime;
    void    *airtime_ctx;
    uint8_t  head;
    uint32_t held_now;                /* waiting on the budget right now */
    uint32_t deferred;                /* own sends that had to queue, ever */
    uint32_t stale;                   /* dropped as no longer worth airing */
} xb_duty_t;

/** What the ledger has to say, shaped for a status line or /api/status. */
typedef struct {
    uint32_t budget_ms;      /* per hour; 0 when unmetered */
    uint32_t spent_ms;       /* in the last rolling hour */
    uint32_t reserve_ms;
    uint32_t free_ms;        /* what ordinary traffic may still spend */
    uint32_t free_prio_ms;   /* what an sos may still spend */
    uint32_t next_free_ms;   /* until the oldest spent minute rolls off */
    uint32_t held;           /* packets sitting on the budget right now */
    uint32_t deferred;
    uint32_t stale;
} xb_duty_report_t;

typedef struct {
    char     id[XB_ID_LEN];
    uint32_t t_ms;
} xb_seen_t;

typedef struct {
    uint64_t peer;
    uint32_t t_ms;
} xb_peer_t;

/**
 * One bearer's state. Held by the bearer, not allocated here — every byte of
 * heap on this board is spoken for, and a caller that can see the struct can
 * see what it costs.
 */
typedef struct {
    xb_ops_t     ops;
    char         call[16];
    bool         active;
    xb_queued_t  queue[XB_QUEUE_MAX];
    uint32_t     last_ms;               /* last rx or tx, for xb_idle_ms  */
    int          last_rssi;             /* signal of the packet below     */
    char         last_rssi_id[XB_ID_LEN];
    xb_seen_t    heard[XB_SEEN_RING];   /* seen on this bearer */
    int          heard_pos;
    xb_seen_t    aired[XB_SEEN_RING];   /* put on this bearer by us */
    int          aired_pos;
    xb_peer_t    peers[XB_PEERS_MAX];
    xb_rx_cb_t     rx_cb;
    xb_heard_cb_t  heard_cb;
    xb_beacon_cb_t beacon_cb;
    uint32_t     beacon_every_ms, beacon_due_ms;
    uint32_t     rx_count, tx_count, cancelled, dupes;
    /* §31.1: what one packet owes this bearer in silence. 0 is unmetered, and
     * that is a real answer -- "the internet | nothing, which is the trap". */
    uint32_t     pace_ms;
    uint32_t     free_at_ms;    /* not before this may we transmit again */
    uint32_t     paced;         /* packets this held back at least once */
    uint32_t     declined;      /* 13.2.2: relays we sat out, not being named */
    xb_duty_t   *duty;          /* NULL = unmetered (LAN, ESP-NOW, BLE) */
} xb_t;

/** Bring a bearer up. @p call is this station, used for `via:` when relaying. */
void xb_init(xb_t *b, const xb_ops_t *ops, const char *call);
void xb_stop(xb_t *b);
bool xb_is_active(const xb_t *b);

void xb_set_rx_cb(xb_t *b, xb_rx_cb_t cb);
void xb_set_heard_cb(xb_t *b, xb_heard_cb_t cb);

/**
 * Air @p cb every @p interval_sec, first after @p first_delay_sec.
 *
 * Called from whatever task drives xb_tick(), never from a timer: building a
 * beacon derives a §5 identifier, which is a SHA-256, and a timer task's stack
 * is not sized for that — putting it there hung the dongle.
 */
void xb_set_beacon(xb_t *b, xb_beacon_cb_t cb, uint32_t interval_sec,
                   uint32_t first_delay_sec);

/**
 * How much silence one packet owes this bearer (§31.1).
 *
 * | bearer | what binds |
 * |---|---|
 * | LoRa on ISM | a legal duty cycle, often 1 % |
 * | Bluetooth, WiFi Direct | range, so traffic is local and cheap |
 * | LAN, the internet | nothing, which is the trap |
 *
 * A re-air that arrives while the bearer still owes silence WAITS in the queue
 * rather than being dropped, and is aired when the debt clears. 0 disables it.
 *
 * This is not a legal duty-cycle regulator: the real figure depends on band,
 * spreading factor and region, and belongs to the operator. What it does
 * guarantee is that a busy neighbour bearer cannot pour traffic onto a slow
 * radio faster than the radio was told it may speak.
 */
/**
 * @brief An extra test for "this packet goes first".
 *
 * The queue ranks by 13.1's own reading of a packet -- sos, warning,
 * urg:urgent -- and knows nothing about who sent it. XPRS.md 25.9 lets a
 * station's OWNER name callsigns whose traffic leaves ahead of everyone
 * else's, which is a question about configuration and therefore not this
 * component's to answer. The station supplies the answer here; NULL (the
 * default) leaves the ranking exactly as the section defines it.
 */
void xb_set_priority_hook(bool (*fn)(const char *wire, int len));

void xb_set_pace(xb_t *b, uint32_t per_packet_ms);

/** Milliseconds until this bearer may transmit again; 0 when free now.
 *  The larger of the pace debt and the duty wait. */
uint32_t xb_owed_ms(const xb_t *b);

/**
 * Meter this bearer against a rolling hour of real airtime.
 *
 * [airtime] answers "how long do [len] bytes take here"; only the bearer
 * knows, and the generic half only counts. [budget_ms] is transmit
 * milliseconds per rolling hour -- EU band g3 (869.4-869.65 MHz) is 10%,
 * which is 360000; band g1 is 1%, 36000. [reserve_ms] is the slice of the
 * budget only t:sos, t:warning and urg:urgent may spend: ordinary traffic
 * stops at budget - reserve so the emergency still gets out. [dwell_ms]
 * caps one single transmission instead (the US/AU 400 ms regime); 0 is no
 * cap, and budget 0 with a dwell is a real configuration.
 *
 * This ACCOUNTS. It does not certify: band, region, antenna gain and which
 * reading of the observation window applies are the operator's, and always
 * were. [d] is caller-owned and must outlive the bearer.
 */
void xb_set_duty(xb_t *b, xb_duty_t *d, xb_airtime_cb_t airtime, void *ctx,
                 uint32_t budget_ms, uint32_t reserve_ms, uint32_t dwell_ms);

void xb_duty_report(const xb_t *b, uint32_t now_ms, xb_duty_report_t *out);

/** One queued packet, for "why is my packet waiting". Returns how many are
 *  queued in total; [i] indexes them 0..n-1 in no particular order. Any out
 *  pointer may be NULL. */
int xb_queue_peek(const xb_t *b, int i, char id[XB_ID_LEN],
                  uint32_t *due_ms, xb_wait_t *why, bool *prio);

/** Human words for xb_wait_t, one table, so the log, the UI and the JSON
 *  cannot drift. */
const char *xb_wait_name(xb_wait_t w);

/** What xb_send_ex says happened. */
typedef enum {
    XB_AIRED = 0,   /* on the medium now */
    XB_QUEUED,      /* the budget is spent -- waiting at the front, will go */
    XB_REFUSED,     /* the radio said no, or there was no room to wait */
} xb_send_t;

/**
 * Air one packet of OUR OWN, now, with no jitter and no `via:` — it has taken
 * no hops yet. Use xb_offer() for anything heard elsewhere.
 *
 * Our own traffic is NOT exempt from the duty budget -- a spent hour is
 * spent whoever is talking, and this header used to promise the opposite
 * ("charged, never blocked") back when the charge was a flat pace with no
 * arithmetic behind it. What our traffic keeps is the reserve (an sos of
 * ours never waits) and the front of the queue: a budget-refused packet is
 * QUEUED with no jitter, not dropped, and airs when the window rolls.
 *
 * The pace, as before, charges our traffic but does not block it.
 */
xb_send_t xb_send_ex(xb_t *b, const char *wire, int len);

/** As xb_send_ex(), true only for XB_AIRED. Kept so callers that render
 *  "which bearers took it" keep telling the truth without changing. */
bool xb_send(xb_t *b, const char *wire, int len);

/**
 * Offer a packet heard on ANOTHER bearer for re-airing on this one.
 *
 * Appends this station to `via:` and queues it with the jitter above. Silently
 * does nothing when the packet must not be relayed — that decision belongs to
 * `xprs_codec`, not here.
 */
void xb_offer(xb_t *b, const char *wire, int len);

/**
 * Re-air on the bearer it was HEARD on — a digipeater (§13.1: "repeats a
 * packet on the medium it heard it, within the hop budget, appending itself to
 * `via:`").
 *
 * The same queue, jitter and cancel as xb_offer(); the difference is that
 * having heard the packet on this bearer does not disqualify it, because here
 * that is the reason to repeat it. Use xb_offer() when the packet arrived on a
 * DIFFERENT bearer.
 */
void xb_digipeat(xb_t *b, const char *wire, int len);

/**
 * Air a packet again, verbatim, as a second chance for whoever was not
 * listening the first time.
 *
 * Unlike xb_offer() and xb_digipeat() this does NOT append anything and is
 * NOT refused by the aired ring -- the wire handed here is one this station
 * already put on the air, via: and all, so a neighbour that relayed it is
 * still in its own path and will not relay it twice. It is for the caller
 * that keeps a short list of what matters and repeats it while the channel
 * is otherwise quiet. Everything else -- pacing, the queue, the cancel
 * window -- is unchanged.
 */
void xb_echo(xb_t *b, const char *wire, int len);

/** Milliseconds since anything was heard or aired on this bearer, or a very
 *  large number when nothing ever has. What "the channel is quiet" means. */
uint32_t xb_idle_ms(const xb_t *b, uint32_t now_ms);

/** One packet arrived on the medium. Called by the bearer's receive path. */
void xb_on_wire(xb_t *b, const char *wire, int len, uint64_t peer, int rssi);

/** Air anything whose moment has come, and the beacon when it is due. */
void xb_tick(xb_t *b, uint32_t now_ms);

/**
 * Ask to be ticked by whatever task is already driving bearers.
 *
 * Pumping a queue means waking every hundred milliseconds, and on this board a
 * task is 5 KB of a heap that has been down to hundreds of bytes. One task can
 * drive every bearer — the granularity a 200–1200 ms jitter needs is the same
 * for all of them — so a bearer registers here instead of starting its own.
 *
 * The bearer that OWNS the task calls xb_tick_all(). If none does, nothing is
 * pumped, which is why a bearer that registers must say so out loud rather than
 * discover it in the field.
 */
void xb_register_ticked(xb_t *b);
void xb_tick_all(uint32_t now_ms);

/** Whether any task has claimed the job of calling xb_tick_all(). */
bool xb_has_driver(void);
void xb_set_driver(bool yes);

/** Stations heard within [max_age_sec]. */
int  xb_peer_count(const xb_t *b, uint32_t max_age_sec);

void xb_stats(const xb_t *b, uint32_t *rx, uint32_t *tx, uint32_t *cancelled);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_BEARER_H */
