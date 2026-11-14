/*
 * xwc_file -- a file by its content reference (XPRS.md 7.7), for the
 * station API's /api/xprs/file/<ref> (spec/API-HTTP.md).
 *
 * A reference is the SHA-256 of the bytes as 43 base64url characters (what a
 * sender writes) or 64 hex, a dot, and 1 to 8 lowercase letters or digits
 * naming the type. The type is advisory: the store keys a file on its digest
 * alone, so two references that differ only in the type are one file.
 *
 * Plain C, stdio and no IDF: the httpd glue (xprs_api/xapi_wc.c) owns the
 * sockets and feeds the bytes; the store itself (where a file goes, what
 * makes room, which part a Range asks for) is here, so
 * test_xwc_file_host.c runs it on the host against a directory in /tmp.
 *
 * The store's layout, under its directory:
 *   <hex>                 a visitor's deposit (XPRS.md 12.11): capped at
 *                         visitor_max, the oldest evicted to make room
 *   .thumbnails/<hex>     a visitor's picture preview (`?preview=1`, 7.7.7),
 *                         evicted the same way
 *   own/<hex>             the station's own (its administrator's and
 *   own/.thumbnails/<hex> moderators'): no cap but the volume's free space,
 *                         never evicted
 * Staff files go to own/ only where the store may hold them (a card).
 */
#ifndef XWC_FILE_H
#define XWC_FILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XWCF_EXT_MAX 8

/** Read [ref] into its digest and type. False when it is not a reference. */
bool xwcf_parse(const char *ref, uint8_t sha[32], char ext[XWCF_EXT_MAX + 1]);

/** The digest as 64 lowercase hex characters: the file's name in the store. */
void xwcf_hex(const uint8_t sha[32], char out[65]);

/** The Content-Type a type is served with; unknown types are octets. */
const char *xwcf_type(const char *ext);

/* ---- the store -------------------------------------------------------- */
typedef struct {
    const char *dir;            /* the store's directory, no trailing slash */
    uint64_t    visitor_max;    /* the largest file a stranger stores (file.max) */
    uint64_t    budget;         /* what strangers' files hold together */
    uint64_t    reserve;        /* left free on the volume by a stranger's file */
    bool        own;            /* staff files kept uncapped in own/ (a card) */
    /* The volume's free and total bytes; false when it cannot say. */
    bool      (*space)(uint64_t *free_b, uint64_t *total_b);
} xwcf_store_t;

/* The volume left free by a staff file: 1 MB or 1 %, the larger. The
 * emulated C3 (XPRS_QEMU) stores on its 832 KB flash volume, where 1 MB is
 * more than there is: 64 KB there. */
#ifndef XWCF_OWN_RESERVE_MIN
#ifdef XPRS_QEMU
#define XWCF_OWN_RESERVE_MIN (64u * 1024u)
#else
#define XWCF_OWN_RESERVE_MIN (1024u * 1024u)
#endif
#endif

/* An upload under way: begin, add every piece of the body, end. */
typedef struct {
    void    *f;                 /* FILE *, NULL when the file is already held */
    void    *h;                 /* the running SHA-256 */
    uint8_t  sha[32];
    uint64_t got;
    char     path[256], part[256], pin[256];
} xwcf_put_t;

/** Start storing [length] bytes said to hash to [sha]. [staff]: the
 *  uploader proved the station's administrator or a moderator (xwc
 *  xwc_is_uploader). 0 to go on, else the HTTP status with [why] set; a
 *  refusal holds nothing, so there is no end to call. */
int  xwcf_put_begin(const xwcf_store_t *s, xwcf_put_t *p, const uint8_t sha[32],
                    uint64_t length, bool staff, bool preview, const char **why);
/** One piece of the body, in order: hashed, and written. False when the
 *  write failed (then end with complete=false). */
bool xwcf_put_add(xwcf_put_t *p, const void *buf, size_t n);
/** Done: 200 when [complete] and the bytes hash to the reference, the file
 *  then filed; else the part is deleted and the status says why. */
int  xwcf_put_end(xwcf_put_t *p, bool complete, const char **why);

/** Make room for [need] more bytes of a stranger's: the oldest of their
 *  files go (never own/) until the budget and the reserve hold. */
bool xwcf_room(const xwcf_store_t *s, uint64_t need);

/** The held file for [sha], open for reading, wherever the store keeps it;
 *  NULL when it is not held. [size] is its length. */
void *xwcf_open(const xwcf_store_t *s, const uint8_t sha[32], uint64_t *size);

/* ---- the rooms files were posted in (XPRS.md 11.2) ---------------------
 * A file's reach is the reach of the words that carried it: a picture posted
 * in a hosted room is served to whoever may read that room, decided when it
 * is asked for (a room made private later closes its old pictures too). The
 * store keeps `.acl` beside the files: one 28-byte record per file, the
 * digest's first 16 bytes and the room's callsign. A file never posted in a
 * room has no record and is public. */
#define XWCF_ACL_CALL 12
/** Bind [sha] to [room]. The newest post wins (11.2) unless [only_new]
 *  (an upload naming its room: it never re-binds a file already posted).
 *  False when the record could not be written (the table is full of held
 *  files, or the volume is). */
bool xwcf_acl_bind(const xwcf_store_t *s, const uint8_t sha[32], const char *room, bool only_new);
/** The room [sha] was posted in, into [room]; false when none (public). */
bool xwcf_acl_room(const xwcf_store_t *s, const uint8_t sha[32], char room[XWCF_ACL_CALL]);
/** Every `file:` reference in [wire] (the field, and tokens in the text),
 *  handed to [each]. Returns how many. */
int  xwcf_refs(const char *wire, int len, void (*each)(const uint8_t sha[32], void *ctx), void *ctx);

/** A `Range:` header against a file of [size] bytes (RFC 9110 14.1.2):
 *   0  serve it whole (no header, or one this store does not take: several
 *      ranges, another unit, nonsense -- a 200 is a valid answer to those)
 *   1  serve bytes [*a, *b], both inclusive: 206
 *  -1  nothing of it is there: 416 */
int  xwcf_range(const char *hdr, uint64_t size, uint64_t *a, uint64_t *b);

#ifdef __cplusplus
}
#endif
#endif /* XWC_FILE_H */
