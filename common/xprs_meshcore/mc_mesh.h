/**
 * @file mc_mesh.h
 * @brief The MeshCore repeater and the XPRS <-> MeshCore bridge.
 *
 * The same shape as mt_mesh.h, because the RULES are the same
 * (docs/meshtastic.md, "The rules we follow"): one instance per LoRa radio,
 * the bearer hands it every frame that is not XPRS, the station hands it
 * every XPRS packet it hears or sends, the bearer task ticks it, and it
 * hands back frames to air and translated wires to deliver.
 *
 * What is different is MeshCore's own arithmetic (mc.h):
 *
 *  - REPEATER. MeshCore's flood: a packet whose hash we have not seen is
 *    re-aired with OUR hash appended to its path, after a wait derived from
 *    its airtime, and the re-air is dropped when somebody else airs it
 *    first. A DIRECT packet is carried only by the node whose hash is at
 *    the front of the path, which takes itself off it first.
 *  - BRIDGE IN. A public-channel message becomes t:message f:MC<key>, a
 *    direct message to one of our virtual nodes becomes t:message
 *    d:<callsign> and is acknowledged, a signed advert becomes an unsigned
 *    t:identity with the node's name as its nick.
 *  - BRIDGE OUT. An XPRS broadcast is mirrored into the public channel
 *    under a per-hour cap; a t:message d:MC<key> becomes a direct message
 *    from the sender's virtual node, whose ACK becomes the signed gateway
 *    receipt (XPRS.md 9.7.1).
 *
 * TWO THINGS MESHCORE DOES NOT GIVE US, and this file does not pretend
 * otherwise:
 *
 *  1. A channel message is not signed and carries only a NAME. A name is
 *     not an address, so a channel message crosses only when that name
 *     matches exactly one node whose advert this station has heard, and it
 *     crosses under that node's address. Anything else is counted
 *     (st.grp_unnamed) and dropped: inventing an address from a name would
 *     put words in the mouth of a node that may not exist.
 *  2. A node is ADDRESSED by the first byte of its key, and a direct
 *     message names no more than that, so opening one means trying the
 *     contacts whose first byte matches. We only try at all when the
 *     destination byte is one of ours.
 */
#ifndef XPRS_MC_MESH_H
#define XPRS_MC_MESH_H

#include <stdbool.h>
#include <stdint.h>

#include "mc.h"
#include "xprs.h"

#ifdef __cplusplus
extern "C" {
#endif

enum { MC_PRIO_RELAY = 0, MC_PRIO_OWN = 1, MC_PRIO_URGENT = 2 };

/* Where an XPRS packet handed to mc_mesh_on_xprs came from. */
enum {
    MC_XPRS_HEARD = 0,  /* heard on some bearer, somebody else's */
    MC_XPRS_OWN   = 1,  /* this station's own, or handed to it to air */
};

typedef struct {
    bool     (*air)(void *ctx, const uint8_t *frame, int len, int prio);
    uint32_t (*now_ms)(void);
    uint32_t (*random)(void);
    void     (*deliver)(void *ctx, const char *wire, int len, bool sign);
    int      (*stamp)(void *ctx, char *out, int cap, bool to_minute);
    uint32_t (*utc_now)(void *ctx);
    bool     (*nick_of)(void *ctx, const char *call, char *out, int cap);
    void     (*log)(void *ctx, const char *line);
    /* The MeshCore contacts learned from adverts, kept across a restart
     * (optional). [buf] is an array of mc_keyrec_t. */
    int      (*keys_load)(void *ctx, void *buf, int cap);
    void     (*keys_save)(void *ctx, const void *buf, int len);
    /* The XPRS callsigns this bridge has advertised on MeshCore, kept
     * across a restart (optional), and for the same reason as on
     * Meshtastic: they are the nodes a MeshCore user can see and write to.
     * [buf] is an array of char[MC_CALL_LEN], bare, NUL-padded. */
    int      (*vnodes_load)(void *ctx, void *buf, int cap);
    void     (*vnodes_save)(void *ctx, const void *buf, int len);
    void      *ctx;
} mc_mesh_ops_t;

typedef struct {
    bool     repeat;          /* relay MeshCore packets */
    bool     bridge;          /* translate both ways */
    uint16_t bcast_per_hour;  /* broadcasts mirrored to the channel, per hour */
    uint16_t advert_min;      /* how often a node of ours re-advertises */
} mc_mesh_cfg_t;

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif
#if defined(MC_SMALL) || (defined(ESP_PLATFORM) && !defined(CONFIG_SPIRAM))
#define MC_KEYS       12
#define MC_TXQ        4
#define MC_NODES      12
#define MC_VNODES     12
#define MC_SEEN       40
#define MC_PENDING    2
#else
#define MC_KEYS       24
#define MC_TXQ        6
#define MC_NODES      24
#define MC_VNODES     24
#define MC_SEEN       64
#define MC_PENDING    4
#endif
/* Packets waiting for the worker. Two is enough for a channel this slow:
 * a frame takes a second and a half on the air, and the worker runs every
 * fifty milliseconds. */
#define MC_INBOX      2
/* XPRS packets already translated, by their section 5 identifier. A packet
 * is heard again and again -- its own echo on the LAN, a digipeat over
 * Bluetooth, a history replay -- and without this ring each repeat spends
 * an hour's broadcast allowance before the duplicate filter downstream
 * catches it. Measured on the bench, 2026-09-20: two messages crossed and
 * the next seven were "capped". */
#define MC_XIDS       12
#define MC_SEEN_MS        600000u
#define MC_RELAY_STALE_MS 60000u
#define MC_OWN_STALE_MS   600000u
#define MC_DM_RETRY_MS    30000u
#define MC_DM_TRIES       3
#define MC_DM_PARK_MS     (24u * 3600u * 1000u)
#define MC_BCAST_FRESH_S  1800u
#define MC_DM_FRESH_S     (6u * 3600u)
/* How far a flood packet travels through us. MeshCore's own limit is the
 * path itself (it stops when there is no room for another hash); this is
 * the same manners as Meshtastic's hop limit, on a channel we share. */
#define MC_RELAY_MAX_HOPS 8
/* How long a direct message waits behind the advert that carries its
 * sender's key. A MeshCore client can only open a message from a contact
 * it knows, and it learns a contact from an advert, so a message that
 * overtakes its own advert is a message nobody can read -- the same
 * lesson Meshtastic taught with NodeInfo (MT_AFTER_NODEINFO_MS), measured
 * again here against a real client on 2026-09-20. */
#define MC_AFTER_ADVERT_MS 8000u

#define MC_CALL_LEN 12

typedef struct {
    bool     used;
    uint8_t  prio;
    uint8_t  len;
    uint32_t hash;               /* mc_packet_hash, for the cancel */
    uint32_t due_ms, queued_ms;
    uint8_t  frame[MC_FRAME_MAX];
} mc_txq_t;

/* A MeshCore node we have heard an advert from: the only frame that carries
 * a key and a name together, and the only one that is signed. */
typedef struct {
    uint8_t  pub[32];
    uint32_t heard_ms;
    uint32_t identity_ms;        /* last t:identity delivered for it */
    uint32_t advert_ts;          /* the advert's own timestamp */
    char     name[32];
    bool     xprs;               /* another bridge's virtual node */
    int8_t   snr;
} mc_node_t;

/* Kept across a restart: the key is the address, so forgetting it is
 * forgetting the contact. */
typedef struct {
    uint8_t  pub[32];
} mc_keyrec_t;

typedef struct {
    uint32_t seen_ms;            /* last XPRS packet from it */
    uint32_t advert_ms;          /* last advert we aired for it; 0 = never */
    bool     advert_due;         /* mc_mesh_work advertises it */
    bool     keep;               /* advertised: saved, evicted last */
    bool     keyed;              /* pub is derived; until then it has none */
    uint8_t  hash;               /* the first byte of its derived key */
    uint8_t  pub[32];            /* derived ONCE, by mc_mesh_work */
    char     call[MC_CALL_LEN];  /* bare */
} mc_vnode_t;

typedef struct {
    uint32_t hash, t_ms;
} mc_seen_t;

typedef struct {
    char xid[XPRS_ID_LEN];
} mc_xid_t;

/*
 * A packet waiting for the curve arithmetic that would open it.
 *
 * NONE OF THE CURVE WORK HAPPENS ON THE TASK THAT HEARD THE PACKET. An
 * Ed25519 verification is about 3.3 KB of stack (measured with
 * -fstack-usage on the target compiler) and the bearer task has roughly two
 * to spare, so verifying an advert where it arrives is a stack overflow and
 * a reboot loop (docs/esp32.md, "Task stacks are heap, and these are the
 * measured floors"). The receive path copies the payload here, and
 * mc_mesh_work() -- which the station runs on a task of its own, on core 1
 * -- does the arithmetic. The same rule the index already follows for the
 * SD card: the receive task decides WHETHER to answer, another task
 * answers.
 */
typedef struct {
    bool     used;
    uint8_t  type;
    uint8_t  len;
    uint32_t hash;
    int8_t   snr;
    uint8_t  payload[MC_PAYLOAD_MAX];
} mc_inbox_t;

/* A direct message out, until its ACK. The plaintext is kept, because each
 * airing is sealed afresh. */
typedef struct {
    bool     used;
    bool     aired;
    bool     due;
    uint8_t  tries;
    uint8_t  to_pub[32];         /* the contact, learned from its advert */
    uint32_t ack;                /* the checksum its ACK will carry */
    uint32_t hash;               /* the packet hash of the last airing */
    uint32_t ts;                 /* the timestamp inside it */
    uint32_t sent_ms, created_ms;
    char     from_call[MC_CALL_LEN];
    char     sender[MC_CALL_LEN];/* the XPRS author, for the receipt */
    char     xid[XPRS_ID_LEN];
    char     text[MC_TEXT_MAX + 1];  /* what fits in one MeshCore payload */
} mc_pending_t;

typedef struct {
    uint32_t rx_frames, rx_dupes, rx_opened;
    uint32_t relayed, relay_cancelled, relay_skipped;
    uint32_t text_in, text_out, dm_acked, receipts;
    uint32_t dm_not_here;      /* DMs to a node this bridge never heard */
    uint32_t grp_unnamed;      /* channel messages whose sender we cannot name */
    uint32_t adverts_in, adverts_out, bcast_capped, dropped;
    uint32_t inbox_full;       /* packets dropped for want of a slot to work on */
    uint32_t paths_in;         /* PATH returns opened (the ack rides in them) */
} mc_mesh_stats_t;

typedef struct {
    mc_mesh_ops_t   ops;
    mc_mesh_cfg_t   cfg;
    char            call[MC_CALL_LEN];   /* this station, bare */
    char            nick[20];
    bool            self_keyed;          /* the worker has derived ours */
    uint8_t         self_hash;
    uint8_t         self_pub3[3];        /* our hash at every hash size */
    uint8_t         chan_hash;
    uint32_t        boot_ms;
    mc_txq_t        q[MC_TXQ];
    mc_node_t       nodes[MC_NODES];
    mc_vnode_t      vnodes[MC_VNODES];
    mc_seen_t       seen[MC_SEEN];
    int             seen_pos;
    mc_xid_t        xids[MC_XIDS];
    int             xids_pos;
    mc_pending_t    pend[MC_PENDING];
    mc_inbox_t      inbox[MC_INBOX];
    mc_keyrec_t     keys[MC_KEYS];
    int             keys_pos;
    bool            keys_dirty;
    uint32_t        keys_saved_ms;
    bool            vnodes_dirty;
    uint32_t        vnodes_saved_ms;
    uint8_t         bcast_min[60];
    uint32_t        bcast_head_ms;
    uint8_t         bcast_head;
    mc_mesh_stats_t st;
    /* Scratch, owned by the instance: the bearer task's stack is shared
     * with every other bearer (docs/esp32.md, the stack floors). */
    xprs_t          xp;
    uint8_t         payload[MC_PAYLOAD_MAX];
    uint8_t         frame[MC_FRAME_MAX];
    char            wire[XPRS_MAX_WIRE + 64];
    char            text[XPRS_MAX_WIRE + 1];
    char            logline[120];
} mc_mesh_t;

void mc_mesh_init(mc_mesh_t *m, const mc_mesh_ops_t *ops,
                  const mc_mesh_cfg_t *cfg, const char *own_call,
                  const char *own_nick);

/* A frame heard on the radio that mc_xprs_unwrap() said is not XPRS. */
void mc_mesh_on_frame(mc_mesh_t *m, const uint8_t *frame, int len, int rssi,
                      int snr);

/* An XPRS frame heard on the radio: only its hash, so the flood filter
 * knows it and never relays it as MeshCore traffic. */
void mc_mesh_note_xprs_frame(mc_mesh_t *m, const uint8_t *frame, int len);

/* An XPRS packet this station heard or originated, on any bearer. Cheap and
 * callable from any task: the curve work (Ed25519, ECDH) is left for
 * mc_mesh_tick, on the bearer task. */
void mc_mesh_on_xprs(mc_mesh_t *m, const char *wire, int len, int origin);

/* The cheap half: what is due goes on the air, what is stale is dropped.
 * Runs on the bearer task, does no curve arithmetic, and may be called as
 * often as the bearer ticks. */
void mc_mesh_tick(mc_mesh_t *m, uint32_t now_ms);

/*
 * The expensive half: signatures, key exchange, sealing, and the keys a
 * virtual node is derived from. THE STATION MUST RUN THIS ON A TASK WITH A
 * STACK OF ITS OWN (6 KB is what the bearer provides), on core 1, and
 * never on a receive path. Call it a few times a second; it returns at
 * once when there is nothing to do.
 */
void mc_mesh_work(mc_mesh_t *m, uint32_t now_ms);

/* Change the station's nick (announced at the next advert). */
void mc_mesh_set_nick(mc_mesh_t *m, const char *nick);

/* MeshCore nodes heard, for a status page. Returns how many; [i] 0..n-1. */
int mc_mesh_node(const mc_mesh_t *m, int i, const mc_node_t **out);

/* For tests and the status page: one slot at this modulation, in ms. */
uint32_t mc_mesh_slot_ms(void);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_MC_MESH_H */
