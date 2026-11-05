/* See rns_link.h. */
#include "rns_link.h"
#include <string.h>

#define LINK_MODE_AES256_CBC 0x01
#define LINK_MTU_OFFER       500

static void signalling(uint16_t mtu, uint8_t out[3])
{
    uint32_t v = ((uint32_t)mtu & 0x1FFFFFu) + ((((uint32_t)LINK_MODE_AES256_CBC << 5) & 0xE0u) << 16);
    out[0] = (uint8_t)(v >> 16);
    out[1] = (uint8_t)(v >> 8);
    out[2] = (uint8_t)v;
}

void rns_link_init(rns_link_t *l, const uint8_t dest_hash[RNS_HASH_LEN],
                   const uint8_t peer_pub[RNS_PUB_LEN],
                   const uint8_t *next_hop)
{
    memset(l, 0, sizeof *l);
    l->state = RNS_LINK_PENDING;
    memcpy(l->dest, dest_hash, RNS_HASH_LEN);
    memcpy(l->dest_ed_pub, peer_pub + RNS_KEY_HALF, 32);
    l->mtu = LINK_MTU_OFFER;
    if (next_hop) {
        l->have_next_hop = true;
        memcpy(l->next_hop, next_hop, RNS_HASH_LEN);
    }
    /* Fresh X25519 per link: it is the ECDH half, and reuse would hand a
     * later eavesdropper every earlier transfer. The Ed25519 pair is only
     * placed in the request; nothing on this side ever signs with it. */
    rns_random_bytes(l->x_prv, 32);
    rns_x25519_base(l->x_prv, l->x_pub);
    uint8_t sec[64];
    rns_ed25519_keypair(l->ed_pub, sec);
    memset(sec, 0, sizeof sec);
}

int rns_link_request(rns_link_t *l, uint8_t *out, size_t cap)
{
    uint8_t data[32 + 32 + 3];
    memcpy(data, l->x_pub, 32);
    memcpy(data + 32, l->ed_pub, 32);
    signalling(l->mtu, data + 64);
    rns_packet_t p;
    memset(&p, 0, sizeof p);
    p.header_type = l->have_next_hop ? RNS_HEADER_2 : RNS_HEADER_1;
    p.transport_type = l->have_next_hop ? RNS_TRANSPORT_TRANSPORT : RNS_TRANSPORT_BROADCAST;
    if (l->have_next_hop) {
        memcpy(p.transport_id, l->next_hop, RNS_HASH_LEN);
        p.have_transport_id = true;
    }
    p.dest_type = RNS_DEST_SINGLE;
    p.packet_type = RNS_PACKET_LINKREQUEST;
    memcpy(p.dest, l->dest, RNS_HASH_LEN);
    p.context = RNS_CTX_NONE;
    p.data = data;
    p.data_len = sizeof data;
    int n = rns_packet_build(&p, out, cap);
    if (n < 0) return -1;
    /* link_id = truncated_hash([flags & 0x0F] + body) with the 3 signalling
     * bytes left off; the body starts after the transport id on HEADER_2 so
     * the relayed HEADER_1 form hashes the same. */
    uint8_t hashable[1 + RNS_HASH_LEN + 1 + 64];
    size_t body = l->have_next_hop ? 2 + RNS_HASH_LEN : 2;
    hashable[0] = out[0] & 0x0F;
    size_t hl = (size_t)n - body - 3;
    if (hl != RNS_HASH_LEN + 1 + 64) return -1;
    memcpy(hashable + 1, out + body, hl);
    uint8_t h[32];
    rns_sha256(hashable, 1 + hl, h);
    memcpy(l->link_id, h, RNS_HASH_LEN);
    return n;
}

bool rns_link_owns(const rns_link_t *l, const rns_packet_t *p)
{
    return p->dest_type == RNS_DEST_LINK &&
           memcmp(p->dest, l->link_id, RNS_HASH_LEN) == 0;
}

int rns_link_proof(rns_link_t *l, const rns_packet_t *proof,
                   uint8_t *rtt_out, size_t cap)
{
    if (l->state != RNS_LINK_PENDING) return -1;
    if (proof->packet_type != RNS_PACKET_PROOF || proof->context != RNS_CTX_LRPROOF)
        return -1;
    if (!rns_link_owns(l, proof)) return -1;
    const uint8_t *d = proof->data;
    size_t n = proof->data_len;
    if (n != 64 + 32 + 3 && n != 64 + 32) return -1;
    const uint8_t *peer_pub = d + 64;
    uint16_t confirmed = LINK_MTU_OFFER;
    uint8_t sgn[3];
    size_t sl = 0;
    if (n == 64 + 32 + 3) {
        uint32_t v = ((uint32_t)d[96] << 16) | ((uint32_t)d[97] << 8) | d[98];
        confirmed = (uint16_t)(v & 0x1FFFFFu);
        signalling(confirmed, sgn);
        sl = 3;
    }
    uint8_t signed_data[RNS_HASH_LEN + 32 + 32 + 3];
    memcpy(signed_data, l->link_id, RNS_HASH_LEN);
    memcpy(signed_data + RNS_HASH_LEN, peer_pub, 32);
    memcpy(signed_data + RNS_HASH_LEN + 32, l->dest_ed_pub, 32);
    if (sl) memcpy(signed_data + RNS_HASH_LEN + 64, sgn, 3);
    if (!rns_ed25519_verify(l->dest_ed_pub, d, signed_data, RNS_HASH_LEN + 64 + sl))
        return -1;
    /* Never above what we offered: our parts must still fit our radio. */
    if (confirmed < 128) confirmed = 128;
    if (confirmed > LINK_MTU_OFFER) confirmed = LINK_MTU_OFFER;
    l->mtu = confirmed;

    uint8_t shared[32];
    rns_x25519_shared(l->x_prv, peer_pub, shared);
    if (!rns_hkdf(l->key, 64, shared, 32, l->link_id, RNS_HASH_LEN)) return -1;
    memset(shared, 0, sizeof shared);
    l->state = RNS_LINK_ACTIVE;

    /* LRRTT: msgpack float64 0.1 -- a small nonzero figure is all the gate
     * on the other side reads. */
    static const uint8_t rtt[9] = { 0xCB, 0x3F, 0xB9, 0x99, 0x99, 0x99, 0x99, 0x99, 0x9A };
    return rns_link_encrypt(l, RNS_CTX_LRRTT, rtt, sizeof rtt, rtt_out, cap);
}

int rns_link_encrypt(const rns_link_t *l, uint8_t ctx,
                     const uint8_t *plain, size_t len, uint8_t *out, size_t cap)
{
    if (l->state != RNS_LINK_ACTIVE) return -1;
    uint8_t tok[RNS_MTU];
    int tn = rns_token_encrypt(l->key, plain, len, tok, sizeof tok);
    if (tn < 0) return -1;
    rns_packet_t p;
    memset(&p, 0, sizeof p);
    /* Link traffic is HEADER_1 to the link id whatever the route: a
     * transport forwards it by its link table, never by a path lookup. */
    p.header_type = RNS_HEADER_1;
    p.transport_type = RNS_TRANSPORT_BROADCAST;
    p.dest_type = RNS_DEST_LINK;
    p.packet_type = RNS_PACKET_DATA;
    memcpy(p.dest, l->link_id, RNS_HASH_LEN);
    p.context = ctx;
    p.data = tok;
    p.data_len = (size_t)tn;
    return rns_packet_build(&p, out, cap);
}

int rns_link_decrypt(const rns_link_t *l, const rns_packet_t *p,
                     uint8_t *out, size_t cap)
{
    if (l->state != RNS_LINK_ACTIVE) return -1;
    return rns_token_decrypt(l->key, p->data, p->data_len, out, cap);
}

int rns_link_proof_packet(const rns_link_t *l, uint8_t ctx,
                          const uint8_t *data, size_t len, uint8_t *out, size_t cap)
{
    rns_packet_t p;
    memset(&p, 0, sizeof p);
    p.header_type = RNS_HEADER_1;
    p.transport_type = RNS_TRANSPORT_BROADCAST;
    p.dest_type = RNS_DEST_LINK;
    p.packet_type = RNS_PACKET_PROOF;
    memcpy(p.dest, l->link_id, RNS_HASH_LEN);
    p.context = ctx;
    p.data = data;
    p.data_len = len;
    return rns_packet_build(&p, out, cap);
}
