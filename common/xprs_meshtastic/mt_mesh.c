/* mt_mesh.c -- the Meshtastic repeater and the XPRS bridge. See mt_mesh.h.
 *
 * Two rules to know before changing anything here:
 *
 *   - every frame this file originates on behalf of an XPRS packet has a
 *     (from, id) DERIVED from that packet, never drawn at random, so that
 *     several bridges hearing the same XPRS packet air the same frame and
 *     the second one cancels on hearing the first;
 *   - a translation back into XPRS is dated to the minute and carries
 *     zmid:, so several bridges hearing the same frame produce the same
 *     XPRS packet and the same section 5 identifier.
 */

#include "mt_mesh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xprs.h"

/* ── Small things ─────────────────────────────────────────────────────── */

static void mlog(mt_mesh_t *m, const char *fmt, ...)
{
    if (!m->ops.log) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(m->logline, sizeof m->logline, fmt, ap);
    va_end(ap);
    m->ops.log(m->ops.ctx, m->logline);
}

static inline int32_t since(uint32_t now, uint32_t then)
{
    return (int32_t)(now - then);
}

/* The first four bytes of sha256(dom || a || b), never 0. */
static uint32_t h4(mt_mesh_t *m, const char *dom, const void *a, int an,
                   const void *b, int bn)
{
    uint8_t *s = m->buf;               /* scratch; MT_FRAME_MAX bytes */
    int n = 0;
    int dl = (int)strlen(dom);
    if (dl + an + bn > MT_FRAME_MAX) {
        if (an > MT_FRAME_MAX - dl) an = MT_FRAME_MAX - dl;
        bn = MT_FRAME_MAX - dl - an;
    }
    memcpy(s, dom, (size_t)dl);
    n += dl;
    if (an > 0) { memcpy(s + n, a, (size_t)an); n += an; }
    if (bn > 0) { memcpy(s + n, b, (size_t)bn); n += bn; }
    uint8_t h[32];
    xprs_sha256(s, (size_t)n, h);
    uint32_t v = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                 ((uint32_t)h[2] << 8) | h[3];
    return v ? v : 1;
}

uint32_t mt_mesh_slot_ms(void)
{
    /* The firmware's slot: CAD (2 symbols + half a symbol, SX126x) plus
     * 7.6 ms of propagation, turnaround and MAC time. At SF11/250 kHz a
     * symbol is 8.192 ms: 2.5 x 8.192 + 7.6 = 28 ms. */
    uint32_t sym_us = (1u << MT_LF_SF) * 1000000u / MT_LF_BW_HZ;
    return (sym_us * 5u / 2u + 7600u) / 1000u;
}

/* How long a relay waits: the firmware's CLIENT rule. A faint copy came
 * from far away and is the one worth repeating, so it waits least. */
static uint32_t relay_delay(mt_mesh_t *m, int snr)
{
    int cw = 3 + (snr + 20) * 5 / 30;      /* SNR -20..10 onto CW 3..8 */
    if (cw < 3) cw = 3;
    if (cw > 8) cw = 8;
    uint32_t slot = mt_mesh_slot_ms();
    return 2u * 8u * slot + (m->ops.random() % (1u << cw)) * slot;
}

/* Our own frames go out after a short random wait: several bridges will
 * want to air the same one, and the first to speak silences the rest. */
static uint32_t own_delay(mt_mesh_t *m)
{
    return 300u + (m->ops.random() % 32u) * mt_mesh_slot_ms();
}

/* ── Rings and tables ─────────────────────────────────────────────────── */

static bool seen_has(mt_mesh_t *m, uint32_t from, uint32_t id, uint32_t now)
{
    for (int i = 0; i < MT_SEEN; i++) {
        const mt_seen_t *s = &m->seen[i];
        if (s->t_ms && s->from == from && s->id == id &&
            since(now, s->t_ms) < (int32_t)MT_SEEN_MS)
            return true;
    }
    return false;
}

static void seen_add(mt_mesh_t *m, uint32_t from, uint32_t id, uint32_t now)
{
    mt_seen_t *s = &m->seen[m->seen_pos];
    s->from = from;
    s->id = id;
    s->t_ms = now ? now : 1;
    m->seen_pos = (m->seen_pos + 1) % MT_SEEN;
}

static mt_node_t *node_get(mt_mesh_t *m, uint32_t num, bool create)
{
    mt_node_t *oldest = NULL;
    for (int i = 0; i < MT_NODES; i++) {
        mt_node_t *n = &m->nodes[i];
        if (n->num == num && n->heard_ms) return n;
        /* A free slot, else the one heard longest ago. */
        if (!oldest || (oldest->heard_ms &&
                        (!n->heard_ms || since(oldest->heard_ms, n->heard_ms) > 0)))
            oldest = n;
    }
    if (!create) return NULL;
    memset(oldest, 0, sizeof *oldest);
    oldest->num = num;
    return oldest;
}

static mt_vnode_t *vnode_by_num(mt_mesh_t *m, uint32_t num)
{
    for (int i = 0; i < MT_VNODES; i++)
        if (m->vnodes[i].seen_ms && m->vnodes[i].num == num)
            return &m->vnodes[i];
    return NULL;
}

/* Bare callsign, uppercase, into [out]. */
static int bare_upper(const char *in, int len, char *out, int cap)
{
    int n = 0;
    for (int i = 0; i < len && in[i] != '-' && n < cap - 1; i++) {
        char c = in[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        out[n++] = c;
    }
    out[n] = 0;
    return n;
}

static mt_vnode_t *vnode_learn(mt_mesh_t *m, const char *call, int len,
                               uint32_t now)
{
    char bare[12];
    if (bare_upper(call, len, bare, sizeof bare) < 3) return NULL;
    uint32_t num = mt_node_of_call(bare, (int)strlen(bare));
    mt_vnode_t *v = vnode_by_num(m, num), *spare = NULL;
    if (!v) {
        /* A free slot; else one never announced on Meshtastic, heard
         * longest ago; else the announced one heard longest ago. An
         * announced node is one somebody on Meshtastic may be writing to. */
        for (int i = 0; i < MT_VNODES; i++) {
            mt_vnode_t *c = &m->vnodes[i];
            if (c->num == m->self && c->seen_ms) continue;   /* ours, kept */
            if (!c->seen_ms) { spare = c; break; }
            if (!spare || (c->keep != spare->keep ? !c->keep
                                                  : since(spare->seen_ms, c->seen_ms) > 0))
                spare = c;
        }
        if (!spare) return NULL;
        if (spare->keep) m->vnodes_dirty = true;   /* one fewer to save */
        v = spare;
        memset(v, 0, sizeof *v);
        v->num = num;
        snprintf(v->call, sizeof v->call, "%s", bare);
    }
    v->seen_ms = now ? now : 1;
    return v;
}

static void idmap_put(mt_mesh_t *m, uint32_t mesh_id, const char *xid)
{
    mt_idmap_t *e = &m->idmap[m->idmap_pos];
    e->mesh_id = mesh_id;
    snprintf(e->xid, sizeof e->xid, "%s", xid);
    m->idmap_pos = (m->idmap_pos + 1) % MT_IDMAP;
}

static const char *idmap_xid(mt_mesh_t *m, uint32_t mesh_id)
{
    if (!mesh_id) return NULL;
    for (int i = 0; i < MT_IDMAP; i++)
        if (m->idmap[i].mesh_id == mesh_id && m->idmap[i].xid[0])
            return m->idmap[i].xid;
    return NULL;
}

static uint32_t idmap_mesh(mt_mesh_t *m, const char *xid, int xlen)
{
    for (int i = 0; i < MT_IDMAP; i++)
        if (m->idmap[i].xid[0] && (int)strlen(m->idmap[i].xid) == xlen &&
            memcmp(m->idmap[i].xid, xid, (size_t)xlen) == 0)
            return m->idmap[i].mesh_id;
    return 0;
}

static const uint8_t *key_of(mt_mesh_t *m, uint32_t num)
{
    for (int i = 0; i < MT_KEYS; i++)
        if (m->keys[i].num == num && num) return m->keys[i].pk;
    return NULL;
}

static void key_put(mt_mesh_t *m, uint32_t num, const uint8_t pk[32])
{
    for (int i = 0; i < MT_KEYS; i++) {
        if (m->keys[i].num != num) continue;
        if (memcmp(m->keys[i].pk, pk, 32) == 0) return;
        memcpy(m->keys[i].pk, pk, 32);
        m->keys_dirty = true;
        return;
    }
    mt_keyrec_t *k = &m->keys[m->keys_pos];
    m->keys_pos = (m->keys_pos + 1) % MT_KEYS;
    k->num = num;
    memcpy(k->pk, pk, 32);
    m->keys_dirty = true;
}

/* Per-hour cap on mirrored broadcasts: sixty one-minute buckets. */
static void bcast_roll(mt_mesh_t *m, uint32_t now)
{
    if (!m->bcast_head_ms) { m->bcast_head_ms = now ? now : 1; return; }
    int32_t gap = since(now, m->bcast_head_ms);
    if (gap >= 7200000) {
        memset(m->bcast_min, 0, sizeof m->bcast_min);
        m->bcast_head_ms = now ? now : 1;
        return;
    }
    while (gap >= 60000) {
        m->bcast_head = (uint8_t)((m->bcast_head + 1) % 60);
        m->bcast_min[m->bcast_head] = 0;
        m->bcast_head_ms += 60000;
        gap -= 60000;
    }
}

static bool bcast_take(mt_mesh_t *m, uint32_t now)
{
    bcast_roll(m, now);
    uint32_t sum = 0;
    for (int i = 0; i < 60; i++) sum += m->bcast_min[i];
    if (sum >= m->cfg.bcast_per_hour) return false;
    if (m->bcast_min[m->bcast_head] < 255) m->bcast_min[m->bcast_head]++;
    return true;
}

/* ── The transmit queue ───────────────────────────────────────────────── */

static bool q_push(mt_mesh_t *m, const uint8_t *frame, int len, int prio,
                   uint32_t delay, uint32_t from, uint32_t id)
{
    uint32_t now = m->ops.now_ms();
    mt_txq_t *slot = NULL;
    for (int i = 0; i < MT_TXQ && !slot; i++)
        if (!m->q[i].used) slot = &m->q[i];
    if (!slot) {
        /* Full: a relay makes room only by displacing an older relay. */
        for (int i = 0; i < MT_TXQ; i++) {
            mt_txq_t *c = &m->q[i];
            if (c->prio > prio) continue;
            if (!slot || since(slot->queued_ms, c->queued_ms) > 0) slot = c;
        }
        if (!slot) { m->st.dropped++; return false; }
        m->st.dropped++;
    }
    memcpy(slot->frame, frame, (size_t)len);
    slot->len = (uint8_t)len;
    slot->prio = (uint8_t)prio;
    slot->from = from;
    slot->id = id;
    slot->queued_ms = now;
    slot->due_ms = now + delay;
    slot->used = true;
    return true;
}

/* Somebody else aired (from, id): whatever we had queued for it is moot. */
static bool q_cancel(mt_mesh_t *m, uint32_t from, uint32_t id)
{
    bool any = false;
    for (int i = 0; i < MT_TXQ; i++) {
        mt_txq_t *c = &m->q[i];
        if (!c->used || c->from != from || c->id != id) continue;
        c->used = false;
        any = true;
        if (c->prio == MT_PRIO_RELAY) m->st.relay_cancelled++;
    }
    /* A DM another bridge put on the air is theirs to see acknowledged. */
    for (int i = 0; i < MT_PENDING; i++) {
        mt_pending_t *p = &m->pend[i];
        if (p->used && !p->aired && p->from == from && p->id == id)
            p->used = false;
    }
    return any;
}

/* ── Frames we originate ──────────────────────────────────────────────── */

/* Build an encrypted LongFast frame into m->frame. Returns its length. */
static int lf_frame(mt_mesh_t *m, uint32_t to, uint32_t from, uint32_t id,
                    bool want_ack, const mt_data_t *d)
{
    mt_hdr_t h = { 0 };
    h.to = to;
    h.from = from;
    h.id = id;
    h.hop_limit = MT_HOP_DEFAULT;
    h.hop_start = MT_HOP_DEFAULT;
    h.want_ack = want_ack;
    h.channel = m->lf_hash;
    h.relay_node = (uint8_t)m->self;
    mt_hdr_build(&h, m->frame);
    int n = mt_data_encode(d, m->frame + MT_HDR_LEN, MT_FRAME_MAX - MT_HDR_LEN);
    if (n < 0) return 0;
    if (!mt_crypt(mt_default_key, 16, from, id, m->frame + MT_HDR_LEN, n))
        return 0;
    return MT_HDR_LEN + n;
}

/* The NodeInfo for one of our nodes, to [to] (broadcast, or whoever asked).
 * [ask]: want_response, so a node whose key we lack answers with its own. */
static void nodeinfo_queue(mt_mesh_t *m, mt_vnode_t *v, uint32_t to, bool ask)
{
    mt_user_t u;
    memset(&u, 0, sizeof u);
    mt_user_id_of(v->num, u.id);
    char nick[20] = "";
    if (v->num == m->self) snprintf(nick, sizeof nick, "%s", m->nick);
    else if (m->ops.nick_of) m->ops.nick_of(m->ops.ctx, v->call, nick, sizeof nick);
    /* The callsign always shows, because it is the address: a Meshtastic
     * user reading "joao X1QZ3N" can say which one they meant. It is also
     * how another bridge recognises this node as ours (mt_mesh_on_frame). */
    if (nick[0])
        snprintf(u.long_name, sizeof u.long_name, "%.*s %s",
                 (int)(sizeof u.long_name - 2 - strlen(v->call)), nick, v->call);
    else
        snprintf(u.long_name, sizeof u.long_name, "%s", v->call);
    int cl = (int)strlen(v->call);
    snprintf(u.short_name, sizeof u.short_name, "%s",
             v->call + (cl > 4 ? cl - 4 : 0));
    u.hw_model = MT_HW_PRIVATE;
    /* The DERIVED key (mt_node_keys): every bridge presents the same one,
     * and Meshtastic keeps the first key it hears for a node. */
    uint8_t priv[32];
    mt_node_keys(v->call, (int)strlen(v->call), priv, u.public_key);
    u.has_public_key = true;

    uint8_t pl[128];
    int pn = mt_user_encode(&u, pl, sizeof pl);
    if (pn < 0) return;
    mt_data_t d = { 0 };
    d.portnum = MT_PORT_NODEINFO;
    d.payload = pl;
    d.payload_len = pn;
    d.want_response = ask;
    uint32_t key[2] = { to, ask };
    uint32_t id = h4(m, "XPRS/mt/ni", u.long_name, (int)strlen(u.long_name),
                     key, (int)sizeof key);
    uint32_t now = m->ops.now_ms();
    if (seen_has(m, v->num, id, now)) return;   /* another bridge said it */
    int n = lf_frame(m, to, v->num, id, false, &d);
    if (!n) return;
    seen_add(m, v->num, id, now);
    if (q_push(m, m->frame, n, MT_PRIO_OWN, own_delay(m) / 2, v->num, id)) {
        if (!v->keep && v->num != m->self) {
            v->keep = true;
            m->vnodes_dirty = true;
        }
        v->announced_ms = now ? now : 1;
        m->st.nodeinfo_out++;
    }
}

static void nodeinfo_if_stale(mt_mesh_t *m, mt_vnode_t *v)
{
    uint32_t now = m->ops.now_ms();
    uint32_t period = (uint32_t)m->cfg.nodeinfo_min * 60000u;
    if (!v->announced_ms || since(now, v->announced_ms) > (int32_t)period)
        nodeinfo_queue(m, v, MT_BROADCAST, false);
}

/* Seal a waiting DM to its recipient's key and queue it; without the key,
 * ask the recipient for its NodeInfo and wait (the DM goes when the key
 * arrives, identity_in / decoded_in). */
static bool pend_air(mt_mesh_t *m, mt_pending_t *p)
{
    const uint8_t *pk = key_of(m, p->to);
    mt_vnode_t *v = vnode_by_num(m, p->from);
    if (!v) return false;
    if (!pk) {
        if (!p->asked_key) {
            p->asked_key = true;
            m->st.key_asks++;
            nodeinfo_queue(m, v, p->to, true);
            mlog(m, "mt: no key for %08x yet -- asked; DM %s waits",
                 (unsigned)p->to, p->xid);
        }
        return false;
    }
    mt_hdr_t h = { 0 };
    h.to = p->to;
    h.from = p->from;
    h.id = p->id;
    h.hop_limit = MT_HOP_DEFAULT;
    h.hop_start = MT_HOP_DEFAULT;
    h.want_ack = true;
    h.channel = 0;                        /* PKI, not a channel */
    h.relay_node = (uint8_t)m->self;
    mt_hdr_build(&h, m->frame);
    uint8_t priv[32];
    mt_node_keys(v->call, (int)strlen(v->call), priv, NULL);
    int n = mt_pki_encrypt(priv, pk, p->from, p->id, m->ops.random(),
                           p->data, p->dlen, m->frame + MT_HDR_LEN,
                           MT_FRAME_MAX - MT_HDR_LEN);
    if (n < 0) return false;
    /* A node that has only just been announced (the tick queues its NodeInfo
     * right before this) speaks after it: a DM that overtakes the key it is
     * sealed under cannot be opened, and the recipient then holds its id as
     * seen, so no retry of it ever gets through (2026-09-19, the desktop's
     * first DM to the witness). */
    uint32_t now = m->ops.now_ms();
    uint32_t delay = own_delay(m);
    if (!p->aired && v->announced_ms && since(now, v->announced_ms) < 10000)
        delay += MT_AFTER_NODEINFO_MS;
    return q_push(m, m->frame, MT_HDR_LEN + n, MT_PRIO_OWN, delay,
                  p->from, p->id);
}

/* A Routing ack from one of our nodes for something it was sent with
 * want_ack, [hop] hops back. The id is derived from what it acknowledges,
 * so a re-ack is the same frame and every bridge sends the same one.
 * [again]: send it even though it was sent before (the sender repeated
 * itself, so the first one did not arrive). */
static void routing_queue(mt_mesh_t *m, uint32_t from_node, const mt_hdr_t *h,
                          int hop, bool again, int err)
{
    uint8_t pl[4];
    int pn = mt_routing_encode(pl, sizeof pl, err);
    mt_data_t d = { 0 };
    d.portnum = MT_PORT_ROUTING;
    d.payload = pl;
    d.payload_len = pn;
    d.request_id = h->id;
    uint32_t key[2] = { h->from, h->id };
    uint32_t id = h4(m, err ? "XPRS/mt/nak" : "XPRS/mt/ack", key,
                     (int)sizeof key, NULL, 0);
    uint32_t now = m->ops.now_ms();
    if (!again && seen_has(m, from_node, id, now)) return;
    for (int i = 0; i < MT_TXQ; i++)
        if (m->q[i].used && m->q[i].from == from_node && m->q[i].id == id)
            return;                       /* already on its way */
    int n = lf_frame(m, h->from, from_node, id, false, &d);
    if (!n) return;
    int back = hop < 0 ? 0 : hop > MT_HOP_MAX ? MT_HOP_MAX : hop;
    m->frame[12] = (uint8_t)((m->frame[12] & ~0xE7) | (back & 7) | ((back & 7) << 5));
    seen_add(m, from_node, id, now);
    q_push(m, m->frame, n, MT_PRIO_OWN, own_delay(m) / 2, from_node, id);
}

static void ack_queue(mt_mesh_t *m, uint32_t from_node, const mt_hdr_t *h,
                      int hop, bool again)
{
    routing_queue(m, from_node, h, hop, again, 0);
}

/* ── Meshtastic to XPRS ───────────────────────────────────────────────── */

static int stamp(mt_mesh_t *m, char *out, int cap)
{
    return m->ops.stamp ? m->ops.stamp(m->ops.ctx, out, cap, true) : 0;
}

/* One wire to the station. */
static void deliver(mt_mesh_t *m, const char *wire, int len, bool sign)
{
    if (m->ops.deliver) m->ops.deliver(m->ops.ctx, wire, len, sign);
}

/* Is [t] one of the two tapbacks XPRS has a word for? (section 28 assigns
 * add:like and add:repost; a heart and a thumb are both a like.) */
static bool is_like(const uint8_t *t, int n)
{
    static const uint8_t thumb[] = { 0xF0, 0x9F, 0x91, 0x8D };
    static const uint8_t heart[] = { 0xE2, 0x9D, 0xA4 };
    return (n >= 4 && memcmp(t, thumb, 4) == 0) ||
           (n >= 3 && memcmp(t, heart, 3) == 0);
}

/* A Meshtastic text into one or more XPRS packets. [dst] is the XPRS
 * callsign a DM was for, NULL for the public channel. */
static void text_in(mt_mesh_t *m, const mt_hdr_t *h, const mt_data_t *d,
                    const char *dst)
{
    char from[12];
    mt_call_of_node(h->from, from, sizeof from);
    char ts[40];
    int tn = stamp(m, ts, sizeof ts);
    /* No consent to the internet without the flag the Meshtastic user set:
     * scope:local keeps it on the short-range bearers (XPRS.md 9.11.1). */
    bool local = !(d->has_bitfield && (d->bitfield & MT_BITFIELD_OK_TO_MQTT));
    const char *rx = idmap_xid(m, d->reply_id);
    char env[160];
    int en;

    if (d->emoji && d->reply_id) {
        if (!rx || !is_like(d->payload, d->payload_len)) return;
        en = snprintf(env, sizeof env,
                      "t:reaction f:%s%s%s%s%.*s%s r:%s zmid:%08x%08x via:%s add:like",
                      from, dst ? " d:" : "", dst ? dst : "", tn ? " " : "",
                      tn, ts, local ? " scope:local" : "", rx,
                      (unsigned)h->from, (unsigned)h->id, m->call);
        if (en > 0 && en <= XPRS_MAX_WIRE) {
            deliver(m, env, en, false);
            m->st.text_in++;
        }
        return;
    }

    /* The text: what a packet may carry in m: (no line breaks, no NUL). */
    int n = 0;
    for (int i = 0; i < d->payload_len && n < (int)sizeof m->text - 1; i++) {
        char c = (char)d->payload[i];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == 0) continue;
        m->text[n++] = c;
    }
    while (n > 0 && m->text[n - 1] == ' ') n--;
    m->text[n] = 0;
    if (!n) return;

    char rpart[16] = "";
    if (rx) snprintf(rpart, sizeof rpart, " r:%s", rx);
    en = snprintf(env, sizeof env, "t:message f:%s%s%s%s%.*s%s%s zmid:%08x%08x",
                  from, dst ? " d:" : "", dst ? dst : "", tn ? " " : "", tn, ts,
                  local ? " scope:local" : "", rpart,
                  (unsigned)h->from, (unsigned)h->id);
    if (en <= 0 || en >= (int)sizeof env) return;
    char via[24];
    int vn = snprintf(via, sizeof via, " via:%s", m->call);

    int whole = en + vn + 3 + n;
    if (whole <= XPRS_MAX_WIRE) {
        int wl = snprintf(m->wire, sizeof m->wire, "%s%s m:%s", env, via, m->text);
        deliver(m, m->wire, wl, false);
        char xid[XPRS_ID_LEN];
        if (xprs_id_of(m->wire, wl, xid)) idmap_put(m, h->id, xid);
        m->st.text_in++;
        return;
    }

    /* Too long for one packet: split at spaces (XPRS.md 7.6), every field
     * repeated on every part. " n:1/9" is six bytes. */
    int room = XPRS_MAX_WIRE - (en + 6 + vn + 3);
    if (room < 16) return;
    int starts[9], lens[9], parts = 0, pos = 0;
    while (pos < n && parts < 9) {
        int take = n - pos;
        if (take > room) {
            take = room;
            for (int k = room; k > room / 2; k--)
                if (m->text[pos + k] == ' ') { take = k; break; }
        }
        starts[parts] = pos;
        lens[parts] = take;
        parts++;
        pos += take;
        while (pos < n && m->text[pos] == ' ') pos++;
    }
    for (int i = 0; i < parts; i++) {
        int wl = snprintf(m->wire, sizeof m->wire, "%s n:%d/%d%s m:%.*s", env,
                          i + 1, parts, via, lens[i], m->text + starts[i]);
        deliver(m, m->wire, wl, false);
    }
    m->st.text_in++;
}

/* A NodeInfo into an unsigned identity, when it says something new. */
static void identity_in(mt_mesh_t *m, const mt_hdr_t *h, mt_node_t *nd,
                        const mt_user_t *u, bool ok_to_mqtt)
{
    uint32_t now = m->ops.now_ms();
    bool changed = strcmp(nd->long_name, u->long_name) != 0;
    snprintf(nd->long_name, sizeof nd->long_name, "%s", u->long_name);
    snprintf(nd->short_name, sizeof nd->short_name, "%s", u->short_name);
    if (!changed && nd->identity_ms &&
        since(now, nd->identity_ms) < 6 * 3600 * 1000)
        return;
    char nick[20];
    if (!mt_nick_from_name(u->long_name, nick, sizeof nick) &&
        !mt_nick_from_name(u->short_name, nick, sizeof nick))
        return;
    char from[12], ts[40];
    mt_call_of_node(h->from, from, sizeof from);
    int tn = stamp(m, ts, sizeof ts);
    int wl = snprintf(m->wire, sizeof m->wire,
                      "t:identity f:%s%s%.*s%s nick:%s zmid:%08x%08x via:%s",
                      from, tn ? " " : "", tn, ts,
                      ok_to_mqtt ? "" : " scope:local", nick,
                      (unsigned)h->from, (unsigned)h->id, m->call);
    if (wl > 0 && wl <= XPRS_MAX_WIRE) {
        deliver(m, m->wire, wl, false);
        nd->identity_ms = now ? now : 1;
    }
}

/* Does [name] carry a callsign whose node number is [num]? That is the
 * mark of a virtual node another bridge announced (nodeinfo_queue). */
static bool names_our_node(const char *name, uint32_t num)
{
    const char *p = name;
    while (*p) {
        while (*p == ' ') p++;
        const char *s = p;
        while (*p && *p != ' ') p++;
        int n = (int)(p - s);
        if (n >= 3 && n <= 11 && mt_node_of_call(s, n) == num) return true;
    }
    return false;
}

/* The receipt a gateway gives for a DM a Meshtastic node acknowledged,
 * signed by this station (docs/lora.md: a gateway receipt). */
static void receipt_out(mt_mesh_t *m, const mt_pending_t *p, bool ok, int err)
{
    char ts[40];
    int tn = stamp(m, ts, sizeof ts);
    int wl;
    if (ok)
        wl = snprintf(m->wire, sizeof m->wire, "t:receipt f:%s d:%s%s%.*s r:%s s:ack",
                      m->call, p->sender, tn ? " " : "", tn, ts, p->xid);
    else
        wl = snprintf(m->wire, sizeof m->wire,
                      "t:receipt f:%s d:%s%s%.*s r:%s s:no m:Meshtastic said %d",
                      m->call, p->sender, tn ? " " : "", tn, ts, p->xid, err);
    if (wl > 0 && wl <= XPRS_MAX_WIRE) {
        deliver(m, m->wire, wl, true);
        m->st.receipts++;
    }
}

static void decoded_in(mt_mesh_t *m, const mt_hdr_t *h, const mt_data_t *d,
                       int snr)
{
    uint32_t now = m->ops.now_ms();
    mt_vnode_t *ours_to = h->to == MT_BROADCAST ? NULL : vnode_by_num(m, h->to);
    bool from_ours = h->from == m->self || vnode_by_num(m, h->from) != NULL;
    mt_node_t *nd = NULL;
    if (!from_ours) {
        nd = node_get(m, h->from, true);
        nd->heard_ms = now ? now : 1;
        nd->snr = (int8_t)snr;
        if (nd->xprs) from_ours = true;
    }

    switch (d->portnum) {
    case MT_PORT_TEXT:
        if (from_ours || !m->cfg.bridge) return;
        if (h->to != MT_BROADCAST && !ours_to) return;   /* somebody else's DM */
        /* A direct message is an exchange somebody is waiting on, and a
         * radio shared with another network should not walk out of it
         * (mt_mesh_busy). A broadcast is not: the channel is never quiet. */
        if (ours_to) m->last_dm_ms = m->ops.now_ms();
        /* text_in first: d->payload lives in m->frame, which the ack's
         * frame is built in. */
        text_in(m, h, d, ours_to ? ours_to->call : NULL);
        break;

    case MT_PORT_NODEINFO: {
        mt_user_t u;
        if (!mt_user_decode(d->payload, d->payload_len, &u)) return;
        if (ours_to && d->want_response) nodeinfo_queue(m, ours_to, h->from, false);
        if (from_ours || !nd) return;
        if (names_our_node(u.long_name, h->from)) {
            nd->xprs = true;             /* another bridge's virtual node */
            return;
        }
        if (u.has_public_key) {
            key_put(m, h->from, u.public_key);
            for (int i = 0; i < MT_PENDING; i++) {
                mt_pending_t *p = &m->pend[i];
                if (p->used && !p->aired && p->to == h->from) pend_air(m, p);
            }
        }
        if (m->cfg.bridge)
            identity_in(m, h, nd, &u, d->has_bitfield &&
                                      (d->bitfield & MT_BITFIELD_OK_TO_MQTT));
        break;
    }

    case MT_PORT_ROUTING: {
        if (!d->request_id) return;
        int err = 0;
        if (!mt_routing_decode(d->payload, d->payload_len, &err)) return;
        for (int i = 0; i < MT_PENDING; i++) {
            mt_pending_t *p = &m->pend[i];
            if (!p->used || p->id != d->request_id || p->from != h->to) continue;
            if (err == MT_ERR_PKI_UNKNOWN_PUBKEY && !p->rekeyed) {
                /* The recipient could not open it: it has no key for our
                 * node. Tell it the key, then say it again under a new id,
                 * because the recipient holds the old one as seen and would
                 * drop a repeat of it. Derived, so every bridge picks the
                 * same new id. Once: a second 35 is an answer. */
                mt_vnode_t *v = vnode_by_num(m, p->from);
                if (v) nodeinfo_queue(m, v, h->from, false);
                uint32_t k[2] = { p->from, p->id };
                p->id = h4(m, "XPRS/mt/dm-again", k, (int)sizeof k, NULL, 0);
                seen_add(m, p->from, p->id, now);
                idmap_put(m, p->id, p->xid);
                p->rekeyed = true;
                p->aired = false;
                p->tries = 0;
                p->due = true;
                m->st.dm_rekeyed++;
                mlog(m, "mt: %08x has no key for %08x -- told it; DM %s again as %08x",
                     (unsigned)h->from, (unsigned)p->from, p->xid, (unsigned)p->id);
                continue;
            }
            p->used = false;
            m->last_dm_ms = m->ops.now_ms();
            m->st.dm_acked++;
            mlog(m, "mt: %08x acked DM %s from %s (err %d)",
                 (unsigned)h->from, p->xid, p->sender, err);
            receipt_out(m, p, err == 0, err);
        }
        break;
    }
    default:
        break;
    }

    /* The firmware's reliability rule (ReliableRouter): a packet to one of
     * our nodes that wants an ack gets one -- back as far as it came -- and
     * an ack or a reply that itself wants one (a DM's ack does) gets a
     * zero-hop ack when it came straight from its sender. Without the
     * second, the witness retried its ack three times and reported
     * MAX_RETRANSMIT for a DM that had been delivered (2026-09-19). */
    if (ours_to && h->want_ack) {
        bool response = d->request_id || d->reply_id;
        int hops_away = h->hop_start - h->hop_limit;
        if (!response)
            ack_queue(m, ours_to->num, h, h->hop_start ? h->hop_start : MT_HOP_DEFAULT,
                      false);
        else if (hops_away == 0 || h->next_hop)
            ack_queue(m, ours_to->num, h, 0, false);
    }
}

void mt_mesh_on_frame(mt_mesh_t *m, const uint8_t *frame, int len, int rssi,
                      int snr)
{
    (void)rssi;
    mt_hdr_t h;
    if (!m || !mt_hdr_parse(frame, len, &h) || !h.from) return;
    m->st.rx_frames++;
    uint32_t now = m->ops.now_ms();

    if (seen_has(m, h.from, h.id, now)) {
        m->st.rx_dupes++;
        /* Somebody relayed it, or another bridge aired the frame we were
         * about to: either way ours adds nothing (the firmware's CLIENT
         * rule, and XPRS 9.2.1's). */
        q_cancel(m, h.from, h.id);
        /* But the original sender repeating a reliable packet to one of our
         * nodes means our ack was lost: send it again. */
        mt_vnode_t *rv = h.to != MT_BROADCAST ? vnode_by_num(m, h.to) : NULL;
        if (rv && h.want_ack && h.hop_start && h.hop_start == h.hop_limit) {
            uint32_t key[2] = { h.from, h.id };
            uint32_t aid = h4(m, "XPRS/mt/ack", key, (int)sizeof key, NULL, 0);
            if (seen_has(m, rv->num, aid, now))   /* we did ack it once */
                ack_queue(m, rv->num, &h, h.hop_start, true);
        }
        return;
    }
    seen_add(m, h.from, h.id, now);

    /* Parked DMs go the moment their recipient is heard (XPRS.md 12.8.1). */
    for (int i = 0; i < MT_PENDING; i++) {
        mt_pending_t *p = &m->pend[i];
        if (p->used && p->to == h.from && p->tries >= MT_DM_TRIES &&
            since(now, p->sent_ms) > 60000) {
            p->tries = MT_DM_TRIES - 1;          /* one more go */
            pend_air(m, p);
        }
    }

    bool to_ours = h.to == m->self || (h.to != MT_BROADCAST &&
                                       vnode_by_num(m, h.to));
    if (h.channel == m->lf_hash && len > MT_HDR_LEN) {
        int pn = len - MT_HDR_LEN;
        memcpy(m->buf, frame + MT_HDR_LEN, (size_t)pn);
        mt_data_t d;
        if (mt_crypt(mt_default_key, 16, h.from, h.id, m->buf, pn) &&
            mt_data_decode(m->buf, pn, &d)) {
            m->st.rx_decoded++;
            /* decoded_in may reuse m->buf through h4(); the Data points
             * into it, so the payload is moved to m->frame first. */
            if (d.payload_len > 0 && d.payload_len <= MT_FRAME_MAX) {
                memcpy(m->frame, d.payload, (size_t)d.payload_len);
                d.payload = m->frame;
            }
            decoded_in(m, &h, &d, snr);
        }
    } else if (h.channel == 0 && to_ours && len > MT_HDR_LEN + MT_PKI_OVERHEAD) {
        /* A DM to one of our nodes, sealed to its key. */
        mt_vnode_t *v = vnode_by_num(m, h.to);
        const uint8_t *spk = key_of(m, h.from);
        if (v && spk) {
            uint8_t priv[32];
            mt_node_keys(v->call, (int)strlen(v->call), priv, NULL);
            int pn = mt_pki_decrypt(priv, spk, h.from, h.id,
                                    frame + MT_HDR_LEN, len - MT_HDR_LEN, m->buf);
            mt_data_t d;
            if (pn > 0 && mt_data_decode(m->buf, pn, &d)) {
                m->st.pki_in++;
                if (d.payload_len > 0 && d.payload_len <= MT_FRAME_MAX) {
                    memcpy(m->frame, d.payload, (size_t)d.payload_len);
                    d.payload = m->frame;
                }
                decoded_in(m, &h, &d, snr);
            } else {
                m->st.pki_fail++;
                mlog(m, "mt: DM %08x from %08x to %s does not open",
                     (unsigned)h.id, (unsigned)h.from, v->call);
            }
        } else if (v) {
            /* We do not know the sender's key. Say so the firmware's way,
             * PKI_UNKNOWN_PUBKEY: the sender answers it with its NodeInfo at
             * once (a NodeInfo REQUEST it answers only once in 12 hours),
             * and its user sees why the DM failed and can send it again. */
            m->st.key_asks++;
            if (h.want_ack)
                routing_queue(m, v->num, &h, h.hop_start ? h.hop_start
                                                          : MT_HOP_DEFAULT,
                              false, MT_ERR_PKI_UNKNOWN_PUBKEY);
            else
                nodeinfo_queue(m, v, h.from, true);
            mlog(m, "mt: DM from %08x to %s, key unknown -- said so",
                 (unsigned)h.from, v->call);
        }
    }

    /* The repeater. Frames are relayed whether or not we could read them,
     * as every Meshtastic router does. */
    if (!m->cfg.repeat) return;
    if (!h.hop_limit || !h.id || to_ours || h.from == m->self) {
        m->st.relay_skipped++;
        return;
    }
    if (h.next_hop && h.next_hop != (uint8_t)m->self) {
        m->st.relay_skipped++;
        return;
    }
    uint8_t *cp = (uint8_t *)m->wire;     /* scratch, >= MT_FRAME_MAX */
    memcpy(cp, frame, (size_t)len);
    cp[12] = (uint8_t)((cp[12] & ~0x07) | ((h.hop_limit - 1) & 0x07));
    cp[15] = (uint8_t)m->self;
    if (q_push(m, cp, len, MT_PRIO_RELAY, relay_delay(m, snr), h.from, h.id))
        m->st.relayed++;
}

void mt_mesh_note_xprs_frame(mt_mesh_t *m, const uint8_t *frame, int len)
{
    mt_hdr_t h;
    if (!m || !mt_hdr_parse(frame, len, &h)) return;
    uint32_t now = m->ops.now_ms();
    if (!seen_has(m, h.from, h.id, now)) seen_add(m, h.from, h.id, now);
}

/* ── XPRS to Meshtastic ───────────────────────────────────────────────── */

static bool get(const xprs_t *p, const char *k, const char **v, int *n)
{
    *v = xprs_get(p, k, n);
    return *v && *n > 0;
}

/* ts:YYYY-MM-DD_HH:MM:SS to seconds since 1970 (0 when it is not one). */
static uint32_t ts_epoch(const char *v, int n)
{
    if (n != 19 || v[4] != '-' || v[7] != '-' || v[10] != '_') return 0;
    int f[6], pos[6] = { 0, 5, 8, 11, 14, 17 }, len[6] = { 4, 2, 2, 2, 2, 2 };
    for (int i = 0; i < 6; i++) {
        f[i] = 0;
        for (int k = 0; k < len[i]; k++) {
            char c = v[pos[i] + k];
            if (c < '0' || c > '9') return 0;
            f[i] = f[i] * 10 + (c - '0');
        }
    }
    /* days from civil (Howard Hinnant's algorithm) */
    int y = f[0] - (f[1] <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int mp = (f[1] + 9) % 12;
    int doy = (153 * mp + 2) / 5 + f[2] - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400L + f[3] * 3600L + f[4] * 60L + f[5]);
}

void mt_mesh_on_xprs(mt_mesh_t *m, const char *wire, int len, int origin)
{
    if (!m || !wire || len <= 0 || len > XPRS_MAX_WIRE) return;
    xprs_t *p = &m->xp;
    if (!xprs_parse(wire, len, p)) return;
    const char *f;
    int fn, vn;
    if (!get(p, "f", &f, &fn)) return;
    uint32_t mtnode;
    /* What came from the mesh never goes back to it, whoever translated it. */
    if (mt_node_of_mtcall(f, fn, &mtnode) || xprs_get(p, "zmid", &vn)) return;
    if (!xprs_is_station(f, fn)) return;

    uint32_t now = m->ops.now_ms();
    mt_vnode_t *sv = vnode_learn(m, f, fn, now);
    if (!sv || !m->cfg.bridge) return;

    char type[16];
    xprs_type(p, type, sizeof type);
    bool msg = strcmp(type, "message") == 0;
    bool react = strcmp(type, "reaction") == 0;
    if (!msg && !react) return;

    const char *dv;
    int dn = 0;
    uint32_t to = MT_BROADCAST;
    if (get(p, "d", &dv, &dn)) {
        if (!mt_node_of_mtcall(dv, dn, &to)) return;   /* XPRS-only address */
        /* A DM goes onto this radio only for a node this bridge has heard
         * on it (since boot, or its key, learned here and kept), or when
         * one of this station's own users handed it over. A reply sent
         * from far away reaches every bridge on the internet; without this
         * each of them would air it, and ask for the key of a node that is
         * nowhere near, on a channel it shares with everybody. The bridge
         * that hears the node is the one that delivers. */
        if (origin != MT_XPRS_OWN && !node_get(m, to, false) && !key_of(m, to)) {
            m->st.dm_not_here++;
            return;
        }
    }
    char xid[XPRS_ID_LEN];
    xprs_id(p, xid);
    /* Heard again (an echo, a digipeat, a history replay): it is on the
     * mesh already, or waiting to be. */
    if (idmap_mesh(m, xid, (int)strlen(xid))) return;
    /* Or old news, which after a restart this bridge cannot tell from new. */
    const char *tv;
    int tn2;
    uint32_t utc = m->ops.utc_now ? m->ops.utc_now(m->ops.ctx) : 0;
    if (utc && get(p, "ts", &tv, &tn2)) {
        uint32_t t = ts_epoch(tv, tn2);
        uint32_t fresh = to == MT_BROADCAST ? MT_BCAST_FRESH_S : MT_DM_FRESH_S;
        if (t && utc > t && utc - t > fresh) return;
    }

    if (xprs_get(p, "x", &vn)) {
        /* A sealed body cannot cross: Meshtastic cannot open section 6.2's
         * cipher, and a bridge that could would be reading the mail. Said
         * out loud only to the station that handed it to us. */
        if (to != MT_BROADCAST && origin == MT_XPRS_OWN) {
            char ts[40];
            int tn = stamp(m, ts, sizeof ts);
            int wl = snprintf(m->wire, sizeof m->wire,
                              "t:receipt f:%s d:%s%s%.*s r:%s s:no m:sealed, "
                              "Meshtastic cannot open it",
                              m->call, sv->call, tn ? " " : "", tn, ts, xid);
            if (wl > 0 && wl <= XPRS_MAX_WIRE) deliver(m, m->wire, wl, true);
        }
        return;
    }
    if (xprs_scope_local(p)) return;       /* never onto a radio band */

    mt_data_t d = { 0 };
    d.portnum = MT_PORT_TEXT;
    const char *sc;
    int scn;
    bool global = !get(p, "scope", &sc, &scn) ||
                  (scn == 6 && memcmp(sc, "global", 6) == 0);
    d.has_bitfield = true;
    d.bitfield = global ? MT_BITFIELD_OK_TO_MQTT : 0;
    const char *rv;
    int rn;
    if (get(p, "r", &rv, &rn)) d.reply_id = idmap_mesh(m, rv, rn);

    if (react) {
        const char *av;
        int an;
        if (!get(p, "add", &av, &an) || an != 4 || memcmp(av, "like", 4) ||
            !d.reply_id)
            return;
        static const uint8_t thumb[] = { 0xF0, 0x9F, 0x91, 0x8D };
        d.payload = thumb;
        d.payload_len = 4;
        d.emoji = 1;
    } else {
        const char *mv;
        int mn;
        if (!get(p, "m", &mv, &mn)) return;
        /* Room for the text in one frame: 239 bytes after the header, less
         * the Data's own fields (at most 17 here). */
        if (mn > 220) mn = 220;
        d.payload = (const uint8_t *)mv;
        d.payload_len = mn;
    }

    if (to == MT_BROADCAST && !bcast_take(m, now)) {
        m->st.bcast_capped++;
        return;
    }

    int tl = xprs_signed_text(p, m->text, sizeof m->text);
    if (tl < 0) return;
    uint32_t id = h4(m, "XPRS/mt/text", m->text, tl, NULL, 0);
    if (seen_has(m, sv->num, id, now)) return;   /* another bridge did it */

    sv->announce_due = true;             /* nodeinfo_if_stale, on the tick */
    idmap_put(m, id, xid);
    m->st.text_out++;
    mlog(m, "mt: %s %s -> %08x as %08x/%08x (%s)", react ? "like" : "text",
         xid, (unsigned)to, (unsigned)sv->num, (unsigned)id, sv->call);

    if (to == MT_BROADCAST) {
        int n = lf_frame(m, to, sv->num, id, false, &d);
        if (!n) return;
        seen_add(m, sv->num, id, now);
        q_push(m, m->frame, n, MT_PRIO_OWN, own_delay(m), sv->num, id);
        return;
    }
    /* A DM: sealed to the recipient's key (mt_pki), watched for its ack,
     * which becomes the gateway receipt. */
    mt_pending_t *slot = NULL;
    for (int i = 0; i < MT_PENDING; i++) {
        mt_pending_t *c = &m->pend[i];
        if (!c->used) { slot = c; break; }
        if (!slot || since(slot->created_ms, c->created_ms) > 0) slot = c;
    }
    memset(slot, 0, sizeof *slot);
    int dl = mt_data_encode(&d, slot->data, (int)sizeof slot->data);
    if (dl < 0) return;
    slot->used = true;
    slot->dlen = (uint8_t)dl;
    slot->to = to;
    slot->from = sv->num;
    slot->id = id;
    slot->created_ms = now ? now : 1;
    snprintf(slot->sender, sizeof slot->sender, "%s", sv->call);
    snprintf(slot->xid, sizeof slot->xid, "%s", xid);
    seen_add(m, sv->num, id, now);
    slot->due = true;                    /* pend_air, on the tick */
}

/* ── The clock ────────────────────────────────────────────────────────── */

void mt_mesh_tick(mt_mesh_t *m, uint32_t now)
{
    if (!m) return;

    /* Our own node says who it is, soon after boot and then on the period;
     * a node about to speak for an XPRS callsign says who it is first. */
    mt_vnode_t *own = vnode_by_num(m, m->self);
    if (own && since(now, m->boot_ms) > 20000) nodeinfo_if_stale(m, own);
    for (int i = 0; i < MT_VNODES; i++) {
        mt_vnode_t *v = &m->vnodes[i];
        if (!v->announce_due) continue;
        v->announce_due = false;
        nodeinfo_if_stale(m, v);
    }
    for (int i = 0; i < MT_PENDING; i++) {
        mt_pending_t *p = &m->pend[i];
        if (!p->used || !p->due) continue;
        p->due = false;
        pend_air(m, p);
    }

    if (m->vnodes_dirty && m->ops.vnodes_save &&
        (!m->vnodes_saved_ms || since(now, m->vnodes_saved_ms) > 60000)) {
        m->vnodes_dirty = false;
        m->vnodes_saved_ms = now ? now : 1;
        char calls[MT_VNODES][MT_CALL_LEN];
        int n = 0;
        memset(calls, 0, sizeof calls);
        for (int i = 0; i < MT_VNODES; i++)
            if (m->vnodes[i].seen_ms && m->vnodes[i].keep &&
                m->vnodes[i].num != m->self)
                memcpy(calls[n++], m->vnodes[i].call, MT_CALL_LEN);
        m->ops.vnodes_save(m->ops.ctx, calls, n * MT_CALL_LEN);
    }

    if (m->keys_dirty && m->ops.keys_save &&
        (!m->keys_saved_ms || since(now, m->keys_saved_ms) > 60000)) {
        m->keys_dirty = false;
        m->keys_saved_ms = now ? now : 1;
        m->ops.keys_save(m->ops.ctx, m->keys, (int)sizeof m->keys);
    }

    /* DMs waiting for their ack. */
    for (int i = 0; i < MT_PENDING; i++) {
        mt_pending_t *p = &m->pend[i];
        if (!p->used) continue;
        if (since(now, p->created_ms) > (int32_t)MT_DM_PARK_MS) {
            p->used = false;
            continue;
        }
        if (!p->aired || p->tries >= MT_DM_TRIES) continue;
        if (since(now, p->sent_ms) < (int32_t)MT_DM_RETRY_MS) continue;
        bool queued = false;
        for (int k = 0; k < MT_TXQ; k++)
            if (m->q[k].used && m->q[k].from == p->from && m->q[k].id == p->id)
                queued = true;
        if (!queued) {
            p->sent_ms = now;            /* the next try's clock starts now */
            pend_air(m, p);
        }
    }

    /* One frame per tick: the one most worth sending among those due. */
    int best = -1;
    for (int i = 0; i < MT_TXQ; i++) {
        mt_txq_t *c = &m->q[i];
        if (!c->used) continue;
        uint32_t stale = c->prio == MT_PRIO_RELAY ? MT_RELAY_STALE_MS
                                                  : MT_OWN_STALE_MS;
        if (since(now, c->queued_ms) > (int32_t)stale) {
            c->used = false;
            m->st.dropped++;
            continue;
        }
        if (since(now, c->due_ms) < 0) continue;
        if (best < 0 || c->prio > m->q[best].prio ||
            (c->prio == m->q[best].prio && since(m->q[best].due_ms, c->due_ms) > 0))
            best = i;
    }
    if (best < 0) return;
    mt_txq_t *c = &m->q[best];
    if (!m->ops.air(m->ops.ctx, c->frame, c->len, c->prio)) {
        /* Busy channel or spent budget: try again a little later. */
        c->due_ms = now + mt_mesh_slot_ms() * (1u + m->ops.random() % 8u);
        return;
    }
    c->used = false;
    for (int i = 0; i < MT_PENDING; i++) {
        mt_pending_t *p = &m->pend[i];
        if (p->used && p->from == c->from && p->id == c->id) {
            p->aired = true;
            p->tries++;
            p->sent_ms = now;
        }
    }
}

void mt_mesh_set_nick(mt_mesh_t *m, const char *nick)
{
    if (!m) return;
    snprintf(m->nick, sizeof m->nick, "%s", nick ? nick : "");
    mt_vnode_t *own = vnode_by_num(m, m->self);
    if (own) own->announced_ms = 0;
}

int mt_mesh_node(const mt_mesh_t *m, int i, const mt_node_t **out)
{
    int n = 0;
    for (int k = 0; k < MT_NODES; k++) {
        if (!m->nodes[k].heard_ms || m->nodes[k].xprs) continue;
        if (n == i && out) *out = &m->nodes[k];
        n++;
    }
    return n;
}

/* See mt_mesh.h. Three reasons to stay, in the order they cost most:
 * a frame already due to go, a direct message still inside its retry
 * budget, and an exchange that was live a moment ago. */
bool mt_mesh_busy(const mt_mesh_t *m, uint32_t now_ms, uint32_t recent_ms)
{
    if (!m) return false;
    for (int i = 0; i < MT_TXQ; i++)
        if (m->q[i].used && (int32_t)(now_ms - m->q[i].due_ms) >= -1000)
            return true;
    for (int i = 0; i < MT_PENDING; i++) {
        const mt_pending_t *p = &m->pend[i];
        if (!p->used) continue;
        if (!p->aired || p->tries < MT_DM_TRIES) return true;
    }
    if (m->last_dm_ms && (uint32_t)(now_ms - m->last_dm_ms) < recent_ms)
        return true;
    return false;
}

void mt_mesh_init(mt_mesh_t *m, const mt_mesh_ops_t *ops,
                  const mt_mesh_cfg_t *cfg, const char *own_call,
                  const char *own_nick)
{
    memset(m, 0, sizeof *m);
    m->ops = *ops;
    m->cfg = *cfg;
    if (!m->cfg.nodeinfo_min) m->cfg.nodeinfo_min = 180;
    bare_upper(own_call, (int)strlen(own_call), m->call, sizeof m->call);
    snprintf(m->nick, sizeof m->nick, "%s", own_nick ? own_nick : "");
    m->self = mt_node_of_call(m->call, (int)strlen(m->call));
    m->lf_hash = mt_longfast_hash();
    m->boot_ms = m->ops.now_ms();
    vnode_learn(m, m->call, (int)strlen(m->call), m->boot_ms);
    if (m->ops.keys_load) {
        int n = m->ops.keys_load(m->ops.ctx, m->keys, (int)sizeof m->keys);
        int got = n > 0 ? n / (int)sizeof(mt_keyrec_t) : 0;
        if (got > MT_KEYS) got = MT_KEYS;
        for (int i = got; i < MT_KEYS; i++) memset(&m->keys[i], 0, sizeof m->keys[i]);
        m->keys_pos = got % MT_KEYS;
        if (got) mlog(m, "mt: %d Meshtastic key(s) remembered", got);
    }
    if (m->ops.vnodes_load) {
        char calls[MT_VNODES][MT_CALL_LEN];
        int n = m->ops.vnodes_load(m->ops.ctx, calls, (int)sizeof calls);
        int got = n > 0 ? n / MT_CALL_LEN : 0, kept = 0;
        for (int i = 0; i < got && i < MT_VNODES; i++) {
            calls[i][MT_CALL_LEN - 1] = 0;
            /* Heard "long ago", announced before: found by a DM to it. */
            mt_vnode_t *v = vnode_learn(m, calls[i], (int)strlen(calls[i]), 1);
            if (v) {
                v->keep = true;
                kept++;
            }
        }
        m->vnodes_dirty = false;
        if (kept) mlog(m, "mt: speaking for %d XPRS callsign(s) again", kept);
    }
}
