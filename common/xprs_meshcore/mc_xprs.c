/* mc_xprs.c -- an XPRS wire as a MeshCore frame, and back. See mc.h.
 *
 * MeshCore's own answer to "a payload this node does not understand" is
 * PAYLOAD_TYPE_RAW_CUSTOM (0x0F), which is what XPRS uses, flood-routed so
 * that a repeater appends its hash and carries it on. Nothing inside is
 * MeshCore's business: the payload is the XPRS wire, with one marker byte
 * in front of it so that a two-part wire can be put back together.
 *
 *   marker 0x00        the whole wire follows
 *   marker 0x80 | part part 1 or 2, then a 16-bit tag, then the bytes
 *
 * The tag is derived from the wire (XPRS.md section 5's identifier), so two
 * stations airing one packet produce identical frames and every duplicate
 * filter on the channel agrees.
 */

#include "mc.h"

#include <string.h>

#include "xprs.h"

int mc_xprs_frames_for(int len)
{
    if (len <= 0 || len > XPRS_MAX_WIRE) return 0;
    return len <= MC_XPRS_ONE_FRAME ? 1 : 2;
}

/* header(1) + path length(1) + payload, with an empty path. */
static int frame_bytes(int payload) { return 2 + payload; }

int mc_xprs_frame_len(int len, int part)
{
    int frames = mc_xprs_frames_for(len);
    if (!frames || part >= frames) return 0;
    if (frames == 1) return frame_bytes(1 + len);
    int this_part = part == 0 ? MC_XPRS_FRAG_MAX : len - MC_XPRS_FRAG_MAX;
    return frame_bytes(MC_XPRS_FRAG_HDR + this_part);
}

/* The 16-bit tag that pairs two fragments: the front of the packet's own
 * identifier, so both stations and both fragments agree on it. */
static uint16_t wire_tag(const char *wire, int len)
{
    char id[XPRS_ID_LEN];
    if (!xprs_id_of(wire, len, id)) return 0;
    uint16_t t = 0;
    for (int i = 0; i < 4 && id[i]; i++) {
        char c = id[i];
        int v = c >= 'a' ? c - 'a' + 10 : c - '0';
        t = (uint16_t)((t << 4) | (uint16_t)(v & 0x0F));
    }
    return t ? t : 1;
}

static int build(const uint8_t *payload, int plen, uint8_t *out, int cap)
{
    mc_pkt_t p;
    memset(&p, 0, sizeof p);
    p.route = MC_ROUTE_FLOOD;
    p.type = MC_PT_RAW_CUSTOM;
    p.hash_size = 1;
    p.payload = payload;
    p.payload_len = plen;
    return mc_build(&p, out, cap);
}

int mc_xprs_wrap(const char *wire, int len, uint8_t frames[2][MC_FRAME_MAX],
                 int frame_len[2])
{
    int n = mc_xprs_frames_for(len);
    if (!n || !wire || !frames || !frame_len) return 0;
    uint8_t payload[MC_PAYLOAD_MAX];

    if (n == 1) {
        payload[0] = 0x00;
        memcpy(payload + 1, wire, (size_t)len);
        frame_len[0] = build(payload, 1 + len, frames[0], MC_FRAME_MAX);
        return frame_len[0] ? 1 : 0;
    }

    uint16_t tag = wire_tag(wire, len);
    for (int i = 0; i < 2; i++) {
        int off = i * MC_XPRS_FRAG_MAX;
        int part = i == 0 ? MC_XPRS_FRAG_MAX : len - MC_XPRS_FRAG_MAX;
        payload[0] = (uint8_t)(0x80 | (i + 1));
        payload[1] = (uint8_t)(tag & 0xFF);
        payload[2] = (uint8_t)(tag >> 8);
        memcpy(payload + MC_XPRS_FRAG_HDR, wire + off, (size_t)part);
        frame_len[i] = build(payload, MC_XPRS_FRAG_HDR + part, frames[i],
                             MC_FRAME_MAX);
        if (!frame_len[i]) return 0;
    }
    return 2;
}

static void reasm_expire(mc_reasm_t *r, uint32_t now_ms)
{
    for (int i = 0; i < MC_XPRS_REASM_SLOTS; i++)
        if (r->s[i].used && now_ms - r->s[i].t_ms > MC_XPRS_FRAG_TTL_MS)
            r->s[i].used = false;
}

int mc_xprs_unwrap(mc_reasm_t *r, const uint8_t *frame, int len,
                   uint32_t now_ms, char *wire, int cap)
{
    mc_pkt_t p;
    if (!mc_parse(frame, len, &p)) return -1;
    if (p.type != MC_PT_RAW_CUSTOM || p.payload_len < 1) return -1;

    uint8_t marker = p.payload[0];
    if (marker == 0x00) {
        int n = p.payload_len - 1;
        if (n <= 0 || n >= cap) return -1;
        memcpy(wire, p.payload + 1, (size_t)n);
        wire[n] = 0;
        return xprs_looks_like((const uint8_t *)wire, n) ? n : -1;
    }
    if ((marker & 0xF0) != 0x80) return -1;

    int part = (marker & 0x0F) - 1;
    if (part < 0 || part > 1 || p.payload_len < MC_XPRS_FRAG_HDR + 1) return -1;
    if (!r) return 0;
    reasm_expire(r, now_ms);

    uint16_t tag = (uint16_t)(p.payload[1] | (p.payload[2] << 8));
    int plen = p.payload_len - MC_XPRS_FRAG_HDR;
    if (plen > MC_XPRS_FRAG_MAX) return -1;

    mc_reasm_slot_t *slot = NULL, *spare = NULL;
    for (int i = 0; i < MC_XPRS_REASM_SLOTS; i++) {
        if (r->s[i].used && r->s[i].tag == tag) { slot = &r->s[i]; break; }
        if (!r->s[i].used && !spare) spare = &r->s[i];
    }
    if (!slot) {
        /* No free slot: the oldest waiting half is the one to lose. */
        if (!spare) {
            spare = &r->s[0];
            for (int i = 1; i < MC_XPRS_REASM_SLOTS; i++)
                if ((int32_t)(r->s[i].t_ms - spare->t_ms) < 0) spare = &r->s[i];
        }
        slot = spare;
        memset(slot, 0, sizeof *slot);
        slot->used = true;
        slot->tag = tag;
    }
    slot->t_ms = now_ms;
    slot->have = (uint8_t)(slot->have | (1 << part));
    slot->len[part] = (uint8_t)plen;
    memcpy(slot->part[part], p.payload + MC_XPRS_FRAG_HDR, (size_t)plen);
    if (slot->have != 0x03) return 0;            /* still short */

    int n = slot->len[0] + slot->len[1];
    if (n <= 0 || n >= cap) { slot->used = false; return -1; }
    memcpy(wire, slot->part[0], slot->len[0]);
    memcpy(wire + slot->len[0], slot->part[1], slot->len[1]);
    wire[n] = 0;
    slot->used = false;
    return xprs_looks_like((const uint8_t *)wire, n) ? n : -1;
}
