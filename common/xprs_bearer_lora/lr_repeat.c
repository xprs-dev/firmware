/* A repeater and nothing else (lr_repeat.h). */
#include "lr_repeat.h"

#include <string.h>

#include "lr_detect.h"
#include "mt.h"
#include "mc.h"

static int32_t since(uint32_t now, uint32_t then) { return (int32_t)(now - then); }

static bool seen_has(const lrr_t *r, uint32_t a, uint32_t b, uint32_t now)
{
    for (int i = 0; i < LRR_SEEN; i++) {
        const lrr_seen_t *s = &r->seen[i];
        if (s->t_ms && s->a == a && s->b == b && since(now, s->t_ms) < (int32_t)LRR_SEEN_MS)
            return true;
    }
    return false;
}

static void seen_add(lrr_t *r, uint32_t a, uint32_t b, uint32_t now)
{
    lrr_seen_t *s = &r->seen[r->seen_pos];
    s->a = a;
    s->b = b;
    s->t_ms = now ? now : 1;
    r->seen_pos = (r->seen_pos + 1) % LRR_SEEN;
}

/* The frame's identity: what every node of this network dedups on. */
static bool key_of(const lrr_t *r, const uint8_t *f, int len, uint32_t *a, uint32_t *b,
                   mt_hdr_t *h, mc_pkt_t *p)
{
    if (r->net == LRD_MT) {
        if (!mt_hdr_parse(f, len, h) || !h->from) return false;
        *a = h->from;
        *b = h->id;
        return true;
    }
    if (!mc_parse(f, len, p)) return false;
    *a = mc_packet_hash(p);
    *b = 0;
    return true;
}

static void q_cancel(lrr_t *r, uint32_t a, uint32_t b)
{
    for (int i = 0; i < LRR_TXQ; i++) {
        lrr_q_t *c = &r->q[i];
        if (c->used && c->a == a && c->b == b) {
            c->used = false;
            r->st.cancelled++;
        }
    }
}

static void q_push(lrr_t *r, const uint8_t *f, int len, uint32_t a, uint32_t b,
                   uint32_t now, uint32_t delay)
{
    lrr_q_t *slot = NULL;
    for (int i = 0; i < LRR_TXQ && !slot; i++)
        if (!r->q[i].used) slot = &r->q[i];
    if (!slot) {
        /* Full: the oldest relay makes room. */
        for (int i = 0; i < LRR_TXQ; i++)
            if (!slot || since(slot->queued_ms, r->q[i].queued_ms) > 0) slot = &r->q[i];
        r->st.dropped++;
    }
    memcpy(slot->frame, f, (size_t)len);
    slot->len = (uint8_t)len;
    slot->a = a;
    slot->b = b;
    slot->queued_ms = now;
    slot->due_ms = now + delay;
    slot->used = true;
    r->st.relayed++;
}

void lrr_init(lrr_t *r, uint8_t net, uint32_t self_node, const uint8_t self_hash[3])
{
    memset(r, 0, sizeof *r);
    r->net = net;
    r->self_node = self_node;
    if (self_hash) memcpy(r->self_hash, self_hash, 3);
    /* The firmware's slot: CAD (2.5 symbols) plus 7.6 ms of turnaround and
     * MAC time (mt_mesh_slot_ms, mc_mesh_slot_ms). */
    uint32_t sym_us = net == LRD_MT ? (1u << MT_LF_SF) * 1000000u / MT_LF_BW_HZ
                                    : (1u << MC_SF) * 1000000u / MC_BW_HZ;
    r->slot_ms = (sym_us * 5u / 2u + 7600u) / 1000u;
}

void lrr_flush(lrr_t *r)
{
    for (int i = 0; i < LRR_TXQ; i++) r->q[i].used = false;
}

void lrr_note(lrr_t *r, const uint8_t *f, int len, uint32_t now)
{
    uint32_t a, b;
    mt_hdr_t h;
    mc_pkt_t p;
    if (!key_of(r, f, len, &a, &b, &h, &p)) return;
    if (!seen_has(r, a, b, now)) seen_add(r, a, b, now);
}

void lrr_on_frame(lrr_t *r, const uint8_t *f, int len, int snr, uint32_t air_ms,
                  uint32_t now, uint32_t rnd)
{
    uint32_t a, b;
    mt_hdr_t h;
    mc_pkt_t p;
    if (len <= 0 || len > LRR_FRAME_MAX || !key_of(r, f, len, &a, &b, &h, &p)) return;
    r->st.heard++;
    if (seen_has(r, a, b, now)) {
        r->st.dupes++;
        q_cancel(r, a, b);
        return;
    }
    seen_add(r, a, b, now);

    if (r->net == LRD_MT) {
        if (!h.hop_limit || !h.id || h.to == r->self_node || h.from == r->self_node ||
            (h.next_hop && h.next_hop != (uint8_t)r->self_node)) {
            r->st.skipped++;
            return;
        }
        uint8_t out[LRR_FRAME_MAX];
        memcpy(out, f, (size_t)len);
        out[12] = (uint8_t)((out[12] & ~0x07) | ((h.hop_limit - 1) & 0x07));
        out[15] = (uint8_t)r->self_node;
        int cw = 3 + (snr + 20) * 5 / 30;          /* SNR -20..10 onto CW 3..8 */
        if (cw < 3) cw = 3;
        if (cw > 8) cw = 8;
        q_push(r, out, len, a, b, now, 2u * 8u * r->slot_ms + (rnd % (1u << cw)) * r->slot_ms);
        return;
    }

    uint8_t out[LRR_FRAME_MAX];
    uint32_t wait = (rnd % 6u) * (air_ms * 52u / 50u) / 2u;
    if (p.route == MC_ROUTE_DIRECT || p.route == MC_ROUTE_TRANSPORT_DIRECT) {
        if (!p.hops || p.path[0] != r->self_hash[0]) {
            r->st.skipped++;
            return;
        }
        mc_pkt_t o = p;
        o.hops = (uint8_t)(p.hops - 1);
        memmove(o.path, p.path + p.hash_size, (size_t)(o.hops * p.hash_size));
        int n = mc_build(&o, out, sizeof out);
        if (n) q_push(r, out, n, a, b, now, wait);
        return;
    }
    if (p.hops >= LRR_MC_MAX_HOPS || (p.hops + 1) * p.hash_size > MC_PATH_MAX ||
        len + p.hash_size > MC_FRAME_MAX) {
        r->st.skipped++;
        return;
    }
    for (int i = 0; i < p.hops; i++)
        if (p.path[i * p.hash_size] == r->self_hash[0]) {
            r->st.skipped++;
            return;
        }
    int n = mc_path_append(&p, r->self_hash, out, sizeof out);
    if (n) q_push(r, out, n, a, b, now, wait);
}

int lrr_due(lrr_t *r, uint32_t now)
{
    int best = -1;
    for (int i = 0; i < LRR_TXQ; i++) {
        lrr_q_t *c = &r->q[i];
        if (!c->used) continue;
        if (since(now, c->queued_ms) > (int32_t)LRR_STALE_MS) {
            c->used = false;
            r->st.dropped++;
            continue;
        }
        if (since(now, c->due_ms) < 0) continue;
        if (best < 0 || since(r->q[best].due_ms, c->due_ms) > 0) best = i;
    }
    return best;
}

void lrr_aired(lrr_t *r, int i, bool ok, uint32_t now, uint32_t rnd)
{
    if (i < 0 || i >= LRR_TXQ || !r->q[i].used) return;
    lrr_q_t *c = &r->q[i];
    if (ok) {
        c->used = false;
        r->st.aired++;
        return;
    }
    /* Busy channel or spent budget: Meshtastic retries a few slots later,
     * MeshCore after rand(1..4) x 120 ms. */
    c->due_ms = now + (r->net == LRD_MT ? r->slot_ms * (1u + rnd % 8u) : 120u * (1u + rnd % 4u));
}

bool lrr_busy(const lrr_t *r, uint32_t now)
{
    for (int i = 0; i < LRR_TXQ; i++)
        if (r->q[i].used && since(now, r->q[i].due_ms) >= -1000) return true;
    return false;
}
