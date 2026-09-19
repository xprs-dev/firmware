/* mt_xprs.c -- an XPRS wire as a Meshtastic frame, and back. See mt.h.
 *
 * Why wrap at all: a Meshtastic router floods on the 16-byte header alone
 * and relays what it cannot decode. A bare XPRS wire read as that header is
 * garbage whose byte 12 happens to be a hop limit, so the neighbours would
 * relay it with bytes 12-15 rewritten. A real header with our own channel
 * hash and a clear Data on a private portnum is ignored cleanly by every
 * Meshtastic node that does not hold the channel, and relayed intact by the
 * ones that relay everything. */

#include "mt.h"

#include <string.h>

#include "xprs.h"

static int varint_len(int v) { return v < 128 ? 1 : 2; }

int mt_xprs_frames_for(int len)
{
    if (len <= 0 || len > XPRS_MAX_WIRE) return 0;
    return len <= MT_XPRS_ONE_FRAME ? 1 : 2;
}

int mt_xprs_frame_len(int len, int part)
{
    int frames = mt_xprs_frames_for(len);
    if (!frames || part >= frames) return 0;
    int payload = len;
    if (frames == 2)
        payload = MT_XPRS_FRAG_HDR +
                  (part == 0 ? MT_XPRS_FRAG_MAX : len - MT_XPRS_FRAG_MAX);
    /* portnum (tag + 2-byte varint) + payload tag + its length varint */
    return MT_HDR_LEN + 3 + 1 + varint_len(payload) + payload;
}

bool mt_xprs_hdr_of(const char *wire, int len, uint32_t self_node,
                    mt_hdr_t *out)
{
    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return false;
    int flen = 0;
    const char *f = xprs_get(&p, "f", &flen);
    if (!f || flen <= 0) return false;

    char text[XPRS_MAX_WIRE + 1];
    int tlen = xprs_signed_text(&p, text, sizeof text);
    if (tlen < 0) return false;
    uint8_t h[32];
    xprs_sha256((const uint8_t *)text, (size_t)tlen, h);

    /* Meshtastic routers will flood this frame too, on its header alone,
     * and every hop is two seconds of SF11 on the one EU channel. So their
     * share is capped at their own default of three hops, whatever the XPRS
     * budget is; XPRS digipeaters keep counting by via: as always. */
    int start = xprs_relay_limit(&p);
    if (start > MT_HOP_DEFAULT) start = MT_HOP_DEFAULT;
    /* And only for what somebody is waiting for. A beacon, a service
     * announcement or an identity is this neighbourhood's business: it
     * goes out with no Meshtastic hops, so their routers let it be, and
     * XPRS digipeaters still carry it as far as via: allows. */
    char type[16];
    xprs_type(&p, type, sizeof type);
    static const char *const carried[] = {
        "message", "sos", "warning", "receipt", "reaction", "command",
        "result", "file", "mailbox",
    };
    bool carry = false;
    for (size_t i = 0; i < sizeof carried / sizeof carried[0]; i++)
        if (strcmp(type, carried[i]) == 0) carry = true;
    if (!carry) start = 0;
    int left = start - xprs_via_count(&p);
    if (left < 0) left = 0;

    memset(out, 0, sizeof *out);
    out->to = MT_BROADCAST;
    out->from = mt_node_of_call(f, flen);
    out->id = ((uint32_t)h[0] << 24) | ((uint32_t)h[1] << 16) |
              ((uint32_t)h[2] << 8) | h[3];
    if (!out->id) out->id = 1;          /* 0 means "no id" to the firmware */
    out->hop_limit = (uint8_t)left;
    out->hop_start = (uint8_t)start;
    out->channel = MT_CH_HASH_XPRS;
    out->relay_node = (uint8_t)self_node;
    return true;
}

static int build(const mt_hdr_t *h, const uint8_t *payload, int plen,
                 uint8_t *frame)
{
    mt_hdr_build(h, frame);
    mt_data_t d = { 0 };
    d.portnum = MT_PORT_XPRS;
    d.payload = payload;
    d.payload_len = plen;
    int n = mt_data_encode(&d, frame + MT_HDR_LEN, MT_FRAME_MAX - MT_HDR_LEN);
    return n < 0 ? 0 : MT_HDR_LEN + n;
}

int mt_xprs_wrap(const char *wire, int len, uint32_t self_node,
                 uint8_t frames[2][MT_FRAME_MAX], int frame_len[2])
{
    int nframes = mt_xprs_frames_for(len);
    if (!nframes) return 0;
    mt_hdr_t h;
    if (!mt_xprs_hdr_of(wire, len, self_node, &h)) return 0;

    if (nframes == 1) {
        frame_len[0] = build(&h, (const uint8_t *)wire, len, frames[0]);
        return frame_len[0] ? 1 : 0;
    }

    /* Two fragments. Each needs its own (from, id), or the second is a
     * duplicate of the first to every relay on the channel; the tag in the
     * payload is what pairs them again. */
    uint16_t tag = (uint16_t)h.id;
    uint8_t buf[MT_XPRS_ONE_FRAME];
    for (int part = 0; part < 2; part++) {
        int off = part ? MT_XPRS_FRAG_MAX : 0;
        int n = part ? len - MT_XPRS_FRAG_MAX : MT_XPRS_FRAG_MAX;
        buf[0] = (uint8_t)(0x80 | (part + 1));
        buf[1] = (uint8_t)(tag >> 8);
        buf[2] = (uint8_t)tag;
        memcpy(buf + MT_XPRS_FRAG_HDR, wire + off, (size_t)n);
        mt_hdr_t hp = h;
        hp.id = h.id + (uint32_t)part;
        frame_len[part] = build(&hp, buf, MT_XPRS_FRAG_HDR + n, frames[part]);
        if (!frame_len[part]) return 0;
    }
    return 2;
}

int mt_xprs_unwrap(mt_reasm_t *r, const uint8_t *frame, int len,
                   uint32_t now_ms, char *wire, int cap)
{
    mt_hdr_t h;
    if (!mt_hdr_parse(frame, len, &h) || h.channel != MT_CH_HASH_XPRS)
        return -1;
    mt_data_t d;
    if (!mt_data_decode(frame + MT_HDR_LEN, len - MT_HDR_LEN, &d) ||
        d.portnum != MT_PORT_XPRS || d.payload_len <= 0)
        return -1;

    const uint8_t *p = d.payload;
    if (p[0] == 't') {
        if (d.payload_len > cap - 1 || d.payload_len > XPRS_MAX_WIRE) return 0;
        memcpy(wire, p, (size_t)d.payload_len);
        wire[d.payload_len] = 0;
        return d.payload_len;
    }

    int part = p[0] & 0x03;
    if ((p[0] & 0xFC) != 0x80 || part < 1 || part > 2 ||
        d.payload_len <= MT_XPRS_FRAG_HDR ||
        d.payload_len - MT_XPRS_FRAG_HDR > MT_XPRS_FRAG_MAX || !r)
        return 0;                       /* ours, but not a shape we know */
    uint16_t tag = (uint16_t)((p[1] << 8) | p[2]);

    mt_reasm_slot_t *slot = NULL, *spare = NULL;
    for (int i = 0; i < MT_XPRS_REASM_SLOTS; i++) {
        mt_reasm_slot_t *s = &r->s[i];
        if (s->used && (int32_t)(now_ms - s->t_ms) > (int32_t)MT_XPRS_FRAG_TTL_MS)
            s->used = false;
        if (s->used && s->from == h.from && s->tag == tag) slot = s;
        /* A free slot, else the oldest one. */
        if (!spare || (spare->used && (!s->used ||
                                       (int32_t)(s->t_ms - spare->t_ms) < 0)))
            spare = s;
    }
    if (!slot) {
        slot = spare;
        memset(slot, 0, sizeof *slot);
        slot->used = true;
        slot->from = h.from;
        slot->tag = tag;
        slot->t_ms = now_ms;
    }
    int n = d.payload_len - MT_XPRS_FRAG_HDR;
    memcpy(slot->part[part - 1], p + MT_XPRS_FRAG_HDR, (size_t)n);
    slot->len[part - 1] = (uint8_t)n;
    slot->have |= (uint8_t)(1u << (part - 1));
    if (slot->have != 3) return 0;

    int total = slot->len[0] + slot->len[1];
    slot->used = false;
    if (total > cap - 1 || total > XPRS_MAX_WIRE) return 0;
    memcpy(wire, slot->part[0], slot->len[0]);
    memcpy(wire + slot->len[0], slot->part[1], slot->len[1]);
    wire[total] = 0;
    return total;
}
