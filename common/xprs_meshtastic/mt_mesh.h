/**
 * @file mt_mesh.h
 * @brief The Meshtastic repeater and the XPRS <-> Meshtastic bridge.
 *
 * One instance per LoRa radio. The LoRa bearer hands it every frame that is
 * not XPRS (mt_mesh_on_frame); the station hands it every XPRS packet it
 * hears or sends on any bearer (mt_mesh_on_xprs); the bearer task ticks it.
 * It hands back raw frames to air (ops.air, which does CAD and charges the
 * duty ledger) and translated XPRS wires to deliver (ops.deliver).
 *
 * What it does, in docs/lora.md's words:
 *
 *  - REPEATER. Meshtastic's managed flood: a frame not heard before, with
 *    hops left, not ours and not addressed to one of our nodes, is re-aired
 *    after an SNR-weighted wait with hop_limit - 1 and our relay byte, and
 *    the re-air is cancelled if somebody else relays it first. Frames are
 *    relayed whether or not we can decode them, as every Meshtastic router
 *    does.
 *  - BRIDGE IN. LongFast text becomes t:message f:MT<node>, a DM to one of
 *    our virtual nodes becomes t:message d:<callsign>, a tapback becomes a
 *    t:reaction, a NodeInfo becomes an unsigned t:identity with the name as
 *    nick. Every translation carries zmid:<from><id> and via:<this station>
 *    and is dated to the minute, so two bridges that hear the same frame
 *    produce the same packet and the same section 5 identifier.
 *  - BRIDGE OUT. t:message d:MT<node> becomes a DM from the sender's virtual
 *    node, acked into a signed gateway receipt; a broadcast t:message is
 *    mirrored to LongFast under a per-hour cap; add:like becomes a tapback.
 *    Every outbound frame's (from, id) is derived from the XPRS packet, so
 *    all bridges air the same frame and cancel each other on hearing it.
 *
 * Platform-free like xprsbearer.c: the host harness drives it with a fake
 * clock and a fake radio (test_mt_mesh_host.c).
 */
#ifndef XPRS_MT_MESH_H
#define XPRS_MT_MESH_H

#include <stdbool.h>
#include <stdint.h>

#include "mt.h"
#include "xprs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { MT_PRIO_RELAY = 0, MT_PRIO_OWN = 1, MT_PRIO_URGENT = 2 };

/* Where an XPRS packet handed to mt_mesh_on_xprs came from. */
enum {
    MT_XPRS_HEARD = 0,  /* heard on some bearer, somebody else's */
    MT_XPRS_OWN   = 1,  /* this station's own, or handed to it to air (a
                           phone on BLE, the local UI): the one case where
                           a refusal is said out loud */
};

typedef struct {
    /* Air one frame now: CAD, the duty ledger, the radio. false = not now
     * (busy channel, spent budget); the frame stays queued and is retried. */
    bool     (*air)(void *ctx, const uint8_t *frame, int len, int prio);
    uint32_t (*now_ms)(void);
    uint32_t (*random)(void);
    /* A translated XPRS wire for every bearer except LoRa, and the local
     * UI. [sign]: sign it as this station (gateway receipts); the
     * translations of other people's words are never signed. */
    void     (*deliver)(void *ctx, const char *wire, int len, bool sign);
    /* The time field for a packet composed now, e.g. "ts:2026-09-19_12:04:00"
     * (seconds zeroed when [to_minute]), or an epoch: when there is no clock.
     * Returns its length, 0 when the station can say nothing. */
    int      (*stamp)(void *ctx, char *out, int cap, bool to_minute);
    /* UTC now in seconds, or 0 when the station has no clock (optional). */
    uint32_t (*utc_now)(void *ctx);
    /* A verified nick for an XPRS callsign, when the station holds one. */
    bool     (*nick_of)(void *ctx, const char *call, char *out, int cap);
    /* One log line (optional). */
    void     (*log)(void *ctx, const char *line);
    /* The Meshtastic keys learned, kept across a restart (optional): the
     * firmware answers a NodeInfo request from one node once in 12 hours,
     * so a bridge that forgot a key at every boot could not DM that node
     * again for half a day. [buf] is an array of mt_keyrec_t. */
    int      (*keys_load)(void *ctx, void *buf, int cap);
    void     (*keys_save)(void *ctx, const void *buf, int len);
    /* The XPRS callsigns this bridge has announced on Meshtastic, kept
     * across a restart (optional). Only those: they are the nodes a
     * Meshtastic user can see and DM, and a node number cannot be turned
     * back into a callsign, so a bridge that forgot them would flood such a
     * DM past itself until that station happened to speak again. Written
     * when one is announced for the first time, not for every callsign
     * heard. [buf] is an array of char[MT_CALL_LEN], bare, NUL-padded. */
    int      (*vnodes_load)(void *ctx, void *buf, int cap);
    void     (*vnodes_save)(void *ctx, const void *buf, int len);
    void      *ctx;
} mt_mesh_ops_t;

typedef struct {
    bool     repeat;          /* relay Meshtastic frames */
    bool     bridge;          /* translate both ways */
    uint16_t bcast_per_hour;  /* broadcasts mirrored to LongFast, per hour */
    uint16_t nodeinfo_min;    /* how often a node of ours re-announces */
} mt_mesh_cfg_t;

/* Two sizes. A board with PSRAM keeps the bridge there and can afford the
 * roomy one; a board without (the Heltec V3, measured 2026-09-19) had
 * 7.8 KB of internal heap left with it, largest block 2.6 KB, so the
 * tables shrink there -- a node list of twelve, two DMs waiting for an
 * ack -- which is what a station on a pole needs anyway. */
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
#if defined(MT_SMALL) || (defined(ESP_PLATFORM) && !defined(CONFIG_SPIRAM))
#define MT_KEYS       12
#define MT_TXQ        4
#define MT_NODES      12
#define MT_VNODES     12
#define MT_SEEN       40
#define MT_IDMAP      12
#define MT_PENDING    2
#else
#define MT_KEYS       32
#define MT_TXQ        6
#define MT_NODES      24
#define MT_VNODES     24
#define MT_SEEN       64
#define MT_IDMAP      16
#define MT_PENDING    4
#endif
#define MT_SEEN_MS    600000u       /* ten minutes, as the firmware's history */
#define MT_RELAY_STALE_MS 60000u
#define MT_OWN_STALE_MS   600000u
#define MT_DM_RETRY_MS    30000u
/* How long a DM waits behind the NodeInfo that carries its key: the frame
 * at SF11 plus the NodeInfo's own contention delay, with room to spare. */
#ifndef MT_AFTER_NODEINFO_MS
#define MT_AFTER_NODEINFO_MS 6000u
#endif
#define MT_DM_TRIES       3
#define MT_DM_PARK_MS     (24u * 3600u * 1000u)
/* How old an XPRS packet may be and still cross. Echo carousels and history
 * replays keep old packets moving on the XPRS side; after a restart the
 * bridge has forgotten what it sent, and Meshtastic's own duplicate filter
 * hides a repeat from its user but not from the channel's airtime. */
#define MT_BCAST_FRESH_S  1800u
#define MT_DM_FRESH_S     (6u * 3600u)

typedef struct {
    bool     used;
    uint8_t  prio;
    uint8_t  len;
    uint32_t from, id;           /* the frame's key, for the cancel */
    uint32_t due_ms, queued_ms;
    uint8_t  frame[MT_FRAME_MAX];
} mt_txq_t;

typedef struct {
    uint32_t num;
    uint32_t heard_ms;
    uint32_t identity_ms;        /* last t:identity delivered for it */
    char     long_name[MT_LONG_NAME_MAX];
    char     short_name[MT_SHORT_NAME_MAX];
    bool     xprs;               /* a virtual node some bridge announced */
    int8_t   snr;
} mt_node_t;

/* A Meshtastic node's X25519 key, learned from its NodeInfo: what a DM to
 * it is sealed to and a DM from it opened with. Kept apart from the node
 * list, which turns over with who was heard last, and saved (ops.keys_save). */
typedef struct {
    uint32_t num;
    uint8_t  pk[32];
} mt_keyrec_t;

#define MT_CALL_LEN 12

typedef struct {
    uint32_t num;
    uint32_t seen_ms;            /* last XPRS packet from it */
    uint32_t announced_ms;       /* last NodeInfo we aired for it; 0 = never */
    bool     announce_due;       /* the tick announces it (curve work) */
    bool     keep;               /* announced on Meshtastic: saved, evicted last */
    char     call[MT_CALL_LEN];  /* bare */
} mt_vnode_t;

typedef struct {
    uint32_t from, id, t_ms;
} mt_seen_t;

typedef struct {
    uint32_t mesh_id;
    char     xid[XPRS_ID_LEN];
} mt_idmap_t;

/* A DM out, until its ack. It keeps the PLAINTEXT Data: a DM is sealed to
 * the recipient's key (mt_pki), and each airing is sealed afresh, so a DM
 * can wait for a key it does not have yet. */
typedef struct {
    bool     used;
    bool     aired;              /* we put it on the air at least once */
    bool     asked_key;          /* NodeInfo requested from the recipient */
    bool     due;                /* the tick seals and queues it */
    bool     rekeyed;            /* re-sent once after a PKI_UNKNOWN_PUBKEY */
    uint8_t  tries;
    uint8_t  dlen;
    uint32_t to, from, id;
    uint32_t sent_ms, created_ms;
    char     sender[12];         /* the XPRS author, for the receipt */
    char     xid[XPRS_ID_LEN];
    uint8_t  data[MT_FRAME_MAX - MT_HDR_LEN - MT_PKI_OVERHEAD];
} mt_pending_t;

typedef struct {
    uint32_t rx_frames, rx_decoded, rx_dupes;
    uint32_t relayed, relay_cancelled, relay_skipped;
    uint32_t text_in, text_out, dm_acked, dm_rekeyed, receipts;
    uint32_t dm_not_here;      /* DMs to a node this bridge never heard, left */
    uint32_t nodeinfo_out, bcast_capped, dropped;
    uint32_t pki_in, pki_fail, key_asks;
} mt_mesh_stats_t;

typedef struct {
    mt_mesh_ops_t   ops;
    mt_mesh_cfg_t   cfg;
    char            call[12];    /* this station, bare */
    char            nick[20];
    uint32_t        self;        /* mt_node_of_call(call) */
    uint8_t         lf_hash;
    uint32_t        boot_ms;
    mt_txq_t        q[MT_TXQ];
    mt_node_t       nodes[MT_NODES];
    mt_vnode_t      vnodes[MT_VNODES];
    mt_seen_t       seen[MT_SEEN];
    int             seen_pos;
    mt_idmap_t      idmap[MT_IDMAP];
    int             idmap_pos;
    mt_pending_t    pend[MT_PENDING];
    mt_keyrec_t     keys[MT_KEYS];
    int             keys_pos;            /* next slot to reuse */
    bool            keys_dirty;
    uint32_t        keys_saved_ms;
    bool            vnodes_dirty;
    uint32_t        vnodes_saved_ms;
    uint8_t         bcast_min[60];   /* mirrored broadcasts per minute */
    uint32_t        bcast_head_ms;
    uint8_t         bcast_head;
    mt_mesh_stats_t st;
    /* Scratch, owned by the instance: the bearer task's stack is 5 KB and
     * shared with every other bearer (docs/esp32.md, the stack floors). */
    xprs_t          xp;
    uint8_t         buf[MT_FRAME_MAX];
    uint8_t         frame[MT_FRAME_MAX];
    char            wire[XPRS_MAX_WIRE + 64];
    char            text[XPRS_MAX_WIRE + 1];
    char            logline[120];
} mt_mesh_t;

void mt_mesh_init(mt_mesh_t *m, const mt_mesh_ops_t *ops,
                  const mt_mesh_cfg_t *cfg, const char *own_call,
                  const char *own_nick);

/* A frame heard on the radio that mt_xprs_unwrap() said is not XPRS. */
void mt_mesh_on_frame(mt_mesh_t *m, const uint8_t *frame, int len, int rssi,
                      int snr);

/* An XPRS frame heard on the radio: only its (from, id), so the flood
 * filter knows it and never relays it as Meshtastic traffic. */
void mt_mesh_note_xprs_frame(mt_mesh_t *m, const uint8_t *frame, int len);

/* An XPRS packet this station heard or originated, on any bearer. Cheap
 * and callable from any task (under the caller's lock): the curve work a
 * DM or a NodeInfo costs (X25519, about 1.3 KB of stack) is left for
 * mt_mesh_tick, on the bearer task. */
void mt_mesh_on_xprs(mt_mesh_t *m, const char *wire, int len, int origin);

void mt_mesh_tick(mt_mesh_t *m, uint32_t now_ms);

/* Change the station's nick (announced at the next NodeInfo). */
void mt_mesh_set_nick(mt_mesh_t *m, const char *nick);

/* Meshtastic nodes heard, for a status page. Returns how many; [i] 0..n-1. */
int mt_mesh_node(const mt_mesh_t *m, int i, const mt_node_t **out);

/* For tests and the status page: slot time in ms at LongFast. */
uint32_t mt_mesh_slot_ms(void);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_MT_MESH_H */
