/**
 * @file m5ui.h
 * @brief LVGL status UI for the M5Stack Core (ILI9342C, 320x240).
 *
 * The T-Dongle's three-band dashboard, scaled up:
 *   - orange top bar     (uptime, panel indicator)
 *   - black centre body  (the active panel's text)
 *   - grey bottom bar    (rotating info + device count)
 *
 * All setters are thread-safe write-flag-and-defer; changes are applied
 * inside m5ui_update(), which must be called from ONE task only (the task
 * that owns LVGL).
 */
#ifndef XPRS_M5UI_H
#define XPRS_M5UI_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>
#include "ili9342.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t m5ui_init(ili9342_handle_t lcd);

/** Pump LVGL and apply deferred label changes. Call every ~10 ms. */
void m5ui_update(void);

/** Replace the centre body verbatim (multi-line, wraps). */
void m5ui_set_body(const char *text);

/** Panel indicator in the top bar's right corner (e.g. "bearers 1/4"). */
void m5ui_set_title(const char *text);

/** Bottom-right device count, drawn with the wireless symbol. */
void m5ui_set_device_count(int count);

/** Bottom-left text, verbatim. */
void m5ui_set_info(const char *text);

/** Bottom-left text with an "IP: " prefix. */
void m5ui_set_ip(const char *ip);

/* ---- The home panel: graphic link status ------------------------------- */

#define M5UI_HOME_ROWS 3

/** Show the graphic home panel (true) or the plain text body (false).
 *  UI task only. */
void m5ui_show_home(bool show);

/** Update one home row: a coloured status dot, the link name, and the
 *  detail text beside it. UI task only. */
void m5ui_home_row(int idx, const char *name, bool up, const char *detail);

/** Big packets-heard figure on the home panel. UI task only. */
void m5ui_home_heard(uint32_t heard);

/** Flash the RX indicator -- call when a packet arrives (any task). */
void m5ui_pulse(void);

/* ---- The flow table ----------------------------------------------------- */

#define M5UI_FLOW_ROWS 8

typedef struct {
    char     from[10];
    char     to[10];       /* "" = broadcast, drawn as "all" */
    char     type[13];
    char     link[7];      /* "espnow" or "lan" */
    float    dist_m;       /* estimated distance; < 0 = unknown */
    uint32_t age_s;        /* seconds since heard */
} m5ui_flow_t;

/** Show the flow table (true) or the plain text body (false). UI task only. */
void m5ui_show_flow(bool show);

/** Replace the flow table's rows, newest first. UI task only. */
void m5ui_flow_rows(const m5ui_flow_t *rows, int n);

/** Debug: stream the next full frame over the UART as base64 slices
 *  (FRAMEDUMP BEGIN/SLICE/END lines). Any task. */
void m5ui_framedump(void);

/* ---- The radar --------------------------------------------------------- */

#define M5UI_BLIP_MAX 12

typedef struct {
    char  label[12];
    float meters;     /* estimated distance; < 0 = unknown (drawn mid-ring) */
} m5ui_blip_t;

/** Replace the radar's blips. Position: distance sets the radius on a log
 *  scale (1 m centre, ~100 m rim); the angle is derived from the label so a
 *  station holds its bearing between updates. UI task only. */
void m5ui_radar_blips(const m5ui_blip_t *blips, int n);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_M5UI_H */
