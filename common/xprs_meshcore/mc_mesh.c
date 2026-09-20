/* mc_mesh.c -- see mc_mesh.h. The MeshCore repeater and the bridge.
 *
 * Written against mt_mesh.c, because the rules are the same rules
 * (docs/meshtastic.md, "The rules we follow"); where the two files differ,
 * MeshCore's own arithmetic is the reason and the comment says so.
 *
 * ONE PROPERTY IS WORTH KNOWING BEFORE READING: everything this bridge
 * airs is DETERMINISTIC. MeshCore's cipher is AES-ECB with no nonce, the
 * timestamp comes from the XPRS packet rather than from our clock, and a
 * callsign's key is derived from the callsign, so two bridges translating
 * one packet produce the same bytes and therefore the same packet hash.
 * That is what makes them cancel each other instead of both airing it.
 */

#include "mc_mesh.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "xlc.h"

/* ── Small things ─────────────────────────────────────────────────────── */

static void mlog(mc_mesh_t *m, const char *fmt, ...)
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

uint32_t mc_mesh_slot_ms(void)
{
    /* CAD (2.5 symbols on an SX126x) plus turnaround and MAC time. At
     * SF11/250 kHz a symbol is 8.192 ms. */
    uint32_t sym_us = (1u << MC_SF) * 1000000u / MC_BW_HZ;
    return (sym_us * 5u / 2u + 7600u) / 1000u;
}

/* What [len] bytes cost on this channel, by the LoRa airtime formula with
 * the low data rate optimisation on (SF11 at 250 kHz). Only the relay wait
 * is built on it, so an approximation is enough; the duty ledger in the
 * bearer is the one that has to be exact. */
static uint32_t airtime_ms(int len)
{
    uint32_t sym_us = (1u << MC_SF) * 1000000u / MC_BW_HZ;
    int num = 8 * len - 4 * MC_SF + 28 + 16;
    int den = 4 * (MC_SF - 2);
    int payload_sym = num <= 0 ? 0 : (num + den - 1) / den * (MC_CR + 4);
    uint32_t sym = (uint32_t)MC_PREAMBLE + 5u + 8u + (uint32_t)payload_sym;
    return sym * sym_us / 1000u;
}

/* MeshCore's own flood wait: rand(0..5) * (airtime * 52 / 50) / 2. A
 * repeater that waits its turn is a repeater that does not collide with
 * the three others that heard the same packet. */
static uint32_t relay_delay(mc_mesh_t *m, int len)
{
    uint32_t air = airtime_ms(len);
    return (m->ops.random() % 6u) * (air * 52u / 50u) / 2u;
}

/* Our own frames go out after a short random wait: several bridges will
 * want to air the same one, and the first to speak silences the rest. */
static uint32_t own_delay(mc_mesh_t *m)
{
    return 300u + (m->ops.random() % 32u) * mc_mesh_slot_ms();
}

static void hex8(uint32_t v, char out[9])
{
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) out[i] = hex[(v >> ((7 - i) * 4)) & 0x0F];
    out[8] = 0;
}

/* ── Rings and tables ─────────────────────────────────────────────────── */

static bool seen_has(mc_mesh_t *m, uint32_t hash, uint32_t now)
{
    for (int i = 0; i < MC_SEEN; i++) {
        const mc_seen_t *s = &m->seen[i];
        if (s->t_ms && s->hash == hash &&
            since(now, s->t_ms) < (int32_t)MC_SEEN_MS)
            return true;
    }
    return false;
}

static void seen_add(mc_mesh_t *m, uint32_t hash, uint32_t now)
{
    mc_seen_t *s = &m->seen[m->seen_pos];
    s->hash = hash;
    s->t_ms = now ? now : 1;
    m->seen_pos = (m->seen_pos + 1) % MC_SEEN;
}

static mc_node_t *node_by_pub(mc_mesh_t *m, const uint8_t pub[32], bool create)
{
    mc_node_t *oldest = NULL;
    for (int i = 0; i < MC_NODES; i++) {
        mc_node_t *n = &m->nodes[i];
        if (n->heard_ms && memcmp(n->pub, pub, 32) == 0) return n;
        if (!oldest || (oldest->heard_ms &&
                        (!n->heard_ms || since(oldest->heard_ms, n->heard_ms) > 0)))
            oldest = n;
    }
    if (!create || !oldest) return NULL;
    memset(oldest, 0, sizeof *oldest);
    memcpy(oldest->pub, pub, 32);
    return oldest;
}

/* The only node whose name is [name], or NULL when none or several are.
 * A channel message carries a name and nothing else, so this is as far as
 * attribution can honestly go (mc_mesh.h). */
static mc_node_t *node_by_name(mc_mesh_t *m, const char *name)
{
    mc_node_t *found = NULL;
    for (int i = 0; i < MC_NODES; i++) {
        mc_node_t *n = &m->nodes[i];
        if (!n->heard_ms || n->xprs || strcmp(n->name, name) != 0) continue;
        if (found) return NULL;
        found = n;
    }
    return found;
}

static const uint8_t *key_of_hash(mc_mesh_t *m, uint8_t hash, int *from)
{
    for (int i = *from; i < MC_KEYS; i++) {
        mc_keyrec_t *k = &m->keys[i];
        bool any = false;
        for (int b = 0; b < 32; b++) if (k->pub[b]) { any = true; break; }
        if (!any || k->pub[0] != hash) continue;
        *from = i + 1;
        return k->pub;
    }
    *from = MC_KEYS;
    return NULL;
}

/* The contact whose key starts with the four bytes [want] (an MC callsign
 * names exactly that much of it, XPRS.md 3.2). */
static const uint8_t *key_by_prefix(mc_mesh_t *m, const uint8_t want[4])
{
    for (int i = 0; i < MC_KEYS; i++)
        if (memcmp(m->keys[i].pub, want, 4) == 0) {
            bool any = false;
            for (int b = 0; b < 32; b++) if (m->keys[i].pub[b]) { any = true; break; }
            if (any) return m->keys[i].pub;
        }
    return NULL;
}

static void key_put(mc_mesh_t *m, const uint8_t pub[32])
{
    for (int i = 0; i < MC_KEYS; i++)
        if (memcmp(m->keys[i].pub, pub, 32) == 0) return;
    mc_keyrec_t *k = &m->keys[m->keys_pos];
    m->keys_pos = (m->keys_pos + 1) % MC_KEYS;
    memcpy(k->pub, pub, 32);
    m->keys_dirty = true;
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

static mc_vnode_t *vnode_by_call(mc_mesh_t *m, const char *bare)
{
    for (int i = 0; i < MC_VNODES; i++)
        if (m->vnodes[i].seen_ms && strcmp(m->vnodes[i].call, bare) == 0)
            return &m->vnodes[i];
    return NULL;
}

static mc_vnode_t *vnode_learn(mc_mesh_t *m, const char *call, int len,
                               uint32_t now)
{
    char bare[MC_CALL_LEN];
    if (bare_upper(call, len, bare, sizeof bare) < 3) return NULL;
    mc_vnode_t *v = vnode_by_call(m, bare), *spare = NULL;
    if (!v) {
        for (int i = 0; i < MC_VNODES; i++) {
            mc_vnode_t *c = &m->vnodes[i];
            if (c->seen_ms && strcmp(c->call, m->call) == 0) continue;  /* ours */
            if (!c->seen_ms) { spare = c; break; }
            if (!spare || (c->keep != spare->keep ? !c->keep
                                                  : since(spare->seen_ms, c->seen_ms) > 0))
                spare = c;
        }
        if (!spare) return NULL;
        if (spare->keep) m->vnodes_dirty = true;   /* one fewer to save */
        v = spare;
        memset(v, 0, sizeof *v);
        snprintf(v->call, sizeof v->call, "%s", bare);
        /* Its key is NOT derived here: this runs on whichever task heard
         * the packet -- the Bluetooth host, the LAN bearer -- and an
         * Ed25519 key pair is scalar multiplication. mc_mesh_work does it,
         * once, and until then the node has no address on MeshCore. */
    }
    v->seen_ms = now ? now : 1;
    return v;
}

/* The key a virtual node wears on MeshCore, derived once and kept. Only
 * mc_mesh_work calls this: it is the expensive half. */
static bool vnode_key(mc_mesh_t *m, mc_vnode_t *v)
{
    (void)m;
    if (v->keyed) return true;
    uint8_t sk[64];
    mc_node_keys(v->call, (int)strlen(v->call), sk, v->pub);
    memset(sk, 0, sizeof sk);
    v->hash = v->pub[0];
    v->keyed = true;
    return true;
}

/* Has this XPRS packet already been translated? The identifier is the
 * packet's own (XPRS.md 5), so an echo of it is the same packet. */
static bool xid_seen(mc_mesh_t *m, const char *xid)
{
    for (int i = 0; i < MC_XIDS; i++)
        if (m->xids[i].xid[0] && strcmp(m->xids[i].xid, xid) == 0) return true;
    return false;
}

static void xid_put(mc_mesh_t *m, const char *xid)
{
    snprintf(m->xids[m->xids_pos].xid, sizeof m->xids[0].xid, "%s", xid);
    m->xids_pos = (m->xids_pos + 1) % MC_XIDS;
}

/* Per-hour cap on mirrored broadcasts: sixty one-minute buckets. */
static void bcast_roll(mc_mesh_t *m, uint32_t now)
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

static bool bcast_take(mc_mesh_t *m, uint32_t now)
{
    bcast_roll(m, now);
    uint32_t sum = 0;
    for (int i = 0; i < 60; i++) sum += m->bcast_min[i];
    if (sum >= m->cfg.bcast_per_hour) return false;
    if (m->bcast_min[m->bcast_head] < 255) m->bcast_min[m->bcast_head]++;
    return true;
}

/* ── The transmit queue ───────────────────────────────────────────────── */

static bool q_push(mc_mesh_t *m, const uint8_t *frame, int len, int prio,
                   uint32_t delay, uint32_t hash)
{
    uint32_t now = m->ops.now_ms();
    mc_txq_t *slot = NULL;
    for (int i = 0; i < MC_TXQ && !slot; i++)
        if (!m->q[i].used) slot = &m->q[i];
    if (!slot) {
        /* Full: a relay makes room only by displacing an older relay. */
        for (int i = 0; i < MC_TXQ; i++) {
            mc_txq_t *c = &m->q[i];
            if (c->prio > prio) continue;
            if (!slot || since(slot->queued_ms, c->queued_ms) > 0) slot = c;
        }
        if (!slot) { m->st.dropped++; return false; }
        m->st.dropped++;          /* the frame we are pushing out, counted once */
    }
    memcpy(slot->frame, frame, (size_t)len);
    slot->len = (uint8_t)len;
    slot->prio = (uint8_t)prio;
    slot->hash = hash;
    slot->queued_ms = now;
    slot->due_ms = now + delay;
    slot->used = true;
    return true;
}

/* Somebody else aired this packet: whatever we had queued for it is moot.
 * MeshCore's hash is over the payload and the type, so their copy and ours
 * are the same packet even though the path differs. */
static void q_cancel(mc_mesh_t *m, uint32_t hash)
{
    for (int i = 0; i < MC_TXQ; i++) {
        mc_txq_t *c = &m->q[i];
        if (!c->used || c->hash != hash) continue;
        c->used = false;
        if (c->prio == MC_PRIO_RELAY) m->st.relay_cancelled++;
    }
    for (int i = 0; i < MC_PENDING; i++) {
        mc_pending_t *p = &m->pend[i];
        if (p->used && !p->aired && p->hash == hash) p->used = false;
    }
}

/* ── Frames we originate ──────────────────────────────────────────────── */

/* A flood-routed packet of [type] carrying [payload], into m->frame.
 * Returns its length and writes its hash. */
static int build(mc_mesh_t *m, uint8_t type, const uint8_t *payload, int plen,
                 uint32_t *hash)
{
    mc_pkt_t p;
    memset(&p, 0, sizeof p);
    p.route = MC_ROUTE_FLOOD;
    p.type = type;
    p.hash_size = 1;
    p.payload = payload;
    p.payload_len = plen;
    int n = mc_build(&p, m->frame, MC_FRAME_MAX);
    if (n && hash) *hash = mc_packet_hash(&p);
    return n;
}

/* The name a virtual node wears on MeshCore: the nick where the station
 * holds one, and ALWAYS the callsign, because the callsign is the address
 * and because it is how another bridge recognises this node as ours. */
static void vnode_name(mc_mesh_t *m, const mc_vnode_t *v, char *out, int cap)
{
    char nick[20] = "";
    if (strcmp(v->call, m->call) == 0) snprintf(nick, sizeof nick, "%s", m->nick);
    else if (m->ops.nick_of) m->ops.nick_of(m->ops.ctx, v->call, nick, sizeof nick);
    if (nick[0]) snprintf(out, cap, "%s %s", nick, v->call);
    else snprintf(out, cap, "%s", v->call);
}

/* The advert for one of our nodes: signed, flooded, and the only way a
 * MeshCore user can write to an XPRS callsign at all. */
static void advert_queue(mc_mesh_t *m, mc_vnode_t *v)
{
    uint32_t utc = m->ops.utc_now ? m->ops.utc_now(m->ops.ctx) : 0;
    if (!utc) return;              /* an advert without a timestamp is noise */
    char name[40];
    vnode_name(m, v, name, sizeof name);
    uint8_t sk[64], pub[32];
    mc_node_keys(v->call, (int)strlen(v->call), sk, pub);
    vnode_key(m, v);
    int pn = mc_advert_build(sk, pub, utc, MC_ADV_CHAT, name, m->payload,
                             (int)sizeof m->payload);
    memset(sk, 0, sizeof sk);
    if (!pn) return;
    uint32_t hash = 0;
    int n = build(m, MC_PT_ADVERT, m->payload, pn, &hash);
    if (!n) return;
    uint32_t now = m->ops.now_ms();
    if (seen_has(m, hash, now)) return;        /* another bridge said it */
    seen_add(m, hash, now);
    if (q_push(m, m->frame, n, MC_PRIO_OWN, own_delay(m) / 2, hash)) {
        if (!v->keep && strcmp(v->call, m->call) != 0) {
            v->keep = true;
            m->vnodes_dirty = true;
        }
        v->advert_ms = now ? now : 1;
        m->st.adverts_out++;
    }
}

static void advert_if_stale(mc_mesh_t *m, mc_vnode_t *v)
{
    uint32_t now = m->ops.now_ms();
    uint32_t period = (uint32_t)m->cfg.advert_min * 60000u;
    if (!v->advert_ms || since(now, v->advert_ms) > (int32_t)period)
        advert_queue(m, v);
}

/* The four bytes MeshCore acknowledges with: a checksum over the message,
 * computed the same way at both ends (mc_ack_checksum). */
static void ack_queue(mc_mesh_t *m, uint32_t checksum)
{
    uint8_t pl[4] = { (uint8_t)(checksum & 0xFF), (uint8_t)(checksum >> 8),
                      (uint8_t)(checksum >> 16), (uint8_t)(checksum >> 24) };
    uint32_t hash = 0;
    int n = build(m, MC_PT_ACK, pl, 4, &hash);
    if (!n) return;
    uint32_t now = m->ops.now_ms();
    if (seen_has(m, hash, now)) return;        /* another bridge acked it */
    seen_add(m, hash, now);
    q_push(m, m->frame, n, MC_PRIO_OWN, own_delay(m) / 2, hash);
}

/* Seal a waiting direct message to its contact and queue it. The key was
 * known when the message was taken (there is no asking for one on
 * MeshCore), so this cannot fail for want of it. */
static void advert_queue(mc_mesh_t *m, mc_vnode_t *v);

static bool dm_air(mc_mesh_t *m, mc_pending_t *p)
{
    /* THE ADVERT GOES FIRST. Nobody on MeshCore can open a message from a
     * node they have never heard advertise: the key is in the advert and
     * nowhere else (mc_mesh.h, MC_AFTER_ADVERT_MS). */
    mc_vnode_t *v = vnode_by_call(m, p->from_call);
    if (v) {
        uint32_t now_ms = m->ops.now_ms();
        if (!v->advert_ms) {
            advert_queue(m, v);
            return false;                 /* try again once it is out */
        }
        if (since(now_ms, v->advert_ms) < (int32_t)MC_AFTER_ADVERT_MS)
            return false;                 /* let it get there first */
    }
    uint8_t sk[64], pub[32], secret[32];
    mc_node_keys(p->from_call, (int)strlen(p->from_call), sk, pub);
    bool ok = xlc_ed25519_key_exchange(secret, p->to_pub, sk);
    memset(sk, 0, sizeof sk);
    if (!ok) return false;
    /* The checksum its ACK will carry, over the message and its author's
     * key: worked out here, where the key already is, rather than on the
     * task that took the message in. */
    /* The plaintext's fifth byte is the type and attempt, and ours is a
     * plain message on its first attempt: zero. */
    if (!p->ack) p->ack = mc_ack_checksum(p->ts, 0, p->text, pub);
    int pn = mc_dm_build(secret, p->to_pub[0], pub[0], p->ts, 0, p->text,
                         m->payload, (int)sizeof m->payload);
    memset(secret, 0, sizeof secret);
    if (!pn) return false;
    uint32_t hash = 0;
    int n = build(m, MC_PT_TXT_MSG, m->payload, pn, &hash);
    if (!n) return false;
    uint32_t now = m->ops.now_ms();
    p->hash = hash;
    seen_add(m, hash, now);
    return q_push(m, m->frame, n, MC_PRIO_OWN, own_delay(m), hash);
}

/* ── MeshCore to XPRS ─────────────────────────────────────────────────── */

static int stamp(mc_mesh_t *m, char *out, int cap)
{
    return m->ops.stamp ? m->ops.stamp(m->ops.ctx, out, cap, true) : 0;
}

static void deliver(mc_mesh_t *m, const char *wire, int len, bool sign)
{
    if (m->ops.deliver) m->ops.deliver(m->ops.ctx, wire, len, sign);
}

/* The text a packet may carry in m: no line breaks, no NUL. */
static int clean_text(mc_mesh_t *m, const char *in)
{
    int n = 0;
    for (int i = 0; in[i] && n < (int)sizeof m->text - 1; i++) {
        char c = in[i];
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        m->text[n++] = c;
    }
    while (n > 0 && m->text[n - 1] == ' ') n--;
    m->text[n] = 0;
    return n;
}

/* One MeshCore text into XPRS, split over parts when it does not fit.
 * [dst] is the callsign a direct message was for, NULL for the channel.
 *
 * SCOPE: MeshCore asks its users nothing about the internet, so there is
 * no consent to read and every translation is scope:local (XPRS.md
 * 9.11.3). This station's own bearers carry it; no gateway publishes it.
 */
static void text_in(mc_mesh_t *m, const uint8_t from_pub[32], uint32_t hash,
                    const char *text, const char *dst)
{
    int n = clean_text(m, text);
    if (!n) return;
    char from[11];
    mc_call_of_pub(from_pub, from);
    char ts[40], zid[9];
    int tn = stamp(m, ts, sizeof ts);
    hex8(hash, zid);

    char env[160];
    int en = snprintf(env, sizeof env,
                      "t:message f:%s%s%s%s%.*s scope:local zmid:%s",
                      from, dst ? " d:" : "", dst ? dst : "", tn ? " " : "",
                      tn, ts, zid);
    if (en <= 0 || en >= (int)sizeof env) return;
    char via[24];
    int vn = snprintf(via, sizeof via, " via:%s", m->call);

    if (en + vn + 3 + n <= XPRS_MAX_WIRE) {
        int wl = snprintf(m->wire, sizeof m->wire, "%s%s m:%s", env, via, m->text);
        deliver(m, m->wire, wl, false);
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

/* An advert's name into an unsigned identity, when it says something new.
 * The signature makes the name the node's own word, which is more than a
 * channel message gives; it is still not a key XPRS knows, so it rides
 * unsigned and as a nick (XPRS.md 6.3.1). */
static void identity_in(mc_mesh_t *m, mc_node_t *nd, uint32_t hash)
{
    uint32_t now = m->ops.now_ms();
    if (!nd->name[0]) return;
    if (nd->identity_ms && since(now, nd->identity_ms) < 6 * 3600 * 1000)
        return;
    /* A nick is one word (XPRS.md 6.3.1): take the first of the name. */
    char nick[20];
    int n = 0;
    for (int i = 0; nd->name[i] && n < (int)sizeof nick - 1; i++) {
        if (nd->name[i] == ' ') { if (n) break; continue; }
        nick[n++] = nd->name[i];
    }
    nick[n] = 0;
    if (n < 2) return;
    char from[11], ts[40], zid[9];
    mc_call_of_pub(nd->pub, from);
    int tn = stamp(m, ts, sizeof ts);
    hex8(hash, zid);
    int wl = snprintf(m->wire, sizeof m->wire,
                      "t:identity f:%s%s%.*s scope:local nick:%s zmid:%s via:%s",
                      from, tn ? " " : "", tn, ts, nick, zid, m->call);
    if (wl > 0 && wl <= XPRS_MAX_WIRE) {
        deliver(m, m->wire, wl, false);
        nd->identity_ms = now ? now : 1;
    }
}

/* The receipt a gateway gives for a direct message MeshCore acknowledged,
 * signed by this station (XPRS.md 9.7.1). */
static void receipt_out(mc_mesh_t *m, const mc_pending_t *p, bool ok)
{
    char ts[40];
    int tn = stamp(m, ts, sizeof ts);
    int wl;
    if (ok)
        wl = snprintf(m->wire, sizeof m->wire,
                      "t:receipt f:%s d:%s%s%.*s r:%s s:ack",
                      m->call, p->sender, tn ? " " : "", tn, ts, p->xid);
    else
        wl = snprintf(m->wire, sizeof m->wire,
                      "t:receipt f:%s d:%s%s%.*s r:%s s:no m:%s",
                      m->call, p->sender, tn ? " " : "", tn, ts, p->xid,
                      "MeshCore never acknowledged it");
    if (wl > 0 && wl <= XPRS_MAX_WIRE) {
        deliver(m, m->wire, wl, true);
        m->st.receipts++;
    }
}

/* Does [name] carry a callsign whose derived key is [pub]? That is the
 * mark of a virtual node another bridge advertised. */
static bool names_our_node(const char *name, const uint8_t pub[32])
{
    const char *p = name;
    while (*p) {
        while (*p == ' ') p++;
        const char *s = p;
        while (*p && *p != ' ') p++;
        int n = (int)(p - s);
        if (n >= 3 && n <= MC_CALL_LEN - 1) {
            char bare[MC_CALL_LEN];
            memcpy(bare, s, (size_t)n);
            bare[n] = 0;
            uint8_t sk[64], k[32];
            mc_node_keys(bare, n, sk, k);
            memset(sk, 0, sizeof sk);
            if (memcmp(k, pub, 32) == 0) return true;
        }
    }
    return false;
}

static void advert_in(mc_mesh_t *m, const uint8_t *payload, int plen,
                      uint32_t hash, int snr)
{
    mc_advert_t a;
    if (!mc_advert_open(payload, plen, &a)) return;
    m->st.adverts_in++;
    uint32_t now = m->ops.now_ms();
    mc_node_t *nd = node_by_pub(m, a.pub, true);
    if (!nd) return;
    bool fresh = nd->heard_ms == 0;
    nd->heard_ms = now ? now : 1;
    nd->snr = (int8_t)snr;
    nd->advert_ts = a.timestamp;
    if (a.name[0]) snprintf(nd->name, sizeof nd->name, "%s", a.name);
    if (names_our_node(a.name, a.pub)) {
        nd->xprs = true;                 /* another bridge's virtual node */
        return;
    }
    /* The key is the address, so it is worth keeping: it is what a direct
     * message to this node is sealed to. */
    key_put(m, a.pub);
    if (fresh)
        mlog(m, "mc: advert from %.20s (%02x%02x%02x%02x)", a.name, a.pub[0],
             a.pub[1], a.pub[2], a.pub[3]);
    if (m->cfg.bridge) identity_in(m, nd, hash);
}

static void grp_in(mc_mesh_t *m, const mc_pkt_t *pkt, uint32_t hash)
{
    mc_text_t t;
    char sender[32];
    if (!mc_grp_txt_open(mc_public_key, pkt->payload, pkt->payload_len, &t,
                         sender, sizeof sender))
        return;
    m->st.rx_opened++;
    if (!m->cfg.bridge) return;
    /* A channel message is not signed and names its sender only by a name.
     * It crosses under the address of the node whose advert carries that
     * name, and only when exactly one does (mc_mesh.h). */
    mc_node_t *nd = sender[0] ? node_by_name(m, sender) : NULL;
    if (!nd) {
        m->st.grp_unnamed++;
        return;
    }
    text_in(m, nd->pub, hash, t.text, NULL);
}

static void ack_value(mc_mesh_t *m, uint32_t ack);

/* A PATH return: the route a client learned back to one of our nodes, and
 * inside it, usually, the acknowledgement of the message that taught it
 * (mc.h). Opened with the same secret as a direct message. */
static bool path_in(mc_mesh_t *m, const uint8_t *secret, const uint8_t *payload,
                    int plen, const char *call)
{
    mc_path_t pr;
    if (!mc_path_open(secret, payload, plen, &pr)) return false;
    m->st.paths_in++;
    if (pr.extra_type == MC_PT_ACK && pr.extra_len >= 4) {
        uint32_t ack = (uint32_t)pr.extra[0] | ((uint32_t)pr.extra[1] << 8) |
                       ((uint32_t)pr.extra[2] << 16) | ((uint32_t)pr.extra[3] << 24);
        mlog(m, "mc: path back to %s, %d hop(s), with the ack in it", call,
             pr.hops);
        ack_value(m, ack);
    } else {
        mlog(m, "mc: path back to %s, %d hop(s)", call, pr.hops);
    }
    return true;
}

/* A direct message, or a path return: the destination is one byte of a key,
 * so opening it means trying the contacts whose key starts with the byte it
 * names. The work is bounded, and only runs when the byte is one of ours. */
static void dm_in(mc_mesh_t *m, const uint8_t *payload, int plen, uint32_t hash,
                  uint8_t type)
{
    if (plen < 4 + 16) return;
    uint8_t dest = payload[0], src = payload[1];
    for (int i = 0; i < MC_VNODES; i++) {
        mc_vnode_t *v = &m->vnodes[i];
        if (!v->seen_ms || !v->keyed || v->hash != dest) continue;
        uint8_t sk[64], pub[32];
        mc_node_keys(v->call, (int)strlen(v->call), sk, pub);
        int from = 0, tried = 0;
        const uint8_t *their;
        while ((their = key_of_hash(m, src, &from)) != NULL && tried < 8) {
            tried++;
            uint8_t secret[32];
            mc_text_t t;
            if (!xlc_ed25519_key_exchange(secret, their, sk)) continue;
            if (type == MC_PT_PATH) {
                bool got = path_in(m, secret, payload, plen, v->call);
                memset(secret, 0, sizeof secret);
                if (!got) continue;
                memset(sk, 0, sizeof sk);
                return;
            }
            bool ok = mc_dm_open(secret, payload, plen, &t);
            memset(secret, 0, sizeof secret);
            if (!ok) continue;
            memset(sk, 0, sizeof sk);
            m->st.rx_opened++;
            mlog(m, "mc: DM for %s from %02x%02x%02x%02x", v->call, their[0],
                 their[1], their[2], their[3]);
            /* Acknowledged the way MeshCore acknowledges: a checksum over
             * the message and its AUTHOR's key, which is theirs. */
            ack_queue(m, mc_ack_checksum(t.timestamp,
                                         (uint8_t)((t.txt_type << 2) | t.attempt),
                                         t.text, their));
            if (m->cfg.bridge) text_in(m, their, hash, t.text, v->call);
            return;
        }
        memset(sk, 0, sizeof sk);
    }
}

static void ack_value(mc_mesh_t *m, uint32_t ack)
{
    for (int i = 0; i < MC_PENDING; i++) {
        mc_pending_t *p = &m->pend[i];
        if (!p->used || p->ack != ack) continue;
        p->used = false;
        m->st.dm_acked++;
        mlog(m, "mc: acked DM %s from %s", p->xid, p->sender);
        receipt_out(m, p, true);
    }
}

static void ack_in(mc_mesh_t *m, const mc_pkt_t *pkt)
{
    if (pkt->payload_len < 4) return;
    ack_value(m, (uint32_t)pkt->payload[0] | ((uint32_t)pkt->payload[1] << 8) |
                 ((uint32_t)pkt->payload[2] << 16) |
                 ((uint32_t)pkt->payload[3] << 24));
}

/* Is [hash] the first byte of one of our virtual nodes' keys? Cheap: the
 * byte was worked out when the node was keyed. */
static bool ours_hash(mc_mesh_t *m, uint8_t hash)
{
    for (int i = 0; i < MC_VNODES; i++)
        if (m->vnodes[i].seen_ms && m->vnodes[i].keyed && m->vnodes[i].hash == hash)
            return true;
    return false;
}

/* Park a payload for mc_mesh_work. The oldest waiting one loses its slot:
 * a channel this slow does not queue, and a packet nobody got to in a
 * second was overtaken anyway. */
static void inbox_put(mc_mesh_t *m, const mc_pkt_t *p, uint32_t hash, int snr)
{
    if (p->payload_len <= 0 || p->payload_len > MC_PAYLOAD_MAX) return;
    mc_inbox_t *slot = NULL;
    for (int i = 0; i < MC_INBOX; i++)
        if (!m->inbox[i].used) { slot = &m->inbox[i]; break; }
    if (!slot) {
        slot = &m->inbox[0];
        m->st.inbox_full++;
    }
    slot->used = true;
    slot->type = p->type;
    slot->len = (uint8_t)p->payload_len;
    slot->hash = hash;
    slot->snr = (int8_t)snr;
    memcpy(slot->payload, p->payload, (size_t)p->payload_len);
}

void mc_mesh_on_frame(mc_mesh_t *m, const uint8_t *frame, int len, int rssi,
                      int snr)
{
    (void)rssi;
    mc_pkt_t p;
    if (!m || !mc_parse(frame, len, &p)) return;
    m->st.rx_frames++;
    uint32_t now = m->ops.now_ms();
    uint32_t hash = mc_packet_hash(&p);

    if (seen_has(m, hash, now)) {
        m->st.rx_dupes++;
        /* Somebody relayed it, or another bridge aired the packet we were
         * about to: either way ours adds nothing. */
        q_cancel(m, hash);
        return;
    }
    seen_add(m, hash, now);

    switch (p.type) {
    case MC_PT_ADVERT:
        /* A signature to check and, inside it, keys to derive: parked for
         * mc_mesh_work, which has the stack for it. */
        inbox_put(m, &p, hash, snr);
        break;
    case MC_PT_GRP_TXT:
        /* The channel's cipher is AES and its MAC is HMAC: both cheap, and
         * neither is worth a second task. */
        if (p.payload_len > 0 && p.payload[0] == m->chan_hash) grp_in(m, &p, hash);
        break;
    case MC_PT_TXT_MSG:
    case MC_PT_PATH:
        /* Opening either means a key exchange per candidate contact, so
         * both are parked for the worker. A PATH addressed to one of our
         * nodes is how a client answers a first message (mc.h). */
        if (p.payload_len > 3 && ours_hash(m, p.payload[0]))
            inbox_put(m, &p, hash, snr);
        break;
    case MC_PT_ACK:
        ack_in(m, &p);
        break;
    default:
        break;
    }

    /* Parked direct messages go the moment their contact is heard again
     * (XPRS.md 12.8.1). A packet names its sender only in a DM, so this is
     * the one place we learn "that node is on the air". */
    if (p.type == MC_PT_TXT_MSG && p.payload_len > 1) {
        for (int i = 0; i < MC_PENDING; i++) {
            mc_pending_t *q = &m->pend[i];
            if (q->used && q->to_pub[0] == p.payload[1] &&
                q->tries >= MC_DM_TRIES && since(now, q->sent_ms) > 60000) {
                q->tries = MC_DM_TRIES - 1;      /* one more go */
                q->due = true;
            }
        }
    }

    /* ── The repeater ────────────────────────────────────────────────── */
    if (!m->cfg.repeat || !m->self_keyed) return;

    uint8_t mine = m->self_hash;
    if (p.route == MC_ROUTE_DIRECT || p.route == MC_ROUTE_TRANSPORT_DIRECT) {
        /* A direct packet is carried only by the node whose hash is at the
         * front of the path, and that node takes itself off it first. */
        if (!p.hops || p.path[0] != mine) {
            m->st.relay_skipped++;
            return;
        }
        mc_pkt_t out = p;
        out.hops = (uint8_t)(p.hops - 1);
        memmove(out.path, p.path + p.hash_size,
                (size_t)(out.hops * p.hash_size));
        int n = mc_build(&out, m->frame, MC_FRAME_MAX);
        if (n && q_push(m, m->frame, n, MC_PRIO_RELAY, relay_delay(m, len), hash))
            m->st.relayed++;
        return;
    }

    /* A flood packet: our hash on the end, and on its way. Not ours to
     * repeat if it has been far enough, if the path has no room, or if we
     * are already on it (which is the loop check XPRS.md 9.2 asks for). */
    if (p.hops >= MC_RELAY_MAX_HOPS ||
        (p.hops + 1) * p.hash_size > MC_PATH_MAX ||
        len + p.hash_size > MC_FRAME_MAX) {
        m->st.relay_skipped++;
        return;
    }
    for (int i = 0; i < p.hops; i++)
        if (p.path[i * p.hash_size] == mine) {
            m->st.relay_skipped++;
            return;
        }
    int n = mc_path_append(&p, m->self_pub3, m->frame, MC_FRAME_MAX);
    if (n && q_push(m, m->frame, n, MC_PRIO_RELAY, relay_delay(m, len), hash))
        m->st.relayed++;
}

void mc_mesh_note_xprs_frame(mc_mesh_t *m, const uint8_t *frame, int len)
{
    mc_pkt_t p;
    if (!m || !mc_parse(frame, len, &p)) return;
    uint32_t now = m->ops.now_ms();
    uint32_t hash = mc_packet_hash(&p);
    if (!seen_has(m, hash, now)) seen_add(m, hash, now);
}

/* ── XPRS to MeshCore ─────────────────────────────────────────────────── */

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
    int y = f[0] - (f[1] <= 2);
    int era = y / 400;
    int yoe = y - era * 400;
    int mp = (f[1] + 9) % 12;
    int doy = (153 * mp + 2) / 5 + f[2] - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400L + f[3] * 3600L + f[4] * 60L + f[5]);
}

/* MC + eight hexadecimal digits into the four key bytes it names. */
static bool pub4_of_call(const char *v, int n, uint8_t out[4])
{
    if (n != 10 || v[0] != 'M' || v[1] != 'C') return false;
    for (int i = 0; i < 8; i++) {
        char c = v[2 + i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return false;
        if (i % 2 == 0) out[i / 2] = (uint8_t)(d << 4);
        else out[i / 2] = (uint8_t)(out[i / 2] | d);
    }
    return true;
}

void mc_mesh_on_xprs(mc_mesh_t *m, const char *wire, int len, int origin)
{
    if (!m || !wire || len <= 0 || len > XPRS_MAX_WIRE) return;
    xprs_t *p = &m->xp;
    if (!xprs_parse(wire, len, p)) return;
    const char *f;
    int fn, vn;
    if (!get(p, "f", &f, &fn)) return;
    /* What came from another network never goes back to it, whoever
     * translated it (XPRS.md 9.11.5). */
    if (xprs_is_foreign_call(f, fn) || xprs_get(p, "zmid", &vn)) return;
    if (!xprs_is_station(f, fn)) return;

    uint32_t now = m->ops.now_ms();
    mc_vnode_t *sv = vnode_learn(m, f, fn, now);
    if (!sv || !m->cfg.bridge) return;

    char type[16];
    xprs_type(p, type, sizeof type);
    /* MeshCore has no tapback and no reply field, so only messages cross.
     * A reaction with nothing to attach to would be a line of its own in
     * somebody's chat. */
    if (strcmp(type, "message") != 0) return;

    const char *dv;
    int dn = 0;
    bool direct = false;
    uint8_t want[4], to_pub[32];
    if (get(p, "d", &dv, &dn)) {
        if (!pub4_of_call(dv, dn, want)) return;      /* XPRS-only address */
        /* Only where the node is: a direct message goes onto this radio
         * only for a contact this bridge has heard an advert from, or when
         * one of this station's own users handed it over. MeshCore names a
         * node by its key, and we have no way to ask for one, so a contact
         * we never heard is a contact we cannot write to at all. */
        const uint8_t *k = key_by_prefix(m, want);
        if (!k) {
            m->st.dm_not_here++;
            return;
        }
        memcpy(to_pub, k, 32);
        direct = true;
    }

    char xid[XPRS_ID_LEN];
    xprs_id(p, xid);
    /* Heard again (its own echo on another bearer, a digipeat, a replay):
     * it is on the channel already, or waiting to be. Checked BEFORE the
     * hourly allowance is spent, because a repeat is not a broadcast. */
    if (xid_seen(m, xid)) return;

    if (xprs_get(p, "x", &vn)) {
        /* A sealed body cannot cross: MeshCore cannot open section 6.2's
         * cipher, and a bridge that could would be reading the mail. Said
         * out loud only to the station that handed it to us. */
        if (direct && origin == MC_XPRS_OWN) {
            char ts[40];
            int tn = stamp(m, ts, sizeof ts);
            int wl = snprintf(m->wire, sizeof m->wire,
                              "t:receipt f:%s d:%s%s%.*s r:%s s:no m:sealed, "
                              "MeshCore cannot open it",
                              m->call, sv->call, tn ? " " : "", tn, ts, xid);
            if (wl > 0 && wl <= XPRS_MAX_WIRE) deliver(m, m->wire, wl, true);
        }
        return;
    }
    if (xprs_scope_local(p)) return;       /* never onto a radio band */

    const char *mv;
    int mn;
    if (!get(p, "m", &mv, &mn)) return;

    /* The timestamp inside the MeshCore message is the XPRS packet's own,
     * not this clock: that is what makes two bridges produce identical
     * bytes, and it is the time the words were said. */
    const char *tv;
    int tn2;
    uint32_t utc = m->ops.utc_now ? m->ops.utc_now(m->ops.ctx) : 0;
    uint32_t ts = 0;
    if (get(p, "ts", &tv, &tn2)) ts = ts_epoch(tv, tn2);
    if (!ts) ts = utc;
    if (!ts) return;                       /* no clock, no message */
    if (utc && utc > ts) {
        uint32_t fresh = direct ? MC_DM_FRESH_S : MC_BCAST_FRESH_S;
        if (utc - ts > fresh) return;      /* old news after a restart */
    }

    /* What one MeshCore message carries (MC_TEXT_MAX), and for a channel
     * message the sender's name is inside that too. Longer is shortened,
     * as it is on the Meshtastic side: the whole of it is on XPRS, where
     * the words were said. */
    char text[MC_TEXT_MAX + 1];
    int room = MC_TEXT_MAX;
    char name[40] = "";
    if (!direct) {
        vnode_name(m, sv, name, sizeof name);
        room -= (int)strlen(name) + 2;          /* "<name>: " */
        if (room < 8) return;
    }
    int tl = mn < room ? mn : room;
    memcpy(text, mv, (size_t)tl);
    text[tl] = 0;

    if (!direct) {
        if (!bcast_take(m, now)) {
            m->st.bcast_capped++;
            return;
        }
        int pn = mc_grp_txt_build(mc_public_key, ts, name, text, m->payload,
                                  (int)sizeof m->payload);
        if (!pn) return;
        uint32_t hash = 0;
        int n = build(m, MC_PT_GRP_TXT, m->payload, pn, &hash);
        if (!n) return;
        if (seen_has(m, hash, now)) return;     /* another bridge did it */
        seen_add(m, hash, now);
        sv->advert_due = true;
        xid_put(m, xid);
        m->st.text_out++;
        mlog(m, "mc: text %s -> channel as %s", xid, sv->call);
        q_push(m, m->frame, n, MC_PRIO_OWN, own_delay(m), hash);
        return;
    }

    /* A direct message: sealed to the contact's key, watched for its ACK,
     * which becomes the gateway receipt. */
    mc_pending_t *slot = NULL;
    for (int i = 0; i < MC_PENDING; i++) {
        mc_pending_t *c = &m->pend[i];
        if (!c->used) { slot = c; break; }
        if (!slot || since(slot->created_ms, c->created_ms) > 0) slot = c;
    }
    if (!slot) return;
    memset(slot, 0, sizeof *slot);
    slot->used = true;
    memcpy(slot->to_pub, to_pub, 32);
    slot->ts = ts;
    slot->created_ms = now ? now : 1;
    snprintf(slot->from_call, sizeof slot->from_call, "%s", sv->call);
    snprintf(slot->sender, sizeof slot->sender, "%s", sv->call);
    snprintf(slot->xid, sizeof slot->xid, "%s", xid);
    xid_put(m, xid);
    snprintf(slot->text, sizeof slot->text, "%s", text);
    /* The checksum its ACK will carry needs the author's key, which is
     * scalar multiplication; dm_air works it out, on the worker. */
    sv->advert_due = true;               /* it must be findable to be written to */
    slot->due = true;                    /* dm_air, on the worker */
    m->st.text_out++;
    mlog(m, "mc: text %s -> %02x%02x%02x%02x as %s", xid, to_pub[0], to_pub[1],
         to_pub[2], to_pub[3], sv->call);
}

/* ── The two clocks ───────────────────────────────────────────────────────
 *
 * mc_mesh_work is where every signature, key exchange and derived key
 * happens, on the station's own task; mc_mesh_tick is what the bearer
 * calls, and it never touches the curve. Splitting them is not tidiness:
 * an Ed25519 verification is about 3.3 KB of stack and the bearer task has
 * two to spare (docs/esp32.md).
 */

void mc_mesh_work(mc_mesh_t *m, uint32_t now)
{
    if (!m) return;

    /* What the receive path parked, opened here. */
    for (int i = 0; i < MC_INBOX; i++) {
        mc_inbox_t *in = &m->inbox[i];
        if (!in->used) continue;
        in->used = false;
        if (in->type == MC_PT_ADVERT)
            advert_in(m, in->payload, in->len, in->hash, in->snr);
        else if (in->type == MC_PT_TXT_MSG || in->type == MC_PT_PATH)
            dm_in(m, in->payload, in->len, in->hash, in->type);
    }

    /* A virtual node has no address on MeshCore until its key is derived,
     * and that is done here, one node per pass: the callsigns arrive from
     * whatever task heard them and there may be a table's worth at once. */
    for (int i = 0; i < MC_VNODES; i++) {
        mc_vnode_t *v = &m->vnodes[i];
        if (!v->seen_ms || v->keyed) continue;
        vnode_key(m, v);
        /* Ours is also what the repeater writes into a path. */
        if (strcmp(v->call, m->call) == 0) {
            memcpy(m->self_pub3, v->pub, 3);
            m->self_hash = v->hash;
            m->self_keyed = true;
        }
        break;
    }

    /* Our own node says who it is, soon after boot and then on the period;
     * a node about to speak for an XPRS callsign advertises first. */
    mc_vnode_t *own = vnode_by_call(m, m->call);
    if (own && since(now, m->boot_ms) > 20000) advert_if_stale(m, own);
    for (int i = 0; i < MC_VNODES; i++) {
        mc_vnode_t *v = &m->vnodes[i];
        if (!v->advert_due) continue;
        v->advert_due = false;
        advert_if_stale(m, v);
    }
    for (int i = 0; i < MC_PENDING; i++) {
        mc_pending_t *p = &m->pend[i];
        if (!p->used || !p->due) continue;
        /* Still due when it could not go yet (its advert is on the way):
         * the next pass tries again, and the park timer is what ends it. */
        p->due = !dm_air(m, p);
    }

    /* Direct messages waiting for their ACK: the retry seals the message
     * again, so it belongs here too. */
    for (int i = 0; i < MC_PENDING; i++) {
        mc_pending_t *p = &m->pend[i];
        if (!p->used) continue;
        if (since(now, p->created_ms) > (int32_t)MC_DM_PARK_MS) {
            /* A day without an ACK: the sender is told, once. */
            p->used = false;
            receipt_out(m, p, false);
            continue;
        }
        if (!p->aired || p->tries >= MC_DM_TRIES) continue;
        if (since(now, p->sent_ms) < (int32_t)MC_DM_RETRY_MS) continue;
        bool queued = false;
        for (int k = 0; k < MC_TXQ; k++)
            if (m->q[k].used && m->q[k].hash == p->hash) queued = true;
        if (!queued) {
            p->sent_ms = now;            /* the next try's clock starts now */
            dm_air(m, p);
        }
    }

    /* Flash writes are slow and stop the cache on both cores: another
     * reason they are on this task rather than the bearer's. */
    if (m->vnodes_dirty && m->ops.vnodes_save &&
        (!m->vnodes_saved_ms || since(now, m->vnodes_saved_ms) > 60000)) {
        m->vnodes_dirty = false;
        m->vnodes_saved_ms = now ? now : 1;
        char calls[MC_VNODES][MC_CALL_LEN];
        int n = 0;
        memset(calls, 0, sizeof calls);
        for (int i = 0; i < MC_VNODES; i++)
            if (m->vnodes[i].seen_ms && m->vnodes[i].keep &&
                strcmp(m->vnodes[i].call, m->call) != 0)
                memcpy(calls[n++], m->vnodes[i].call, MC_CALL_LEN);
        m->ops.vnodes_save(m->ops.ctx, calls, n * MC_CALL_LEN);
    }

    if (m->keys_dirty && m->ops.keys_save &&
        (!m->keys_saved_ms || since(now, m->keys_saved_ms) > 60000)) {
        m->keys_dirty = false;
        m->keys_saved_ms = now ? now : 1;
        m->ops.keys_save(m->ops.ctx, m->keys, (int)sizeof m->keys);
    }
}

/* The bearer's half: what is due goes on the air. No curve arithmetic. */
void mc_mesh_tick(mc_mesh_t *m, uint32_t now)
{
    if (!m) return;

    /* One frame per tick: the one most worth sending among those due. */
    int best = -1;
    for (int i = 0; i < MC_TXQ; i++) {
        mc_txq_t *c = &m->q[i];
        if (!c->used) continue;
        uint32_t stale = c->prio == MC_PRIO_RELAY ? MC_RELAY_STALE_MS
                                                  : MC_OWN_STALE_MS;
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
    mc_txq_t *c = &m->q[best];
    if (!m->ops.air(m->ops.ctx, c->frame, c->len, c->prio)) {
        /* Busy channel or spent budget: try again a little later. MeshCore's
         * own retry after a failed CAD is rand(1..4) * 120 ms. */
        c->due_ms = now + 120u * (1u + m->ops.random() % 4u);
        return;
    }
    c->used = false;
    for (int i = 0; i < MC_PENDING; i++) {
        mc_pending_t *p = &m->pend[i];
        if (p->used && p->hash == c->hash) {
            p->aired = true;
            p->tries++;
            p->sent_ms = now;
        }
    }
}

void mc_mesh_set_nick(mc_mesh_t *m, const char *nick)
{
    if (!m) return;
    snprintf(m->nick, sizeof m->nick, "%s", nick ? nick : "");
    mc_vnode_t *own = vnode_by_call(m, m->call);
    if (own) own->advert_ms = 0;
}

int mc_mesh_node(const mc_mesh_t *m, int i, const mc_node_t **out)
{
    int n = 0;
    for (int k = 0; k < MC_NODES; k++) {
        if (!m->nodes[k].heard_ms || m->nodes[k].xprs) continue;
        if (n == i && out) *out = &m->nodes[k];
        n++;
    }
    return n;
}

void mc_mesh_init(mc_mesh_t *m, const mc_mesh_ops_t *ops,
                  const mc_mesh_cfg_t *cfg, const char *own_call,
                  const char *own_nick)
{
    memset(m, 0, sizeof *m);
    m->ops = *ops;
    m->cfg = *cfg;
    if (!m->cfg.advert_min) m->cfg.advert_min = 180;
    bare_upper(own_call, (int)strlen(own_call), m->call, sizeof m->call);
    snprintf(m->nick, sizeof m->nick, "%s", own_nick ? own_nick : "");
    m->chan_hash = mc_channel_hash(mc_public_key);
    m->boot_ms = m->ops.now_ms();
    /* Our own node is learned here and KEYED BY THE WORKER, on its first
     * pass: deriving it costs about 2.8 KB of stack, and this runs on
     * whichever task asked for the mode -- the screen's, whose stack is
     * 4 KB on the smaller boards (docs/esp32.md, "Task stacks are heap").
     * Until it is keyed the station repeats nothing, which is fifty
     * milliseconds of silence at the start of a mode. */
    vnode_learn(m, m->call, (int)strlen(m->call), m->boot_ms);
    if (m->ops.keys_load) {
        int n = m->ops.keys_load(m->ops.ctx, m->keys, (int)sizeof m->keys);
        int got = n > 0 ? n / (int)sizeof(mc_keyrec_t) : 0;
        if (got > MC_KEYS) got = MC_KEYS;
        for (int i = got; i < MC_KEYS; i++) memset(&m->keys[i], 0, sizeof m->keys[i]);
        m->keys_pos = got % MC_KEYS;
        if (got) mlog(m, "mc: %d MeshCore contact(s) remembered", got);
    }
    if (m->ops.vnodes_load) {
        char calls[MC_VNODES][MC_CALL_LEN];
        int n = m->ops.vnodes_load(m->ops.ctx, calls, (int)sizeof calls);
        int got = n > 0 ? n / MC_CALL_LEN : 0, kept = 0;
        for (int i = 0; i < got && i < MC_VNODES; i++) {
            calls[i][MC_CALL_LEN - 1] = 0;
            mc_vnode_t *v = vnode_learn(m, calls[i], (int)strlen(calls[i]), 1);
            if (v) {
                v->keep = true;
                kept++;
            }
        }
        m->vnodes_dirty = false;
        if (kept) mlog(m, "mc: speaking for %d XPRS callsign(s) again", kept);
    }
}
