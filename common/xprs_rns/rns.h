/**
 * @file rns.h
 * @brief Enough Reticulum for a station to reach an indexer.
 *
 * Wire-compatible with RNS 1.3.5 and with the `reticulum-dart` sibling package,
 * which is the implementation this one is tested against — the host harness
 * decrypts bytes that Dart produced and reproduces them byte for byte
 * (`test_rns_host.sh`). Crypto interop is where this kind of code fails
 * quietly, so it is pinned by vectors rather than by reading.
 *
 * ── What is here, and what deliberately is not ──────────────────────────────
 *
 * Here: identities and destination addressing, the HDLC framing a TCP
 * interface speaks, the packet header, and **encrypted single packets** —
 * ephemeral X25519 to the destination's public key, so a station can send to
 * another station without establishing anything first.
 *
 * Not here: LINK, ratchets, resources, transport/routing. A dongle that talks
 * to an indexer it already knows the address of needs none of them, and each
 * would cost memory this board does not have. If a link is ever needed, this
 * file is the layer it would sit on rather than something to unpick.
 *
 * ── The shapes ──────────────────────────────────────────────────────────────
 *
 *   private key   x25519_prv(32) || ed25519_prv(32)
 *   public key    x25519_pub(32) || ed25519_pub(32)
 *   identity hash SHA-256(public key)[:16]
 *   name hash     SHA-256("app.aspect1.aspect2")[:10]
 *   destination   SHA-256(name_hash || identity_hash)[:16]
 *   encrypted     eph_pub(32) || iv(16) || AES-256-CBC(PKCS7(plain)) || HMAC(32)
 */

#ifndef XPRS_RNS_H
#define XPRS_RNS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RNS_KEY_HALF        32
#define RNS_PRV_LEN         64      /* x25519_prv || ed25519_prv */
#define RNS_PUB_LEN         64      /* x25519_pub || ed25519_pub */
#define RNS_HASH_LEN        16      /* identity and destination hashes */
#define RNS_NAME_HASH_LEN   10
#define RNS_IV_LEN          16
#define RNS_MAC_LEN         32
/** Smallest possible encrypted payload: eph pub + iv + one AES block + mac. */
#define RNS_ENC_OVERHEAD    (RNS_KEY_HALF + RNS_IV_LEN + RNS_MAC_LEN)

/** RNS default MTU (RNS/Reticulum.py). */
#define RNS_MTU             500

/* ── Primitives ─────────────────────────────────────────────────────────── */

void rns_sha256(const uint8_t *in, size_t len, uint8_t out[32]);
void rns_hmac_sha256(const uint8_t *key, size_t keylen,
                     const uint8_t *in, size_t len, uint8_t out[32]);

/**
 * @brief RNS's own HKDF (RNS/Cryptography/HKDF.py).
 *
 * Not RFC 5869: the expand step feeds back `block || context || counter` with
 * the counter taken modulo 256, and an absent salt is 32 zero bytes. Written
 * to match, because a key that is one byte different is a key that fails with
 * no message at the far end.
 */
bool rns_hkdf(uint8_t *out, size_t out_len,
              const uint8_t *ikm, size_t ikm_len,
              const uint8_t *salt, size_t salt_len);

/** X25519. [priv] is used as the scalar, as RNS does. */
void rns_x25519_base(const uint8_t priv[32], uint8_t pub_out[32]);
void rns_x25519_shared(const uint8_t priv[32], const uint8_t peer_pub[32],
                       uint8_t shared_out[32]);

/* ── Identity and addressing ────────────────────────────────────────────── */

typedef struct {
    uint8_t prv[RNS_PRV_LEN];       /* zeroed when this is a peer's identity */
    uint8_t pub[RNS_PUB_LEN];
    uint8_t hash[RNS_HASH_LEN];
    bool    have_private;
} rns_identity_t;

/**
 * @brief Set up an identity from the keys the caller already holds.
 *
 * @param prv 64 bytes, or NULL for a peer whose private key we do not have.
 *            The Ed25519 public half cannot be derived from a seed with the
 *            TweetNaCl this tree carries, so the full public key is passed in
 *            rather than half-derived and half-guessed.
 */
void rns_identity_init(const uint8_t *prv, const uint8_t pub[RNS_PUB_LEN],
                       rns_identity_t *out);
/** A peer, known only by its public key. */
void rns_identity_from_public(const uint8_t pub[RNS_PUB_LEN],
                              rns_identity_t *out);

/** SHA-256("app.aspect...")[:10]. Aspects are joined with '.'. */
void rns_name_hash(const char *app, const char *const *aspects, int naspects,
                   uint8_t out[RNS_NAME_HASH_LEN]);
/** SHA-256(name_hash || identity_hash)[:16] — what a packet is addressed to. */
void rns_destination_hash(const uint8_t name_hash[RNS_NAME_HASH_LEN],
                          const uint8_t identity_hash[RNS_HASH_LEN],
                          uint8_t out[RNS_HASH_LEN]);

/* ── Encrypted single packets ───────────────────────────────────────────── */

/**
 * @brief Encrypt to [peer], whose X25519 public half and destination hash are
 *        known. The hash is the HKDF salt, which is why it is a parameter and
 *        not derived here: it is the DESTINATION's hash, not the identity's.
 *
 * @param eph_priv  ephemeral X25519 private key — pass NULL for a random one.
 *                  Only tests pass it, and only so the bytes are reproducible.
 * @param iv        16 bytes, NULL for random. Same reasoning.
 * @return bytes written to @p out, or -1 if it would not fit.
 */
int rns_encrypt_to(const uint8_t peer_x25519_pub[32],
                   const uint8_t salt[RNS_HASH_LEN],
                   const uint8_t *plain, size_t plain_len,
                   const uint8_t *eph_priv, const uint8_t *iv,
                   uint8_t *out, size_t out_cap);

/**
 * @brief Decrypt what rns_encrypt_to() produced. Verifies the HMAC before
 *        touching the ciphertext, and refuses a bad one without explanation —
 *        there is nothing useful to say about a forgery.
 * @return plaintext length, or -1.
 */
int rns_decrypt_from(const uint8_t our_x25519_priv[32],
                     const uint8_t salt[RNS_HASH_LEN],
                     const uint8_t *in, size_t in_len,
                     uint8_t *out, size_t out_cap);

/* ── HDLC framing (what a TCP interface speaks) ─────────────────────────── */

#define RNS_HDLC_FLAG   0x7E
#define RNS_HDLC_ESC    0x7D
#define RNS_HDLC_MASK   0x20

/** Frame [in] as FLAG ... FLAG with escaping. Returns length or -1. */
int rns_hdlc_frame(const uint8_t *in, size_t len, uint8_t *out, size_t out_cap);

/**
 * @brief Feed received bytes; calls @p cb once per complete frame.
 *
 * A stream reassembler, because TCP gives no message boundaries: partial
 * frames are held across calls, and a frame longer than the buffer is dropped
 * rather than truncated into something that would parse as a shorter packet.
 */
typedef struct {
    uint8_t  buf[RNS_MTU + 64];
    size_t   len;
    bool     in_frame;
    bool     escaped;
    uint32_t dropped;          /* frames lost to overrun — worth logging */
} rns_hdlc_rx_t;

void rns_hdlc_rx_init(rns_hdlc_rx_t *rx);
void rns_hdlc_rx_feed(rns_hdlc_rx_t *rx, const uint8_t *in, size_t len,
                      void (*cb)(const uint8_t *frame, size_t len, void *ctx),
                      void *ctx);

/* ── Packet header ──────────────────────────────────────────────────────── */

#define RNS_PACKET_DATA        0x00
#define RNS_PACKET_ANNOUNCE    0x01
#define RNS_PACKET_LINKREQUEST 0x02
#define RNS_PACKET_PROOF       0x03

#define RNS_DEST_SINGLE        0x00
#define RNS_DEST_GROUP         0x01
#define RNS_DEST_PLAIN         0x02
#define RNS_DEST_LINK          0x03

#define RNS_HEADER_1           0x00
#define RNS_HEADER_2           0x01

#define RNS_TRANSPORT_BROADCAST 0x00
#define RNS_TRANSPORT_TRANSPORT 0x01

typedef struct {
    uint8_t  header_type;
    /* HEADER_2 only: the transport node that relayed it. Everything a hub
     * relays arrives this way, so a station that refuses HEADER_2 hears
     * nothing from the wider network — which is exactly what this one did
     * until a real hub was pointed at it. */
    uint8_t  transport_id[RNS_HASH_LEN];
    bool     have_transport_id;
    uint8_t  context_flag;
    uint8_t  transport_type;
    uint8_t  dest_type;
    uint8_t  packet_type;
    uint8_t  hops;
    uint8_t  dest[RNS_HASH_LEN];
    uint8_t  context;
    const uint8_t *data;
    size_t   data_len;
} rns_packet_t;

/* ── Announces ──────────────────────────────────────────────────────────── */

#define RNS_RANDOM_HASH_LEN 10
#define RNS_SIG_LEN         64

/**
 * @brief One parsed (and verified) announce.
 *
 * [app_data] points into the caller's packet buffer. [pub] is the announcing
 * identity's full public key -- what a receiver needs to talk back.
 */
typedef struct {
    uint8_t  dest[RNS_HASH_LEN];
    uint8_t  pub[RNS_PUB_LEN];
    uint8_t  name_hash[RNS_NAME_HASH_LEN];
    uint8_t  random_hash[RNS_RANDOM_HASH_LEN];
    const uint8_t *app_data;
    size_t   app_len;
    /* Into the caller's packet buffer, like app_data: what the signature
     * covers and what it is. Filled by rns_announce_open() so the verify
     * does not have to walk the packet again. */
    const uint8_t *sig;
    const uint8_t *ratchet;      /* NULL unless the announce carried one */
} rns_announce_t;

/**
 * @brief Build a signed HEADER_1 announce for [id]'s destination under
 *        [name_hash], carrying [app] as app_data.
 *
 * The parts that were learned the expensive way, kept here so nobody learns
 * them twice (they cost the T-Dongle hours against real hubs):
 *   - random_hash is 5 random bytes + 5 big-endian epoch seconds. Hubs judge
 *     freshness by that timestamp; ten random bytes reads as a replay and the
 *     announce is taken and silently never propagated.
 *   - the signature covers dest+pub+name_hash+random_hash+app, in that order.
 *   - announcing one destination faster than about once an hour to a PUBLIC
 *     hub burns its reputation there. Pacing is the caller's duty; toward a
 *     directly-connected peer (an XPRS node's own TCP server) there is no
 *     such police and the caller may pace by channel sense instead.
 *
 * @param id       must hold the private half.
 * @param epoch_s  seconds since the epoch, for the freshness stamp.
 * @return packet length, or -1 if it would not fit.
 */
int rns_announce_build(const rns_identity_t *id,
                       const uint8_t name_hash[RNS_NAME_HASH_LEN],
                       const uint8_t *app, size_t app_len,
                       uint64_t epoch_s,
                       uint8_t *out, size_t out_cap);

/**
 * @brief Parse a packet already identified as an announce, and VERIFY the
 *        Ed25519 signature against the key the announce itself carries.
 *
 * Verifying against the carried key proves internal consistency, not
 * authorship of anything beyond this announce -- which is exactly Reticulum's
 * own contract: an announce introduces an identity, it does not vouch for it.
 * A context-flagged announce (ratchet present) is handled; the ratchet is
 * skipped, as this codec keeps no ratchet state.
 *
 * @return true only on a well-formed announce whose signature verifies AND
 *         whose destination equals hash(name_hash, identity) -- an announce
 *         claiming a destination its own keys cannot produce is a forgery
 *         however valid its signature.
 */
bool rns_announce_parse(const rns_packet_t *p, rns_announce_t *out);

/**
 * @brief Read an announce WITHOUT checking its signature.
 *
 * Everything rns_announce_parse() does except the Ed25519, which is the
 * expensive part by three orders of magnitude. Structure, lengths and the
 * destination-hash consistency check are all still enforced.
 *
 * This exists so a station can decide whether an announce is any of its
 * business before paying to verify it. On a bridge to the public Reticulum
 * network the overwhelming majority are not: they are for other
 * applications entirely, and verifying them all is what took two T-Decks
 * into a reboot loop (see docs/esp32.md). Anything that survives the
 * caller's filter must still go through rns_announce_verify() before it is
 * believed -- the fields here are UNTRUSTED until it does.
 */
bool rns_announce_open(const rns_packet_t *p, rns_announce_t *out);

/** @brief The Ed25519 check for an announce read by rns_announce_open(). */
bool rns_announce_verify(const rns_packet_t *p, const rns_announce_t *a);

/**
 * @brief Build a HEADER_1 packet: flags(1) hops(1) dest(16) context(1) data.
 *
 * Only HEADER_1 is built: a leaf addresses a destination, it does not relay for
 * anybody, and HEADER_2 is what a transport node emits.
 * @return length, or -1.
 */
int rns_packet_build(const rns_packet_t *p, uint8_t *out, size_t out_cap);

/**
 * @brief Parse either header form. [out]->data points into [in].
 *
 * HEADER_2 carries a 16-byte transport id BEFORE the destination; read as
 * HEADER_1 its transport id would be taken for the address, which is a packet
 * silently attributed to the wrong destination.
 */
bool rns_packet_parse(const uint8_t *in, size_t len, rns_packet_t *out);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_RNS_H */
