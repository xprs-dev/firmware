/**
 * @file tinylv_mem.h
 * @brief LVGL's heap, moved out of internal DRAM into PSRAM.
 *
 * LVGL's default allocator is a TLSF pool declared as a plain array, so
 * `CONFIG_LV_MEM_SIZE_KILOBYTES` kilobytes of internal DRAM are spent whether
 * the UI uses them or not. On the T-Deck that array measured 50,249 B of
 * `.bss` in lv_mem.c.o -- the single largest static claim on internal RAM in
 * the image, and internal RAM is the constraint on this board (see
 * docs/esp32.md, "PSRAM does not mean more memory"). Measured on X3R8XX with
 * the pool still internal, the station reached the hotspot with 3,492 bytes
 * of internal heap left and a largest block of 1,600 -- small enough that a
 * 752-byte softAP beacon buffer is a coin toss.
 *
 * So: build LVGL with LV_MEM_CUSTOM and point its three allocator macros
 * here. Everything LVGL keeps in that pool -- objects, styles, the
 * intermediate lv_mem_buf buffers -- is touched by the CPU only and never by
 * DMA, so PSRAM is a correct home for it. The draw buffer is NOT allocated
 * here; xprs_ui asks for that separately with MALLOC_CAP_DMA, and it must
 * stay internal.
 *
 * Wired in models/<board>/firmware/CMakeLists.txt, which -D's
 * LV_MEM_CUSTOM_INCLUDE / _ALLOC / _FREE / _REALLOC at the LVGL component.
 * Those are all #ifndef-guarded in lv_conf_internal.h, so a command-line
 * definition wins without patching the managed component.
 *
 * On a board with no PSRAM the capability flags fall back to internal, which
 * is exactly where the pool would have been anyway -- so this component is
 * safe to link everywhere, and only pays off where there is PSRAM.
 */
#ifndef TINYLV_MEM_H
#define TINYLV_MEM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void *tinylv_malloc(size_t size);
void  tinylv_free(void *p);
void *tinylv_realloc(void *p, size_t size);

/** Bytes currently handed out, and the high-water mark. For the UI's own
 *  diagnostics: LVGL's lv_mem_monitor() reports nothing under LV_MEM_CUSTOM. */
size_t tinylv_used(void);
size_t tinylv_peak(void);

#ifdef __cplusplus
}
#endif

#endif /* TINYLV_MEM_H */
