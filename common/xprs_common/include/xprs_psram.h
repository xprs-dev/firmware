/**
 * @file xprs_psram.h
 * @brief XPRS_PSRAM_BSS -- put a large static buffer in PSRAM, not DRAM.
 *
 * Internal DRAM is the binding constraint on every XPRS board (docs/esp32.md,
 * "PSRAM does not mean more memory"); flash is not. A static array costs
 * internal `.bss` whether it is used or not, and on the T-Deck our own
 * components held ~62 KB of it -- chat rings, the log ring, table-row
 * scratch, statistics blobs.
 *
 * ESP-IDF can relocate a static into the external RAM BSS segment, which
 * costs PSRAM instead. This is a one-word annotation with no change to how
 * the variable is used:
 *
 *     static XPRS_PSRAM_BSS xst_chat_t s_chat[XST_CHAT_MAX];
 *
 * WHAT MAY NOT GO HERE. PSRAM is reached through the cache, so a buffer in it
 * is unreachable whenever the cache is off -- during a flash write (NVS, OTA)
 * and inside any IRAM ISR. So:
 *
 *   - nothing touched from an interrupt handler;
 *   - nothing touched by DMA (PSRAM is not DMA-capable on the ESP32-S3 for
 *     our peripherals -- the LVGL draw buffer and the SPI TX buffers must
 *     stay internal);
 *   - nothing read or written while flash is being written.
 *
 * Everything annotated today is task-context, CPU-only application state.
 *
 * The attribute is inert unless CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY
 * is set, and expands to nothing off-target, so annotated code still builds
 * for the host tests and for boards with no PSRAM -- there the buffer simply
 * stays where it always was.
 */
#ifndef XPRS_PSRAM_H
#define XPRS_PSRAM_H

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#define XPRS_PSRAM_BSS EXT_RAM_BSS_ATTR
#else
#define XPRS_PSRAM_BSS
#endif

/**
 * xprs_big_calloc -- the allocation twin of XPRS_PSRAM_BSS, for buffers that
 * cannot be statics at all. On the xwifi boards the RF sample banks cap how
 * far .bss may grow, so the biggest buffers moved from annotated statics to
 * boot-time allocations; this keeps their placement rule: PSRAM when the
 * board has it (what the annotation did), internal heap when it does not.
 * Same restrictions as the annotation above -- task-context, CPU-only state.
 */
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static inline void *xprs_big_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p;
}
#else
#include <stdlib.h>
static inline void *xprs_big_calloc(size_t n, size_t size) { return calloc(n, size); }
#endif

#endif /* XPRS_PSRAM_H */
