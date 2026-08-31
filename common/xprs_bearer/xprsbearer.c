/*
 * xprsbearer — the half of a bearer that has nothing to do with the radio.
 *
 * Lifted verbatim from xprs_bearer_lan, which had already isolated it behind
 * three hooks; the only real change is that the state is now per-instance and
 * a peer is an opaque 64-bit number rather than an IPv4 address, so a MAC fits.
 *
 * Read xprsbearer.h first. The two rules worth knowing before changing
 * anything here:
 *
 *   - a copy that has ALREADY been relayed cancels ours; the origin repeating
 *     itself does the opposite (xb_on_wire)
 *   - whether a packet may be relayed at all is xprs_append_via's decision
 */

#include "xprsbearer.h"
#include "xb_airtime.h"

#include <string.h>
#include <stdio.h>

/* The only thing in this file that was ever ESP-specific: two log lines.
 *
 * ESP_PLATFORM is defined by the IDF build and by nothing else, so a target
 * that is neither the IDF nor the host harness -- the nRF52840 under
 * Arduino, say -- lands in the same silent case the host test uses rather
 * than failing to find esp_log.h. A caller that wants these lines back on
 * such a target defines XB_LOGI/XB_LOGW itself before including this. */
#if defined(XB_HOST_TEST) || !defined(ESP_PLATFORM)
#ifndef XB_LOGI
#define XB_LOGI(fmt, ...) ((void)0)
#endif
#ifndef XB_LOGW
#define XB_LOGW(fmt, ...) ((void)0)
#endif
#else
#include "esp_log.h"
static const char *TAG = "xprsbearer";
#define XB_LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define XB_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#endif

#define XB_LOCK(b)   do { if ((b)->ops.lock) (b)->ops.lock((b)->ops.ctx); } while (0)
#define XB_UNLOCK(b) do { if ((b)->ops.unlock) (b)->ops.unlock((b)->ops.ctx); } while (0)

/* ── Identifier rings ───────────────────────────────────────────────────── */

/*
 * Elapsed milliseconds, SIGNED.
 *
 * Two clocks reach this file. A packet is stamped with `ops.now_ms()` sampled
 * inside the enqueue call; the pump is driven by `xb_tick(b, now_ms)` with a
 * `now` the CALLER sampled, which can be a few milliseconds older. So
 * `now - queued_ms` is occasionally negative, and unsigned that is 4.29
 * billion -- past every limit in this file at once.
 *
 * Measured on a T-Deck: `espnow: 33bb4e waited 4294967s -- no longer worth
 * its airtime, dropped`. 4294967 is 2^32/1000, and the packet had been in the
 * queue for under a millisecond. Every age test here goes through this, and
 * every DEADLINE test already casts the same way ((int32_t)(now - due) < 0).
 */
static inline int32_t xb_since(uint32_t now, uint32_t then)
{
    return (int32_t)(now - then);
}

static bool xb_ring_has(const xb_seen_t *ring, const char *id, uint32_t now)
{
    for (int i = 0; i < XB_SEEN_RING; i++) {
        if (!ring[i].id[0]) continue;
        if (xb_since(now, ring[i].t_ms) >= (int32_t)XB_SEEN_MS) continue;
        if (strcmp(ring[i].id, id) == 0) return true;
    }
    return false;
}

static void xb_ring_add(xb_seen_t *ring, int *pos, const char *id, uint32_t now)
{
    snprintf(ring[*pos].id, XB_ID_LEN, "%s", id);
    ring[*pos].t_ms = now;
    *pos = (*pos + 1) % XB_SEEN_RING;
}

/* ── The queue ──────────────────────────────────────────────────────────── */

/* Somebody aired this identifier. Anything of ours waiting to say the same
 * thing is now pointless — this is the whole reason for the delay. */
static void xb_cancel(xb_t *b, const char *id)
{
    for (int i = 0; i < XB_QUEUE_MAX; i++) {
        if (!b->queue[i].used) continue;
        if (strcmp(b->queue[i].id, id) != 0) continue;
        b->queue[i].used = false;
        b->cancelled++;
        XB_LOGI("%s: %s already aired by somebody else — dropping our copy",
                b->ops.name ? b->ops.name : "?", id);
    }
}

/*
 * Is this packet one the emergency reserve exists for?
 *
 * A token walk, deliberately not xprs_parse: an xprs_t is 512 bytes and
 * xb_queue_relay already holds one on this stack -- this tree has stack
 * scars in exactly that shape. xprs_looks_like has already guaranteed the
 * wire begins "t:", so the type is the first token.
 */
static bool xb_is_priority(const char *wire, int len)
{
    if (len >= 6 && memcmp(wire, "t:sos", 5) == 0 &&
        (wire[5] == ' ' || len == 5)) return true;
    if (len >= 10 && memcmp(wire, "t:warning", 9) == 0 &&
        (wire[9] == ' ' || len == 9)) return true;
    for (int i = 0; i + 11 <= len; i++) {
        if (wire[i] == ' ' && memcmp(wire + i, " urg:urgent", 11) == 0 &&
            (i + 11 == len || wire[i + 11] == ' '))
            return true;
    }
    return false;
}

/* Roll the ledger's window forward. Bounded: a gap beyond two full windows
 * (a long sleep, or the 49.7-day millisecond turnover) clears the ring --
 * losing an hour of accounting once in seven weeks beats inventing spend
 * that cannot be placed. */
static void xb_duty_roll(xb_duty_t *d, uint32_t now)
{
    /* Signed, then clamped: a head_ms a few milliseconds AHEAD of now (the two
     * clocks of xb_since) read as a 49-day gap unsigned, and the branch below
     * would clear the whole ledger -- an hour of airtime accounting thrown
     * away because two samples arrived out of order. */
    int32_t since = xb_since(now, d->head_ms);
    uint32_t gap = since > 0 ? (uint32_t)since : 0u;
    if (gap >= XB_DUTY_BUCKET_MS * XB_DUTY_BUCKETS * 2u) {
        memset(d->bucket, 0, sizeof d->bucket);
        d->spent_ms = 0;
        d->head = 0;
        d->head_ms = now;
        return;
    }
    while (gap >= XB_DUTY_BUCKET_MS) {
        d->head = (uint8_t)((d->head + 1) % XB_DUTY_BUCKETS);
        d->spent_ms -= d->bucket[d->head];
        d->bucket[d->head] = 0;
        d->head_ms += XB_DUTY_BUCKET_MS;
        gap -= XB_DUTY_BUCKET_MS;
    }
}

static void xb_duty_charge(xb_duty_t *d, uint32_t air_ms)
{
    if (!d) return;
    uint32_t v = (uint32_t)d->bucket[d->head] + air_ms;
    d->bucket[d->head] = v > 65535u ? 65535u : (uint16_t)v;
    d->spent_ms += air_ms;
}

/* May [air_ms] more go out? Ordinary traffic stops where the reserve
 * begins; priority runs to the whole budget. */
static bool xb_afford(const xb_duty_t *d, uint32_t air_ms, bool prio)
{
    if (!d || !d->budget_ms) return true;
    uint32_t cap = prio ? d->budget_ms
                        : (d->budget_ms > d->reserve_ms
                               ? d->budget_ms - d->reserve_ms : 0);
    return d->spent_ms + air_ms <= cap;
}

/* The cost of airing [len] bytes here, and whether it may ever be aired at
 * all: a dwell-capped region refuses a single transmission longer than the
 * cap, whatever the hour looks like. */
static uint32_t xb_air_cost(const xb_t *b, int len)
{
    if (!b->duty || !b->duty->airtime) return 0;
    return b->duty->airtime(len, b->duty->airtime_ctx);
}

/* Pick the due packet that should go next: earliest due among the wanted
 * class. Two scans of eight, no sort. */
static int xb_pick(const xb_t *b, uint32_t now, bool want_prio)
{
    int best = -1;
    for (int i = 0; i < XB_QUEUE_MAX; i++) {
        if (!b->queue[i].used) continue;
        if ((bool)b->queue[i].prio != want_prio) continue;
        if ((int32_t)(now - b->queue[i].due_ms) < 0) continue;
        if (best < 0 ||
            (int32_t)(b->queue[i].due_ms - b->queue[best].due_ms) < 0)
            best = i;
    }
    return best;
}

/* Drop what is no longer worth its airtime (see XB_STALE_MS). */
static void xb_drop_stale(xb_t *b, uint32_t now)
{
    for (int i = 0; i < XB_QUEUE_MAX; i++) {
        if (!b->queue[i].used) continue;
        /* Our own deferred packet never stales: it is not a relay whose
         * moment passes, it is this station's word waiting for the window
         * to roll, which can honestly take most of an hour. */
        if (b->queue[i].own) continue;
        uint32_t limit = b->queue[i].prio ? XB_STALE_PRIO_MS : XB_STALE_MS;
        if (xb_since(now, b->queue[i].queued_ms) < (int32_t)limit) continue;
        b->queue[i].used = false;
        if (b->duty) b->duty->stale++;
        XB_LOGW("%s: %s waited %lds -- no longer worth its airtime, dropped",
                b->ops.name ? b->ops.name : "?", b->queue[i].id,
                (long)(xb_since(now, b->queue[i].queued_ms) / 1000));
    }
}

static int xb_pump(xb_t *b, uint32_t now)
{
    if (b->duty) {
        xb_duty_roll(b->duty, now);
        b->duty->held_now = 0;
    }
    xb_drop_stale(b, now);

    int sent = 0;
    for (;;) {
        /* §31.1, a WAIT and not a refusal: the debt is the bearer's, so
         * everything due waits with it. */
        if (b->pace_ms && (int32_t)(now - b->free_at_ms) < 0) {
            for (int i = 0; i < XB_QUEUE_MAX; i++) {
                if (!b->queue[i].used) continue;
                if ((int32_t)(now - b->queue[i].due_ms) < 0) continue;
                b->queue[i].why = XB_WAIT_PACE;
                if (!b->queue[i].held) { b->queue[i].held = true; b->paced++; }
            }
            break;
        }

        /* Priority first, then ordinary; each class by earliest due. This is
         * also what finally drains the queue in due order rather than in
         * whatever slot order the packets happened to land in. */
        bool prio = true;
        int i = xb_pick(b, now, true);
        if (i < 0) { prio = false; i = xb_pick(b, now, false); }
        if (i < 0) break;

        uint32_t air_ms = xb_air_cost(b, b->queue[i].len);
        if (b->duty && b->duty->dwell_ms && air_ms > b->duty->dwell_ms) {
            /* Longer than one transmission may EVER be here: it will never
             * fit, so holding it would wedge the queue behind it. */
            b->queue[i].used = false;
            b->duty->stale++;
            XB_LOGW("%s: %s is %lums of airtime against a %lums dwell cap"
                    " -- dropped", b->ops.name ? b->ops.name : "?",
                    b->queue[i].id, (unsigned long)air_ms,
                    (unsigned long)b->duty->dwell_ms);
            continue;
        }
        if (b->duty && !xb_afford(b->duty, air_ms, prio)) {
            /* A refused priority packet means even the reserve is gone;
             * a refused ordinary packet means the prio pass found nothing.
             * Either way nothing can go this tick. */
            for (int j = 0; j < XB_QUEUE_MAX; j++) {
                if (!b->queue[j].used) continue;
                if ((int32_t)(now - b->queue[j].due_ms) < 0) continue;
                b->queue[j].why = XB_WAIT_DUTY;
                b->duty->held_now++;
            }
            static uint32_t s_duty_logs;
            s_duty_logs++;
            if (s_duty_logs == 1 || (s_duty_logs % 16) == 0) {
                XB_LOGW("%s: %s held -- %lu.%lu s of %lu.%lu s spent this "
                        "hour", b->ops.name ? b->ops.name : "?",
                        b->queue[i].id,
                        (unsigned long)(b->duty->spent_ms / 1000u),
                        (unsigned long)(b->duty->spent_ms % 1000u / 100u),
                        (unsigned long)(b->duty->budget_ms / 1000u),
                        (unsigned long)(b->duty->budget_ms % 1000u / 100u));
            }
            break;
        }

        b->queue[i].used = false;
        b->queue[i].held = false;
        XB_LOCK(b);
        bool ok = b->ops.air(b->ops.ctx, b->queue[i].wire, b->queue[i].len);
        /* Charged whether or not the radio said yes: a send that timed out
         * still keyed the PA up, and a failing radio must not be free. */
        xb_duty_charge(b->duty, air_ms);
        uint32_t after = b->ops.now_ms();
        if (ok) {
            xb_ring_add(b->aired, &b->aired_pos, b->queue[i].id, now);
            b->tx_count++;
            b->last_ms = after;
        }
        /* The debt starts when the transmission ENDED, not when the tick
         * began -- ops.air blocks for the whole airtime on a radio. */
        b->free_at_ms = after + b->pace_ms;
        XB_UNLOCK(b);
        if (ok) sent++;

        /* The ones that were due and did not go: say why. */
        for (int j = 0; j < XB_QUEUE_MAX; j++) {
            if (!b->queue[j].used) continue;
            if ((int32_t)(now - b->queue[j].due_ms) < 0) continue;
            b->queue[j].why = prio && !b->queue[j].prio ? XB_WAIT_PRIO
                                                        : XB_WAIT_PACE;
        }
        /* One per tick while metered, as before. */
        if (b->pace_ms || b->duty) break;
    }
    return sent;
}

/*
 * When to say it again.
 *
 * The random part of 13.2.1 is there so that a room full of relays does not
 * answer in chorus: the first to speak cancels the rest. Which one speaks
 * first was pure chance, and chance picks the station standing next to the
 * sender as often as the one at the edge of its range -- and only the far
 * one extends anything. A packet heard at -95 dBm came from the edge of
 * this station's reach, so repeating it covers ground the sender could not;
 * one heard at -35 dBm came from across the table.
 *
 * So the wait slides with the signal: faint goes first, loud goes last, and
 * a quarter of the span stays random so that two stations at the same
 * distance still do not collide. Where the bearer reports no signal at all
 * (the LAN), it is the old uniform draw.
 */
static uint32_t xb_due_ms(xb_t *b, uint32_t now, int rssi)
{
    uint32_t span = XB_JITTER_MAX_MS - XB_JITTER_MIN_MS;
    if (rssi >= 0) return now + XB_JITTER_MIN_MS + (b->ops.random() % (span + 1));

    if (rssi > XB_RSSI_CLOSE) rssi = XB_RSSI_CLOSE;
    if (rssi < XB_RSSI_EDGE)  rssi = XB_RSSI_EDGE;
    /* 0 at the edge, 1000 next to the sender. */
    uint32_t near = (uint32_t)((rssi - XB_RSSI_EDGE) * 1000 /
                               (XB_RSSI_CLOSE - XB_RSSI_EDGE));
    uint32_t graded = span * 3 / 4 * near / 1000;
    uint32_t jitter = b->ops.random() % (span / 4 + 1);
    return now + XB_JITTER_MIN_MS + graded + jitter;
}

static void xb_queue_push(xb_t *b, const char *wire, int len, const char *id,
                          uint32_t now)
{
    uint32_t due = xb_due_ms(b, now, b->last_rssi_id[0] &&
                             strcmp(b->last_rssi_id, id) == 0
                                 ? b->last_rssi : 0);
    bool prio = xb_is_priority(wire, len);

    int slot = -1;
    for (int i = 0; i < XB_QUEUE_MAX; i++) {
        if (!b->queue[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        /* Full. The old rule -- evict whoever is furthest from their moment
         * -- inverts under a spent budget: an sos that arrived a second ago
         * is the furthest out, and the rule would drop the emergency for a
         * weather report. So: evict the lowest priority first, furthest due
         * within that, and an ordinary packet NEVER displaces a priority
         * one -- if only priority packets are waiting, the ordinary
         * newcomer is the one that loses. */
        for (int i = 0; i < XB_QUEUE_MAX; i++) {
            if (slot < 0) { slot = i; continue; }
            if (b->queue[i].prio != b->queue[slot].prio) {
                if (b->queue[i].prio < b->queue[slot].prio) slot = i;
                continue;
            }
            if ((int32_t)(b->queue[i].due_ms - b->queue[slot].due_ms) > 0)
                slot = i;
        }
        if (!prio && b->queue[slot].prio) {
            XB_LOGW("%s: re-air queue full of priority traffic — refusing %s",
                    b->ops.name ? b->ops.name : "?", id);
            return;
        }
        XB_LOGW("%s: re-air queue full — dropping %s for %s",
                b->ops.name ? b->ops.name : "?", b->queue[slot].id, id);
    }
    memcpy(b->queue[slot].wire, wire, (size_t)len);
    b->queue[slot].wire[len] = 0;
    b->queue[slot].len = len;
    snprintf(b->queue[slot].id, XB_ID_LEN, "%s", id);
    b->queue[slot].due_ms = due;
    b->queue[slot].queued_ms = now;
    b->queue[slot].used = true;
    b->queue[slot].held = false;
    b->queue[slot].prio = prio ? 1 : 0;
    b->queue[slot].own = 0;
    b->queue[slot].why = XB_WAIT_JITTER;
}

/* ── Offering a packet from another bearer ──────────────────────────────── */

static void xb_queue_relay(xb_t *b, const char *wire, int len, bool same_medium)
{
    if (!b || !b->active || !wire || len <= 0 || len > XB_WIRE_MAX) return;
    if (!xprs_looks_like((const uint8_t *)wire, len)) return;

    char id[XB_ID_LEN];
    if (!xprs_id_of(wire, len, id)) return;

    uint32_t now = b->ops.now_ms();
    /* Never repeat what WE have already put on this bearer, either way. */
    if (xb_ring_has(b->aired, id, now)) return;
    /* Whether HEARING it here disqualifies it is the whole difference between
     * the two callers, and getting it wrong makes one of them do nothing:
     *
     *   cross-bearer (xb_offer)   heard here already means the packet is
     *                             on this medium and we would add nothing.
     *
     *   same-medium (xb_digipeat) heard here is the REASON to repeat it --
     *                             that is what a digipeater is (13.1,
     *                             "repeats a packet on the medium it heard
     *                             it"). Refusing on the heard ring made
     *                             every digipeat a no-op, because
     *                             xb_on_wire records the hearing before the
     *                             callback that offers it back.
     *
     * What stops a storm here is not the heard ring: it is 13.2's own-callsign
     * check inside xprs_append_via, the aired ring above, and 13.2.1's cancel
     * when somebody else's relayed copy arrives first. */
    if (!same_medium && xb_ring_has(b->heard, id, now)) return;
    for (int i = 0; i < XB_QUEUE_MAX; i++) {
        if (b->queue[i].used && strcmp(b->queue[i].id, id) == 0) return;
    }

    /* Whether this may be relayed at all is xprs_codec's decision, not ours:
     * -1 means we are already in via: (§13.2) or the type's budget is spent
     * (§13.1). Relaying is also what puts us in the path for everyone else. */
    /* 13.2.2: when the sender named the relays, only a named one repeats it.
     *
     * Parsed here rather than inside xprs_append_via(), which the mail release
     * and the dongle also call and which must not acquire path semantics by
     * surprise. Cheap: a field lookup and two string walks, no allocation --
     * which is what a receive path can afford (docs/esp32.md).
     *
     * On a bearer where every station hears every other, 13.2.1 leaves exactly
     * one relay standing and which one is a matter of whose random wait was
     * shortest. This is how a sender gets a particular second hop instead. */
    xprs_t rp;
    if (xprs_parse(wire, len, &rp) && xprs_has_relay(&rp)) {
        bool mine = xprs_relay_next_is(&rp, b->call);
        /* Said out loud, rate-limited. "Why did my station not repeat that"
         * is otherwise unanswerable from outside, and the first bench run of
         * this gate was spent guessing at exactly that. */
        b->declined++;
        if (b->declined == 1 || (b->declined % 16) == 0) {
            XB_LOGI("%s: %s relay: names %s, we are %s -- %s",
                    b->ops.name ? b->ops.name : "?", id,
                    mine ? "us next" : "somebody else",
                    b->call[0] ? b->call : "(no callsign)",
                    mine ? "repeating" : "staying quiet");
        }
        if (!mine) return;
        b->declined--;   /* we are relaying it; not a decline */
    }

    char out[XB_WIRE_MAX + 1];
    int n = xprs_append_via(wire, len, b->call, out, (int)sizeof out);
    if (n <= 0) return;

    xb_queue_push(b, out, n, id, now);
}

void xb_offer(xb_t *b, const char *wire, int len)
{
    xb_queue_relay(b, wire, len, false);
}

void xb_digipeat(xb_t *b, const char *wire, int len)
{
    xb_queue_relay(b, wire, len, true);
}

void xb_echo(xb_t *b, const char *wire, int len)
{
    if (!b || !b->active || !wire || len <= 0 || len > XB_WIRE_MAX) return;
    if (!xprs_looks_like((const uint8_t *)wire, len)) return;

    char id[XB_ID_LEN];
    if (!xprs_id_of(wire, len, id)) return;
    uint32_t now = b->ops.now_ms();

    /* Already waiting to go out: leave it, an echo is the lower priority. */
    for (int i = 0; i < XB_QUEUE_MAX; i++)
        if (b->queue[i].used && strcmp(b->queue[i].id, id) == 0) return;

    /* Verbatim, and deliberately without the aired-ring refusal that stops
     * xb_offer(): this packet is one we aired, and saying it again is the
     * whole point. The wait is the plain one -- an echo is nobody's race.
     * And NEVER priority, whatever the wire says: an echoed sos on a timer
     * must not draw on the emergency reserve. */
    xb_queue_push(b, wire, len, id, now);
    for (int i = 0; i < XB_QUEUE_MAX; i++)
        if (b->queue[i].used && strcmp(b->queue[i].id, id) == 0)
            b->queue[i].prio = 0;
}

uint32_t xb_idle_ms(const xb_t *b, uint32_t now_ms)
{
    if (!b || !b->active || !b->last_ms) return 0xFFFFFFFFu;
    return now_ms - b->last_ms;
}

/* ── Receiving ──────────────────────────────────────────────────────────── */

static void xb_peer_touch(xb_t *b, uint64_t peer, uint32_t now)
{
    int slot = -1, oldest = 0;
    for (int i = 0; i < XB_PEERS_MAX; i++) {
        if (b->peers[i].peer == peer || b->peers[i].peer == 0) { slot = i; break; }
        if ((int32_t)(b->peers[i].t_ms - b->peers[oldest].t_ms) < 0) oldest = i;
    }
    if (slot < 0) slot = oldest;
    b->peers[slot].peer = peer;
    b->peers[slot].t_ms = now;
}

void xb_on_wire(xb_t *b, const char *wire, int len, uint64_t peer, int rssi)
{
    /* NULL-ops-safe too: a frame handed to a bearer that is not running is
     * dropped, rather than timestamped against a clock that does not exist. */
    if (!b || !b->active || len <= 0 || len > XB_WIRE_MAX) return;
    if (!xprs_looks_like((const uint8_t *)wire, len)) return;

    char id[XB_ID_LEN];
    if (!xprs_id_of(wire, len, id)) return;

    uint32_t now = b->ops.now_ms();
    if (peer) xb_peer_touch(b, peer, now);
    b->rx_count++;
    b->last_ms = now;
    /* Kept for the re-air wait, which is decided a few calls further down
     * this same stack (xb_queue_push) and has no other way to know it. */
    b->last_rssi = rssi;
    snprintf(b->last_rssi_id, sizeof b->last_rssi_id, "%s", id);

    /* Only a copy that has ALREADY been relayed cancels ours. The origin
     * repeating itself is the opposite signal — it means nobody has carried the
     * packet yet, which is exactly when a digipeater should — so `via:` is what
     * distinguishes "somebody else got there first" from "say it again". */
    /* "By ANOTHER" is the load-bearing word, and a non-empty `via:` is not
     * enough to establish it. A station that wrongly appends itself to the
     * `via:` of its own packet — a defect real phones shipped with — emits an
     * origin copy that looks relayed, and every board in the room then cancels
     * its queued repeat on hearing the author say it again. The chain dies and
     * nothing logs a reason.
     *
     * So the test is whether `via:` names anybody who is not the author. */
    xprs_t hp;
    bool relayed_by_other = false;
    if (xprs_parse(wire, len, &hp) && xprs_via_count(&hp) > 0) {
        char from[10] = "";
        /* No `f:` at all: treat a via: as a genuine relay rather than guess. */
        relayed_by_other = !xprs_get_str(&hp, "f", from, sizeof from) ||
                           !from[0] || !xprs_via_only(&hp, from);
    }
    if (relayed_by_other) xb_cancel(b, id);
    /* Every hearing, duplicates included — an owner with its own queue on
     * another bearer needs the repeats, which is exactly what the line below
     * throws away. */
    if (b->heard_cb) b->heard_cb(id, wire, len);

    if (xb_ring_has(b->heard, id, now)) {
        /* Say so, rate-limited. Silent duplicate-suppression hid a real bug:
         * §23.7's step 4 re-airs the SAME packet deliberately, and it died here
         * without a word for as long as it took to read the code. */
        b->dupes++;
        if (b->dupes == 1 || (b->dupes % 32) == 0) {
            XB_LOGI("%s: %s heard again — swallowed (%u so far)",
                    b->ops.name ? b->ops.name : "?", id, (unsigned)b->dupes);
        }
        return;
    }
    xb_ring_add(b->heard, &b->heard_pos, id, now);

    if (b->rx_cb) b->rx_cb(wire, len, peer, rssi);
}

/* ── Our own packets ────────────────────────────────────────────────────── */

xb_send_t xb_send_ex(xb_t *b, const char *wire, int len)
{
    if (!b || !b->active || !wire || len <= 0 || len > XB_WIRE_MAX)
        return XB_REFUSED;
    if (!xprs_looks_like((const uint8_t *)wire, len)) return XB_REFUSED;

    uint32_t now = b->ops.now_ms();
    char id[XB_ID_LEN];
    bool have_id = xprs_id_of(wire, len, id);   /* a SHA-256: outside the lock */
    bool prio = xb_is_priority(wire, len);
    uint32_t air_ms = xb_air_cost(b, len);

    if (b->duty) {
        xb_duty_roll(b->duty, now);
        if (b->duty->dwell_ms && air_ms > b->duty->dwell_ms)
            return XB_REFUSED;
        if (!xb_afford(b->duty, air_ms, prio)) {
            /* The hour is spent and this is not (or no longer) affordable:
             * our packet WAITS, at the front -- due immediately, no jitter,
             * ours has no 13.2.1 race to lose. It goes out unmodified when
             * the window rolls. */
            if (!have_id) return XB_REFUSED;
            for (int i = 0; i < XB_QUEUE_MAX; i++)
                if (b->queue[i].used && strcmp(b->queue[i].id, id) == 0)
                    return XB_QUEUED;
            int free_slot = -1;
            for (int i = 0; i < XB_QUEUE_MAX; i++)
                if (!b->queue[i].used) { free_slot = i; break; }
            if (free_slot < 0 && !prio) return XB_REFUSED;
            xb_queue_push(b, wire, len, id, now);
            for (int i = 0; i < XB_QUEUE_MAX; i++)
                if (b->queue[i].used && strcmp(b->queue[i].id, id) == 0) {
                    b->queue[i].due_ms = now;
                    b->queue[i].own = 1;
                    b->queue[i].why = XB_WAIT_DUTY;
                }
            b->duty->deferred++;
            return XB_QUEUED;
        }
    }

    XB_LOCK(b);
    if (have_id) xb_ring_add(b->aired, &b->aired_pos, id, now);
    bool ok = b->ops.air(b->ops.ctx, wire, len);
    xb_duty_charge(b->duty, air_ms);      /* whether or not it said yes */
    uint32_t after = b->ops.now_ms();
    if (ok) {
        b->tx_count++;
        b->last_ms = after;
    }
    /* The pace, as ever, charges our traffic without blocking it -- and now
     * accumulates from the end of the transmission instead of resetting to
     * a fixed gap, so two quick beacons owe two gaps, not one. */
    b->free_at_ms = ((int32_t)(b->free_at_ms - after) > 0 ? b->free_at_ms
                                                          : after) + b->pace_ms;
    XB_UNLOCK(b);
    return ok ? XB_AIRED : XB_REFUSED;
}

bool xb_send(xb_t *b, const char *wire, int len)
{
    return xb_send_ex(b, wire, len) == XB_AIRED;
}

/* ── Beacon and tick ────────────────────────────────────────────────────── */

void xb_set_pace(xb_t *b, uint32_t per_packet_ms)
{
    if (b) b->pace_ms = per_packet_ms;
}

void xb_set_duty(xb_t *b, xb_duty_t *d, xb_airtime_cb_t airtime, void *ctx,
                 uint32_t budget_ms, uint32_t reserve_ms, uint32_t dwell_ms)
{
    if (!b) return;
    if (d) {
        memset(d, 0, sizeof *d);
        d->airtime = airtime;
        d->airtime_ctx = ctx;
        d->budget_ms = budget_ms;
        d->reserve_ms = reserve_ms < budget_ms ? reserve_ms : budget_ms;
        d->dwell_ms = dwell_ms;
        d->head_ms = b->ops.now_ms ? b->ops.now_ms() : 0;
    }
    b->duty = d;
}

void xb_duty_report(const xb_t *b, uint32_t now_ms, xb_duty_report_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof *out);
    if (!b || !b->duty) return;
    xb_duty_t *d = b->duty;
    xb_duty_roll(d, now_ms);
    out->budget_ms = d->budget_ms;
    out->spent_ms = d->spent_ms;
    out->reserve_ms = d->reserve_ms;
    uint32_t ord_cap = d->budget_ms > d->reserve_ms
                           ? d->budget_ms - d->reserve_ms : 0;
    out->free_ms = d->spent_ms < ord_cap ? ord_cap - d->spent_ms : 0;
    out->free_prio_ms = d->spent_ms < d->budget_ms
                            ? d->budget_ms - d->spent_ms : 0;
    out->held = d->held_now;
    out->deferred = d->deferred;
    out->stale = d->stale;
    /* When does the oldest spent minute roll off? Walk the ring once --
     * this runs on a status tick, not in the pump. */
    if (d->budget_ms && d->spent_ms) {
        for (int k = 1; k <= XB_DUTY_BUCKETS; k++) {
            int idx = (d->head + k) % XB_DUTY_BUCKETS;
            if (!d->bucket[idx]) continue;
            uint32_t age = now_ms - d->head_ms
                         + (uint32_t)(XB_DUTY_BUCKETS - k) * XB_DUTY_BUCKET_MS;
            out->next_free_ms = age < XB_DUTY_BUCKET_MS * XB_DUTY_BUCKETS
                ? XB_DUTY_BUCKET_MS * XB_DUTY_BUCKETS - age : 0;
            break;
        }
    }
}

int xb_queue_peek(const xb_t *b, int i, char id[XB_ID_LEN],
                  uint32_t *due_ms, xb_wait_t *why, bool *prio)
{
    if (!b) return 0;
    int n = 0;
    for (int k = 0; k < XB_QUEUE_MAX; k++) {
        if (!b->queue[k].used) continue;
        if (n == i) {
            if (id) snprintf(id, XB_ID_LEN, "%s", b->queue[k].id);
            if (due_ms) *due_ms = b->queue[k].due_ms;
            if (why) *why = (xb_wait_t)b->queue[k].why;
            if (prio) *prio = b->queue[k].prio != 0;
        }
        n++;
    }
    return n;
}

const char *xb_wait_name(xb_wait_t w)
{
    switch (w) {
    case XB_WAIT_JITTER: return "jitter";
    case XB_WAIT_PACE:   return "pace";
    case XB_WAIT_DUTY:   return "duty";
    case XB_WAIT_PRIO:   return "priority";
    default:             return "none";
    }
}

uint32_t xb_owed_ms(const xb_t *b)
{
    if (!b || !b->ops.now_ms) return 0;
    uint32_t now = b->ops.now_ms();
    uint32_t owed = 0;
    if (b->pace_ms && (int32_t)(now - b->free_at_ms) < 0)
        owed = b->free_at_ms - now;
    if (b->duty && b->duty->budget_ms) {
        xb_duty_report_t r;
        xb_duty_report(b, now, &r);
        if (!r.free_ms && r.next_free_ms > owed) owed = r.next_free_ms;
    }
    return owed;
}

void xb_set_rx_cb(xb_t *b, xb_rx_cb_t cb)       { if (b) b->rx_cb = cb; }
void xb_set_heard_cb(xb_t *b, xb_heard_cb_t cb) { if (b) b->heard_cb = cb; }

void xb_set_beacon(xb_t *b, xb_beacon_cb_t cb, uint32_t interval_sec,
                   uint32_t first_delay_sec)
{
    if (!b) return;
    b->beacon_cb = cb;
    b->beacon_every_ms = interval_sec * 1000u;
    /* Same NULL-ops hazard, but NOT guarded on `active`: a caller is allowed
     * to configure the beacon before the bearer starts, and refusing that
     * would silently leave it unset. Only the clock read needs the guard;
     * xb_start stamps the real due time. */
    b->beacon_due_ms = (b->ops.now_ms ? b->ops.now_ms() : 0u)
                     + first_delay_sec * 1000u;
}

static void xb_beacon_tick(xb_t *b, uint32_t now)
{
    if (!b->beacon_cb || !b->beacon_every_ms) return;
    if ((int32_t)(now - b->beacon_due_ms) < 0) return;
    b->beacon_due_ms = now + b->beacon_every_ms;

    char wire[XB_WIRE_MAX + 1];
    int n = b->beacon_cb(wire, (int)sizeof wire);
    if (n > 0) xb_send(b, wire, n);
}

void xb_tick(xb_t *b, uint32_t now_ms)
{
    if (!b || !b->active) return;
    /* Inbound first: a packet heard this tick can still cancel a re-air that
     * was due this tick, which is the whole point of §13.2.1. */
    if (b->ops.drain) b->ops.drain(b->ops.ctx);
    xb_pump(b, now_ms);
    xb_beacon_tick(b, now_ms);
}

/* Every bearer that asked to be driven by somebody else's task. Four is more
 * than this firmware has bearers; the array exists so the driver does not need
 * to know who they are. */
#define XB_TICKED_MAX 4
static xb_t *s_ticked[XB_TICKED_MAX];
static int   s_ticked_n;
static bool  s_have_driver;

void xb_register_ticked(xb_t *b)
{
    if (!b) return;
    for (int i = 0; i < s_ticked_n; i++) if (s_ticked[i] == b) return;
    if (s_ticked_n >= XB_TICKED_MAX) {
        XB_LOGW("no room to drive %s — it will not re-air or beacon",
                b->ops.name ? b->ops.name : "?");
        return;
    }
    s_ticked[s_ticked_n++] = b;
}

void xb_tick_all(uint32_t now_ms)
{
    for (int i = 0; i < s_ticked_n; i++) xb_tick(s_ticked[i], now_ms);
}

bool xb_has_driver(void) { return s_have_driver; }
void xb_set_driver(bool yes) { s_have_driver = yes; }

/* ── Lifecycle and observation ──────────────────────────────────────────── */

void xb_init(xb_t *b, const xb_ops_t *ops, const char *call)
{
    if (!b || !ops || !ops->air || !ops->now_ms || !ops->random) return;
    memset(b, 0, sizeof *b);
    b->ops = *ops;
    snprintf(b->call, sizeof b->call, "%s", call ? call : "");
    b->active = true;
}

void xb_stop(xb_t *b) { if (b) b->active = false; }
bool xb_is_active(const xb_t *b) { return b && b->active; }

int xb_peer_count(const xb_t *b, uint32_t max_age_sec)
{
    /* `active`, not merely `b`: ops is filled in by xb_start, so on a bearer
     * that never started every function pointer in it is NULL and
     * b->ops.now_ms() fetches instructions from nothing. Not hypothetical --
     * a T-Deck carrying espnow_on=false in its NVS logged "ESP-NOW disabled
     * by config" and then reboot-looped on InstrFetchProhibited every status
     * tick, because status_task asks each bearer for its peer count whether
     * or not that bearer is running. Turning a bearer off in config must
     * never be able to panic a station. */
    if (!b || !b->active) return 0;
    uint32_t now = b->ops.now_ms();
    int n = 0;
    for (int i = 0; i < XB_PEERS_MAX; i++) {
        if (!b->peers[i].peer) continue;
        if (max_age_sec &&
            xb_since(now, b->peers[i].t_ms) >= (int32_t)max_age_sec * 1000)
            continue;
        n++;
    }
    return n;
}

void xb_stats(const xb_t *b, uint32_t *rx, uint32_t *tx, uint32_t *cancelled)
{
    if (rx) *rx = b ? b->rx_count : 0;
    if (tx) *tx = b ? b->tx_count : 0;
    if (cancelled) *cancelled = b ? b->cancelled : 0;
}
