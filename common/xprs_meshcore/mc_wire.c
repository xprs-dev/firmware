/* mc_wire.c -- MeshCore's frame, its payloads and its keys. See mc.h. */

#include "mc.h"

#include <stdio.h>
#include <string.h>

#include "xlc.h"
#include "xprs.h"

/* ── The header ───────────────────────────────────────────────────────── */

bool mc_parse(const uint8_t *frame, int len, mc_pkt_t *out)
{
    if (!frame || !out || len < 2) return false;
    memset(out, 0, sizeof *out);
    uint8_t h = frame[0];
    out->route = (uint8_t)(h & 0x03);
    out->type = (uint8_t)((h >> 2) & 0x0F);
    out->version = (uint8_t)((h >> 6) & 0x03);

    int i = 1;
    if (out->route == MC_ROUTE_TRANSPORT_FLOOD ||
        out->route == MC_ROUTE_TRANSPORT_DIRECT) {
        if (len < i + 4) return false;
        out->transport[0] = (uint16_t)(frame[i] | (frame[i + 1] << 8));
        out->transport[1] = (uint16_t)(frame[i + 2] | (frame[i + 3] << 8));
        i += 4;
    }
    if (len < i + 1) return false;
    uint8_t pl = frame[i++];
    out->hops = (uint8_t)(pl & 0x3F);
    out->hash_size = (uint8_t)(((pl >> 6) & 0x03) + 1);
    int path_bytes = out->hops * out->hash_size;
    if (path_bytes > MC_PATH_MAX || len < i + path_bytes) return false;
    memcpy(out->path, frame + i, (size_t)path_bytes);
    i += path_bytes;

    out->payload_len = len - i;
    if (out->payload_len < 0 || out->payload_len > MC_PAYLOAD_MAX) return false;
    out->payload = frame + i;
    return true;
}

int mc_build(const mc_pkt_t *p, uint8_t *out, int cap)
{
    if (!p || !out || p->payload_len < 0 || p->payload_len > MC_PAYLOAD_MAX)
        return 0;
    uint8_t hash_size = p->hash_size ? p->hash_size : 1;
    if (hash_size > 3) return 0;
    int path_bytes = p->hops * hash_size;
    if (p->hops > 63 || path_bytes > MC_PATH_MAX) return 0;

    bool transport = p->route == MC_ROUTE_TRANSPORT_FLOOD ||
                     p->route == MC_ROUTE_TRANSPORT_DIRECT;
    int n = 1 + (transport ? 4 : 0) + 1 + path_bytes + p->payload_len;
    if (n > cap || n > MC_FRAME_MAX) return 0;

    int i = 0;
    out[i++] = (uint8_t)((p->route & 0x03) | ((p->type & 0x0F) << 2) |
                         ((p->version & 0x03) << 6));
    if (transport) {
        out[i++] = (uint8_t)(p->transport[0] & 0xFF);
        out[i++] = (uint8_t)(p->transport[0] >> 8);
        out[i++] = (uint8_t)(p->transport[1] & 0xFF);
        out[i++] = (uint8_t)(p->transport[1] >> 8);
    }
    out[i++] = (uint8_t)((p->hops & 0x3F) | (uint8_t)((hash_size - 1) << 6));
    memcpy(out + i, p->path, (size_t)path_bytes);
    i += path_bytes;
    if (p->payload_len) memcpy(out + i, p->payload, (size_t)p->payload_len);
    return i + p->payload_len;
}

int mc_path_append(const mc_pkt_t *p, const uint8_t *hash, uint8_t *out,
                   int cap)
{
    if (!p || !hash) return 0;
    mc_pkt_t q = *p;
    uint8_t hs = q.hash_size ? q.hash_size : 1;
    int used = q.hops * hs;
    if (used + hs > MC_PATH_MAX || q.hops >= 63) return 0;
    memcpy(q.path + used, hash, (size_t)hs);
    q.hops = (uint8_t)(q.hops + 1);
    return mc_build(&q, out, cap);
}

uint32_t mc_packet_hash(const mc_pkt_t *p)
{
    /* MeshCore hashes "payload + type", which is what makes the identifier
     * the same at every hop: the path changes, the packet does not. */
    uint8_t buf[MC_PAYLOAD_MAX + 1], out[32];
    if (!p || p->payload_len < 0 || p->payload_len > MC_PAYLOAD_MAX) return 0;
    buf[0] = p->type;
    if (p->payload_len) memcpy(buf + 1, p->payload, (size_t)p->payload_len);
    xprs_sha256(buf, (size_t)p->payload_len + 1, out);
    return (uint32_t)out[0] | ((uint32_t)out[1] << 8) |
           ((uint32_t)out[2] << 16) | ((uint32_t)out[3] << 24);
}

/* ── Identities ───────────────────────────────────────────────────────── */

void mc_call_of_pub(const uint8_t pub[32], char out[11])
{
    static const char hex[] = "0123456789ABCDEF";
    out[0] = 'M';
    out[1] = 'C';
    for (int i = 0; i < 4; i++) {
        out[2 + i * 2] = hex[(pub[i] >> 4) & 0x0F];
        out[3 + i * 2] = hex[pub[i] & 0x0F];
    }
    out[10] = 0;
}

void mc_node_keys(const char *call, int len, uint8_t sk[64], uint8_t pub[32])
{
    uint8_t seed[32];
    xlc_seed_from_call("XPRS/mc/ed25519", call, len, seed);
    xlc_ed25519_from_seed(seed, sk, pub);
    memset(seed, 0, sizeof seed);
}

/* ── The cipher ───────────────────────────────────────────────────────── */

/* MeshCore's public channel, the key every node ships with. */
const uint8_t mc_public_key[16] = {
    0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a,
    0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72
};

uint8_t mc_channel_hash(const uint8_t key[16])
{
    uint8_t h[32];
    xprs_sha256(key, 16, h);
    return h[0];
}

/* AES-128-ECB over whole blocks, the last one zero-padded: the cipher
 * MeshCore uses, which is ECB because a packet is short and the MAC below
 * is what is checked. In place is not allowed; [out] holds the padded
 * length. Returns that length, or 0. */
static int ecb_encrypt(const uint8_t key[16], const uint8_t *in, int len,
                       uint8_t *out, int cap)
{
    int blocks = (len + 15) / 16;
    if (blocks * 16 > cap) return 0;
    uint8_t block[16];
    for (int b = 0; b < blocks; b++) {
        int n = len - b * 16;
        if (n > 16) n = 16;
        memset(block, 0, sizeof block);
        memcpy(block, in + b * 16, (size_t)n);
        if (!xlc_aes_encrypt_block(key, 16, block, out + b * 16)) return 0;
    }
    return blocks * 16;
}

static int ecb_decrypt(const uint8_t key[16], const uint8_t *in, int len,
                       uint8_t *out, int cap)
{
    if (len % 16 || len / 16 > cap / 16) return 0;
    for (int b = 0; b < len / 16; b++)
        if (!xlc_aes_decrypt_block(key, 16, in + b * 16, out + b * 16))
            return 0;
    return len;
}

/* The two bytes MeshCore checks: HMAC-SHA256 over the ciphertext, under the
 * secret, cut to the front. */
static void mac2(const uint8_t *secret, int secret_len, const uint8_t *cipher,
                 int len, uint8_t out[2])
{
    uint8_t full[32];
    xlc_hmac_sha256(secret, (size_t)secret_len, cipher, (size_t)len, full);
    out[0] = full[0];
    out[1] = full[1];
}

/* ── The text inside ──────────────────────────────────────────────────── */

/* timestamp(4) txt_type+attempt(1) text: the plaintext of both a channel
 * message and a direct one. */
static int text_pack(uint32_t timestamp, uint8_t txt_type, uint8_t attempt,
                     const char *text, uint8_t *out, int cap)
{
    int tl = (int)strlen(text);
    if (5 + tl > cap) tl = cap - 5;
    if (tl < 0) return 0;
    out[0] = (uint8_t)(timestamp & 0xFF);
    out[1] = (uint8_t)((timestamp >> 8) & 0xFF);
    out[2] = (uint8_t)((timestamp >> 16) & 0xFF);
    out[3] = (uint8_t)((timestamp >> 24) & 0xFF);
    out[4] = (uint8_t)(((txt_type & 0x3F) << 2) | (attempt & 0x03));
    memcpy(out + 5, text, (size_t)tl);
    return 5 + tl;
}

static bool text_unpack(const uint8_t *in, int len, mc_text_t *out)
{
    if (len < 5) return false;
    memset(out, 0, sizeof *out);
    out->timestamp = (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
                     ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
    out->txt_type = (uint8_t)((in[4] >> 2) & 0x3F);
    out->attempt = (uint8_t)(in[4] & 0x03);
    int tl = len - 5;
    if (tl > (int)sizeof out->text - 1) tl = (int)sizeof out->text - 1;
    /* The padding the cipher added is zeroes, and a text does not carry
     * any: stop at the first one. */
    int n = 0;
    while (n < tl && in[5 + n]) n++;
    memcpy(out->text, in + 5, (size_t)n);
    out->text[n] = 0;
    return true;
}

/* ── Group text ───────────────────────────────────────────────────────── */

int mc_grp_txt_build(const uint8_t key[16], uint32_t timestamp,
                     const char *sender, const char *text, uint8_t *out,
                     int cap)
{
    if (cap < 3 + 16) return 0;
    /* Whole blocks, and the five bytes before the body: what is left is
     * what the body may be (MC_TEXT_MAX). Clamped HERE as well as by the
     * caller, so a long message is shortened rather than lost. */
    int room = ((cap - 3) / 16) * 16 - 5;
    if (room > MC_TEXT_MAX) room = MC_TEXT_MAX;
    if (room < 1) return 0;
    char body[MC_TEXT_MAX + 1];
    int bl = snprintf(body, sizeof body, "%s: %s", sender ? sender : "",
                      text ? text : "");
    if (bl <= 0) return 0;
    if (bl > room) body[room] = 0;

    uint8_t plain[MC_PAYLOAD_MAX];
    int pl = text_pack(timestamp, 0, 0, body, plain, 5 + room);
    if (!pl) return 0;
    int cl = ecb_encrypt(key, plain, pl, out + 3, cap - 3);
    if (!cl) return 0;
    out[0] = mc_channel_hash(key);
    mac2(key, 16, out + 3, cl, out + 1);
    return 3 + cl;
}

bool mc_grp_txt_open(const uint8_t key[16], const uint8_t *payload, int len,
                     mc_text_t *out, char *sender, int sender_cap)
{
    if (!key || !payload || len < 3 + 16 || !out) return false;
    if (payload[0] != mc_channel_hash(key)) return false;
    uint8_t want[2];
    mac2(key, 16, payload + 3, len - 3, want);
    if (want[0] != payload[1] || want[1] != payload[2]) return false;

    uint8_t plain[MC_PAYLOAD_MAX];
    int pl = ecb_decrypt(key, payload + 3, len - 3, plain, (int)sizeof plain);
    if (!pl || !text_unpack(plain, pl, out)) return false;

    /* `<sender name>: <message>`, and the name is nobody's word but the
     * sender's: MeshCore does not sign a channel message. */
    if (sender && sender_cap > 0) {
        sender[0] = 0;
        char *colon = strstr(out->text, ": ");
        if (colon) {
            int n = (int)(colon - out->text);
            if (n > sender_cap - 1) n = sender_cap - 1;
            memcpy(sender, out->text, (size_t)n);
            sender[n] = 0;
            memmove(out->text, colon + 2, strlen(colon + 2) + 1);
        }
    }
    return true;
}

/* ── Direct messages ──────────────────────────────────────────────────── */

int mc_dm_build(const uint8_t secret[32], uint8_t dest_hash, uint8_t src_hash,
                uint32_t timestamp, uint8_t attempt, const char *text,
                uint8_t *out, int cap)
{
    if (cap < 4 + 16) return 0;
    int room = ((cap - 4) / 16) * 16 - 5;
    if (room > MC_TEXT_MAX) room = MC_TEXT_MAX;
    if (room < 1) return 0;
    uint8_t plain[MC_PAYLOAD_MAX];
    int pl = text_pack(timestamp, 0, attempt, text ? text : "", plain,
                       5 + room);
    if (!pl) return 0;
    int cl = ecb_encrypt(secret, plain, pl, out + 4, cap - 4);
    if (!cl) return 0;
    out[0] = dest_hash;
    out[1] = src_hash;
    mac2(secret, 32, out + 4, cl, out + 2);
    return 4 + cl;
}

bool mc_dm_open(const uint8_t secret[32], const uint8_t *payload, int len,
                mc_text_t *out)
{
    if (!secret || !payload || len < 4 + 16 || !out) return false;
    uint8_t want[2];
    mac2(secret, 32, payload + 4, len - 4, want);
    if (want[0] != payload[2] || want[1] != payload[3]) return false;
    uint8_t plain[MC_PAYLOAD_MAX];
    int pl = ecb_decrypt(secret, payload + 4, len - 4, plain, (int)sizeof plain);
    return pl && text_unpack(plain, pl, out);
}

int mc_path_build(const uint8_t secret[32], uint8_t dest_hash, uint8_t src_hash,
                  const uint8_t *path, int hops, int hash_size,
                  uint8_t extra_type, const uint8_t *extra, int extra_len,
                  uint8_t *out, int cap)
{
    if (!secret || !out || hops < 0 || hops > 63 || hash_size < 1 || hash_size > 3)
        return 0;
    uint8_t plain[MC_PAYLOAD_MAX];
    int n = 0;
    plain[n++] = (uint8_t)((hops & 0x3F) | ((hash_size - 1) << 6));
    int pn = hops * hash_size;
    if (1 + pn + 1 + extra_len > (int)sizeof plain) return 0;
    if (pn && path) { memcpy(plain + n, path, (size_t)pn); n += pn; }
    if (extra && extra_len > 0) {
        plain[n++] = extra_type;
        memcpy(plain + n, extra, (size_t)extra_len);
        n += extra_len;
    }
    if (cap < 4 + 16) return 0;
    int cl = ecb_encrypt(secret, plain, n, out + 4, cap - 4);
    if (!cl) return 0;
    out[0] = dest_hash;
    out[1] = src_hash;
    mac2(secret, 32, out + 4, cl, out + 2);
    return 4 + cl;
}

bool mc_path_open(const uint8_t secret[32], const uint8_t *payload, int len,
                  mc_path_t *out)
{
    if (!secret || !payload || !out || len < 4 + 16) return false;
    uint8_t want[2];
    mac2(secret, 32, payload + 4, len - 4, want);
    if (want[0] != payload[2] || want[1] != payload[3]) return false;
    uint8_t plain[MC_PAYLOAD_MAX];
    int pl = ecb_decrypt(secret, payload + 4, len - 4, plain, (int)sizeof plain);
    if (pl < 1) return false;

    memset(out, 0, sizeof *out);
    out->extra_type = 0xFF;
    uint8_t pb = plain[0];
    out->hops = (uint8_t)(pb & 0x3F);
    out->hash_size = (uint8_t)((pb >> 6) + 1);
    int n = out->hops * out->hash_size;
    if (n > MC_PATH_MAX || 1 + n > pl) return false;
    if (n) memcpy(out->path, plain + 1, (size_t)n);
    int i = 1 + n;
    if (i < pl) {
        out->extra_type = plain[i++];
        int e = pl - i;
        /* The cipher pads with zeroes and an ACK is four bytes; take what
         * is there, bounded. */
        if (e > (int)sizeof out->extra) e = (int)sizeof out->extra;
        if (e > 0) {
            memcpy(out->extra, plain + i, (size_t)e);
            out->extra_len = e;
        }
    }
    return true;
}

uint32_t mc_ack_checksum(uint32_t timestamp, uint8_t flags, const char *text,
                         const uint8_t sender_pub[32])
{
    /* The message's own first five bytes, its text, and the sender's key:
     * the four bytes an ACK carries, and the reason a node knows which
     * message was acknowledged without the message being named (mc.h). */
    uint8_t buf[5 + MC_PAYLOAD_MAX + 32], h[32];
    int n = 0;
    buf[n++] = (uint8_t)(timestamp & 0xFF);
    buf[n++] = (uint8_t)((timestamp >> 8) & 0xFF);
    buf[n++] = (uint8_t)((timestamp >> 16) & 0xFF);
    buf[n++] = (uint8_t)((timestamp >> 24) & 0xFF);
    buf[n++] = flags;
    int tl = text ? (int)strlen(text) : 0;
    if (tl > MC_PAYLOAD_MAX) tl = MC_PAYLOAD_MAX;
    if (tl) memcpy(buf + n, text, (size_t)tl);
    n += tl;
    if (sender_pub) {
        memcpy(buf + n, sender_pub, 32);
        n += 32;
    }
    xprs_sha256(buf, (size_t)n, h);
    return (uint32_t)h[0] | ((uint32_t)h[1] << 8) | ((uint32_t)h[2] << 16) |
           ((uint32_t)h[3] << 24);
}

/* ── Adverts ──────────────────────────────────────────────────────────── */

int mc_advert_build(const uint8_t sk[64], const uint8_t pub[32],
                    uint32_t timestamp, uint8_t flags, const char *name,
                    uint8_t *out, int cap)
{
    int nl = name ? (int)strlen(name) : 0;
    if (nl > 31) nl = 31;
    int app = 1 + nl;
    int n = 32 + 4 + 64 + app;
    if (n > cap || n > MC_PAYLOAD_MAX) return 0;

    /* The signature covers the key, the timestamp and the app data, which
     * is what makes a name believable. */
    uint8_t signed_part[32 + 4 + 1 + 31];
    int s = 0;
    memcpy(signed_part, pub, 32);
    s = 32;
    signed_part[s++] = (uint8_t)(timestamp & 0xFF);
    signed_part[s++] = (uint8_t)((timestamp >> 8) & 0xFF);
    signed_part[s++] = (uint8_t)((timestamp >> 16) & 0xFF);
    signed_part[s++] = (uint8_t)((timestamp >> 24) & 0xFF);
    signed_part[s++] = (uint8_t)(flags | (nl ? MC_ADV_HAS_NAME : 0));
    if (nl) memcpy(signed_part + s, name, (size_t)nl);
    s += nl;

    uint8_t sig[64];
    xlc_ed25519_sign(sig, signed_part, (size_t)s, sk, pub);

    memcpy(out, pub, 32);
    memcpy(out + 32, signed_part + 32, 4);
    memcpy(out + 36, sig, 64);
    memcpy(out + 100, signed_part + 36, (size_t)app);
    return 100 + app;
}

bool mc_advert_open(const uint8_t *payload, int len, mc_advert_t *out)
{
    if (!payload || !out || len < 100 + 1) return false;
    memset(out, 0, sizeof *out);
    memcpy(out->pub, payload, 32);
    out->timestamp = (uint32_t)payload[32] | ((uint32_t)payload[33] << 8) |
                     ((uint32_t)payload[34] << 16) | ((uint32_t)payload[35] << 24);

    int app = len - 100;
    if (app > 1 + 31) app = 1 + 31;
    uint8_t signed_part[32 + 4 + 1 + 31];
    memcpy(signed_part, payload, 36);
    memcpy(signed_part + 36, payload + 100, (size_t)app);
    if (!xlc_ed25519_verify(payload + 36, signed_part, (size_t)(36 + app),
                            out->pub))
        return false;

    /* The blocks, in the order MeshCore writes them, each one moving the
     * name along (mc.h). A block that does not fit means the app data is
     * not what its flags claim, and a name read past it would be noise. */
    const uint8_t *ad = payload + 100;
    out->flags = ad[0];
    int i = 1;
    if (out->flags & MC_ADV_LATLON) {
        if (i + 8 > app) return false;
        out->lat = (int32_t)((uint32_t)ad[i] | ((uint32_t)ad[i + 1] << 8) |
                             ((uint32_t)ad[i + 2] << 16) | ((uint32_t)ad[i + 3] << 24));
        out->lon = (int32_t)((uint32_t)ad[i + 4] | ((uint32_t)ad[i + 5] << 8) |
                             ((uint32_t)ad[i + 6] << 16) | ((uint32_t)ad[i + 7] << 24));
        i += 8;
    }
    if (out->flags & MC_ADV_FEAT1) {
        if (i + 2 > app) return false;
        out->feat1 = (uint16_t)(ad[i] | (ad[i + 1] << 8));
        i += 2;
    }
    if (out->flags & MC_ADV_FEAT2) {
        if (i + 2 > app) return false;
        out->feat2 = (uint16_t)(ad[i] | (ad[i + 1] << 8));
        i += 2;
    }
    if ((out->flags & MC_ADV_HAS_NAME) && app > i) {
        int nl = app - i;
        if (nl > (int)sizeof out->name - 1) nl = (int)sizeof out->name - 1;
        memcpy(out->name, ad + i, (size_t)nl);
        out->name[nl] = 0;
    }
    return true;
}
