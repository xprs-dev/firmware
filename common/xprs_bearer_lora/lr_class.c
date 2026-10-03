/* lr_class.c -- see lr_class.h. Platform-free on purpose: the headers it
 * takes carry constants and nothing else, so this compiles on the host. */

#include "lr_class.h"

#include "mc.h"
#include "mt.h"

/* The smallest payload each MeshCore type can have, which is a far stronger
 * filter than "at most 184". Each number is the engine's own, so the two
 * cannot drift: an advert is a key, a timestamp, a signature and a flags
 * byte (mc_advert_build), an ACK is a checksum (mc_mesh.c ack_in), a direct
 * message and a path carry destination, source and MAC before any
 * ciphertext (mc_mesh.c, "Opening either means a key exchange"), and a
 * channel message starts with the channel's hash. 0 means "no such type",
 * which is a rejection: 0x0C to 0x0E are not on the air. */
static int mc_payload_min(uint8_t type)
{
    switch (type) {
    case MC_PT_ADVERT:   return 32 + 4 + 64 + 1;
    case MC_PT_ACK:      return 4;
    case MC_PT_TXT_MSG:
    case MC_PT_PATH:     return 4;
    case MC_PT_REQ:
    case MC_PT_RESPONSE:
    case MC_PT_GRP_TXT:
    case MC_PT_GRP_DATA:
    case MC_PT_ANON_REQ:
    case MC_PT_TRACE:
    case MC_PT_MULTIPART:
    case MC_PT_CONTROL:
    case MC_PT_RAW_CUSTOM: return 1;
    default:             return 0;
    }
}

bool lr_class_mc_fits(const uint8_t *frame, int len)
{
    if (!frame || len < 2) return false;

    /* `0bVVPPPPRR`: route in bits 0-1, payload type in bits 2-5, version in
     * bits 6-7 (mc.h). Version 0 is what is on the air, and this is the
     * cheapest discriminator there is: a Meshtastic broadcast's first byte
     * is 0xFF, whose top two bits are 11, so it can never pass here. */
    uint8_t route = (uint8_t)(frame[0] & 0x03);
    uint8_t type = (uint8_t)((frame[0] >> 2) & 0x0F);
    if (((frame[0] >> 6) & 0x03) != 0) return false;
    int pmin = mc_payload_min(type);
    if (!pmin) return false;

    int i = 1;
    if (route == MC_ROUTE_TRANSPORT_FLOOD || route == MC_ROUTE_TRANSPORT_DIRECT) {
        if (len < i + 4) return false;
        i += 4;
    }
    if (len < i + 1) return false;
    uint8_t pl = frame[i++];
    int hops = pl & 0x3F;
    int hash_size = ((pl >> 6) & 0x03) + 1;
    int path_bytes = hops * hash_size;
    if (path_bytes > MC_PATH_MAX || len < i + path_bytes) return false;
    i += path_bytes;

    int payload_len = len - i;
    return payload_len >= pmin && payload_len <= MC_PAYLOAD_MAX;
}

bool lr_class_mt_fits(const uint8_t *frame, int len, const lr_class_in_t *in)
{
    if (!frame || !in || len < MT_HDR_LEN + 1) return false;

    /* `to(4) from(4) id(4) flags channel next_hop relay_node`, little
     * endian (mt.h). A frame with no sender is not one Meshtastic aired;
     * mt_mesh_on_frame refuses it too, so agreeing here costs nothing. */
    uint32_t to = (uint32_t)frame[0] | ((uint32_t)frame[1] << 8) |
                  ((uint32_t)frame[2] << 16) | ((uint32_t)frame[3] << 24);
    uint32_t from = (uint32_t)frame[4] | ((uint32_t)frame[5] << 8) |
                    ((uint32_t)frame[6] << 16) | ((uint32_t)frame[7] << 24);
    if (!from) return false;

    /* The flags byte: hop_limit in bits 0-2, hop_start in bits 5-7. A frame
     * cannot have more hops left than it started with. hop_start 0 is a
     * sender that never set it, which the network still carries. */
    uint8_t flags = frame[12];
    uint8_t hop_limit = (uint8_t)(flags & 0x07);
    uint8_t hop_start = (uint8_t)((flags >> 5) & 0x07);
    if (hop_start && hop_limit > hop_start) return false;

    /* A broadcast says so in `to` and needs no channel we know. Otherwise
     * the channel hash has to be one that exists on this channel: LongFast,
     * the clear XPRS channel, or 0 for a public-key direct message. */
    if (to == MT_BROADCAST) return true;
    uint8_t ch = frame[13];
    return ch == in->lf_hash || ch == in->xprs_hash || ch == 0;
}

bool lr_class_mt_is_xprs(const uint8_t *frame, int len, const lr_class_in_t *in)
{
    /* The clear XPRS channel. mt_xprs_unwrap also insists on the private
     * portnum inside a protobuf that decodes, which is proof; this is the
     * half of it that costs nothing and needs no reassembly state. */
    return lr_class_mt_fits(frame, len, in) && frame[13] == in->xprs_hash;
}

bool lr_class_mc_is_xprs(const uint8_t *frame, int len)
{
    return lr_class_mc_fits(frame, len) &&
           ((frame[0] >> 2) & 0x0F) == MC_PT_RAW_CUSTOM;
}

lr_class_t lr_classify(const uint8_t *frame, int len, const lr_class_in_t *in)
{
    bool mt = lr_class_mt_fits(frame, len, in);
    bool mc = lr_class_mc_fits(frame, len);
    if (mt && mc) return LR_CLASS_EITHER;
    if (mt) return LR_CLASS_MT;
    if (mc) return LR_CLASS_MC;
    return LR_CLASS_NEITHER;
}
