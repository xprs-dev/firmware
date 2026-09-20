/* mt_wire.c -- the Meshtastic header, the protobuf messages we touch, the
 * channel arithmetic and the identities. See mt.h. */

#include "mt.h"

#include <stdio.h>
#include <string.h>

#include "xlc.h"
#include "xprs.h"

/* ── Header ───────────────────────────────────────────────────────────── */

static uint32_t rd32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void wr32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

bool mt_hdr_parse(const uint8_t *buf, int len, mt_hdr_t *out)
{
    if (!buf || !out || len < MT_HDR_LEN) return false;
    out->to = rd32le(buf);
    out->from = rd32le(buf + 4);
    out->id = rd32le(buf + 8);
    uint8_t f = buf[12];
    out->hop_limit = f & 0x07;
    out->want_ack = (f & 0x08) != 0;
    out->via_mqtt = (f & 0x10) != 0;
    out->hop_start = (uint8_t)((f >> 5) & 0x07);
    out->channel = buf[13];
    out->next_hop = buf[14];
    out->relay_node = buf[15];
    return true;
}

void mt_hdr_build(const mt_hdr_t *h, uint8_t out[MT_HDR_LEN])
{
    wr32le(out, h->to);
    wr32le(out + 4, h->from);
    wr32le(out + 8, h->id);
    out[12] = (uint8_t)((h->hop_limit & 0x07) | (h->want_ack ? 0x08 : 0) |
                        (h->via_mqtt ? 0x10 : 0) |
                        ((h->hop_start & 0x07) << 5));
    out[13] = h->channel;
    out[14] = h->next_hop;
    out[15] = h->relay_node;
}

/* ── Protobuf, by hand ────────────────────────────────────────────────────
 *
 * Three wire types are all these messages use: varint (0), 64-bit (1, only
 * ever skipped), length-delimited (2) and fixed32 (5). An unknown field is
 * skipped, which is protobuf's rule and XPRS's design rule 8 at once. */

typedef struct {
    const uint8_t *p, *end;
} pb_rd_t;

static bool pb_varint(pb_rd_t *r, uint64_t *v)
{
    uint64_t x = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (r->p >= r->end) return false;
        uint8_t b = *r->p++;
        x |= (uint64_t)(b & 0x7F) << shift;
        if (!(b & 0x80)) { *v = x; return true; }
    }
    return false;
}

static bool pb_fixed32(pb_rd_t *r, uint32_t *v)
{
    if (r->end - r->p < 4) return false;
    *v = rd32le(r->p);
    r->p += 4;
    return true;
}

static bool pb_bytes(pb_rd_t *r, const uint8_t **b, int *n)
{
    uint64_t len;
    if (!pb_varint(r, &len)) return false;
    if (len > (uint64_t)(r->end - r->p)) return false;
    *b = r->p;
    *n = (int)len;
    r->p += len;
    return true;
}

/* Read the next tag. false at the clean end of the message. */
static bool pb_tag(pb_rd_t *r, uint32_t *field, uint32_t *wt, bool *err)
{
    *err = false;
    if (r->p >= r->end) return false;
    uint64_t t;
    if (!pb_varint(r, &t) || (t >> 3) == 0) { *err = true; return false; }
    *field = (uint32_t)(t >> 3);
    *wt = (uint32_t)(t & 7);
    return true;
}

static bool pb_skip(pb_rd_t *r, uint32_t wt)
{
    uint64_t v;
    const uint8_t *b;
    int n;
    switch (wt) {
    case 0: return pb_varint(r, &v);
    case 1: if (r->end - r->p < 8) return false; r->p += 8; return true;
    case 2: return pb_bytes(r, &b, &n);
    case 5: if (r->end - r->p < 4) return false; r->p += 4; return true;
    default: return false;
    }
}

typedef struct {
    uint8_t *p, *end;
    bool     ok;
} pb_wr_t;

static void pbw_byte(pb_wr_t *w, uint8_t b)
{
    if (!w->ok || w->p >= w->end) { w->ok = false; return; }
    *w->p++ = b;
}

static void pbw_varint(pb_wr_t *w, uint64_t v)
{
    do {
        uint8_t b = v & 0x7F;
        v >>= 7;
        pbw_byte(w, (uint8_t)(b | (v ? 0x80 : 0)));
    } while (v);
}

static void pbw_tag(pb_wr_t *w, uint32_t field, uint32_t wt)
{
    pbw_varint(w, ((uint64_t)field << 3) | wt);
}

static void pbw_fixed32(pb_wr_t *w, uint32_t field, uint32_t v)
{
    pbw_tag(w, field, 5);
    for (int i = 0; i < 4; i++) pbw_byte(w, (uint8_t)(v >> (8 * i)));
}

static void pbw_bytes(pb_wr_t *w, uint32_t field, const void *b, int n)
{
    pbw_tag(w, field, 2);
    pbw_varint(w, (uint64_t)n);
    if (!w->ok || w->end - w->p < n) { w->ok = false; return; }
    if (n) memcpy(w->p, b, (size_t)n);
    w->p += n;
}

static void pbw_str(pb_wr_t *w, uint32_t field, const char *s)
{
    if (s && *s) pbw_bytes(w, field, s, (int)strlen(s));
}

static void pbw_uint(pb_wr_t *w, uint32_t field, uint64_t v)
{
    pbw_tag(w, field, 0);
    pbw_varint(w, v);
}

bool mt_data_decode(const uint8_t *buf, int len, mt_data_t *out)
{
    if (!buf || !out || len <= 0) return false;
    memset(out, 0, sizeof *out);
    pb_rd_t r = { buf, buf + len };
    uint32_t f, wt;
    bool err;
    bool have_port = false;
    while (pb_tag(&r, &f, &wt, &err)) {
        uint64_t v;
        uint32_t x;
        if (f == 1 && wt == 0) {
            if (!pb_varint(&r, &v)) return false;
            out->portnum = (uint32_t)v;
            have_port = true;
        } else if (f == 2 && wt == 2) {
            if (!pb_bytes(&r, &out->payload, &out->payload_len)) return false;
        } else if (f == 3 && wt == 0) {
            if (!pb_varint(&r, &v)) return false;
            out->want_response = v != 0;
        } else if (f >= 4 && f <= 8 && wt == 5) {
            if (!pb_fixed32(&r, &x)) return false;
            if (f == 4) out->dest = x;
            else if (f == 5) out->source = x;
            else if (f == 6) out->request_id = x;
            else if (f == 7) out->reply_id = x;
            else out->emoji = x;
        } else if (f == 9 && wt == 0) {
            if (!pb_varint(&r, &v)) return false;
            out->has_bitfield = true;
            out->bitfield = (uint32_t)v;
        } else if (!pb_skip(&r, wt)) {
            return false;
        }
    }
    /* A Data with no portnum is how garbage from a wrong key usually looks
     * once in a while: the firmware refuses portnum 0 too. */
    return !err && have_port && out->portnum != 0;
}

int mt_data_encode(const mt_data_t *d, uint8_t *out, int cap)
{
    pb_wr_t w = { out, out + cap, true };
    pbw_uint(&w, 1, d->portnum);
    if (d->payload_len > 0) pbw_bytes(&w, 2, d->payload, d->payload_len);
    if (d->want_response) pbw_uint(&w, 3, 1);
    if (d->dest) pbw_fixed32(&w, 4, d->dest);
    if (d->source) pbw_fixed32(&w, 5, d->source);
    if (d->request_id) pbw_fixed32(&w, 6, d->request_id);
    if (d->reply_id) pbw_fixed32(&w, 7, d->reply_id);
    if (d->emoji) pbw_fixed32(&w, 8, d->emoji);
    if (d->has_bitfield) pbw_uint(&w, 9, d->bitfield);
    return w.ok ? (int)(w.p - out) : -1;
}

static void copy_str(char *dst, int cap, const uint8_t *b, int n)
{
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, b, (size_t)n);
    dst[n] = 0;
}

bool mt_user_decode(const uint8_t *buf, int len, mt_user_t *out)
{
    if (!buf || !out) return false;
    memset(out, 0, sizeof *out);
    pb_rd_t r = { buf, buf + len };
    uint32_t f, wt;
    bool err;
    while (pb_tag(&r, &f, &wt, &err)) {
        const uint8_t *b;
        int n;
        uint64_t v;
        if (wt == 2 && (f == 1 || f == 2 || f == 3 || f == 8)) {
            if (!pb_bytes(&r, &b, &n)) return false;
            if (f == 1) copy_str(out->id, sizeof out->id, b, n);
            else if (f == 2) copy_str(out->long_name, sizeof out->long_name, b, n);
            else if (f == 3) copy_str(out->short_name, sizeof out->short_name, b, n);
            else if (n == 32) {
                out->has_public_key = true;
                memcpy(out->public_key, b, 32);
            }
        } else if (wt == 0 && (f == 5 || f == 6 || f == 7)) {
            if (!pb_varint(&r, &v)) return false;
            if (f == 5) out->hw_model = (uint32_t)v;
            else if (f == 6) out->is_licensed = v != 0;
            else out->role = (uint32_t)v;
        } else if (!pb_skip(&r, wt)) {
            return false;
        }
    }
    return !err;
}

int mt_user_encode(const mt_user_t *u, uint8_t *out, int cap)
{
    pb_wr_t w = { out, out + cap, true };
    pbw_str(&w, 1, u->id);
    pbw_str(&w, 2, u->long_name);
    pbw_str(&w, 3, u->short_name);
    if (u->hw_model) pbw_uint(&w, 5, u->hw_model);
    if (u->is_licensed) pbw_uint(&w, 6, 1);
    if (u->role) pbw_uint(&w, 7, u->role);
    if (u->has_public_key) pbw_bytes(&w, 8, u->public_key, 32);
    return w.ok ? (int)(w.p - out) : -1;
}

bool mt_routing_decode(const uint8_t *buf, int len, int *error_reason)
{
    pb_rd_t r = { buf, buf + len };
    uint32_t f, wt;
    bool err;
    bool have = false;
    while (pb_tag(&r, &f, &wt, &err)) {
        uint64_t v;
        if (f == 3 && wt == 0) {
            if (!pb_varint(&r, &v)) return false;
            if (error_reason) *error_reason = (int)v;
            have = true;
        } else if (!pb_skip(&r, wt)) {
            return false;
        }
    }
    /* proto3 elides a zero enum -- but a oneof member is always written, so
     * an ack arrives as 18 00. An EMPTY Routing is still read as success,
     * the way nanopb's defaults would read it. */
    if (!have && !err && error_reason) *error_reason = 0;
    return !err;
}

int mt_routing_encode(uint8_t *out, int cap, int error_reason)
{
    if (cap < 2 || error_reason < 0 || error_reason > 127) return -1;
    out[0] = 0x18;      /* field 3, varint */
    out[1] = (uint8_t)error_reason;
    return 2;
}

int mt_routing_encode_ack(uint8_t *out, int cap)
{
    return mt_routing_encode(out, cap, 0);
}

/* ── Channels, keys and frequencies ───────────────────────────────────── */

const uint8_t mt_default_key[16] = {
    0xd4, 0xf1, 0xbb, 0x3a, 0x20, 0x29, 0x07, 0x59,
    0xf0, 0xbc, 0xff, 0xab, 0xcf, 0x4e, 0x69, 0x01,
};

static uint8_t xor_bytes(const uint8_t *p, int n)
{
    uint8_t h = 0;
    for (int i = 0; i < n; i++) h ^= p[i];
    return h;
}

uint8_t mt_channel_hash(const char *name, const uint8_t *key, int key_len)
{
    uint8_t h = name ? xor_bytes((const uint8_t *)name, (int)strlen(name)) : 0;
    return h ^ (key && key_len > 0 ? xor_bytes(key, key_len) : 0);
}

int mt_psk_expand(const uint8_t *psk, int psk_len, uint8_t key[32])
{
    if (psk_len == 0 || (psk_len == 1 && psk[0] == 0)) return 0;
    if (psk_len == 1) {
        memcpy(key, mt_default_key, 16);
        key[15] = (uint8_t)(key[15] + psk[0] - 1);
        return 16;
    }
    if (psk_len == 16 || psk_len == 32) {
        memcpy(key, psk, (size_t)psk_len);
        return psk_len;
    }
    return -1;
}

uint8_t mt_longfast_hash(void)
{
    return mt_channel_hash(MT_CH_NAME_LONGFAST, mt_default_key, 16);
}

uint32_t mt_djb2(const char *s)
{
    uint32_t h = 5381;
    for (; s && *s; s++) h = ((h << 5) + h) + (uint8_t)*s;
    return h;
}

/* Meshtastic's regional table, the rows an 868/915 MHz SX1262 can use.
 * Values are the firmware's own (RadioInterface.cpp, RDEF) in kHz. The
 * `xprs` column is what this tree's lora_region setting has always said. */
static const mt_region_t k_regions[] = {
    { "EU_433", "eu-433", 433000, 434000, 10, 10 },
    { "EU_868", "eu", 869400, 869650, 10, 27 },
    { "US",     "us", 902000, 928000, 100, 30 },
    { "ANZ",    "au", 915000, 928000, 100, 30 },
    { "JP",     NULL, 920500, 923500, 100, 13 },
    { "RU",     NULL, 868700, 869200, 100, 20 },
    { "KR",     NULL, 920000, 923000, 100, 23 },
    { "TW",     NULL, 920000, 925000, 100, 27 },
    { "IN",     NULL, 865000, 867000, 100, 30 },
    { "NZ_865", NULL, 864000, 868000, 100, 36 },
    { "TH",     NULL, 920000, 925000, 10, 27 },
    { "UA_868", NULL, 868000, 868600, 1, 14 },
    { "MY_919", NULL, 919000, 924000, 100, 27 },
    { "SG_923", NULL, 917000, 925000, 100, 20 },
    { "PH_868", NULL, 868000, 869400, 100, 14 },
    { "PH_915", NULL, 915000, 918000, 100, 24 },
    { "KZ_863", NULL, 863000, 868000, 100, 30 },
    { "NP_865", NULL, 865000, 868000, 100, 30 },
    { "BR_902", NULL, 902000, 907500, 100, 30 },
};

const mt_region_t *mt_regions(int *count)
{
    if (count) *count = (int)(sizeof k_regions / sizeof k_regions[0]);
    return k_regions;
}

static bool same_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x = (char)(x - 32);
        if (y >= 'a' && y <= 'z') y = (char)(y - 32);
        if (x != y) return false;
    }
    return *a == *b;
}

const mt_region_t *mt_region_find(const char *name)
{
    if (!name || !*name) return &k_regions[0];
    int n;
    const mt_region_t *r = mt_regions(&n);
    for (int i = 0; i < n; i++) {
        if (r[i].xprs && same_ci(name, r[i].xprs)) return &r[i];
        if (same_ci(name, r[i].name)) return &r[i];
    }
    return NULL;
}

uint32_t mt_slot_freq_hz(const mt_region_t *r, const char *channel_name,
                         uint32_t bw_hz)
{
    if (!r || !bw_hz) return 0;
    uint32_t bw_khz = bw_hz / 1000u;
    uint32_t span = r->end_khz - r->start_khz;
    uint32_t slots = span / bw_khz;       /* spacing is 0 in every row */
    if (!slots) return 0;
    uint32_t slot = mt_djb2(channel_name) % slots;
    return (r->start_khz * 1000u) + bw_hz / 2u + slot * bw_hz;
}

/* ── Encryption ───────────────────────────────────────────────────────── */

bool mt_crypt(const uint8_t *key, int key_len, uint32_t from, uint32_t id,
              uint8_t *buf, int len)
{
    if (key_len == 0) return true;
    if (key_len != 16 && key_len != 32) return false;
    uint8_t ctr[16] = { 0 };
    wr32le(ctr, id);            /* the packet id is a u64 there; top half 0 */
    wr32le(ctr + 8, from);
    uint8_t ks[16];
    for (int off = 0; off < len; off += 16) {
        if (!xlc_aes_encrypt_block(key, key_len, ctr, ks)) return false;
        int n = len - off < 16 ? len - off : 16;
        for (int i = 0; i < n; i++) buf[off + i] ^= ks[i];
        /* The counter is the last four bytes, big-endian. */
        for (int i = 15; i >= 12; i--)
            if (++ctr[i]) break;
    }
    return true;
}

/* ── Identities ───────────────────────────────────────────────────────── */

static int bare_len(const char *call, int len)
{
    for (int i = 0; i < len; i++)
        if (call[i] == '-') return i;
    return len;
}

uint32_t mt_node_of_call(const char *call, int len)
{
    static const char dom[] = "XPRS/node";
    uint8_t buf[sizeof dom - 1 + 24];
    int n = bare_len(call, len);
    if (n > 24) n = 24;
    memcpy(buf, dom, sizeof dom - 1);
    for (int i = 0; i < n; i++) {
        char c = call[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 32);   /* callsigns are upper */
        buf[sizeof dom - 1 + i] = (uint8_t)c;
    }
    uint8_t h[32];
    xprs_sha256(buf, sizeof dom - 1 + (size_t)n, h);
    uint32_t v = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
                 ((uint32_t)h[2] << 8) | h[3];
    /* 0 is "nobody", 1-3 are reserved by the firmware, and all-ones is the
     * broadcast address. Moving them is deterministic, so every bridge
     * still agrees. */
    if (v <= 3u) v += 4u;
    if (v == MT_BROADCAST) v = 0xFFFFFFFEu;
    return v;
}

int mt_call_of_node(uint32_t node, char *out, int cap)
{
    if (cap < 11) return -1;
    snprintf(out, (size_t)cap, "MT%08X", (unsigned)node);
    return 10;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool mt_node_of_mtcall(const char *call, int len, uint32_t *node)
{
    if (!call || len != 10 || call[0] != 'M' || call[1] != 'T') return false;
    uint32_t v = 0;
    for (int i = 2; i < 10; i++) {
        int h = hexval(call[i]);
        if (h < 0) return false;
        v = (v << 4) | (uint32_t)h;
    }
    if (node) *node = v;
    return true;
}

void mt_user_id_of(uint32_t node, char out[10])
{
    snprintf(out, 10, "!%08x", (unsigned)node);
}

int mt_nick_from_name(const char *name, char *out, int cap)
{
    int n = 0;
    bool last_dash = true;       /* no leading dash */
    for (const char *p = name; p && *p && n < cap - 1 && n < 16; p++) {
        char c = *p;
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (c == ' ') {
            if (!last_dash) { out[n++] = '-'; last_dash = true; }
            continue;
        }
        if (!ok) continue;       /* emoji, accents: the nick type is ASCII */
        out[n++] = c;
        last_dash = c == '-';
    }
    while (n > 0 && out[n - 1] == '-') n--;
    if (cap > 0) out[n] = 0;
    return n;
}
