/**
 * @file mt.h
 * @brief Meshtastic on the XPRS LoRa radio: the wire, the repeater, the bridge.
 *
 * Since 2026-09 an XPRS LoRa station runs Meshtastic's LongFast modulation
 * (SF11, 250 kHz, CR 4/5, preamble 16, sync word 0x2B) on Meshtastic's own
 * frequency slot, so one radio hears both networks. docs/meshtastic.md is
 * the design; this header is the part of it a radio needs:
 *
 *   - the 16-byte Meshtastic header, and the few protobuf messages we read
 *     and write (Data, User, Routing), coded by hand from the public field
 *     numbers. Meshtastic's firmware and .proto files are GPL-3.0 and this
 *     tree is Apache-2.0, so nothing of theirs is copied or linked here.
 *   - XPRS on that channel: every XPRS wire rides as the payload of a clear
 *     Data on the private portnum MT_PORT_XPRS, channel "XPRS" (hash 0x09).
 *     A wire over MT_XPRS_ONE_FRAME bytes goes as two frames. The bare
 *     250-byte packet of XPRS.md section 4 is unchanged on every other
 *     bearer; this framing is LoRa's alone.
 *   - the repeater: Meshtastic's managed flood (dedup on from+id, hop_limit,
 *     next_hop, SNR-weighted contention, cancel on hearing a relay).
 *   - the bridge: LongFast text to XPRS t:message and back, with Meshtastic
 *     nodes addressed in XPRS as `MT` + 8 uppercase hex digits and XPRS
 *     callsigns shown on Meshtastic as virtual nodes whose number is derived
 *     from the callsign (mt_node_of_call).
 *
 * No ESP-IDF in the core files, so the P1-Pro (nRF52, Arduino) can take this
 * component by symlink the way it takes xprs_codec and xprs_bearer, and the
 * host harness (test_mt_host.sh) can run all of it. The two platform hooks
 * are xprs_loracrypto's xlc_aes_encrypt_block() and xprs_sha256()
 * (xprs_codec).
 */
#ifndef XPRS_MT_H
#define XPRS_MT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── The wire ─────────────────────────────────────────────────────────── */

#define MT_HDR_LEN        16
#define MT_FRAME_MAX      255          /* the SX1262's FIFO, and Meshtastic's */
#define MT_BROADCAST      0xFFFFFFFFu
#define MT_HOP_MAX        7            /* three bits of hop_limit */
#define MT_HOP_DEFAULT    3

/* Portnums (the public PortNum enum). */
#define MT_PORT_TEXT      1
#define MT_PORT_NODEINFO  4
#define MT_PORT_ROUTING   5
/* XPRS's own, in the range Meshtastic leaves to private applications
 * (256-511; 256 is PRIVATE_APP and 257 ATAK_FORWARDER). 0x158 is 256 + 'X'. */
#define MT_PORT_XPRS      0x158

#define MT_HW_PRIVATE     255          /* HardwareModel PRIVATE_HW */
#define MT_BITFIELD_OK_TO_MQTT 0x01u   /* Data.bitfield bit 0 */

/* The two channels a station speaks: Meshtastic's public LongFast with its
 * well-known key, and the clear channel "XPRS" that carries XPRS itself. */
#define MT_CH_NAME_LONGFAST "LongFast"
#define MT_CH_NAME_XPRS     "XPRS"
#define MT_CH_HASH_XPRS     0x09       /* xor("XPRS"), no key */

/* LongFast. The modem half lives in xprslora.c; these are the numbers both
 * ends of a link must share, stated once. */
#define MT_LF_SF          11
#define MT_LF_BW_HZ       250000u
#define MT_LF_CR          1            /* 4/5 */
#define MT_LF_PREAMBLE    16
#define MT_LF_SYNC        0x2B         /* 0x24B4 in the SX126x register */

typedef struct {
    uint32_t to, from, id;
    uint8_t  hop_limit;    /* 0..7 */
    uint8_t  hop_start;    /* 0..7, what the originator started with */
    bool     want_ack;
    bool     via_mqtt;
    uint8_t  channel;      /* the channel hash, not an index */
    uint8_t  next_hop;     /* 0 = no preference */
    uint8_t  relay_node;   /* last byte of whoever aired this copy */
} mt_hdr_t;

bool mt_hdr_parse(const uint8_t *buf, int len, mt_hdr_t *out);
void mt_hdr_build(const mt_hdr_t *h, uint8_t out[MT_HDR_LEN]);

/* The Data message. payload points into the caller's buffer on decode. */
typedef struct {
    uint32_t       portnum;
    const uint8_t *payload;
    int            payload_len;
    bool           want_response;
    uint32_t       dest, source;
    uint32_t       request_id;
    uint32_t       reply_id;
    uint32_t       emoji;
    bool           has_bitfield;
    uint32_t       bitfield;
} mt_data_t;

bool mt_data_decode(const uint8_t *buf, int len, mt_data_t *out);
/* Returns the encoded length, or -1 when it does not fit [cap]. */
int  mt_data_encode(const mt_data_t *d, uint8_t *out, int cap);

/* The User message, trimmed to what a node list shows. */
#define MT_LONG_NAME_MAX  40           /* bytes with NUL, per the firmware */
#define MT_SHORT_NAME_MAX 5
typedef struct {
    char     id[16];                   /* "!a1b2c3d4" */
    char     long_name[MT_LONG_NAME_MAX];
    char     short_name[MT_SHORT_NAME_MAX];
    uint32_t hw_model;
    uint32_t role;
    bool     has_public_key;
    uint8_t  public_key[32];           /* X25519, when has_public_key */
    bool     is_licensed;
} mt_user_t;

bool mt_user_decode(const uint8_t *buf, int len, mt_user_t *out);
int  mt_user_encode(const mt_user_t *u, uint8_t *out, int cap);

/* Routing: only the error_reason arm (an ack is error_reason NONE = 0).
 * Decode returns false when the message carries some other arm. */
bool mt_routing_decode(const uint8_t *buf, int len, int *error_reason);
int  mt_routing_encode_ack(uint8_t *out, int cap);
/* A Routing with any error_reason (0 is the ack). */
int  mt_routing_encode(uint8_t *out, int cap, int error_reason);
#define MT_ERR_PKI_UNKNOWN_PUBKEY 35

/* ── Channels, keys and frequencies ───────────────────────────────────── */

/* Meshtastic's 1-byte channel hash: xor of the name, xor of the key. */
uint8_t mt_channel_hash(const char *name, const uint8_t *key, int key_len);

/* Expand a channel PSK the way the firmware does: 0 bytes or {0} is no
 * encryption, one byte n >= 1 is the default key with its last byte raised
 * by n - 1, 16 or 32 bytes are the key itself. Returns the key length (0,
 * 16 or 32), or -1 for a PSK length Meshtastic does not accept. */
int mt_psk_expand(const uint8_t *psk, int psk_len, uint8_t key[32]);

/* The well-known LongFast key (the PSK "AQ==", a single byte 1). */
extern const uint8_t mt_default_key[16];

/* The channel hash of LongFast with the default key (0x08), computed. */
uint8_t mt_longfast_hash(void);

/* djb2, which the firmware uses to pick a frequency slot from a name. */
uint32_t mt_djb2(const char *s);

/* A Meshtastic region, as far as the frequency is concerned. */
typedef struct {
    const char *name;     /* Meshtastic's name: "EU_868" */
    const char *xprs;     /* this tree's lora_region word: "eu"; NULL = none */
    uint32_t    start_khz, end_khz;
    uint8_t     duty_pct; /* 100 = no hourly duty */
    int8_t      max_dbm;
} mt_region_t;

const mt_region_t *mt_regions(int *count);
/* By this tree's word ("eu", "us", "au") or by Meshtastic's name. */
const mt_region_t *mt_region_find(const char *name);

/* The centre frequency Meshtastic's slot rule gives [channel_name] at
 * [bw_hz] in [r]: freq_start + bw/2 + (djb2(name) % slots) * bw. */
uint32_t mt_slot_freq_hz(const mt_region_t *r, const char *channel_name,
                         uint32_t bw_hz);

/* ── Encryption ───────────────────────────────────────────────────────── */

/* The AES block, X25519 and the callsign seed live in xprs_loracrypto
 * (xlc.h) since MeshCore wanted the same three. What stays here is what is
 * Meshtastic's: the CTR stream, the CCM of a direct message, and the rule
 * that turns a callsign into a node number. */

/* Meshtastic channel encryption: AES-CTR, nonce = packet id (u64 LE) ||
 * from (u32 LE) || four zero bytes, counter in the last four bytes. The same
 * call encrypts and decrypts, in place. key_len 0 leaves [buf] alone. */
bool mt_crypt(const uint8_t *key, int key_len, uint32_t from, uint32_t id,
              uint8_t *buf, int len);

/* ── Direct messages (Meshtastic's PKI) ───────────────────────────────
 *
 * Since firmware 2.5 a DM is X25519 between the two nodes' keys, SHA-256
 * of the shared secret as an AES-256 key, and AES-CCM (8-byte tag, 13-byte
 * nonce); on the air it is ciphertext, tag and a 4-byte extra nonce, on
 * channel hash 0. A DM on the channel key is refused and rejected. */

#define MT_PKI_OVERHEAD 12

bool mt_ccm_encrypt(const uint8_t *key, int key_len, const uint8_t nonce[13],
                    const uint8_t *plain, int len, uint8_t *out, uint8_t tag8[8]);
bool mt_ccm_decrypt(const uint8_t *key, int key_len, const uint8_t nonce[13],
                    const uint8_t *crypt, int len, const uint8_t tag8[8],
                    uint8_t *out);

/* Returns the bytes written (len + 12), or -1. [extra] is the random extra
 * nonce, passed in so the host test can pin it. */
int mt_pki_encrypt(const uint8_t my_priv[32], const uint8_t their_pub[32],
                   uint32_t from, uint32_t id, uint32_t extra,
                   const uint8_t *plain, int len, uint8_t *out, int cap);
/* Returns the plaintext length, or -1 when it does not authenticate. */
int mt_pki_decrypt(const uint8_t my_priv[32], const uint8_t their_pub[32],
                   uint32_t from, uint32_t id, const uint8_t *in, int len,
                   uint8_t *out);

/* The key pair an XPRS callsign wears on Meshtastic. DERIVED, and so not
 * secret: private = sha256("XPRS/mt/x25519" || bare callsign), clamped.
 * Every bridge presents the same key for a callsign and can open a DM to
 * it; Meshtastic locks in the first key it hears for a node, so a key that
 * differed between bridges would lock everybody but one out. [pub] may be
 * NULL (it costs a scalar multiplication). */
void mt_node_keys(const char *call, int len, uint8_t priv[32], uint8_t pub[32]);

/* ── Identities ───────────────────────────────────────────────────────── */

/* The node number an XPRS callsign wears on Meshtastic: the first four
 * bytes, big-endian, of sha256("XPRS/node" || the bare callsign), moved off
 * the reserved values (0..3 and the broadcast address). Suffixes are
 * dropped first, so X1QZ3N-7 and X1QZ3N are one node (XPRS.md 3.1.3). */
uint32_t mt_node_of_call(const char *call, int len);

/* `MT` + 8 uppercase hex digits: a Meshtastic node written as an XPRS
 * callsign. Returns the length written (10), or -1 when [cap] is short. */
int  mt_call_of_node(uint32_t node, char *out, int cap);

/* Parse "MTA1B2C3D4" (exactly; matched whole, XPRS.md 3.0.1). */
bool mt_node_of_mtcall(const char *call, int len, uint32_t *node);

/* Meshtastic's own name for a node: "!a1b2c3d4". */
void mt_user_id_of(uint32_t node, char out[10]);

/* Bring a Meshtastic long name into the XPRS `nick` type: 1-16 of
 * [A-Za-z0-9_-], spaces become '-', everything else is dropped. Returns
 * the length (0 when nothing usable survives). */
int  mt_nick_from_name(const char *name, char *out, int cap);

/* ── XPRS frames on the shared channel ────────────────────────────────── */

/* The Data overhead around an XPRS payload is 3 (portnum 0x158) + 1 + the
 * length varint, so one frame carries 255 - 16 - 6 = 233 bytes of XPRS. */
#define MT_XPRS_ONE_FRAME 233
/* A fragment spends three payload bytes on its marker: 0x80 | part (1, 2)
 * and a 16-bit tag. Two fragments carry 2 x 230, more than any packet. */
#define MT_XPRS_FRAG_HDR  3
#define MT_XPRS_FRAG_MAX  (MT_XPRS_ONE_FRAME - MT_XPRS_FRAG_HDR)
#define MT_XPRS_FRAG_TTL_MS 15000u

/* Frames a wire of [len] bytes needs: 1, 2, or 0 when it is not a packet. */
int mt_xprs_frames_for(int len);

/* Bytes on the air for XPRS frame [part] (0-based) of a [len]-byte wire,
 * header included: what the duty ledger charges. */
int mt_xprs_frame_len(int len, int part);

/* Build the frames for one XPRS wire. The header is derived from the wire
 * itself, so any two stations that air the same packet air the same
 * (from, id) and every duplicate filter on the channel agrees:
 *   from      = mt_node_of_call(f:)
 *   id        = first four bytes of sha256 over the signed text (XPRS.md 5)
 *   hop_limit = 3 (Meshtastic's default) minus the via: count, for traffic
 *               somebody waits for (message, sos, receipt ...); 0 for beacons
 *   relay     = [self_node]'s last byte
 * Returns the number of frames written (1 or 2), 0 on a wire that does not
 * parse or has no f:. frames[i] must hold MT_FRAME_MAX bytes. */
int mt_xprs_wrap(const char *wire, int len, uint32_t self_node,
                 uint8_t frames[2][MT_FRAME_MAX], int frame_len[2]);

/* The header a wire would get (for logs and for the fragment's id). */
bool mt_xprs_hdr_of(const char *wire, int len, uint32_t self_node,
                    mt_hdr_t *out);

/* Reassembly of two-part wires. Small, fixed, and per radio. */
#if defined(MT_SMALL) || (defined(ESP_PLATFORM) && !defined(CONFIG_SPIRAM))
#define MT_XPRS_REASM_SLOTS 2
#else
#define MT_XPRS_REASM_SLOTS 3
#endif
typedef struct {
    bool     used;
    uint32_t from;
    uint16_t tag;
    uint32_t t_ms;
    uint8_t  have;          /* bit 0 = part 1, bit 1 = part 2 */
    uint8_t  len[2];
    uint8_t  part[2][MT_XPRS_FRAG_MAX];
} mt_reasm_slot_t;
typedef struct { mt_reasm_slot_t s[MT_XPRS_REASM_SLOTS]; } mt_reasm_t;

/* Is this frame XPRS (our channel, our portnum)? If so, the XPRS bytes it
 * carries are written to [wire] (cap >= 251) and the length returned; a
 * fragment that completes a wire returns the whole wire; a fragment still
 * waiting for its sibling returns 0. -1 means "not an XPRS frame". */
int mt_xprs_unwrap(mt_reasm_t *r, const uint8_t *frame, int len,
                   uint32_t now_ms, char *wire, int cap);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_MT_H */
