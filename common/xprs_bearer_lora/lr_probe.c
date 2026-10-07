/* The auto-detect probe and its echo (lr_probe.h). */
#include "lr_probe.h"

#include <string.h>

#include "mt.h"
#include "mc.h"

int lrp_mt_build(lrp_probe_t *pr, uint32_t self_node, uint32_t rnd,
                 uint8_t *out, int cap)
{
    if (!pr || !out || cap < MT_FRAME_MAX) return 0;
    mt_hdr_t h;
    memset(&h, 0, sizeof h);
    h.to = MT_BROADCAST;
    h.from = self_node;
    h.id = rnd | 1u;
    h.hop_limit = MT_HOP_DEFAULT;
    h.hop_start = MT_HOP_DEFAULT;
    h.channel = mt_longfast_hash();
    h.relay_node = (uint8_t)self_node;
    mt_hdr_build(&h, out);
    mt_data_t d;
    memset(&d, 0, sizeof d);
    uint8_t one = 0x01;                 /* not a wrapped wire: nobody reads it */
    d.portnum = MT_PORT_XPRS;
    d.payload = &one;
    d.payload_len = 1;
    int dn = mt_data_encode(&d, out + MT_HDR_LEN, cap - MT_HDR_LEN);
    if (dn < 0) return 0;
    pr->mt_from = h.from;
    pr->mt_id = h.id;
    pr->mt_live = true;
    return MT_HDR_LEN + dn;
}

int lrp_mc_build(lrp_probe_t *pr, uint32_t rnd, uint8_t *out, int cap)
{
    if (!pr || !out || cap < MC_FRAME_MAX) return 0;
    uint8_t pl[4];
    for (int i = 0; i < 4; i++) pl[i] = (uint8_t)(rnd >> (8 * i));
    mc_pkt_t p;
    memset(&p, 0, sizeof p);
    p.route = MC_ROUTE_FLOOD;
    p.type = MC_PT_ACK;
    p.hash_size = 1;
    p.payload = pl;
    p.payload_len = 4;
    int n = mc_build(&p, out, cap);
    if (!n) return 0;
    pr->mc_hash = mc_packet_hash(&p);
    pr->mc_live = true;
    return n;
}

bool lrp_mt_echo(const lrp_probe_t *pr, const uint8_t *frame, int len)
{
    if (!pr || !pr->mt_live) return false;
    mt_hdr_t h;
    if (!mt_hdr_parse(frame, len, &h)) return false;
    return h.from == pr->mt_from && h.id == pr->mt_id && h.hop_limit < h.hop_start;
}

bool lrp_mc_echo(const lrp_probe_t *pr, const uint8_t *frame, int len)
{
    if (!pr || !pr->mc_live) return false;
    mc_pkt_t p;
    if (!mc_parse(frame, len, &p)) return false;
    return mc_packet_hash(&p) == pr->mc_hash && p.hops > 0;
}
