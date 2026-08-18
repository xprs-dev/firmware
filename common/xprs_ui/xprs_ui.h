/**
 * @file xprs_ui.h
 * @brief Generic LVGL status UI for XPRS stations with a colour display.
 *
 * The T-Dongle's three-band dashboard, board-independent: give xui_init()
 * the panel size and a flush callback and every element scales to fit.
 * Proven on the M5Stack Core (ILI9342C, 320x240); any ESP32 with a
 * 240px-or-taller RGB565 panel and an LVGL build should be comfortable.
 *
 *   +--------------------------------------------+
 *   |  orange top bar   (uptime | panel title)   |
 *   +--------------------------------------------+
 *   |  body: text panel, home w/ radar, or table |
 *   +--------------------------------------------+
 *   |  grey bottom bar  (button legend | count)  |
 *   +--------------------------------------------+
 *
 * All setters are thread-safe write-flag-and-defer unless marked "UI task
 * only"; changes apply inside xui_update(), which must be called from ONE
 * task -- the task that owns LVGL.
 */
#ifndef XPRS_UI_H
#define XPRS_UI_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Push one flushed window of big-endian RGB565 pixels to the panel. */
typedef void (*xui_flush_fn)(int x1, int y1, int x2, int y2,
                             const uint16_t *px, void *ctx);

esp_err_t xui_init(int width, int height, xui_flush_fn flush, void *ctx);

/** Pump LVGL and apply deferred label changes. Call every ~10 ms. */
void xui_update(void);

/** Replace the centre body verbatim (multi-line, wraps). */
void xui_set_body(const char *text);

/** Panel indicator in the top bar's right corner (e.g. "Flow 2/5"). */
void xui_set_title(const char *text);

/** Bottom-right device count, drawn with the wireless symbol. */
void xui_set_device_count(int count);

/* Arrow glyphs for the button legend, usable without including lvgl.h
 * (the same UTF-8 bytes as LV_SYMBOL_UP / LV_SYMBOL_DOWN). */
#define XUI_KEY_UP   "\xEF\x81\xB7"
#define XUI_KEY_DOWN "\xEF\x81\xB8"

/** Button legend on the bottom bar, one label above each physical button
 *  (left / middle / right, at 20%, 50% and 80% of the width). Pass "" to
 *  blank a slot. LV_SYMBOL_UP / LV_SYMBOL_DOWN make arrows. UI task only. */
void xui_set_keys(const char *left, const char *mid, const char *right);

/** Debug: stream the next full frame over the UART as base64 slices
 *  (FRAMEDUMP BEGIN/SLICE/END lines). Any task. */
void xui_framedump(void);

/* ---- The home panel: link status + radar ------------------------------- */

#define XUI_HOME_ROWS 3

/** Show the graphic home panel (true) or the plain text body (false).
 *  UI task only. */
void xui_show_home(bool show);

/** Update one home row: a coloured status dot, the link name, and the
 *  detail text beside it. UI task only. */
void xui_home_row(int idx, const char *name, bool up, const char *detail);

/** Big packets-heard figure on the home panel. UI task only. */
void xui_home_heard(uint32_t heard);

/** Flash the RX indicator -- call when a packet arrives (any task). */
void xui_pulse(void);

/* ---- The radar --------------------------------------------------------- */

#define XUI_BLIP_MAX 12

typedef struct {
    char  label[12];
    float meters;     /* estimated distance; < 0 = unknown (drawn mid-ring) */
} xui_blip_t;

/** Replace the radar's blips. Position: distance sets the radius on a log
 *  scale (1 m centre, ~100 m rim); the angle is derived from the label so a
 *  station holds its bearing between updates. UI task only. */
void xui_radar_blips(const xui_blip_t *blips, int n);

/* ---- The flow table ----------------------------------------------------- */

#define XUI_FLOW_ROWS 8

typedef struct {
    char     from[10];
    char     to[10];       /* "" = broadcast, drawn as "all" */
    char     type[13];
    char     link[7];      /* "espnow", "lan", "ble", ... */
    float    dist_m;       /* estimated distance; < 0 = unknown */
    uint32_t age_s;        /* seconds since heard */
    char     text[160];    /* packet content, shown when the row is selected */
} xui_flow_t;

/** Show the flow table (true) or the plain text body (false). UI task only. */
void xui_show_flow(bool show);

/** Replace the flow table's rows, newest first. UI task only. */
void xui_flow_rows(const xui_flow_t *rows, int n);

/** Highlight one row and show its content in the strip under the table.
 *  Pass -1 for no selection. UI task only. */
void xui_flow_select(int idx);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_UI_H */
