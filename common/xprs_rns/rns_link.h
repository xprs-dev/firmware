/**
 * @file rns_link.h
 * @brief An RNS Link, initiator side (wire-compatible, RNS 1.3.5).
 *
 * The one thing a station needs a link for is the bulk lane (XPRS.md 11.2):
 * a `cmd:file` is answered with bytes that travel as a Reticulum resource,
 * and a resource rides a link. This is the three-packet handshake the
 * initiator drives, mirrored from reticulum-dart's rns_link.dart so the two
 * agree byte for byte (test_rns_link_host.sh drives this code against that
 * implementation):
 *
 *   1. LINKREQUEST  -> the holder's destination: eph_x25519_pub(32) +
 *      eph_ed25519_pub(32) + signalling(3). link_id = truncated hash of the
 *      packet's hashable part without the signalling bytes.
 *   2. LRPROOF      <- the holder: sig(64) + its eph_x25519_pub(32) +
 *      signalling(3), the signature by the DESTINATION identity's Ed25519
 *      key over link_id + its pub + its ed pub + signalling.
 *   3. LRRTT        -> encrypted DATA on the link, msgpack(rtt).
 *
 * key = HKDF(64, X25519(ours, theirs), salt = link_id); link traffic is an
 * RNS token under it. Only MTU 500 is ever offered: a station streams a
 * resource through a few kilobytes of RAM, and a 500-byte part is the size
 * that keeps.
 */
#pragma once
#include "rns.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { RNS_LINK_PENDING, RNS_LINK_ACTIVE, RNS_LINK_CLOSED } rns_link_state_t;

typedef struct {
    rns_link_state_t state;
    uint8_t dest[RNS_HASH_LEN];        /* the holder's destination */
    uint8_t dest_ed_pub[32];           /* its identity's Ed25519 half */
    uint8_t link_id[RNS_HASH_LEN];
    uint8_t x_prv[32], x_pub[32];      /* our ephemeral X25519 */
    uint8_t ed_pub[32];                /* our ephemeral Ed25519, request only */
    uint8_t key[64];                   /* derived: signing(32) + encryption(32) */
    uint16_t mtu;                      /* negotiated; 500 unless the holder lowered it */
    bool have_next_hop;
    uint8_t next_hop[RNS_HASH_LEN];
} rns_link_t;

/**
 * Prepare a link toward the destination [dest_hash] of the identity whose
 * full public key is [peer_pub] (x25519 || ed25519), through [next_hop]
 * when the peer is behind a transport node (NULL for a direct neighbour).
 */
void rns_link_init(rns_link_t *l, const uint8_t dest_hash[RNS_HASH_LEN],
                   const uint8_t peer_pub[RNS_PUB_LEN],
                   const uint8_t *next_hop);

/** The LINKREQUEST packet, packed. Sets link_id. @return length or -1. */
int rns_link_request(rns_link_t *l, uint8_t *out, size_t cap);

/** Is this packet addressed to this link (dest LINK, hash == link_id)? */
bool rns_link_owns(const rns_link_t *l, const rns_packet_t *p);

/**
 * Feed the LRPROOF. On a valid proof the key is derived, the link is ACTIVE,
 * and the LRRTT packet to send is written. @return its length, or -1 when
 * the proof is not for this link, does not verify, or the link is not
 * pending.
 */
int rns_link_proof(rns_link_t *l, const rns_packet_t *proof,
                   uint8_t *rtt_out, size_t cap);

/** An encrypted DATA packet on the link with [ctx]. @return length or -1. */
int rns_link_encrypt(const rns_link_t *l, uint8_t ctx,
                     const uint8_t *plain, size_t len, uint8_t *out, size_t cap);

/** Decrypt a DATA packet addressed to this link. @return length or -1. */
int rns_link_decrypt(const rns_link_t *l, const rns_packet_t *p,
                     uint8_t *out, size_t cap);

/** An unencrypted PROOF-type packet on the link (a resource proof). */
int rns_link_proof_packet(const rns_link_t *l, uint8_t ctx,
                          const uint8_t *data, size_t len, uint8_t *out, size_t cap);

/** The resource part size this link carries: mtu - 35 - 1. */
static inline int rns_link_sdu(const rns_link_t *l) { return (int)l->mtu - 36; }

#ifdef __cplusplus
}
#endif
