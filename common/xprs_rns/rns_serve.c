/* See rns_serve.h. */
#include "rns_serve.h"
#include "rns_msgpack.h"
#include "rns_resource.h"

#include <stdlib.h>
#include <string.h>

/* Why a link was refused, on the board's log; nothing on a host build. */
#ifdef ESP_PLATFORM
#include "esp_log.h"
#define RNS_SERVE_LOG(...) ESP_LOGW("rns_serve", __VA_ARGS__)
#define RNS_SERVE_INFO(...) ESP_LOGI("rns_serve", __VA_ARGS__)
#else
#define RNS_SERVE_LOG(...) ((void)0)
#define RNS_SERVE_INFO(...) ((void)0)
#endif

#define MAX_EFFICIENT  1048575u        /* Resource.MAX_EFFICIENT_SIZE: one segment */
#define MAP_LEN        4               /* map hash bytes */
#define BATCH          74              /* map hashes per advertisement / HMU */
#define WINDOW_MAX     75
#define GUARD          (2 * WINDOW_MAX + BATCH)
#define HMU_FLAG       0xFF
#define QUEUE          3               /* responses waiting behind a Resource */
#define PRE_LEN        19              /* 0x92, bin8 header, the 16-byte id */
#define PART_MAX       (RNS_MTU - 36)
#define ADV_RETRY_S    5               /* no request yet: advertise again */
#define ADV_TRIES      4               /* RNS Resource.MAX_ADV_RETRIES */
#define TX_STALL_S     30              /* asked before, silent since: give up */
#define EVICT_IDLE_S   60              /* full: a link quiet this long gives way */

/* ── a Resource being sent ─────────────────────────────────────────────── */

typedef struct {
    rns_src_t *data;
    uint8_t   pre[PRE_LEN];            /* [rid, ... : what precedes data */
    size_t    total;                   /* PRE_LEN + data->len */
    uint32_t  nseg, seg;
    size_t    seg_off, seg_len, padded, enc_len;
    uint8_t   rand4[4], mrand[4], iv[16];
    uint8_t   hash[32], proof[32], orig[32], hmac[32];
    uint32_t  nparts, rmch;
    int       sdu;
    uint8_t  *maps;                    /* nparts * 4 */
    uint8_t  *chain;                   /* nparts * 16: the CBC block before each part */
    uint32_t  last_s;                  /* advertised or asked, last */
    uint32_t  start_s;
    uint8_t   adv_tries;
    bool      asked;                   /* a request came for this segment */
} tx_t;

typedef struct { rns_src_t *data; uint8_t rid[RNS_HASH_LEN]; } queued_t;

/* An upload: a request too large for one packet, gathered as it streams. */
typedef struct {
    uint8_t *buf;
    size_t   n, cap;
    bool     over, done, ok;
    rns_sha256_ctx_t sha;
} up_t;

typedef struct {
    uint32_t   gen;
    rns_link_t link;
    uint8_t    proof[128];
    uint16_t   proof_len;
    uint32_t   last_s;
    rns_res_t *rx;
    up_t      *up;
    tx_t      *tx;
    queued_t   q[QUEUE];
    int        nq;
} slink_t;

static rns_serve_cfg_t s_cfg;
static slink_t *s_links[RNS_SERVE_LINKS_MAX];
static uint32_t s_gen;
static uint32_t s_now;                 /* the clock the host last gave */
/* Scratch, claimed at init and kept: a board without PSRAM fragments its
 * heap within minutes of boot (a C3's largest free block falls under 1 KB
 * with 15 KB free), and a server that claimed these per link would refuse
 * every link after that. Four blocks of about 500 bytes rather than one of
 * 2 KB, so a late claim (init found no room) still has a chance. */
#define PKT_CAP   (RNS_MTU + 64)
#define PLAIN_CAP (RNS_MTU + 16)
#define PT_CAP    (PART_MAX + 32)
static uint8_t *s_pkt, *s_plain, *s_part, *s_pt;

static bool scratch_get(void)
{
    if (!s_pkt) s_pkt = malloc(PKT_CAP);
    if (!s_plain) s_plain = malloc(PLAIN_CAP);
    if (!s_part) s_part = malloc(PART_MAX);
    if (!s_pt) s_pt = malloc(PT_CAP);
    return s_pkt && s_plain && s_part && s_pt;
}

static void out(const uint8_t *pkt, size_t len, void *ctx)
{
    (void)ctx;
    if (s_cfg.send) s_cfg.send(pkt, len, s_cfg.ctx);
}

static void send_enc(slink_t *s, uint8_t ctx, const uint8_t *plain, size_t len)
{
    if (!s_pkt) return;
    int n = rns_link_encrypt(&s->link, ctx, plain, len, s_pkt, PKT_CAP);
    if (n > 0) out(s_pkt, (size_t)n, NULL);
}

/* ── the payload of a Resource: [rid, ...] then the host's data ───────── */

static bool payload_read(tx_t *t, size_t off, uint8_t *buf, size_t n)
{
    while (n) {
        if (off < PRE_LEN) {
            size_t k = PRE_LEN - off < n ? PRE_LEN - off : n;
            memcpy(buf, t->pre + off, k);
            off += k; buf += k; n -= k;
            continue;
        }
        return t->data->read(t->data, off - PRE_LEN, buf, n);
    }
    return true;
}

/* The segment's plaintext stream: 4 random bytes, the segment, PKCS7. */
static bool stream_read(tx_t *t, size_t j, uint8_t *buf, size_t n)
{
    size_t body_end = 4 + t->seg_len;
    uint8_t pad = (uint8_t)(t->padded - body_end);
    while (n) {
        if (j < 4) {
            *buf++ = t->rand4[j++]; n--;
        } else if (j < body_end) {
            size_t k = body_end - j < n ? body_end - j : n;
            if (!payload_read(t, t->seg_off + (j - 4), buf, k)) return false;
            j += k; buf += k; n -= k;
        } else {
            *buf++ = pad; j++; n--;
        }
    }
    return true;
}

static void tx_free(tx_t *t)
{
    if (!t) return;
    free(t->maps);
    free(t->chain);
    if (t->data && t->data->release) t->data->release(t->data);
    free(t);
}

/* Assembling parts while the token streams past once, to learn each part's
 * map hash and the CBC block before it. */
typedef struct {
    tx_t *t;
    size_t off;                        /* token offset of the next byte */
    uint32_t part;
    size_t in_part;
    rns_sha256_ctx_t sha;
    const uint8_t *prev;               /* the ciphertext block before the current one */
} asm_t;

static void asm_feed(asm_t *a, const uint8_t *b, size_t n)
{
    tx_t *t = a->t;
    while (n) {
        if (a->in_part == 0) {
            rns_sha256_init(&a->sha);
            memcpy(t->chain + 16 * a->part, a->prev, 16);
        }
        size_t room = (size_t)t->sdu - a->in_part;
        size_t k = n < room ? n : room;
        rns_sha256_update(&a->sha, b, k);
        a->in_part += k; a->off += k; b += k; n -= k;
        if (a->in_part == (size_t)t->sdu || a->off == t->enc_len) {
            uint8_t h[32];
            rns_sha256_update(&a->sha, t->mrand, 4);
            rns_sha256_final(&a->sha, h);
            memcpy(t->maps + MAP_LEN * a->part, h, MAP_LEN);
            a->part++;
            a->in_part = 0;
        }
    }
}

static bool tx_segment(tx_t *t, const rns_link_t *l, uint32_t idx)
{
    t->seg = idx;
    t->rmch = 0;
    t->seg_off = (size_t)idx * MAX_EFFICIENT;
    t->seg_len = t->total - t->seg_off < MAX_EFFICIENT ? t->total - t->seg_off : MAX_EFFICIENT;
    rns_random_bytes(t->rand4, 4);
    rns_random_bytes(t->mrand, 4);
    rns_random_bytes(t->iv, 16);
    t->padded = ((4 + t->seg_len) / 16 + 1) * 16;
    t->enc_len = 16 + t->padded + 32;
    t->sdu = rns_link_sdu(l);
    if (t->sdu > PART_MAX) t->sdu = PART_MAX;
    t->nparts = (uint32_t)((t->enc_len + (size_t)t->sdu - 1) / (size_t)t->sdu);
    free(t->maps); free(t->chain);
    t->maps = malloc((size_t)t->nparts * MAP_LEN);
    t->chain = malloc((size_t)t->nparts * 16);
    if (!t->maps || !t->chain) return false;

    /* The resource hash: the segment's plaintext, then the map random. */
    uint8_t buf[256];
    rns_sha256_ctx_t h;
    rns_sha256_init(&h);
    for (size_t o = 0; o < t->seg_len; o += sizeof buf) {
        size_t k = t->seg_len - o < sizeof buf ? t->seg_len - o : sizeof buf;
        if (!payload_read(t, t->seg_off + o, buf, k)) return false;
        rns_sha256_update(&h, buf, k);
    }
    rns_sha256_update(&h, t->mrand, 4);
    rns_sha256_final(&h, t->hash);
    if (idx == 0) memcpy(t->orig, t->hash, 32);

    /* One pass over the token: the proof (the segment, then the hash), the
     * ciphertext's HMAC, and each part's map hash and chaining block. */
    rns_sha256_ctx_t pr;
    rns_sha256_init(&pr);
    rns_hmac_ctx_t mac;
    if (!rns_hmac_init(&mac, l->key, 32)) return false;
    rns_hmac_update(&mac, t->iv, 16);
    asm_t a = { .t = t, .prev = t->iv };
    asm_feed(&a, t->iv, 16);
    uint8_t prev[16], ct[256];
    memcpy(prev, t->iv, 16);
    for (size_t j = 0; j < t->padded; j += sizeof buf) {
        size_t k = t->padded - j < sizeof buf ? t->padded - j : sizeof buf;
        if (!stream_read(t, j, buf, k)) { rns_hmac_final(&mac, ct); return false; }
        /* The proof covers the segment itself: the stream minus its 4
         * random bytes and its padding. */
        size_t s0 = j < 4 ? 4 : j, s1 = j + k < 4 + t->seg_len ? j + k : 4 + t->seg_len;
        if (s1 > s0) rns_sha256_update(&pr, buf + (s0 - j), s1 - s0);
        if (!rns_aes256_cbc_encrypt(l->key + 32, prev, buf, k, ct)) {
            rns_hmac_final(&mac, ct);
            return false;
        }
        rns_hmac_update(&mac, ct, k);
        for (size_t b = 0; b < k; b += 16) {
            asm_feed(&a, ct + b, 16);
            memcpy(prev, ct + b, 16);
            a.prev = prev;
        }
    }
    rns_hmac_final(&mac, t->hmac);
    asm_feed(&a, t->hmac, 32);
    rns_sha256_update(&pr, t->hash, 32);
    rns_sha256_final(&pr, t->proof);
    return a.part == t->nparts;
}

/* Part [i] of the current segment, made again: [out] gets its bytes. */
static int tx_part(tx_t *t, const rns_link_t *l, uint32_t i, uint8_t *outb)
{
    size_t o = (size_t)i * (size_t)t->sdu;
    size_t plen = t->enc_len - o < (size_t)t->sdu ? t->enc_len - o : (size_t)t->sdu;
    size_t end = o + plen, w = 0;
    /* The IV. */
    for (size_t k = o; k < end && k < 16; k++) outb[w++] = t->iv[k];
    /* The ciphertext: from the block holding the first byte wanted, chained
     * from the block before it. */
    size_t c0 = (o > 16 ? o : 16) - 16, c1 = (end < 16 + t->padded ? end : 16 + t->padded);
    if (c1 > 16 && c1 - 16 > c0) {
        c1 -= 16;
        size_t b0 = c0 / 16, b1 = (c1 + 15) / 16;
        const uint8_t *prev = (o >= 16 && b0 > 0) ? t->chain + 16 * i : t->iv;
        uint8_t *pt = s_pt;                /* encrypted in place */
        size_t n = (b1 - b0) * 16;
        if (!pt || n > PT_CAP) return -1;
        if (!stream_read(t, b0 * 16, pt, n)) return -1;
        if (!rns_aes256_cbc_encrypt(l->key + 32, prev, pt, n, pt)) return -1;
        memcpy(outb + w, pt + (c0 - b0 * 16), c1 - c0);
        w += c1 - c0;
    }
    /* The HMAC. */
    size_t h0 = 16 + t->padded;
    for (size_t k = o > h0 ? o : h0; k < end; k++) outb[w++] = t->hmac[k - h0];
    return w == plen ? (int)plen : -1;
}

static void tx_send_part(slink_t *s, uint32_t i)
{
    uint8_t *part = s_part;
    if (!part || !s_pkt) return;
    int n = tx_part(s->tx, &s->link, i, part);
    if (n <= 0) return;
    rns_packet_t p;
    memset(&p, 0, sizeof p);
    p.header_type = RNS_HEADER_1;
    p.dest_type = RNS_DEST_LINK;
    p.packet_type = RNS_PACKET_DATA;
    memcpy(p.dest, s->link.link_id, RNS_HASH_LEN);
    p.context = RNS_CTX_RESOURCE;
    p.data = part;
    p.data_len = (size_t)n;
    int pn = rns_packet_build(&p, s_pkt, PKT_CAP);
    if (pn > 0) out(s_pkt, (size_t)pn, NULL);
}

static void tx_advertise(slink_t *s)
{
    tx_t *t = s->tx;
    uint8_t adv[420];
    mp_wr_t w;
    mp_wr_init(&w, adv, sizeof adv);
    uint32_t first = t->nparts < BATCH ? t->nparts : BATCH;
    mp_map(&w, 11);
    mp_cstr(&w, "t"); mp_uint(&w, t->enc_len);
    mp_cstr(&w, "d"); mp_uint(&w, t->total);
    mp_cstr(&w, "n"); mp_uint(&w, t->nparts);
    mp_cstr(&w, "h"); mp_bin(&w, t->hash, 32);
    mp_cstr(&w, "r"); mp_bin(&w, t->mrand, 4);
    mp_cstr(&w, "o"); mp_bin(&w, t->orig, 32);
    mp_cstr(&w, "i"); mp_uint(&w, t->seg + 1);
    mp_cstr(&w, "l"); mp_uint(&w, t->nseg);
    mp_cstr(&w, "q"); mp_nil(&w);
    mp_cstr(&w, "f"); mp_uint(&w, 0x01);
    mp_cstr(&w, "m"); mp_bin(&w, t->maps, (size_t)first * MAP_LEN);
    if (!w.err) send_enc(s, RNS_CTX_RESOURCE_ADV, adv, w.n);
    if (t->adv_tries) RNS_SERVE_INFO("answer advertised again (%u)", (unsigned)t->adv_tries + 1);
    t->last_s = s_now;
    t->adv_tries++;
    t->asked = false;
}

static void tx_hmu(slink_t *s, uint32_t segment)
{
    tx_t *t = s->tx;
    uint32_t start = segment * BATCH;
    uint32_t end = (segment + 1) * BATCH < t->nparts ? (segment + 1) * BATCH : t->nparts;
    if (start >= end) return;
    uint8_t b[32 + 16 + BATCH * MAP_LEN];
    memcpy(b, t->hash, 32);
    mp_wr_t w;
    mp_wr_init(&w, b + 32, sizeof b - 32);
    mp_arr(&w, 2);
    mp_uint(&w, segment);
    mp_bin(&w, t->maps + (size_t)start * MAP_LEN, (size_t)(end - start) * MAP_LEN);
    if (!w.err) send_enc(s, RNS_CTX_RESOURCE_HMU, b, 32 + w.n);
}

/* A RESOURCE_REQ (decrypted): the parts it names, then perhaps the next
 * hashmap batch (RNS/Resource.request, reticulum-dart handleRequest). */
static void tx_request(slink_t *s, const uint8_t *d, size_t n)
{
    tx_t *t = s->tx;
    if (!t || n < 1) return;
    bool more = d[0] == HMU_FLAG;
    size_t pad = more ? 1 + MAP_LEN : 1;
    if (n < pad + 32 || memcmp(d + pad, t->hash, 32) != 0) return;
    t->asked = true;
    t->last_s = s_now;
    uint32_t end = t->rmch + GUARD < t->nparts ? t->rmch + GUARD : t->nparts;
    for (uint32_t i = t->rmch; i < end; i++)
        for (size_t off = pad + 32; off + MAP_LEN <= n; off += MAP_LEN)
            if (memcmp(t->maps + (size_t)i * MAP_LEN, d + off, MAP_LEN) == 0) {
                tx_send_part(s, i);
                break;
            }
    if (more) {
        const uint8_t *last = d + 1;
        uint32_t part = t->rmch;
        for (uint32_t i = t->rmch; i < end; i++) {
            part++;
            if (memcmp(t->maps + (size_t)i * MAP_LEN, last, MAP_LEN) == 0) break;
        }
        t->rmch = part > 1 + WINDOW_MAX ? part - 1 - WINDOW_MAX : 0;
        if (part % BATCH == 0) tx_hmu(s, part / BATCH);
    }
}

static void tx_start(slink_t *s, const uint8_t rid[RNS_HASH_LEN], rns_src_t *data);

static void tx_next(slink_t *s)
{
    tx_free(s->tx);
    s->tx = NULL;
    if (s->nq) {
        queued_t q = s->q[0];
        memmove(s->q, s->q + 1, (size_t)(s->nq - 1) * sizeof s->q[0]);
        s->nq--;
        tx_start(s, q.rid, q.data);
    }
}

static void tx_proof(slink_t *s, const uint8_t *d, size_t n)
{
    tx_t *t = s->tx;
    if (!t || n != 64 || memcmp(d, t->hash, 32) != 0 || memcmp(d + 32, t->proof, 32) != 0) return;
    RNS_SERVE_INFO("answer segment %u/%u delivered: %u parts in %u s", (unsigned)t->seg + 1,
                   (unsigned)t->nseg, (unsigned)t->nparts, (unsigned)(s_now - t->start_s));
    if (t->seg + 1 < t->nseg) {
        t->adv_tries = 0;
        if (tx_segment(t, &s->link, t->seg + 1)) tx_advertise(s);
        else tx_next(s);
    } else {
        tx_next(s);
    }
}

static void tx_start(slink_t *s, const uint8_t rid[RNS_HASH_LEN], rns_src_t *data)
{
    tx_t *t = calloc(1, sizeof *t);
    if (!t) { if (data->release) data->release(data); return; }
    t->data = data;
    t->pre[0] = 0x92; t->pre[1] = 0xc4; t->pre[2] = RNS_HASH_LEN;
    memcpy(t->pre + 3, rid, RNS_HASH_LEN);
    t->total = PRE_LEN + data->len;
    t->nseg = (uint32_t)((t->total - 1) / MAX_EFFICIENT + 1);
    t->start_s = s_now;
    s->tx = t;
    if (!tx_segment(t, &s->link, 0)) { tx_next(s); return; }
    tx_advertise(s);
}

/* ── links ─────────────────────────────────────────────────────────────── */

static void up_free(slink_t *s)
{
    if (s->rx) rns_res_free(s->rx);
    s->rx = NULL;
    if (s->up) free(s->up->buf);
    free(s->up);
    s->up = NULL;
}

int rns_serve_links(void);

static void link_free(int i, bool notify)
{
    slink_t *s = s_links[i];
    if (!s) return;
    if (notify && s->link.state == RNS_LINK_ACTIVE)
        send_enc(s, RNS_CTX_LINKCLOSE, s->link.link_id, RNS_HASH_LEN);
    up_free(s);
    tx_free(s->tx);
    for (int k = 0; k < s->nq; k++)
        if (s->q[k].data && s->q[k].data->release) s->q[k].data->release(s->q[k].data);
    memset(s, 0, sizeof *s);
    free(s);
    s_links[i] = NULL;
}

static int link_find(const uint8_t id[RNS_HASH_LEN])
{
    for (int i = 0; i < RNS_SERVE_LINKS_MAX; i++)
        if (s_links[i] && memcmp(s_links[i]->link.link_id, id, RNS_HASH_LEN) == 0) return i;
    return -1;
}

void rns_serve_init(const rns_serve_cfg_t *cfg)
{
    for (int i = 0; i < RNS_SERVE_LINKS_MAX; i++) link_free(i, false);
    s_cfg = *cfg;
    if (s_cfg.max_links > RNS_SERVE_LINKS_MAX) s_cfg.max_links = RNS_SERVE_LINKS_MAX;
    if (s_cfg.max_links < 1) s_cfg.max_links = 1;
    if (!s_cfg.mtu_cap || s_cfg.mtu_cap > RNS_MTU) s_cfg.mtu_cap = RNS_MTU;
    scratch_get();
}

int rns_serve_links(void)
{
    int n = 0;
    for (int i = 0; i < RNS_SERVE_LINKS_MAX; i++) if (s_links[i]) n++;
    return n;
}

int rns_serve_sending(void)
{
    int n = 0;
    for (int i = 0; i < RNS_SERVE_LINKS_MAX; i++)
        if (s_links[i] && (s_links[i]->tx || s_links[i]->nq)) n += 1 + s_links[i]->nq;
    return n;
}

size_t rns_serve_link_bytes(int slot)
{
    if (slot < 0 || slot >= RNS_SERVE_LINKS_MAX || !s_links[slot]) return 0;
    slink_t *s = s_links[slot];
    size_t n = sizeof *s;
    if (s->up) n += sizeof *s->up + s->up->cap;
    if (s->tx) n += sizeof *s->tx + (size_t)s->tx->nparts * (MAP_LEN + 16);
    return n;
}

void rns_serve_tick(uint32_t now_s)
{
    s_now = now_s;
    for (int i = 0; i < RNS_SERVE_LINKS_MAX; i++) {
        slink_t *s = s_links[i];
        if (!s) continue;
        if (now_s - s->last_s > s_cfg.idle_s) { link_free(i, true); continue; }
        /* An answer nobody takes is not held until the link idles out: on a
         * board without PSRAM it is most of the free heap. The advertisement
         * again while none came back, then the next answer. */
        tx_t *t = s->tx;
        if (!t) continue;
        if (!t->asked && now_s - t->last_s >= ADV_RETRY_S) {
            if (t->adv_tries < ADV_TRIES) tx_advertise(s);
            else { RNS_SERVE_LOG("answer dropped: never asked for"); tx_next(s); }
        } else if (t->asked && now_s - t->last_s >= TX_STALL_S) {
            RNS_SERVE_LOG("answer dropped: the receiver went quiet");
            tx_next(s);
        }
    }
}

static void link_accept(const rns_packet_t *p, uint32_t now_s)
{
    uint8_t id[RNS_HASH_LEN];
    rns_link_id_of_request(p, id);
    int known = link_find(id);
    if (known >= 0) {
        /* The same request again (another path, a retry): the same proof. */
        out(s_links[known]->proof, s_links[known]->proof_len, NULL);
        return;
    }
    rns_serve_tick(now_s);
    int slot = -1, open = 0;
    for (int i = 0; i < RNS_SERVE_LINKS_MAX; i++) {
        if (s_links[i]) open++;
        else if (slot < 0) slot = i;
    }
    if (open >= s_cfg.max_links || slot < 0) {
        /* Full. A visitor who shut the page without closing its link holds
         * its place until the idle time, and with two places two closed tabs
         * shut the station to everybody for five minutes (seen: "link
         * refused: 2 open already" for a third browser). So the link quiet
         * the longest gives way, if it has been quiet for EVICT_IDLE_S and
         * nothing is being sent or received on it; a visitor still there
         * opens another when it next asks. */
        int old = -1;
        for (int i = 0; i < RNS_SERVE_LINKS_MAX; i++) {
            slink_t *l = s_links[i];
            if (!l || l->tx || l->nq || l->rx || l->up) continue;
            if (now_s - l->last_s < EVICT_IDLE_S) continue;
            if (old < 0 || l->last_s < s_links[old]->last_s) old = i;
        }
        if (old < 0) {
            RNS_SERVE_LOG("link refused: %d open already", open);
            return;
        }
        RNS_SERVE_INFO("a link quiet for %u s gives way to a new one",
                       (unsigned)(now_s - s_links[old]->last_s));
        link_free(old, true);
        slot = old;
    }
    if (!scratch_get()) { RNS_SERVE_LOG("link refused: no room for scratch"); return; }
    slink_t *s = calloc(1, sizeof *s);
    if (!s) { RNS_SERVE_LOG("link refused: no room for a link (%u B)", (unsigned)sizeof *s); return; }
    int n = rns_link_respond(&s->link, s_cfg.id, p, s_cfg.mtu_cap, s->proof, sizeof s->proof);
    if (n <= 0) { free(s); RNS_SERVE_LOG("link refused: could not prove it (%d)", n); return; }
    s->proof_len = (uint16_t)n;
    s->gen = ++s_gen;
    s->last_s = now_s;
    s_links[slot] = s;
    out(s->proof, s->proof_len, NULL);
}

static void request_in(int slot, const uint8_t *packed, size_t len)
{
    slink_t *s = s_links[slot];
    uint8_t h[32], rid[RNS_HASH_LEN];
    rns_sha256(packed, len, h);
    memcpy(rid, h, RNS_HASH_LEN);
    if (s_cfg.request)
        s_cfg.request(slot, s->gen, s->link.link_id, rid, packed, len, false, s_cfg.ctx);
}

/* The sink an upload streams into: the first upload_max bytes kept, all of
 * them hashed for the request id. */
static bool up_write(const uint8_t *d, size_t n, void *ctx)
{
    up_t *u = ctx;
    rns_sha256_update(&u->sha, d, n);
    if (u->over || u->n + n > u->cap) { u->over = true; return true; }
    memcpy(u->buf + u->n, d, n);
    u->n += n;
    return true;
}
static void up_done(bool ok, const char *why, void *ctx)
{
    (void)why;
    up_t *u = ctx;
    u->done = true;
    u->ok = ok;
}

static void upload(int slot, const rns_packet_t *p)
{
    slink_t *s = s_links[slot];
    if (!s->rx) {
        if (p->context != RNS_CTX_RESOURCE_ADV) return;
        s->up = calloc(1, sizeof *s->up);
        if (!s->up) return;
        s->up->cap = s_cfg.upload_max;
        s->up->buf = malloc(s->up->cap ? s->up->cap : 1);
        if (!s->up->buf) { up_free(s); return; }
        rns_sha256_init(&s->up->sha);
        rns_res_sink_t sk = { up_write, up_done, s->up };
        s->rx = rns_res_new(&s->link, &sk);
        if (!s->rx) { up_free(s); return; }
    }
    rns_res_feed(s->rx, p, out, NULL);
    up_t *u = s->up;
    if (!u || !u->done) return;
    if (u->ok) {
        uint8_t h[32], rid[RNS_HASH_LEN];
        rns_sha256_final(&u->sha, h);
        memcpy(rid, h, RNS_HASH_LEN);
        if (u->over) {
            if (s_cfg.request)
                s_cfg.request(slot, s->gen, s->link.link_id, rid, NULL, 0, true, s_cfg.ctx);
        } else {
            /* Handed over before the receiver is let go: the buffer is ours. */
            if (s_cfg.request)
                s_cfg.request(slot, s->gen, s->link.link_id, rid, u->buf, u->n, false, s_cfg.ctx);
        }
    }
    up_free(s);
}

bool rns_serve_feed(const rns_packet_t *p, uint32_t now_s)
{
    s_now = now_s;
    if (p->packet_type == RNS_PACKET_LINKREQUEST && p->dest_type == RNS_DEST_SINGLE &&
        memcmp(p->dest, s_cfg.dest, RNS_HASH_LEN) == 0) {
        link_accept(p, now_s);
        return true;
    }
    if (p->dest_type != RNS_DEST_LINK) return false;
    int slot = link_find(p->dest);
    if (slot < 0) return false;
    slink_t *s = s_links[slot];
    s->last_s = now_s;
    if (s->link.state != RNS_LINK_ACTIVE) {
        if (p->context == RNS_CTX_LRRTT) rns_link_rtt(&s->link, p);
        return true;
    }
    if (p->packet_type == RNS_PACKET_PROOF) {
        if (p->context == RNS_CTX_RESOURCE_PRF) tx_proof(s, p->data, p->data_len);
        return true;
    }
    switch (p->context) {
    case RNS_CTX_LINKCLOSE: {
        int n = rns_link_decrypt(&s->link, p, s_plain, PLAIN_CAP);
        if (n == RNS_HASH_LEN && memcmp(s_plain, s->link.link_id, RNS_HASH_LEN) == 0)
            link_free(slot, false);
        break;
    }
    case RNS_CTX_KEEPALIVE: {
        int n = rns_link_decrypt(&s->link, p, s_plain, PLAIN_CAP);
        if (n == 1 && s_plain[0] == 0xFF) {
            uint8_t back = 0xFE;
            send_enc(s, RNS_CTX_KEEPALIVE, &back, 1);
        }
        break;
    }
    case 0x09: {                              /* REQUEST */
        int n = rns_link_decrypt(&s->link, p, s_plain, PLAIN_CAP);
        if (n > 0) request_in(slot, s_plain, (size_t)n);
        break;
    }
    case RNS_CTX_RESOURCE_REQ: {
        int n = rns_link_decrypt(&s->link, p, s_plain, PLAIN_CAP);
        if (n > 0) tx_request(s, s_plain, (size_t)n);
        break;
    }
    case RNS_CTX_RESOURCE_ADV:
    case RNS_CTX_RESOURCE:
    case RNS_CTX_RESOURCE_HMU:
        upload(slot, p);
        break;
    default:
        break;
    }
    return true;
}

void rns_serve_respond(int slot, uint32_t gen, const uint8_t rid[RNS_HASH_LEN],
                       rns_src_t *data)
{
    if (!data) return;
    slink_t *s = (slot >= 0 && slot < RNS_SERVE_LINKS_MAX) ? s_links[slot] : NULL;
    if (!s || s->gen != gen || s->link.state != RNS_LINK_ACTIVE) {
        if (data->release) data->release(data);
        return;
    }
    if (PRE_LEN + data->len <= RNS_SERVE_INLINE_MAX) {
        uint8_t b[RNS_SERVE_INLINE_MAX];
        b[0] = 0x92; b[1] = 0xc4; b[2] = RNS_HASH_LEN;
        memcpy(b + 3, rid, RNS_HASH_LEN);
        bool ok = data->read(data, 0, b + PRE_LEN, data->len);
        if (ok) send_enc(s, 0x0A, b, PRE_LEN + data->len);     /* RESPONSE */
        if (data->release) data->release(data);
        return;
    }
    if (s->tx) {
        if (s->nq >= QUEUE) { if (data->release) data->release(data); return; }
        memcpy(s->q[s->nq].rid, rid, RNS_HASH_LEN);
        s->q[s->nq++].data = data;
        return;
    }
    tx_start(s, rid, data);
}
