/**
 * @file xasset.h
 * @brief Read-only blobs that live in flash instead of in the binary.
 *
 * A 320x240 RGB565 splash is 153,600 bytes. Compiled in as a C array that
 * is 153 KB of .rodata, which is more than the entire legacy multiboard
 * image has left in its slot -- so "we want a splash logo" had been
 * reading as a space problem when it is really a *placement* problem.
 *
 * The T-Deck's 16 MB ends at 0x1000000 and its coredump partition ended at
 * 0xF90000, so 448 KB were sitting unallocated. This reads a small archive
 * out of a partition carved from that tail. Nothing above it moved, which
 * is the point: adding these partitions does not erase the 11 MB FAT.
 *
 * Raw partition, NOT a filesystem, and that is the whole design:
 *
 *   - esp_partition_read() works before any filesystem is mounted, so the
 *     splash can be on the glass before WiFi, the bearers or LVGL exist.
 *   - Mounting FATFS costs 4-8 KB of heap on a board that has been taken
 *     off the air by less (docs/esp32.md), and every open FILE holds a
 *     4 KB sector cache. An asset reader must not be worth that.
 *   - There is no path, so there is no path traversal. A name is looked up
 *     in a table; anything not in the table does not exist.
 *
 * Deliberately allocation-free, for the same reason xprs_health is: the
 * moment you most want a splash or an icon is boot, which is also the
 * moment the heap is least predictable. The directory is NOT cached -- it
 * is re-read from flash 40 bytes at a time on each lookup, so this whole
 * component costs about a dozen bytes of .bss and no heap at all. Lookups
 * happen a handful of times per boot; flash reads are cheap and RAM is not.
 *
 * ON-WIRE FORMAT (little-endian, written by tools/mkassets.py):
 *
 *   offset 0   "XASS"
 *   offset 4   u16 version (1)
 *   offset 6   u16 count
 *   offset 8   count x 36-byte entries:
 *                char name[24]   NUL-padded, not necessarily NUL-terminated
 *                u32  off        payload offset from the start of the partition
 *                u32  len        payload length in bytes
 *                u16  kind       XASSET_KIND_*
 *                u16  reserved
 *   then       the payloads
 *
 * For XASSET_KIND_RGB565 the payload begins u16 w, u16 h, then w*h*2 bytes
 * of pixel data stored in EXACTLY the byte order the panel driver wants to
 * hand to SPI -- see the note on byte order in tools/mkassets.py. This
 * component never interprets pixels; it copies bytes.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XASSET_NAME_MAX      24
#define XASSET_KIND_RGB565   1   /* u16 w, u16 h, then pixels */
#define XASSET_KIND_RAW      2   /* opaque bytes */

typedef struct {
    uint32_t off;      /* payload offset from the start of the partition */
    uint32_t len;      /* payload length, pixel header included */
    uint16_t kind;
    uint16_t w, h;     /* both 0 unless kind == XASSET_KIND_RGB565 */
} xasset_t;

/**
 * Find the `assets` partition and validate its header.
 *
 * Returns ESP_ERR_NOT_FOUND when the board has no such partition and
 * ESP_ERR_INVALID_STATE when the partition is present but unwritten or
 * corrupt. BOTH are ordinary: a station with no assets is still a station,
 * so callers log and carry on rather than failing boot.
 */
esp_err_t xasset_open(void);

/** True if the archive is open and holds at least one entry. */
bool xasset_ready(void);

/** Look `name` up. False if absent -- which is not an error anywhere. */
bool xasset_find(const char *name, xasset_t *out);

/**
 * Copy `len` bytes from `off` bytes into the asset's payload.
 *
 * `off` is relative to the payload, so for an image the pixels start at 4.
 * Reads are clipped to the asset. Returns the byte count, or -1.
 */
int xasset_read(const xasset_t *a, size_t off, void *buf, size_t len);

#ifdef __cplusplus
}
#endif
