/*
 * xwc_file -- a file by its content reference (XPRS.md 7.7), for the
 * station API's /api/xprs/file/<ref> (spec/API-HTTP.md).
 *
 * A reference is the SHA-256 of the bytes as 43 base64url characters (what a
 * sender writes) or 64 hex, a dot, and 1 to 8 lowercase letters or digits
 * naming the type. The type is advisory: the store keys a file on its digest
 * alone, so two references that differ only in the type are one file.
 *
 * Plain C, no storage and no IDF: the httpd glue (xprs_api/xapi_wc.c) owns
 * the files and the streaming; test_xwc_file_host.c runs this on the host.
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

#ifdef __cplusplus
}
#endif
#endif /* XWC_FILE_H */
