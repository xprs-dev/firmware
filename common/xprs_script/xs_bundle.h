/**
 * @file xs_bundle.h
 * @brief The signed container a station will run scripts out of.
 *
 * A script is executable code that arrives over the air, so the rule is the
 * same one xprs_ota states for firmware and for the same reason: **the station
 * accepts an artefact, never a source**, and it verifies a signature before it
 * hands the VM a single byte.
 *
 * That is not belt-and-braces. Wrench's own integrity check is a CRC
 * (WR_ERR_bad_bytecode_CRC), which is trivially forged, and the VM does not
 * bounds-check bytecode -- upstream issue #54 is an instruction fetch from
 * 0x00000080. Malformed bytecode is a memory-safety problem, not a script
 * error. Unsigned bytecode reaching the VM is remote code execution on every
 * station that hears it.
 *
 * WIRE FORMAT (little-endian; produced by tools/mkbundle.py)
 *
 *   0   char magic[4]      "XSCB"
 *   4   u16  format        1
 *   6   u16  nmod          modules in this bundle, <= XSB_MODS_MAX
 *   8   char id[16]        which bundle this is; two publishers can ship two
 *  24   char version[24]   bundles without either replacing the other's
 *  48   char sig[64]       60 base85 chars, NUL padded
 * 112   u32  signed_len    bytes from XSB_BODY_OFF to the end
 * 116   u32  reserved
 * 120   module table       nmod x { char name[24]; u32 off; u32 len;
 *                                   u16 tick_ms; u16 flags }
 *       char types[8][16]  the on_packet filter, here so the bearer task can
 *                          match it with no allocation and no lock
 *       payloads           bytecode blobs, `off` relative to the bundle start
 *
 * The signature covers `signed_len` bytes starting at XSB_BODY_OFF, bound to
 * the board and the bundle id and version by the signed line:
 *
 *     xprsscr1 <board> <id> <version> <signed_len> <sha256 hex>
 *
 * so a bundle built for one board cannot install on another, and last
 * version's approval cannot be replayed against this one. Same shape as
 * xprsfw1 (xprs_ota.c), deliberately -- one idea, learned once.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XSB_MAGIC        "XSCB"
#define XSB_FORMAT       1
#define XSB_ID_MAX       16
#define XSB_VER_MAX      24
#define XSB_NAME_MAX     24
#define XSB_SIG_MAX      64
#define XSB_MODS_MAX      8
#define XSB_TYPES_MAX     8
#define XSB_TYPE_LEN     16

#define XSB_BODY_OFF    120
#define XSB_MOD_ENTRY    36

typedef struct {
    char     name[XSB_NAME_MAX + 1];
    uint32_t off;        /* from the start of the bundle */
    uint32_t len;
    uint16_t tick_ms;    /* 0 = this module wants no tick */
    uint16_t flags;
} xsb_module_t;

typedef struct {
    char         id[XSB_ID_MAX + 1];
    char         version[XSB_VER_MAX + 1];
    char         sig[XSB_SIG_MAX + 1];
    uint32_t     signed_len;
    uint16_t     nmod;
    xsb_module_t mod[XSB_MODS_MAX];
    char         types[XSB_TYPES_MAX][XSB_TYPE_LEN + 1];
} xsb_t;

/**
 * Parse and range-check a bundle held in `buf`. STRUCTURE ONLY -- this says
 * nothing about whether the bundle is trusted; see xsb_signed_line().
 *
 * Every offset is checked against `len` here so that nothing downstream has to
 * remember to: a truncated push or a half-erased partition must not become an
 * out-of-range read. False on anything that does not add up.
 */
bool xsb_parse(const uint8_t *buf, size_t len, xsb_t *out);

/**
 * Build the exact line the signature is taken over, so the device verifier and
 * the host signer cannot drift. Returns the length, or -1 if it does not fit.
 *
 * `sha_hex` is the lowercase hex SHA-256 of the `signed_len` bytes starting at
 * XSB_BODY_OFF.
 */
int xsb_signed_line(char *out, size_t cap, const char *board,
                    const xsb_t *b, const char *sha_hex);

/* Provided by xs_sha256_idf.c on the target (mbedTLS); the host test harness
 * links its own, so xs_bundle.c itself stays free of ESP-IDF. Same arrangement
 * as xprs_codec. */
void xsb_sha256(const uint8_t *in, size_t len, uint8_t out[32]);

/**
 * Is this bundle signed, by the holder of `pub`, FOR THIS BOARD?
 *
 * Call after xsb_parse() and BEFORE handing any byte of it to the VM. A
 * forgery must cost nothing but the read that discovered it.
 *
 * Verifies the signature over `xprsscr1 <board> <id> <version> <len> <sha>`,
 * so the same bytes signed for one board or one version do not verify for
 * another -- a bundle cannot be moved between boards, and an old approval
 * cannot be replayed against a new bundle.
 *
 * `pub` is the publisher's x-only key: config `scriptkey`, falling back to
 * `fwkey`. With no key configured nothing verifies, which is the point --
 * a station that has not been told whom to trust runs nobody's code.
 */
bool xsb_verify(const uint8_t *buf, size_t len, const xsb_t *b,
                const char *board, const uint8_t pub[32]);

/** True if this bundle declares an interest in XPRS packets of `type`. */
bool xsb_wants_type(const xsb_t *b, const char *type);

#ifdef __cplusplus
}
#endif
