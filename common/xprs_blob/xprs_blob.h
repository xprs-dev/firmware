/*
 * xprs_blob -- a lean 1:1 BLE GATT bulk-transfer protocol (XBLOB).
 *
 * The norm for moving a BINARY blob (first user: a firmware image) between two
 * XPRS stations over a private GATT connection -- not the broadcast plane, and
 * NOT wrapped in XPRS text messaging. It is BitTorrent-shaped:
 *
 *   1. the SERVER sends a MANIFEST (whole-file sha256, block size, block count)
 *      and a HASHES list -- one truncated sha per block;
 *   2. it then BLASTS every BLOCK as raw bytes, windowed, no per-block ack;
 *   3. the RECEIVER verifies each block against its manifest hash, keeps a
 *      `have` bitmap, and asks for exactly what is still missing or corrupt with
 *      a single NEED bitmap (selective repeat) -- on the server's DONE and, if
 *      the stream stalls, on its own timer;
 *   4. when nothing is missing the receiver hands the whole blob to the app,
 *      which does the real integrity/authentication (the whole-file sha and,
 *      for OTA, the signed approval). XBLOB itself carries no trust: it is a
 *      fast, self-correcting pipe and nothing more.
 *
 * SHAPE. Pure logic, like common/xprs_blemesh: no radio, storage or OS deps.
 * The app binds a transport (one send()), pushes inbound frames with
 * xblob_rx(), resumes a paused blast with xblob_tx_ready(), and ticks it ~10 Hz
 * with xblob_tick(). One frame per ATT PDU; no fragmentation.
 *
 * NOT common/xprs_blemesh. That is MSP (custody + in-order file lane), a
 * byte-for-byte mirror of a Dart peer; it must not change. XBLOB is separate,
 * with its own magic byte, so the two never collide on a link.
 */
#ifndef XPRS_BLOB_H
#define XPRS_BLOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Wire ────────────────────────────────────────────────────────────────
 * All frames little-endian, byte 0 = magic, byte 1 = type. 0x42 ('B') is
 * distinct from XPRS text 't:' (0x74) and MSP's 0x4D, so a receiver tells the
 * three apart on the first byte. */
#define XBLOB_MAGIC        0x42
#define XBLOB_VER          1

#define XBLOB_T_MANIFEST   0x01  /* server->recv: ver,sha32,size,blksz,nblocks,hashlen,siglen,sig */
#define XBLOB_T_BLOCK      0x02  /* server->recv: idx u16, raw payload */
#define XBLOB_T_NEED       0x03  /* recv->server: nblocks u16, bitmap (bit i set = still needed) */
#define XBLOB_T_HASHES     0x04  /* server->recv: start u16, count u8, count*hashlen bytes */
#define XBLOB_T_DONE       0x06  /* server->recv: every requested block has been sent */
#define XBLOB_T_OK         0x07  /* recv->server: complete, all blocks present */
#define XBLOB_T_FAIL       0x08  /* either way: reason u8 */
#define XBLOB_T_BYE        0x09  /* either way: closing */
#define XBLOB_T_START      0x0A  /* recv->server: sha32 -- begin, and only for this image */
#define XBLOB_T_ACK        0x0B  /* recv->server: consumed u32 -- credit, paces the blast */
#define XBLOB_T_READY      0x0C  /* recv->server: manifest+hashes held -- blast the blocks */

/* FAIL reasons. */
#define XBLOB_FAIL_SHA     1     /* MANIFEST sha != the one we were told to accept */
#define XBLOB_FAIL_BIG     2     /* nblocks over XBLOB_MAX_BLOCKS */
#define XBLOB_FAIL_ROUNDS  3     /* too many NEED rounds without converging */
#define XBLOB_FAIL_IO      4     /* block_read/write failed */

/* Limits. A 256 KB image slot at the 240-byte block below is ~1093 blocks. */
#define XBLOB_HASHLEN      4                 /* truncated sha256 per block */
#define XBLOB_MAX_BLOCKS   1200
#define XBLOB_BITMAP_BYTES ((XBLOB_MAX_BLOCKS + 7) / 8)
#define XBLOB_SIG_MAX      64                /* room for the 60-char approval + NUL */

/* Transport send() return codes (match MSP so the shape is familiar). */
#define XBLOB_SEND_OK      0
#define XBLOB_SEND_BUSY    (-2)   /* transport queue full -- retry on xblob_tx_ready */

/* Tunables. */
/* Credit window: the server runs at most this many data frames ahead of
 * the receiver's ACK. BLE notifications are dropped, not backpressured,
 * when the receiver's host (flash writes) lags, so this is what keeps the
 * transfer from shedding 99% of its frames. */
#define XBLOB_WINDOW       24
#define XBLOB_ACK_EVERY    8      /* receiver credits after this many frames */
#define XBLOB_STALL_MS     750    /* no block in this long -> ask for what's missing */
#define XBLOB_MAX_ROUNDS   12     /* NEED passes before giving up (app then falls back) */

/* True when [d] is an XBLOB frame -- the GATT rx demux test. */
static inline bool xblob_is_frame(const uint8_t *d, int len)
{
    return len >= 2 && d[0] == XBLOB_MAGIC;
}
/* The START sha, for a server whose app must look the image up before it can
 * bind a session. Returns true and fills sha[32] when [d] is a START frame. */
static inline bool xblob_is_start(const uint8_t *d, int len, uint8_t sha[32])
{
    if (len < 2 + 32 || d[0] != XBLOB_MAGIC || d[1] != XBLOB_T_START) return false;
    for (int i = 0; i < 32; i++) sha[i] = d[2 + i];
    return true;
}

/* ── Host-supplied operations ────────────────────────────────────────────── */
typedef struct {
    void *ctx;

    /* Queue one frame on the link. XBLOB_SEND_OK, XBLOB_SEND_BUSY, or any other
     * negative = link dead (the session gives up). */
    int  (*send)(void *ctx, const uint8_t *frame, int len);

    /* SERVER: read [cap] bytes of the blob at byte [off] into [dst]. Return
     * bytes read (== cap except the last block), or <=0 on error. */
    int  (*block_read)(void *ctx, uint32_t off, uint8_t *dst, int cap);

    /* RECEIVER: persist one VERIFIED block at byte [off] ([len] bytes). The
     * session only calls this for a block whose hash matched the manifest, so a
     * corrupt or duplicate block never reaches here. Return 0 on success. */
    int  (*block_write)(void *ctx, uint32_t off, const uint8_t *src, int len);

    /* Transfer ended. RECEIVER: ok=1 means every block is present and verified
     * -- the app now does the whole-file sha / approval / install; ok=0 means
     * XBLOB gave up (the app should fall back to its slow lane). SERVER: ok=1
     * when the receiver reported OK. */
    void (*done)(void *ctx, bool ok);
} xblob_ops_t;

/* ── Session ─────────────────────────────────────────────────────────────
 * Public so it can be allocated statically. Do not poke the fields except
 * `sig85` (the approval the receiver lifts from the MANIFEST on done). */
typedef struct {
    const xblob_ops_t *ops;
    uint8_t  role;         /* 0 = receiver, 1 = server */
    uint8_t  state;
    uint8_t  sha[32];
    uint32_t size;
    uint16_t blksz;
    uint16_t nblocks;
    uint8_t  hashlen;
    char     sig85[XBLOB_SIG_MAX];

    /* receiver */
    uint8_t  have[XBLOB_BITMAP_BYTES];      /* block present + verified */
    uint8_t  hbits[XBLOB_BITMAP_BYTES];     /* manifest hash for block received */
    uint8_t  bhash[XBLOB_MAX_BLOCKS * XBLOB_HASHLEN];
    uint16_t got;
    uint16_t hashes_got;
    uint32_t last_rx_ms;
    uint8_t  rounds;
    bool     manifest_ok;
    uint32_t consumed;        /* recv: data frames taken, cumulative (for ACK) */
    uint32_t last_ack_sent;

    /* server */
    uint8_t  send_bm[XBLOB_BITMAP_BYTES];   /* blocks still to send this pass */
    uint32_t cursor;
    uint16_t hashes_sent;
    uint32_t frames_sent;     /* server: data frames put on the link this pass */
    uint32_t ack;             /* server: receiver's latest consumed count */
    bool     manifest_sent;
    bool     done_sent;
} xblob_t;

/* ── API ─────────────────────────────────────────────────────────────────── */

/* RECEIVER: begin. Sends START(sha) so the server knows which image to serve,
 * then waits for the MANIFEST it must match against [sha]. [size] is the
 * expected image length (the app got both from the signed cmd:update). */
void xblob_recv_start(xblob_t *s, const xblob_ops_t *ops,
                      const uint8_t sha[32], uint32_t size, uint32_t now_ms);

/* SERVER: begin serving [size] bytes of the image whose whole-file sha is
 * [sha], block size [blksz] (<= max payload), approval [sig85] (may be ""),
 * to a receiver that has just START-ed. Sends MANIFEST + HASHES and blasts. */
void xblob_server_start(xblob_t *s, const xblob_ops_t *ops,
                        const uint8_t sha[32], uint32_t size,
                        uint16_t blksz, const char *sig85, uint32_t now_ms);

/* Feed one inbound frame. */
void xblob_rx(xblob_t *s, const uint8_t *d, int len, uint32_t now_ms);

/* The transport can take more: resume a blast paused on XBLOB_SEND_BUSY. */
void xblob_tx_ready(xblob_t *s);

/* Call ~10 Hz: drives the receiver's stall->NEED timer. */
void xblob_tick(xblob_t *s, uint32_t now_ms);

/* True once the receiver has every block verified (app may read sig85). */
bool xblob_complete(const xblob_t *s);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_BLOB_H */
