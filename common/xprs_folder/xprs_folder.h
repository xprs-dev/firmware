/**
 * @file xprs_folder.h
 * @brief Reading a published folder (XPRS.md 7.7.2, 7.7.3) on a station.
 *
 * A person publishes a folder as a signed `t:file ... kind:folder` naming an
 * XFL1 listing, and each member as a signed `t:file file: size: name:`. A
 * station finds those through its archivers (`cmd:history only:<publisher>
 * kind:file`), and what it needs from them is small: which listing is the
 * folder's now, and, for the firmware case, which member is the newest image
 * for THIS board. Everything here is pure text over caller-owned buffers,
 * host-tested in test_xfolder_host.c; nothing fetches, verifies a signature
 * or writes flash. The caller verifies the packet against the key it pins
 * (xprsid_verify with `fwkey`) BEFORE handing it here.
 *
 * The firmware folder's members are named `<board>-<version>.bin`, e.g.
 * `tdeck-0.5.1.bin`: that name, with `size:` and `file:`, in a packet the
 * publisher signed, is the binding 11.8's approval line makes (board,
 * version, size, digest). A listing line carries the same three facts.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xprs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XFOLDER_NAME_MAX   64
#define XFOLDER_BOARD_MAX  24
#define XFOLDER_VER_MAX    24

/** One member of a folder, as a listing line or a `t:file` describes it. */
typedef struct {
    uint8_t  sha[32];
    uint32_t size;                       /* bytes; 0 = not stated */
    char     name[XFOLDER_NAME_MAX + 1];
} xfolder_entry_t;

/** A firmware image a member name describes: `<board>-<version>.bin`. */
typedef struct {
    char board[XFOLDER_BOARD_MAX + 1];
    char version[XFOLDER_VER_MAX + 1];
} xfolder_image_t;

/* ── references ───────────────────────────────────────────────────────── */

/** Decode a `file:` value or a bare hash -- 43 base64url characters or 64
 *  hex, with or without `.ext` -- into 32 bytes. False when it is neither. */
bool xfolder_ref_sha(const char *ref, int len, uint8_t out[32]);

/** A `qty` of bytes: a bare count, or with k, M or G (and an optional B). */
uint32_t xfolder_qty_bytes(const char *v, int len);

/* ── the listing ──────────────────────────────────────────────────────── */

/** Walk an XFL1 listing: @p cb once per well-formed line, in file order;
 *  return false from it to stop. Returns -1 when the magic is wrong, else
 *  how many lines were handed over. A malformed line is skipped. */
int xfolder_listing_each(const char *text, size_t len,
                         bool (*cb)(const xfolder_entry_t *e, void *ctx),
                         void *ctx);

/* ── descriptions ─────────────────────────────────────────────────────── */

/** The listing a folder description names: true for `t:file ... kind:folder`
 *  with a decodable `file:`. */
bool xfolder_desc_folder(const xprs_t *p, uint8_t listing_sha[32]);

/** A member description (`t:file file: size: name:`, no `kind:folder`, no
 *  `b:`, no `n:`): true when it names a file with a size and a name. */
bool xfolder_desc_member(const xprs_t *p, xfolder_entry_t *out);

/* ── firmware images ──────────────────────────────────────────────────── */

/** `<board>-<version>.bin` -> board and version. False for any other name. */
bool xfolder_image_of(const char *name, xfolder_image_t *out);

/** Dotted numeric versions, `0.5.1` against `0.10.0`; a `-tag` suffix sorts
 *  before the bare version (a beta before its release). <0, 0, >0. */
int xfolder_version_cmp(const char *a, const char *b);

/** Is @p e a newer image for @p board than @p running? */
bool xfolder_newer_for(const xfolder_entry_t *e, const char *board,
                       const char *running, xfolder_image_t *img);

#ifdef __cplusplus
}
#endif
