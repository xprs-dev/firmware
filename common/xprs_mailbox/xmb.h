/*
 * Store and forward for a station without a filesystem: mail, the mailbox
 * declarations that name this station, and one small state blob, kept as
 * 256-byte records in a handful of raw flash pages.
 *
 * WHAT IS KEPT (XPRS.md 12.11). Mail is a directed packet (d:) for somebody
 * else. Class 3 when the recipient's verified t:mailbox hold: names this
 * station (9.12), class 2 otherwise ("custody", 6.3); the caller decides
 * whether class 2 is worth carrying at all. When full, the oldest of the
 * lowest class goes, and new mail never pushes out a better class: it is
 * refused instead. Nothing outlives its until:.
 *
 * DELIVERY (12.8.1, 12.8.2). On hearing X directly, xmb_due() hands over a
 * page of X's mail, newest first, skipping what was just tried. A copy that
 * was aired backs off (30 s, 2 min, 10 min); the rest of a backlog is due
 * at once, so the next sighting carries the next page. A verified receipt
 * releases the copy, and the release is written to flash before it counts:
 * a holder that forgets a release re-airs mail at every sighting.
 *
 * THE FLASH. Each record: magic, length, kind and class, a status byte that
 * is cleared in place when the record dies (the word is written a second
 * time, which nRF52 flash allows), a CRC and a sequence number, then the
 * payload. Records are appended at a head that walks the pages; one page is
 * always kept erased, and refilling it moves the few live records of the
 * emptiest page to the head before that page is erased. Capacity is
 * (pages - 2) x 16 live records.
 *
 * Platform-free; the store is four callbacks. Host test: test_xmb_host.sh.
 */
#ifndef XPRS_XMB_H
#define XPRS_XMB_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XMB_REC          256
#define XMB_PER_PAGE     16
#define XMB_PAGE         (XMB_REC * XMB_PER_PAGE)
#define XMB_PAGES_MAX    8
#define XMB_HDR          8
#define XMB_PAYLOAD_MAX  (XMB_REC - XMB_HDR)      /* 248 */
#define XMB_PAGE_OUT     3                         /* mail per delivery page */
#define XMB_PER_TO       10
#define XMB_PER_FROM     20
#define XMB_DECLS        16

enum { XMB_K_MAIL = 1, XMB_K_DECL = 2, XMB_K_STATE = 3 };
enum { XMB_FREE = 0, XMB_LIVE = 1, XMB_DEAD = 2 };

typedef struct {
    int    pages;
    /* Byte offsets from the start of the area. write: word-aligned offset,
     * length a multiple of 4, may only clear bits. */
    bool (*read)(void *ctx, uint32_t off, void *buf, int len);
    bool (*write)(void *ctx, uint32_t off, const void *buf, int len);
    bool (*erase)(void *ctx, int page);
    void  *ctx;
} xmb_store_t;

typedef struct {
    uint8_t  state, kind, cls, tries;
    uint16_t seq;
    uint32_t to_h, from_h, id_h;
    uint32_t until;      /* epoch seconds, 0 = none */
    uint32_t next_ms;    /* not re-aired before (mail) */
} xmb_slot_t;

typedef struct {
    uint32_t held, refused, evicted, expired, released, aired, relocated, erased, bad;
} xmb_stats_t;

typedef struct {
    xmb_store_t st;
    int         nslots, head, live;
    uint16_t    seq;
    xmb_slot_t  s[XMB_PAGES_MAX * XMB_PER_PAGE];
    xmb_stats_t stats;
} xmb_t;

/* The callsign hash every *_h field holds (case-insensitive FNV-1a). */
uint32_t xmb_hash(const char *s, int n);

/* Read the area and rebuild the index. False if the store is unusable. */
bool xmb_open(xmb_t *m, const xmb_store_t *st);
/* Live records the area can hold. */
int  xmb_capacity(const xmb_t *m);

/* Mail: is this packet mail for somebody else, and of which class?
 * 3 = its recipient declared us (a live declaration), 2 = custody, 0 = not
 * mail (no d:, for us, or not a kind that is delivered). [contact]: the
 * caller knows the sender or the recipient (a class 2 admission is only
 * offered for those). [now] epoch seconds, 0 if unknown. */
int  xmb_class(const xmb_t *m, const char *wire, int len, const char *self, bool contact,
               uint32_t now);
/* Keep it. Returns the slot, or -1 (refused: full of better mail, a cap, a
 * duplicate, too long). */
int  xmb_hold(xmb_t *m, const char *wire, int len, int cls, uint32_t now);
/* A declaration (t:mailbox) the caller has VERIFIED. Kept if its hold:
 * names [self]; a signed remove:mailbox r:<id> kills the one it names. */
bool xmb_decl(xmb_t *m, const char *wire, int len, const char *self, uint32_t now);
/* The receipt for [id] from the mail's recipient [from]: releases it. */
bool xmb_receipt(xmb_t *m, const char *id, const char *from);

/* A page of mail for [to] due now: up to [max] slots into [out], newest
 * first. Returns how many. */
int  xmb_due(xmb_t *m, const char *to, uint32_t now_ms, uint32_t now, int *out, int max);
/* Copy a record's payload out. Returns its length, 0 if not live. */
int  xmb_read(const xmb_t *m, int slot, char *out, int cap);
/* The caller aired [slot]: back off before trying it again. */
void xmb_aired(xmb_t *m, int slot, uint32_t now_ms);
/* Mail held for [to] (NULL: for everybody). */
int  xmb_count(const xmb_t *m, const char *to);

/* The one state blob (the caller's own format). */
bool xmb_state_put(xmb_t *m, const void *blob, int len);
int  xmb_state_get(const xmb_t *m, void *blob, int cap);

/* Drop what outlived its until: (call now and then). */
void xmb_expire(xmb_t *m, uint32_t now);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_XMB_H */
