/* See rns_resource.h. */
#include "rns_resource.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* The adaptive window, as RNS/Resource.py and the Dart receiver run it,
 * capped by the cache: the window is how far ahead of the frontier a part
 * may be, and a part ahead of the frontier has to be held. */
#define WIN_START     4
#define WIN_MIN0      2
#define WIN_MAX_SLOW  10
#define WIN_MAX_FAST  75
#define WIN_FLEX      4
#define FAST_ROUNDS   (WIN_MAX_SLOW - WIN_START - 2)

#define HASHMAP_EXHAUSTED 0xFF

/* The hashmap is a RING, not the whole segment's map: only the hashes of
 * parts between the frontier and the known horizon are ever consulted, and
 * the horizon runs at most two batches plus the window ahead (the sender
 * hands out RNS_RES_BATCH hashes per advertisement or HMU, and the next
 * batch is asked only when fewer than a window of known parts remain). A
 * 1 MB segment has ~2,260 parts: 9 KB as a map, 1 KB as a ring, and no
 * allocation while the transfer runs. */
#ifndef RNS_RES_RING
#define RNS_RES_RING 256
#endif
_Static_assert(RNS_RES_RING % 8 == 0, "ring is a whole number of bitmap bytes");
_Static_assert(RNS_RES_RING >= 2 * RNS_RES_BATCH + RNS_RES_CACHE_PARTS + WIN_MAX_FAST,
               "the ring must hold two batches beyond the widest window");

typedef struct {
    bool     used;
    uint32_t index;
    uint16_t len;
    uint8_t  data[RNS_RES_SDU_MAX];
} cached_part_t;

struct rns_res {
    const rns_link_t *link;
    rns_res_sink_t sink;

    /* Whole resource. */
    uint32_t total_segments, seg_index;      /* seg_index 0-based */
    uint8_t  original_hash[32];
    bool     have_original;
    uint32_t data_total;                     /* advertised d: */
    uint32_t delivered;                      /* plaintext handed to the sink */
    bool     done, failed;

    /* Current segment. */
    uint32_t n;                              /* parts */
    uint32_t transfer_size;                  /* encrypted stream length */
    uint8_t  res_hash[32], map_random[4];
    bool     encrypted;
    uint8_t  hashmap[4 * RNS_RES_RING];      /* hash of part i at slot i % RING */
    uint8_t  have[RNS_RES_RING / 8];         /* bitmap, same slots: hash known */
    uint32_t known;                          /* contiguous known hashes from 0 */
    bool     hmu_out;
    uint32_t frontier;                       /* next part index to stream */
    uint32_t outstanding[WIN_MAX_FAST];
    uint32_t n_out;
    /* The part cache is RNS_RES_CACHE_PARTS SEPARATE blocks, taken from the
     * heap the first time each is needed and kept until the receiver is
     * freed. One 472-byte block fits a fragmented heap where a 4 KB one does
     * not: on the C3 a receiver in one piece failed its calloc with 16 KB
     * free. A slot that cannot be had is no error: the part is dropped and
     * asked again, and the window never grows past the slots there are. */
    cached_part_t *cache[RNS_RES_CACHE_PARTS];
    uint32_t cache_n;                        /* slots allocated so far */
    uint32_t window, win_min, win_max, fast_rounds;
    bool     seg_complete;

    /* The stream through the segment: token = iv + ct + hmac. */
    uint32_t stream_off;                     /* bytes of the stream consumed */
    uint8_t  iv[16];
    rns_cbc_ctx_t  cbc;
    rns_hmac_ctx_t hmac;
    rns_sha256_ctx_t seg_sha;                /* over plaintext + map_random */
    rns_sha256_ctx_t prf_sha;                /* over plaintext + res_hash */
    bool     cbc_on, hmac_on;
    uint8_t  tail[32];                       /* the last 32 stream bytes: the HMAC */
    uint8_t  hold[16];                       /* last plaintext block, for the pad */
    bool     have_hold;
    uint32_t plain_seen;                     /* plaintext bytes decrypted */
    uint8_t  pend[15];                       /* ciphertext bytes short of a block */
    uint8_t  n_pend;
    uint8_t  proof[32];
    bool     have_proof;
    char     why[48];
};

static uint32_t cache_cap(void) { return RNS_RES_CACHE_PARTS; }

/* A free slot, allocating the next one when every allocated slot is in
 * use; NULL when the heap will not give another. */
static cached_part_t *cache_slot(struct rns_res *r)
{
    for (uint32_t k = 0; k < r->cache_n; k++) if (!r->cache[k]->used) return r->cache[k];
    if (r->cache_n >= RNS_RES_CACHE_PARTS) return NULL;
    cached_part_t *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    r->cache[r->cache_n++] = c;
    return c;
}

/* ── msgpack, the subset the advertisement and the HMU use ───────────── */

typedef struct { const uint8_t *p; size_t n, i; bool bad; } mp_t;

static uint32_t mp_u(mp_t *m, int bytes)
{
    if (m->i + (size_t)bytes > m->n) { m->bad = true; return 0; }
    uint32_t v = 0;
    for (int k = 0; k < bytes; k++) v = (v << 8) | m->p[m->i++];
    return v;
}

/* Skip one value; on an int or bin, report it. */
static void mp_value(mp_t *m, bool *is_int, uint64_t *ival,
                     const uint8_t **bin, size_t *blen)
{
    *is_int = false; *bin = NULL; *blen = 0;
    if (m->i >= m->n) { m->bad = true; return; }
    uint8_t c = m->p[m->i++];
    if (c <= 0x7f) { *is_int = true; *ival = c; return; }
    if (c >= 0xe0) { *is_int = true; *ival = (uint64_t)(int64_t)(int8_t)c; return; }
    if ((c & 0xe0) == 0xa0) { m->i += c & 0x1f; return; }          /* fixstr */
    switch (c) {
    case 0xc0: case 0xc2: case 0xc3: return;
    case 0xcc: *is_int = true; *ival = mp_u(m, 1); return;
    case 0xcd: *is_int = true; *ival = mp_u(m, 2); return;
    case 0xce: *is_int = true; *ival = mp_u(m, 4); return;
    case 0xcf: { uint64_t hi = mp_u(m, 4), lo = mp_u(m, 4); *is_int = true; *ival = (hi << 32) | lo; return; }
    case 0xc4: case 0xc5: case 0xc6: {
        size_t l = mp_u(m, c == 0xc4 ? 1 : c == 0xc5 ? 2 : 4);
        if (m->i + l > m->n) { m->bad = true; return; }
        *bin = m->p + m->i; *blen = l; m->i += l; return;
    }
    case 0xd9: m->i += mp_u(m, 1); return;
    case 0xda: m->i += mp_u(m, 2); return;
    default: m->bad = true; return;
    }
}

/* ── state ───────────────────────────────────────────────────────────── */

static void fail(rns_res_t *r, const char *why)
{
    if (r->failed || r->done) return;
    r->failed = true;
    snprintf(r->why, sizeof r->why, "%s", why);
    if (r->cbc_on) { rns_cbc_decrypt_free(&r->cbc); r->cbc_on = false; }
    if (r->hmac_on) { uint8_t t[32]; rns_hmac_final(&r->hmac, t); r->hmac_on = false; }
    if (r->sink.done) r->sink.done(false, why, r->sink.ctx);
}

rns_res_t *rns_res_new(const rns_link_t *link, const rns_res_sink_t *sink)
{
    rns_res_t *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->link = link;
    r->sink = *sink;
    r->total_segments = 1;
    return r;
}

void rns_res_free(rns_res_t *r)
{
    if (!r) return;
    if (r->cbc_on) rns_cbc_decrypt_free(&r->cbc);
    if (r->hmac_on) { uint8_t t[32]; rns_hmac_final(&r->hmac, t); }
    for (uint32_t k = 0; k < r->cache_n; k++) free(r->cache[k]);
    free(r);
}

bool rns_res_done(const rns_res_t *r) { return r && r->done; }
bool rns_res_failed(const rns_res_t *r) { return r && r->failed; }
uint32_t rns_res_total(const rns_res_t *r) { return r ? r->data_total : 0; }
uint32_t rns_res_received(const rns_res_t *r) { return r ? r->delivered : 0; }

const char *rns_res_state(const rns_res_t *r, char *buf, size_t cap)
{
    snprintf(buf, cap, "seg=%u/%u frontier=%u/%u known=%u win=%u out=%u%s%s",
             (unsigned)r->seg_index + 1, (unsigned)r->total_segments,
             (unsigned)r->frontier, (unsigned)r->n, (unsigned)r->known,
             (unsigned)r->window, (unsigned)r->n_out,
             r->done ? " done" : "", r->failed ? " FAILED" : "");
    return buf;
}

/* Ring slots: a part's hash lives at i % RING while i is within RING of
 * the frontier; the slot is reused for i + RING once the frontier has
 * passed i, so the known bit is cleared as the frontier advances. */
static bool in_ring(const rns_res_t *r, uint32_t i)
{ return i >= r->frontier && i < r->frontier + RNS_RES_RING; }
static uint8_t *slot_of(rns_res_t *r, uint32_t i) { return r->hashmap + 4 * (i % RNS_RES_RING); }
static const uint8_t *slot_rd(const rns_res_t *r, uint32_t i) { return r->hashmap + 4 * (i % RNS_RES_RING); }
static bool known_bit(const rns_res_t *r, uint32_t i)
{ return in_ring(r, i) && ((r->have[(i % RNS_RES_RING) >> 3] >> (i & 7)) & 1); }
static void set_known(rns_res_t *r, uint32_t i)
{ if (in_ring(r, i)) r->have[(i % RNS_RES_RING) >> 3] |= (uint8_t)(1u << (i & 7)); }
static void clear_known(rns_res_t *r, uint32_t i)
{ r->have[(i % RNS_RES_RING) >> 3] &= (uint8_t)~(1u << (i & 7)); }
/* The frontier moves past its part: that slot now belongs to part + RING. */
static void advance(rns_res_t *r) { clear_known(r, r->frontier); r->frontier++; }

static int find_hash(const rns_res_t *r, const uint8_t h[4])
{
    /* Only parts at or ahead of the frontier can still arrive; a collision
     * far away must not place a part wrongly, so the search is confined. */
    uint32_t lo = r->frontier, hi = r->frontier + r->window + cache_cap();
    if (hi > r->known) hi = r->known;
    for (uint32_t i = lo; i < hi; i++)
        if (memcmp(slot_rd(r, i), h, 4) == 0) return (int)i;
    return -1;
}

static void out_remove(rns_res_t *r, uint32_t idx)
{
    for (uint32_t k = 0; k < r->n_out; k++)
        if (r->outstanding[k] == idx) { r->outstanding[k] = r->outstanding[--r->n_out]; return; }
}

static bool out_has(const rns_res_t *r, uint32_t idx)
{
    for (uint32_t k = 0; k < r->n_out; k++) if (r->outstanding[k] == idx) return true;
    return false;
}

static cached_part_t *cache_find(rns_res_t *r, uint32_t idx)
{
    for (uint32_t k = 0; k < r->cache_n; k++)
        if (r->cache[k]->used && r->cache[k]->index == idx) return r->cache[k];
    return NULL;
}

static void grow_window(rns_res_t *r)
{
    if (r->window < r->win_max) {
        r->window++;
        if (r->window - r->win_min > WIN_FLEX - 1) r->win_min++;
    }
    if (r->fast_rounds < FAST_ROUNDS) {
        r->fast_rounds++;
        if (r->fast_rounds == FAST_ROUNDS) r->win_max = WIN_MAX_FAST;
    }
    if (r->win_max > cache_cap()) r->win_max = cache_cap();
    if (r->window > r->win_max) r->window = r->win_max;
}

static void shrink_window(rns_res_t *r)
{
    if (r->window > r->win_min) {
        r->window--;
        if (r->win_max > r->win_min) {
            r->win_max--;
            if (r->win_max - r->window > WIN_FLEX - 1) r->win_max--;
        }
    }
    r->fast_rounds = 0;
}

/* ── the stream ──────────────────────────────────────────────────────── */

static void stream_begin(rns_res_t *r)
{
    r->stream_off = 0;
    r->cbc_on = r->hmac_on = false;
    r->have_hold = false;
    r->plain_seen = 0;
    r->n_pend = 0;
    r->have_proof = false;
    rns_sha256_init(&r->seg_sha);
    rns_sha256_init(&r->prf_sha);
    if (r->encrypted) {
        r->hmac_on = rns_hmac_init(&r->hmac, r->link->key, 32);
    }
}

/* Plaintext of the segment, after the 4-byte stream random. */
static bool plain_out(rns_res_t *r, const uint8_t *d, size_t len)
{
    rns_sha256_update(&r->seg_sha, d, len);
    rns_sha256_update(&r->prf_sha, d, len);
    r->delivered += (uint32_t)len;
    return !r->sink.write || r->sink.write(d, len, r->sink.ctx);
}

/* Decrypted blocks: the first four bytes are the random, the last block
 * carries the PKCS7 pad, so one block is always held back. */
static bool plain_blocks(rns_res_t *r, uint8_t *pt, size_t len)
{
    size_t off = 0;
    while (off < len) {
        if (r->have_hold) {
            const uint8_t *d = r->hold;
            size_t dl = 16;
            if (r->plain_seen < 4) {           /* the stream random */
                size_t skip = 4 - r->plain_seen;
                if (skip > dl) skip = dl;
                d += skip; dl -= skip; r->plain_seen += (uint32_t)skip;
            }
            if (dl && !plain_out(r, d, dl)) return false;
            r->plain_seen += (uint32_t)dl;
        }
        memcpy(r->hold, pt + off, 16);
        r->have_hold = true;
        off += 16;
    }
    return true;
}

static bool stream_feed(rns_res_t *r, const uint8_t *d, size_t len)
{
    if (!r->encrypted) {
        /* Unencrypted resource: the 4-byte random then the data. */
        while (len && r->stream_off < 4) { d++; len--; r->stream_off++; }
        r->stream_off += (uint32_t)len;
        return plain_out(r, d, len);
    }
    const uint32_t ct_end = r->transfer_size - 32;
    while (len) {
        uint32_t off = r->stream_off;
        if (off < 16) {                        /* the IV */
            size_t k = 16 - off; if (k > len) k = len;
            memcpy(r->iv + off, d, k);
            rns_hmac_update(&r->hmac, d, k);
            d += k; len -= k; r->stream_off += (uint32_t)k;
            if (r->stream_off == 16) {
                if (!rns_cbc_decrypt_init(&r->cbc, r->link->key + 32, r->iv))
                    return false;
                r->cbc_on = true;
            }
            continue;
        }
        if (off < ct_end) {                    /* ciphertext */
            size_t k = ct_end - off; if (k > len) k = len;
            rns_hmac_update(&r->hmac, d, k);
            /* Whole blocks through CBC; a remainder waits for the next part. */
            uint8_t buf[RNS_RES_SDU_MAX + 16];
            size_t bl = 0;
            if (r->n_pend) { memcpy(buf, r->pend, r->n_pend); bl = r->n_pend; r->n_pend = 0; }
            size_t take = k;
            if (bl + take > sizeof buf) take = sizeof buf - bl;
            memcpy(buf + bl, d, take); bl += take;
            size_t whole = bl & ~(size_t)15;
            if (whole) {
                if (!rns_cbc_decrypt_update(&r->cbc, buf, whole, buf)) return false;
                if (!plain_blocks(r, buf, whole)) return false;
            }
            r->n_pend = (uint8_t)(bl - whole);
            if (r->n_pend) memcpy(r->pend, buf + whole, r->n_pend);
            d += take; len -= take; r->stream_off += (uint32_t)take;
            continue;
        }
        /* the HMAC */
        size_t k = r->transfer_size - off; if (k > len) k = len;
        memcpy(r->tail + (off - ct_end), d, k);
        d += k; len -= k; r->stream_off += (uint32_t)k;
    }
    return true;
}

static bool stream_end(rns_res_t *r)
{
    if (r->encrypted) {
        if (r->stream_off != r->transfer_size || r->n_pend) { fail(r, "stream short"); return false; }
        uint8_t mac[32];
        rns_hmac_final(&r->hmac, mac); r->hmac_on = false;
        rns_cbc_decrypt_free(&r->cbc); r->cbc_on = false;
        uint8_t diff = 0;
        for (int i = 0; i < 32; i++) diff |= (uint8_t)(mac[i] ^ r->tail[i]);
        if (diff) { fail(r, "stream HMAC"); return false; }
        if (!r->have_hold) { fail(r, "stream empty"); return false; }
        uint8_t pad = r->hold[15];
        if (pad == 0 || pad > 16) { fail(r, "stream pad"); return false; }
        size_t dl = 16 - pad;
        const uint8_t *d = r->hold;
        if (r->plain_seen < 4) {
            size_t skip = 4 - r->plain_seen; if (skip > dl) skip = dl;
            d += skip; dl -= skip; r->plain_seen += (uint32_t)skip;
        }
        if (dl && !plain_out(r, d, dl)) { fail(r, "sink"); return false; }
        r->have_hold = false;
    }
    uint8_t h[32];
    rns_sha256_update(&r->seg_sha, r->map_random, 4);
    rns_sha256_final(&r->seg_sha, h);
    if (memcmp(h, r->res_hash, 32) != 0) { fail(r, "segment hash"); return false; }
    rns_sha256_update(&r->prf_sha, r->res_hash, 32);
    rns_sha256_final(&r->prf_sha, r->proof);
    r->have_proof = true;
    return true;
}

/* ── parts ───────────────────────────────────────────────────────────── */

static bool take_part(rns_res_t *r, const uint8_t *d, size_t len)
{
    /* Stream the frontier part and everything cached right behind it. */
    if (!stream_feed(r, d, len)) { fail(r, "sink"); return false; }
    advance(r);
    for (;;) {
        cached_part_t *c = cache_find(r, r->frontier);
        if (!c) break;
        c->used = false;
        if (!stream_feed(r, c->data, c->len)) { fail(r, "sink"); return false; }
        advance(r);
    }
    return true;
}

static void finish_segment(rns_res_t *r)
{
    if (!stream_end(r)) return;
    r->seg_complete = true;
    if (r->seg_index + 1 >= r->total_segments) {
        if (r->data_total && r->delivered != r->data_total) { fail(r, "total size"); return; }
        r->done = true;
        if (r->sink.done) r->sink.done(true, "complete", r->sink.ctx);
    }
}

static bool ingest_part(rns_res_t *r, const uint8_t *d, size_t len)
{
    if (r->seg_complete || r->done || r->failed || !r->n) return true;
    if (len > RNS_RES_SDU_MAX) { fail(r, "part too large"); return false; }
    uint8_t buf[RNS_RES_SDU_MAX + 4], mh[32];
    memcpy(buf, d, len);
    memcpy(buf + len, r->map_random, 4);
    rns_sha256(buf, len + 4, mh);
    int idx = find_hash(r, mh);
    if (idx < 0) return true;                 /* unknown, or far away */
    if ((uint32_t)idx < r->frontier || cache_find(r, (uint32_t)idx)) return true; /* dup */
    out_remove(r, (uint32_t)idx);
    if ((uint32_t)idx == r->frontier) {
        if (!take_part(r, d, len)) return false;
    } else {
        cached_part_t *slot = cache_slot(r);
        if (!slot) return true;               /* no room: it will be asked again */
        slot->used = true; slot->index = (uint32_t)idx; slot->len = (uint16_t)len;
        memcpy(slot->data, d, len);
    }
    if (r->n_out == 0 && r->frontier < r->n) grow_window(r);
    if (r->frontier >= r->n) finish_segment(r);
    return !r->failed;
}

/* ── requests ────────────────────────────────────────────────────────── */

static void emit(rns_res_t *r, uint8_t ctx, const uint8_t *plain, size_t len,
                 void (*send)(const uint8_t *, size_t, void *), void *sctx)
{
    uint8_t pkt[RNS_MTU + 64];
    int n = rns_link_encrypt(r->link, ctx, plain, len, pkt, sizeof pkt);
    if (n > 0 && send) send(pkt, (size_t)n, sctx);
}

static void pump(rns_res_t *r, void (*send)(const uint8_t *, size_t, void *), void *sctx)
{
    if (r->done || r->failed || r->seg_complete || !r->n) return;
    /* More of the hashmap, asked at a batch boundary, when the known parts
     * still to come are fewer than the window. */
    if (r->known < r->n && !r->hmu_out) {
        uint32_t known_not_received = r->known > r->frontier ? r->known - r->frontier : 0;
        if (known_not_received < r->window && r->known > 0 && r->known % RNS_RES_BATCH == 0) {
            uint8_t req[1 + 4 + 32];
            req[0] = HASHMAP_EXHAUSTED;
            memcpy(req + 1, slot_rd(r, r->known - 1), 4);
            memcpy(req + 5, r->res_hash, 32);
            r->hmu_out = true;
            emit(r, RNS_CTX_RESOURCE_REQ, req, sizeof req, send, sctx);
        }
    }
    /* Fill the window with known, missing, not-outstanding parts, never
     * further ahead of the frontier than the cache can hold. */
    uint32_t cap = r->window > r->n_out ? r->window - r->n_out : 0;
    uint32_t limit = r->frontier + cache_cap() + 1;
    if (limit > r->known) limit = r->known;
    uint8_t req[1 + 32 + 4 * RNS_RES_BATCH];
    req[0] = 0x00;
    memcpy(req + 1, r->res_hash, 32);
    size_t rl = 33;
    uint32_t picked = 0;
    for (uint32_t i = r->frontier; i < limit && picked < cap; i++) {
        if (cache_find(r, i) || out_has(r, i)) continue;
        if (r->n_out >= WIN_MAX_FAST) break;
        r->outstanding[r->n_out++] = i;
        memcpy(req + rl, slot_rd(r, i), 4);
        rl += 4;
        picked++;
        if (rl + 4 > sizeof req) break;
    }
    if (picked) emit(r, RNS_CTX_RESOURCE_REQ, req, rl, send, sctx);
}

static void send_proof(rns_res_t *r, void (*send)(const uint8_t *, size_t, void *), void *sctx)
{
    if (!r->have_proof) return;
    uint8_t data[64], pkt[RNS_MTU];
    memcpy(data, r->res_hash, 32);
    memcpy(data + 32, r->proof, 32);
    int n = rns_link_proof_packet(r->link, RNS_CTX_RESOURCE_PRF, data, 64, pkt, sizeof pkt);
    if (n > 0 && send) send(pkt, (size_t)n, sctx);
}

/* ── the advertisement ───────────────────────────────────────────────── */

static bool ingest_adv(rns_res_t *r, const uint8_t *plain, size_t len)
{
    if (len < 1 || (plain[0] & 0xf0) != 0x80) return false;   /* fixmap */
    mp_t m = { plain, len, 1, false };
    int pairs = plain[0] & 0x0f;
    uint64_t t = 0, d = 0, n = 0, i = 0, l = 0, f = 1;
    const uint8_t *h = NULL, *rnd = NULL, *o = NULL, *map = NULL;
    size_t hl = 0, rl = 0, ol = 0, ml = 0;
    for (int k = 0; k < pairs && !m.bad; k++) {
        if (m.i >= m.n) return false;
        uint8_t kc = m.p[m.i++];
        if ((kc & 0xe0) != 0xa0 || (kc & 0x1f) != 1 || m.i >= m.n) return false;
        char key = (char)m.p[m.i++];
        bool is_int; uint64_t iv; const uint8_t *bin; size_t bl;
        mp_value(&m, &is_int, &iv, &bin, &bl);
        switch (key) {
        case 't': t = iv; break;
        case 'd': d = iv; break;
        case 'n': n = iv; break;
        case 'i': i = iv; break;
        case 'l': l = iv; break;
        case 'f': f = iv; break;
        case 'h': h = bin; hl = bl; break;
        case 'r': rnd = bin; rl = bl; break;
        case 'o': o = bin; ol = bl; break;
        case 'm': map = bin; ml = bl; break;
        default: break;
        }
    }
    if (m.bad || hl != 32 || rl != 4 || n > 0x100000) return false;
    if (i <= 1) {
        if (ol == 32) memcpy(r->original_hash, o, 32);
        r->have_original = ol == 32;
        r->total_segments = l < 1 ? 1 : (uint32_t)l;
        r->delivered = 0;
    } else if (r->have_original && (ol != 32 || memcmp(o, r->original_hash, 32) != 0)) {
        return false;
    }
    r->seg_index = i ? (uint32_t)i - 1 : 0;
    r->n = (uint32_t)n;
    r->transfer_size = (uint32_t)t;
    r->data_total = (uint32_t)d;
    memcpy(r->res_hash, h, 32);
    memcpy(r->map_random, rnd, 4);
    r->encrypted = (f & 1) != 0;
    if (r->encrypted && r->transfer_size < 48) return false;

    memset(r->have, 0, sizeof r->have);
    r->known = 0; r->hmu_out = false; r->frontier = 0; r->n_out = 0;
    for (uint32_t k = 0; k < r->cache_n; k++) r->cache[k]->used = false;
    r->window = WIN_START; r->win_min = WIN_MIN0; r->win_max = WIN_MAX_SLOW;
    if (r->win_max > cache_cap()) r->win_max = cache_cap();
    r->fast_rounds = 0;
    r->seg_complete = false;
    uint32_t first = (uint32_t)(ml / 4);
    if (first > r->n) first = r->n;
    if (first > RNS_RES_RING) first = RNS_RES_RING;   /* parts 0..first-1 are slots 0..first-1 */
    memcpy(r->hashmap, map, 4 * first);
    for (uint32_t k = 0; k < first; k++) set_known(r, k);
    r->known = first;
    stream_begin(r);
    if (r->n == 0) finish_segment(r);
    return true;
}

static void ingest_hmu(rns_res_t *r, const uint8_t *plain, size_t len)
{
    if (len < 32 + 2 || memcmp(plain, r->res_hash, 32) != 0) return;
    mp_t m = { plain, len, 32, false };
    if (m.i >= m.n || (m.p[m.i] & 0xf0) != 0x90) return;        /* fixarray */
    int items = m.p[m.i++] & 0x0f;
    if (items < 2) return;
    bool is_int; uint64_t seg; const uint8_t *bin; size_t bl;
    mp_value(&m, &is_int, &seg, &bin, &bl);
    if (!is_int || m.bad) return;
    uint64_t dummy; const uint8_t *map; size_t ml;
    mp_value(&m, &is_int, &dummy, &map, &ml);
    if (!map || m.bad) return;
    uint32_t base = (uint32_t)seg * RNS_RES_BATCH;
    uint32_t count = (uint32_t)(ml / 4);
    for (uint32_t k = 0; k < count; k++) {
        uint32_t idx = base + k;
        if (idx < r->n && in_ring(r, idx) && !known_bit(r, idx)) {
            memcpy(slot_of(r, idx), map + 4 * k, 4);
            set_known(r, idx);
        }
    }
    while (r->known < r->n && known_bit(r, r->known)) r->known++;
    r->hmu_out = false;
}

/* ── the public drivers ──────────────────────────────────────────────── */

bool rns_res_feed(rns_res_t *r, const rns_packet_t *p,
                  void (*send)(const uint8_t *, size_t, void *), void *sctx)
{
    if (!r || r->failed) return false;
    if (r->done) return true;
    uint8_t plain[RNS_MTU];
    switch (p->context) {
    case RNS_CTX_RESOURCE_ADV: {
        int n = rns_link_decrypt(r->link, p, plain, sizeof plain);
        if (n < 0) return true;
        if (!ingest_adv(r, plain, (size_t)n)) { fail(r, "advertisement"); return false; }
        if (r->seg_complete) send_proof(r, send, sctx); else pump(r, send, sctx);
        return !r->failed;
    }
    case RNS_CTX_RESOURCE_HMU: {
        int n = rns_link_decrypt(r->link, p, plain, sizeof plain);
        if (n < 0) return true;
        ingest_hmu(r, plain, (size_t)n);
        pump(r, send, sctx);
        return true;
    }
    case RNS_CTX_RESOURCE:
        if (!ingest_part(r, p->data, p->data_len)) return false;
        if (r->seg_complete) send_proof(r, send, sctx); else pump(r, send, sctx);
        return !r->failed;
    default:
        return true;
    }
}

void rns_res_retry(rns_res_t *r,
                   void (*send)(const uint8_t *, size_t, void *), void *sctx)
{
    if (!r || r->done || r->failed) return;
    if (r->seg_complete) { send_proof(r, send, sctx); return; }
    shrink_window(r);
    r->n_out = 0;
    r->hmu_out = false;
    pump(r, send, sctx);
}
