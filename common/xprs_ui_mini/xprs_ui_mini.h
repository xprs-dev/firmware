/*
 * xprs_ui_mini -- the XPRS station UI for TINY screens (the T-Dongle's
 * 160x80 ST7735 is the reference). Three views, meant to rotate hands-off
 * on a board whose one button is barely reachable:
 *
 *   DEVICES  who is in reach, one line each: CALL bearer ~distance
 *   STATS    three bar charts: devices heard / packets rx / packets tx
 *   CHAT     the last human messages passing through this station
 *
 * Same contract as xprs_ui (the big-screen sibling): the board supplies
 * its size and a flush callback; one task owns every call in here. The
 * FRAMEDUMP serial screenshot speaks the same wire format, so the same
 * host decoder (tools/scripts/framedump.py) reads both.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

enum {
    XUM_VIEW_DEVICES = 0,
    XUM_VIEW_STATS,
    XUM_VIEW_CHAT,
    XUM_VIEW_COUNT
};

#define XUM_DEV_ROWS     5
#define XUM_CHAT_ROWS    4
#define XUM_STATS_POINTS 24

typedef void (*xum_flush_fn)(int x1, int y1, int x2, int y2,
                             const uint16_t *px, void *ctx);

/* Once at boot, from the task that will own the UI. Partial draw buffer
 * (w x 20 rows), DMA-capable when it can be. */
esp_err_t xum_init(int width, int height, xum_flush_fn flush, void *ctx);

/* Pump LVGL (tick + timer handler + pending framedump). Call every loop. */
void xum_update(void);

/* Switch the visible view. The caller owns the rotation cadence. */
void xum_show(int view);
int  xum_view(void);

/* Pause glyph in the top bar while the operator holds a view. */
void xum_set_held(bool held);

typedef struct {
    char call[10];
    char bearer[7];
    int  dist_m;                 /* -1 = unknown on this link */
    int  age_s;
} xum_dev_t;
void xum_devices(const xum_dev_t *rows, int n);

/* Series oldest-first; n <= XUM_STATS_POINTS, right-aligned so "now" is
 * the rightmost bar. n == 0 shows "waiting for time". */
void xum_stats(const uint16_t *dev, const uint16_t *rx, const uint16_t *tx,
               int n, const char *suffix);

typedef struct {
    char    from[10];
    char    text[64];
    uint8_t kind;                /* 0 global, 1 local, 2 direct */
} xum_chat_t;
void xum_chat(const xum_chat_t *rows, int n);

/* Devices-in-range figure in the top bar. */
void xum_set_count(int devices);

/* Mirror the next refresh onto the UART as base64 (host screenshot). */
void xum_framedump(void);

/* Repaint and hand every slice to cb; see xui_capture in xprs_ui.h. */
struct xui_slice_fn_fwd;
esp_err_t xum_capture(void (*cb)(int, int, int, int, const uint16_t *, void *),
                      void *ctx, int *w, int *h);

#ifdef __cplusplus
}
#endif
