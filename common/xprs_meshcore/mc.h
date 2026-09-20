/**
 * @file mc.h
 * @brief MeshCore on the XPRS LoRa radio: the frame, the payloads, the keys.
 *
 * The third LoRa mode (XPRS.md 14.8, docs/meshtastic.md "LoRa modes").
 * MeshCore's EU channel is 869.525 MHz, SF11, 250 kHz, CR 4/5, preamble 16,
 * which is Meshtastic's LongFast with ANOTHER SYNC WORD (0x12 against
 * 0x2B): the two networks sit on one frequency and are deaf to each other,
 * and a station picks one.
 *
 * Written from MeshCore's published format (docs.meshcore.io packet_format
 * and payloads, and the field names in meshcore-dev/MeshCore's Packet.h,
 * Identity.cpp and Utils.cpp), not from its code: MeshCore is MIT but this
 * is a reimplementation, as the Meshtastic side is, so the same C builds on
 * the ESP32 and on the nRF52 and is checked on the host.
 *
 *   [header][transport codes (4, optional)][path length][path][payload]
 *
 * The header is one byte, `0bVVPPPPRR`: the route type in bits 0-1, the
 * payload type in bits 2-5, the version in bits 6-7. The path length byte
 * carries the hop count in bits 0-5 and the size of each hop's hash, minus
 * one, in bits 6-7. A payload is at most 184 bytes.
 *
 * WHAT XPRS PUTS ON THAT CHANNEL is a RAW_CUSTOM payload, flood-routed, so
 * a MeshCore repeater carries it without understanding it, exactly as a
 * private portnum does on Meshtastic (mc_xprs.c).
 *
 * The crypto is not ours either: AES-128-ECB with the shared secret's first
 * sixteen bytes (or the channel key), a two-byte truncation of
 * HMAC-SHA256 over the ciphertext as the MAC, and Ed25519 over a node's
 * identity for its advert. All of it comes from xprs_loracrypto.
 */
#ifndef XPRS_MESHCORE_H
#define XPRS_MESHCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── The channel ──────────────────────────────────────────────────────── */

/*
 * MEASURED ON A STOCK MESHCORE NODE, 2026-09-20, not taken from the docs:
 * a Heltec V3 running the published repeater build v1.17.1 answers
 * `get radio` with `869.6179809,62.5,8,5`, and its source sets the
 * preamble to 16 and the sync word to RADIOLIB_SX126X_SYNC_WORD_PRIVATE
 * (0x12) in CustomSX1262.h. This is NOT Meshtastic's LongFast with another
 * sync word, which is what this file assumed before the bench said
 * otherwise: it is a narrower, faster channel.
 */
#define MC_SF          8
#define MC_BW_HZ       62500u
#define MC_CR          1            /* 4/5, as the SX1262 counts it */
#define MC_PREAMBLE    16           /* CustomSX1262.h passes 16 to begin() */
#define MC_SYNC        0x12         /* RADIOLIB_SX126X_SYNC_WORD_PRIVATE */

/** The frame the radio carries, and the payload inside it. */
#define MC_FRAME_MAX   255
#define MC_PAYLOAD_MAX 184
#define MC_PATH_MAX    64

/* ── The header ───────────────────────────────────────────────────────── */

enum {
    MC_ROUTE_TRANSPORT_FLOOD = 0,
    MC_ROUTE_FLOOD           = 1,
    MC_ROUTE_DIRECT          = 2,
    MC_ROUTE_TRANSPORT_DIRECT = 3,
};

enum {
    MC_PT_REQ        = 0x00,
    MC_PT_RESPONSE   = 0x01,
    MC_PT_TXT_MSG    = 0x02,
    MC_PT_ACK        = 0x03,
    MC_PT_ADVERT     = 0x04,
    MC_PT_GRP_TXT    = 0x05,
    MC_PT_GRP_DATA   = 0x06,
    MC_PT_ANON_REQ   = 0x07,
    MC_PT_PATH       = 0x08,
    MC_PT_TRACE      = 0x09,
    MC_PT_MULTIPART  = 0x0A,
    MC_PT_CONTROL    = 0x0B,
    MC_PT_RAW_CUSTOM = 0x0F,
};

typedef struct {
    uint8_t  route;              /* MC_ROUTE_* */
    uint8_t  type;               /* MC_PT_* */
    uint8_t  version;            /* 0 = v1, which is what is on the air */
    uint8_t  hops;               /* how many hashes the path holds */
    uint8_t  hash_size;          /* 1 to 3 bytes each */
    uint8_t  path[MC_PATH_MAX];
    uint16_t transport[2];       /* only with a transport route type */
    int      payload_len;
    const uint8_t *payload;      /* into the frame the caller passed */
} mc_pkt_t;

/** Read a frame. False when it is not one (too short, a path that does not
 *  fit, a payload past MC_PAYLOAD_MAX). */
bool mc_parse(const uint8_t *frame, int len, mc_pkt_t *out);

/** Write one. Returns its length, or 0 when it does not fit. */
int mc_build(const mc_pkt_t *p, uint8_t *out, int cap);

/** Append [hash] (hash_size bytes) to the path of a parsed frame and write
 *  it back out: what a repeater does before it re-airs a flood packet.
 *  Returns the new length, or 0 when the path is full. */
int mc_path_append(const mc_pkt_t *p, const uint8_t *hash, uint8_t *out,
                   int cap);

/** The identifier a receiver dedups on: MeshCore hashes the payload and the
 *  type, so two nodes airing one packet agree. Four bytes of that hash. */
uint32_t mc_packet_hash(const mc_pkt_t *p);

/* ── Identities ───────────────────────────────────────────────────────── */

/** A node is addressed by the FIRST BYTE of its Ed25519 public key. */
#define mc_node_hash(pub) ((pub)[0])

/** The callsign a MeshCore node wears in XPRS: `MC` and the first four
 *  bytes of its public key, uppercase hex (XPRS.md 3.2). [out] needs 11. */
void mc_call_of_pub(const uint8_t pub[32], char out[11]);

/** The Ed25519 identity an XPRS callsign wears on MeshCore, derived from
 *  the callsign so that every bridge presents the same one. */
void mc_node_keys(const char *call, int len, uint8_t sk[64], uint8_t pub[32]);

/* ── The public channel ───────────────────────────────────────────────── */

/** MeshCore's well-known public channel key, and the byte that names it on
 *  the air (the first byte of its SHA-256). */
extern const uint8_t mc_public_key[16];
uint8_t mc_channel_hash(const uint8_t key[16]);

/* ── Messages ─────────────────────────────────────────────────────────── */

/*
 * THE MOST TEXT ONE MESSAGE CAN CARRY, and the arithmetic behind it,
 * because getting it wrong is silent: the cipher pads to whole 16-byte
 * blocks, so a payload that fits before padding may not fit after it, and
 * the build then returns 0 and the message is simply never sent.
 *
 *   184 payload - 4 (a direct message's dest, src and MAC) = 180
 *   180 rounded DOWN to whole blocks                       = 176
 *   176 - 5 (the timestamp and the type byte)              = 171
 *
 * A channel message spends one byte fewer on its header and the same 176
 * after rounding, so the figure is the same for both; for that one the
 * limit is on `<sender name>: <text>` together.
 */
#define MC_TEXT_MAX 171

/** What rides inside a text payload once it is open. */
typedef struct {
    uint32_t timestamp;
    uint8_t  txt_type;           /* 0 plain, 1 CLI command, 2 signed */
    uint8_t  attempt;
    char     text[MC_PAYLOAD_MAX];
} mc_text_t;

/**
 * A group (public channel) text: `channel hash, MAC, ciphertext`, the body
 * being `<sender name>: <message>`, which MeshCore does not authenticate.
 * Returns the payload length or 0.
 */
int mc_grp_txt_build(const uint8_t key[16], uint32_t timestamp,
                     const char *sender, const char *text, uint8_t *out,
                     int cap);

/** Open one. False when the channel hash or the MAC says it is not ours. */
bool mc_grp_txt_open(const uint8_t key[16], const uint8_t *payload, int len,
                     mc_text_t *out, char *sender, int sender_cap);

/**
 * A direct message: `dest hash, src hash, MAC, ciphertext` under the secret
 * the two identities share (xlc_ed25519_key_exchange).
 */
int mc_dm_build(const uint8_t secret[32], uint8_t dest_hash, uint8_t src_hash,
                uint32_t timestamp, uint8_t attempt, const char *text,
                uint8_t *out, int cap);

/** Open one addressed to us. False when the MAC does not hold. */
bool mc_dm_open(const uint8_t secret[32], const uint8_t *payload, int len,
                mc_text_t *out);

/*
 * A PATH return, which is how a MeshCore client answers the FIRST direct
 * message it gets from a node it has no route to: it sends back the path
 * it learned, and it puts the ACKNOWLEDGEMENT INSIDE THAT PACKET rather
 * than sending one of its own (Mesh.cpp, createPathReturn). A bridge that
 * only listens for type 0x03 therefore never hears the ack, retries the
 * message three times and tells its user nobody answered. Measured against
 * a real client on 2026-09-20.
 *
 *   dest(1) src(1) MAC(2) cipher{ path length(1), path, [type(1), extra] }
 *
 * The cipher and the MAC are a direct message's, so it opens the same way.
 */
typedef struct {
    uint8_t hops, hash_size;
    uint8_t path[MC_PATH_MAX];
    uint8_t extra_type;          /* 0xFF when it carried nothing */
    uint8_t extra[32];
    int     extra_len;
} mc_path_t;

bool mc_path_open(const uint8_t secret[32], const uint8_t *payload, int len,
                  mc_path_t *out);

/** Build one. [extra] may be NULL; with MC_PT_ACK and four bytes it is the
 *  acknowledgement a first message gets. Returns the payload length or 0. */
int mc_path_build(const uint8_t secret[32], uint8_t dest_hash, uint8_t src_hash,
                  const uint8_t *path, int hops, int hash_size,
                  uint8_t extra_type, const uint8_t *extra, int extra_len,
                  uint8_t *out, int cap);

/**
 * The four bytes MeshCore acknowledges a message with.
 *
 * It is the front of sha256 over the FIRST FIVE BYTES OF THE MESSAGE'S OWN
 * PLAINTEXT (the timestamp and the type/attempt byte), then the text
 * without its terminator, then the SENDER's public key
 * (BaseChatMesh::composeMsgPacket). Leaving the fifth byte out gives a
 * checksum that matches nothing, and the sender then reports that nobody
 * answered: found against a real client on 2026-09-20.
 */
uint32_t mc_ack_checksum(uint32_t timestamp, uint8_t flags, const char *text,
                         const uint8_t sender_pub[32]);

/* ── Adverts ──────────────────────────────────────────────────────────── */

/*
 * The app data of an advert is a flags byte and then OPTIONAL BLOCKS in a
 * fixed order, which is the part the published format does not spell out
 * and the bench does (MeshCore's AdvertDataHelpers.cpp, and a stock
 * repeater's advert read here on 2026-09-20): the low nibble of the flags
 * is the node type, and each of the high bits adds a block BEFORE the
 * name. Reading the name straight after the flags gives a node with no
 * name at all, which is what this firmware did until it was tried.
 *
 *   flags(1) [lat(4) lon(4)] [feat1(2)] [feat2(2)] [name, the rest]
 */
enum {
    MC_ADV_CHAT     = 0x01,   /* the low nibble is the type */
    MC_ADV_REPEATER = 0x02,
    MC_ADV_ROOM     = 0x03,
    MC_ADV_SENSOR   = 0x04,
    MC_ADV_TYPE     = 0x0F,
    MC_ADV_LATLON   = 0x10,   /* two int32, degrees times a million */
    MC_ADV_FEAT1    = 0x20,
    MC_ADV_FEAT2    = 0x40,
    MC_ADV_HAS_NAME = 0x80,
};

typedef struct {
    uint8_t  pub[32];
    uint32_t timestamp;
    uint8_t  flags;
    int32_t  lat, lon;        /* degrees x 1e6, 0 when the advert has none */
    uint16_t feat1, feat2;
    char     name[32];
} mc_advert_t;

/** Build a signed advert payload. Returns its length or 0. */
int mc_advert_build(const uint8_t sk[64], const uint8_t pub[32],
                    uint32_t timestamp, uint8_t flags, const char *name,
                    uint8_t *out, int cap);

/** Read one, signature checked. False when it does not verify. */
bool mc_advert_open(const uint8_t *payload, int len, mc_advert_t *out);

/* ── XPRS frames on MeshCore's channel ────────────────────────────────── */

/* A RAW_CUSTOM payload is 184 bytes, and ours spends one on a marker, so a
 * wire up to 183 bytes is one frame. Anything longer goes as two, each
 * spending three bytes (0x80 | part, and a 16-bit tag that pairs them). */
#define MC_XPRS_ONE_FRAME 183
#define MC_XPRS_FRAG_HDR  3
#define MC_XPRS_FRAG_MAX  (MC_PAYLOAD_MAX - MC_XPRS_FRAG_HDR)
#define MC_XPRS_FRAG_TTL_MS 15000u

/* Frames a wire of [len] bytes needs: 1, 2, or 0 when it is not a packet. */
int mc_xprs_frames_for(int len);

/* Bytes on the air for frame [part] of a [len]-byte wire, header included:
 * what the duty ledger charges. */
int mc_xprs_frame_len(int len, int part);

/* Build the frames for one XPRS wire: a flood-routed RAW_CUSTOM with an
 * empty path, which a MeshCore repeater carries without reading. The tag
 * that pairs two fragments is derived from the wire, so two stations airing
 * one packet air the same bytes. Returns 1, 2, or 0. */
int mc_xprs_wrap(const char *wire, int len, uint8_t frames[2][MC_FRAME_MAX],
                 int frame_len[2]);

/* Reassembly of two-part wires, small and fixed, one per radio. */
#if defined(MC_SMALL) || (defined(ESP_PLATFORM) && !defined(CONFIG_SPIRAM))
#define MC_XPRS_REASM_SLOTS 2
#else
#define MC_XPRS_REASM_SLOTS 3
#endif
typedef struct {
    bool     used;
    uint16_t tag;
    uint32_t t_ms;
    uint8_t  have;              /* bit 0 = part 1, bit 1 = part 2 */
    uint8_t  len[2];
    uint8_t  part[2][MC_XPRS_FRAG_MAX];
} mc_reasm_slot_t;
typedef struct { mc_reasm_slot_t s[MC_XPRS_REASM_SLOTS]; } mc_reasm_t;

/* Is this frame XPRS (a RAW_CUSTOM of ours)? The wire is written to [wire]
 * and its length returned; a fragment still waiting returns 0; -1 means it
 * is somebody else's frame, which is what the repeater and the bridge get. */
int mc_xprs_unwrap(mc_reasm_t *r, const uint8_t *frame, int len,
                   uint32_t now_ms, char *wire, int cap);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_MESHCORE_H */
