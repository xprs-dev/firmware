/*
 * xprs_ui_none.c: the station UI interface (xprs_ui.h), on no screen.
 *
 * xprs_app is one program on every board and calls some thirty xui_*
 * functions whether or not there is a panel to call them for; a board has
 * to link one implementation or the station does not link at all. The
 * other three draw (a colour dashboard, a strip, a page of e-paper) and
 * bring LVGL with them. This one draws nothing, costs nothing, and exists
 * for the ESP32-C3, which has no screen and cannot spare the flash.
 *
 * Nothing here is an error. xui_init() refuses, so a board that wires a
 * display to this UI by mistake is told so at boot; a board with no
 * display never calls it (display_init NULL, xprs_app.h). A screenshot has
 * nothing to show and says so. The one thing kept is idleness, because
 * the app asks for it.
 */

#include "esp_timer.h"

#include "xprs_ui.h"

static uint32_t s_activity_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

esp_err_t xui_init(int width, int height, xui_flush_fn flush, void *ctx)
{
    (void)width; (void)height; (void)flush; (void)ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

void xui_update(void) { }
void xui_framedump(void) { }
esp_err_t xui_capture(xui_slice_fn cb, void *ctx, int *w, int *h)
{
    (void)cb; (void)ctx;
    if (w) *w = 0;
    if (h) *h = 0;
    return ESP_ERR_NOT_SUPPORTED;
}
void xui_flush_enable(bool on) { (void)on; }

void xui_activity(void) { s_activity_ms = now_ms(); }
uint32_t xui_idle_ms(void) { return now_ms() - s_activity_ms; }

void xui_set_body(const char *text)  { (void)text; }
void xui_set_title(const char *text) { (void)text; }
void xui_set_panel(int idx) { (void)idx; }
void xui_set_device_count(int count) { (void)count; }
void xui_set_keys(const char *l, const char *m, const char *r)
{ (void)l; (void)m; (void)r; }
void xui_set_note(const char *text) { (void)text; }
void xui_set_battery(int pct, bool charging) { (void)pct; (void)charging; }
void xui_set_call(const char *call) { (void)call; }
void xui_pulse(void) { }

void xui_show_home(bool show)  { (void)show; }
void xui_show_table(bool show) { (void)show; }
void xui_show_flow(bool show)  { (void)show; }
void xui_show_stats(bool show) { (void)show; }
void xui_show_chat(bool show)  { (void)show; }

void xui_home_row(int idx, const char *name, bool up, const char *detail,
                  const char *note)
{ (void)idx; (void)name; (void)up; (void)detail; (void)note; }
void xui_home_counts(int devices, uint32_t packets)
{ (void)devices; (void)packets; }
void xui_radar_blips(const xui_blip_t *blips, int n) { (void)blips; (void)n; }

void xui_table_setup(int ncols, const char *const headers[], const int ref_w[])
{ (void)ncols; (void)headers; (void)ref_w; }
void xui_table_rows(const xui_row_t *rows, int n) { (void)rows; (void)n; }
void xui_table_select(int idx) { (void)idx; }
void xui_flow_rows(const xui_flow_t *rows, int n) { (void)rows; (void)n; }
void xui_flow_select(int idx) { (void)idx; }
void xui_stats_set(int idx, const char *title, const uint16_t *vals, int n)
{ (void)idx; (void)title; (void)vals; (void)n; }

void xui_chat_rooms(const xui_room_t *rooms, int n, int sel)
{ (void)rooms; (void)n; (void)sel; }
void xui_chat_msgs(const xui_msg_t *msgs, int n, const char *header)
{ (void)msgs; (void)n; (void)header; }
void xui_chat_input(const char *text, bool focused)
{ (void)text; (void)focused; }

void xui_splash_show(void) { }
void xui_splash_status(const char *what) { (void)what; }
bool xui_splash_dismiss(void) { return true; }

void xui_touch_enable(xui_touch_fn fn) { (void)fn; }
bool xui_ev_pop(xui_ev_t *out) { (void)out; return false; }
