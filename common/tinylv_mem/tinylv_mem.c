/**
 * @file tinylv_mem.c
 * @brief PSRAM-preferring allocator for LVGL (see tinylv_mem.h).
 */

#include "tinylv_mem.h"
#include "esp_heap_caps.h"

/* SPIRAM first, and internal only if there is no PSRAM on this board (or it
 * is full). heap_caps_malloc with MALLOC_CAP_SPIRAM returns NULL rather than
 * falling back on its own, so the fallback is explicit. */
#define TLV_CAPS_PREFERRED  (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define TLV_CAPS_FALLBACK   (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static size_t s_used;
static size_t s_peak;

static void account(void *p, size_t old_sz)
{
    /* heap_caps_get_allocated_size is the true cost including the header. */
    s_used -= old_sz;
    if (p) s_used += heap_caps_get_allocated_size(p);
    if (s_used > s_peak) s_peak = s_used;
}

void *tinylv_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, TLV_CAPS_PREFERRED);
    if (!p) p = heap_caps_malloc(size, TLV_CAPS_FALLBACK);
    account(p, 0);
    return p;
}

void tinylv_free(void *p)
{
    if (!p) return;
    s_used -= heap_caps_get_allocated_size(p);
    heap_caps_free(p);
}

void *tinylv_realloc(void *p, size_t size)
{
    size_t old = p ? heap_caps_get_allocated_size(p) : 0;
    void *q = heap_caps_realloc(p, size, TLV_CAPS_PREFERRED);
    if (!q && size) q = heap_caps_realloc(p, size, TLV_CAPS_FALLBACK);
    /* On failure p is still live and still costs what it cost. */
    if (!q && size) return NULL;
    account(q, old);
    return q;
}

size_t tinylv_used(void) { return s_used; }
size_t tinylv_peak(void) { return s_peak; }
