/**
 * @file xprs_ui.c
 * @brief Generic LVGL status UI for XPRS stations (see xprs_ui.h).
 *
 * Every dimension derives from the width/height given to xui_init(), using
 * the M5Stack Core's 320x240 proportions as the reference. Memory: no
 * full-screen buffer is assumed -- LVGL renders into a partial DMA buffer
 * about a fifth of the screen tall and the flush callback pushes stripes.
 */

#include "xprs_ui.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "lvgl.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "mbedtls/base64.h"

static const char *TAG = "xprs_ui";

/* ---- geometry, resolved at init ----------------------------------------- */

static int s_w, s_h;
static int s_top_h, s_bot_h, s_center_h;
static int s_radar_size, s_radar_r;
static int s_home_col_w;
static int s_flow_h;                 /* table height; content strip below */
#define FLOW_CONTENT_H 60

/* ---- state -------------------------------------------------------------- */

static xui_flush_fn         s_flush;
static void                *s_flush_ctx;
static lv_disp_draw_buf_t   s_draw_buf;
static lv_disp_drv_t        s_disp_drv;
static lv_disp_t           *s_disp;

static lv_obj_t *s_status_label;   /* top-left: uptime */
static lv_obj_t *s_title_label;    /* top-right: panel indicator */
static lv_obj_t *s_body_label;     /* centre */
static lv_obj_t *s_key_label[3];   /* bottom: button legend */
static lv_obj_t *s_count_label;    /* bottom-right */

static char s_body[768];
static volatile bool s_body_dirty;
static char s_title[32];
static volatile bool s_title_dirty;
static int  s_dev_count;
static volatile bool s_dev_dirty;

/* Home panel widgets (created once, shown/hidden as a group). */
static lv_obj_t *s_home;
static lv_obj_t *s_home_dot[XUI_HOME_ROWS];
static lv_obj_t *s_home_name[XUI_HOME_ROWS];
static lv_obj_t *s_home_detail[XUI_HOME_ROWS];
static lv_obj_t *s_home_heard_label;
static lv_obj_t *s_home_heard_caption;
static lv_obj_t *s_rx_dot;
static volatile bool s_pulse_pending;

/* Radar: a green tower-control sonar. The sweep is an animated line; blips
 * are small green dots with tiny labels, placed by distance (radius, log
 * scale) and by a label-derived bearing that never moves between updates. */
static lv_obj_t *s_radar;
static lv_obj_t *s_sweep_line;
static lv_point_t s_sweep_pts[2];
#define SWEEP_TRAIL 5
static lv_obj_t *s_sweep_trail[SWEEP_TRAIL];
static lv_point_t s_trail_pts[SWEEP_TRAIL][2];
static lv_point_t s_cross_pts[2][2];
static lv_point_t s_tick_pts[12][2];
static lv_obj_t *s_blip_dot[XUI_BLIP_MAX];
static lv_obj_t *s_blip_halo[XUI_BLIP_MAX];
static lv_obj_t *s_blip_lbl[XUI_BLIP_MAX];

/* The generic table + the detail strip under it (the Flow panel is one
 * preset of it; any panel may reconfigure columns and rows). */
static lv_obj_t *s_flowtab;
static lv_obj_t *s_flow_content;
static char s_tab_detail[XUI_TAB_ROWS][160];
static int s_tab_n;
static int s_tab_ncols = 5;
static int s_flow_sel = -1;
static void flow_draw_cb(lv_event_t *e);

/* ---- LVGL flush callback ------------------------------------------------ */

static volatile bool s_dump_pending;
static bool s_dump_active;

static void lcd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                         lv_color_t *color_p)
{
    uint32_t size = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    uint16_t *px = (uint16_t *)color_p;

    /* LVGL keeps RGB565 little-endian; the panels want big-endian. */
    for (uint32_t i = 0; i < size; i++) {
        px[i] = (px[i] >> 8) | (px[i] << 8);
    }

    /* Debug frame dump: mirror every slice of this refresh onto the UART as
     * base64, so a host script can reassemble a screenshot. Costs nothing
     * unless a dump was requested. */
    if (s_dump_active) {
        size_t b64_need = ((size * 2 + 2) / 3) * 4 + 4;
        unsigned char *b64 = malloc(b64_need);
        if (b64) {
            size_t olen = 0;
            mbedtls_base64_encode(b64, b64_need, &olen,
                                  (const unsigned char *)px, size * 2);
            printf("SLICE %d %d %d %d %u\n",
                   area->x1, area->y1, area->x2, area->y2, (unsigned)olen);
            for (size_t off = 0; off < olen; off += 512) {
                size_t nch = olen - off > 512 ? 512 : olen - off;
                fwrite(b64 + off, 1, nch, stdout);
                fputc('\n', stdout);
            }
            free(b64);
        }
    }

    s_flush(area->x1, area->y1, area->x2, area->y2, px, s_flush_ctx);
    lv_disp_flush_ready(drv);
}

void xui_framedump(void)
{
    s_dump_pending = true;
}

/* ---- update ------------------------------------------------------------- */

static uint32_t s_uptime_last;
static uint64_t s_last_tick_us;

void xui_update(void)
{
    /* The managed LVGL component does not see our lv_conf.h, so
     * LV_TICK_CUSTOM is inert -- advance the tick by hand (same story as the
     * T-Dongle, tdongle_ui.c). */
    uint64_t now_us = esp_timer_get_time();
    uint32_t elapsed_ms = (uint32_t)((now_us - s_last_tick_us) / 1000);
    if (elapsed_ms > 0) {
        lv_tick_inc(elapsed_ms);
        s_last_tick_us = now_us;
    }

    lv_timer_handler();

    if (s_dump_pending) {
        s_dump_pending = false;
        esp_log_level_set("*", ESP_LOG_NONE);   /* keep the stream clean */
        printf("FRAMEDUMP BEGIN %d %d\n", s_w, s_h);
        s_dump_active = true;
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(NULL);
        s_dump_active = false;
        printf("FRAMEDUMP END\n");
        esp_log_level_set("*", ESP_LOG_INFO);
    }

    /* Sweep: one revolution every 4 s, driven from here so it costs one line
     * redraw per frame and nothing when the panel is hidden. */
    if (s_sweep_line && s_home && !lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN)) {
        float ang = (float)((now_us / 1000) % 4000) * (2.0f * (float)M_PI / 4000.0f);
        int rc = s_radar_size / 2;
        s_sweep_pts[1].x = rc + (lv_coord_t)(sinf(ang) * s_radar_r);
        s_sweep_pts[1].y = rc - (lv_coord_t)(cosf(ang) * s_radar_r);
        lv_line_set_points(s_sweep_line, s_sweep_pts, 2);
        /* The afterglow trails a few degrees behind the leading edge. */
        for (int i = 0; i < SWEEP_TRAIL; i++) {
            float ta = ang - 0.10f * (float)(i + 1);
            s_trail_pts[i][1].x = rc + (lv_coord_t)(sinf(ta) * s_radar_r);
            s_trail_pts[i][1].y = rc - (lv_coord_t)(cosf(ta) * s_radar_r);
            lv_line_set_points(s_sweep_trail[i], s_trail_pts[i], 2);
        }
    }

    uint32_t total_sec = (uint32_t)(esp_timer_get_time() / 1000000);
    if (total_sec != s_uptime_last && s_status_label) {
        s_uptime_last = total_sec;
        static char buf[64];
        uint32_t days = total_sec / 86400;
        uint32_t h = (total_sec / 3600) % 24;
        uint32_t m = (total_sec / 60) % 60;
        uint32_t s = total_sec % 60;
        if (days == 0) {
            snprintf(buf, sizeof buf, "XPRS uptime: %02lu:%02lu:%02lu",
                     (unsigned long)h, (unsigned long)m, (unsigned long)s);
        } else {
            snprintf(buf, sizeof buf, "XPRS uptime: %lu day%s %02lu h",
                     (unsigned long)days, days == 1 ? "" : "s",
                     (unsigned long)h);
        }
        lv_label_set_text(s_status_label, buf);
    }

    if (s_body_dirty) {
        s_body_dirty = false;
        if (s_body_label) {
            lv_label_set_text(s_body_label, s_body);
            lv_obj_t *parent = lv_obj_get_parent(s_body_label);
            lv_obj_update_layout(parent);
            lv_obj_scroll_to_y(parent, 0, LV_ANIM_OFF);
        }
    }
    if (s_title_dirty) {
        s_title_dirty = false;
        if (s_title_label) lv_label_set_text(s_title_label, s_title);
    }
    if (s_pulse_pending && s_rx_dot) {
        s_pulse_pending = false;
        /* A packet just arrived: flash the RX dot and let it fade. */
        lv_anim_t an;
        lv_anim_init(&an);
        lv_anim_set_var(&an, s_rx_dot);
        lv_anim_set_values(&an, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_time(&an, 600);
        lv_anim_set_exec_cb(&an, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
        lv_anim_start(&an);
    }

    if (s_dev_dirty) {
        s_dev_dirty = false;
        if (s_count_label) {
            char buf[24];
            if (s_dev_count > 0)
                snprintf(buf, sizeof buf, LV_SYMBOL_WIFI " %d", s_dev_count);
            else
                buf[0] = '\0';
            lv_label_set_text(s_count_label, buf);
        }
    }
}

/* ---- UI construction ---------------------------------------------------- */

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- Top bar (orange) ---- */
    lv_obj_t *top = lv_obj_create(scr);
    lv_obj_set_size(top, s_w, s_top_h);
    lv_obj_set_style_bg_color(top, lv_color_make(255, 140, 0), 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);

    s_status_label = lv_label_create(top);
    lv_label_set_text(s_status_label, "XPRS  00:00:00");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_black(), 0);
    lv_obj_align(s_status_label, LV_ALIGN_LEFT_MID, 6, 0);

    s_title_label = lv_label_create(top);
    lv_label_set_text(s_title_label, "");
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_black(), 0);
    lv_obj_align(s_title_label, LV_ALIGN_RIGHT_MID, -6, 0);

    /* ---- Centre body (black) ---- */
    lv_obj_t *center = lv_obj_create(scr);
    lv_obj_remove_style_all(center);
    lv_obj_set_size(center, s_w, s_center_h);
    lv_obj_align(center, LV_ALIGN_TOP_LEFT, 0, s_top_h);
    lv_obj_set_style_bg_color(center, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(center, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(center, 0, 0);
    lv_obj_set_style_pad_all(center, 8, 0);
    lv_obj_set_scrollbar_mode(center, LV_SCROLLBAR_MODE_OFF);

    s_body_label = lv_label_create(center);
    lv_label_set_text(s_body_label, "--");
    lv_obj_set_style_text_font(s_body_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_body_label, lv_color_white(), 0);
    lv_label_set_long_mode(s_body_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body_label, LV_PCT(100));
    lv_obj_align(s_body_label, LV_ALIGN_TOP_LEFT, 0, 0);

    /* ---- Home panel: graphic link status, hidden until shown ---- */
    s_home = lv_obj_create(scr);
    lv_obj_remove_style_all(s_home);
    lv_obj_set_size(s_home, s_w, s_center_h);
    lv_obj_align(s_home, LV_ALIGN_TOP_LEFT, 0, s_top_h);
    lv_obj_set_style_bg_color(s_home, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_home, 10, 0);
    lv_obj_clear_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < XUI_HOME_ROWS; i++) {
        int y = 6 + i * 40;

        s_home_dot[i] = lv_obj_create(s_home);
        lv_obj_remove_style_all(s_home_dot[i]);
        lv_obj_set_size(s_home_dot[i], 16, 16);
        lv_obj_set_style_radius(s_home_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_home_dot[i],
                                  lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_bg_opa(s_home_dot[i], LV_OPA_COVER, 0);
        lv_obj_align(s_home_dot[i], LV_ALIGN_TOP_LEFT, 0, y);

        s_home_name[i] = lv_label_create(s_home);
        lv_label_set_text(s_home_name[i], "");
        lv_obj_set_style_text_font(s_home_name[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(s_home_name[i], lv_color_white(), 0);
        lv_label_set_long_mode(s_home_name[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_home_name[i], s_home_col_w);
        lv_obj_align(s_home_name[i], LV_ALIGN_TOP_LEFT, 26, y);

        s_home_detail[i] = lv_label_create(s_home);
        lv_label_set_text(s_home_detail[i], "");
        lv_obj_set_style_text_font(s_home_detail[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_home_detail[i],
                                    lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
        lv_label_set_long_mode(s_home_detail[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_home_detail[i], s_home_col_w);
        lv_obj_align(s_home_detail[i], LV_ALIGN_TOP_LEFT, 26, y + 17);
    }

    /* ---- The radar, right half: a PPI scope like a tower console ---- */
    s_radar = lv_obj_create(s_home);
    lv_obj_remove_style_all(s_radar);
    lv_obj_set_size(s_radar, s_radar_size, s_radar_size);
    lv_obj_set_style_radius(s_radar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_radar, lv_color_make(0, 14, 0), 0);
    lv_obj_set_style_bg_opa(s_radar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_radar, 3, 0);
    lv_obj_set_style_border_color(s_radar, lv_color_make(0, 150, 0), 0);
    lv_obj_clear_flag(s_radar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(s_radar, LV_ALIGN_RIGHT_MID, -4, 0);

    int rc = s_radar_size / 2;

    /* Crosshairs, faint, through the centre. */
    for (int i = 0; i < 2; i++) {
        s_cross_pts[i][0].x = i ? rc : 4;
        s_cross_pts[i][0].y = i ? 4 : rc;
        s_cross_pts[i][1].x = i ? rc : s_radar_size - 4;
        s_cross_pts[i][1].y = i ? s_radar_size - 4 : rc;
        lv_obj_t *ln = lv_line_create(s_radar);
        lv_line_set_points(ln, s_cross_pts[i], 2);
        lv_obj_set_style_line_width(ln, 1, 0);
        lv_obj_set_style_line_color(ln, lv_color_make(0, 60, 0), 0);
    }

    /* Range rings: ~1 m centre, ~10 m, ~100 m rim (log scale), with the
     * range written on them the way a scope prints its scale. */
    static const int ring_frac[2] = { 33, 66 };
    static const char *const ring_txt[2] = { "10m", "100m" };
    for (int i = 0; i < 2; i++) {
        lv_obj_t *ring = lv_obj_create(s_radar);
        lv_obj_remove_style_all(ring);
        int rd = s_radar_r * 2 * ring_frac[i] / 100;
        lv_obj_set_size(ring, rd, rd);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(ring, 1, 0);
        lv_obj_set_style_border_color(ring, lv_color_make(0, 80, 0), 0);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_center(ring);

        lv_obj_t *rl = lv_label_create(s_radar);
        lv_label_set_text(rl, ring_txt[i]);
        lv_obj_set_style_text_font(rl, &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(rl, lv_color_make(0, 120, 0), 0);
        /* On the south-east diagonal of its ring, clear of the crosshair. */
        int rr = s_radar_r * ring_frac[i] / 100;
        lv_obj_set_pos(rl, rc + (int)(rr * 0.707f) - 8,
                       rc + (int)(rr * 0.707f) - 4);
    }

    /* Bearing ticks every 30 degrees on the rim, heavier on the cardinals. */
    for (int i = 0; i < 12; i++) {
        float a = (float)i * (2.0f * (float)M_PI / 12.0f);
        float sx = sinf(a), cy_ = cosf(a);
        s_tick_pts[i][0].x = rc + (lv_coord_t)(sx * (s_radar_r - 1));
        s_tick_pts[i][0].y = rc - (lv_coord_t)(cy_ * (s_radar_r - 1));
        s_tick_pts[i][1].x = rc + (lv_coord_t)(sx * (s_radar_r - 7));
        s_tick_pts[i][1].y = rc - (lv_coord_t)(cy_ * (s_radar_r - 7));
        lv_obj_t *tk = lv_line_create(s_radar);
        lv_line_set_points(tk, s_tick_pts[i], 2);
        lv_obj_set_style_line_width(tk, i % 3 == 0 ? 2 : 1, 0);
        lv_obj_set_style_line_color(tk, lv_color_make(0, 130, 0), 0);
    }

    /* The sweep: a bright leading edge with a fading afterglow behind it,
     * the classic PPI phosphor look. */
    static const lv_opa_t trail_opa[SWEEP_TRAIL] = { 150, 105, 70, 42, 22 };
    for (int i = 0; i < SWEEP_TRAIL; i++) {
        s_trail_pts[i][0].x = rc; s_trail_pts[i][0].y = rc;
        s_trail_pts[i][1].x = rc; s_trail_pts[i][1].y = rc;
        s_sweep_trail[i] = lv_line_create(s_radar);
        lv_line_set_points(s_sweep_trail[i], s_trail_pts[i], 2);
        lv_obj_set_style_line_width(s_sweep_trail[i], 2, 0);
        lv_obj_set_style_line_color(s_sweep_trail[i],
                                    lv_color_make(0, 200, 30), 0);
        lv_obj_set_style_line_opa(s_sweep_trail[i], trail_opa[i], 0);
    }
    s_sweep_pts[0].x = rc; s_sweep_pts[0].y = rc;
    s_sweep_pts[1].x = rc; s_sweep_pts[1].y = 4;
    s_sweep_line = lv_line_create(s_radar);
    lv_line_set_points(s_sweep_line, s_sweep_pts, 2);
    lv_obj_set_style_line_width(s_sweep_line, 2, 0);
    lv_obj_set_style_line_color(s_sweep_line, lv_color_make(80, 255, 100), 0);
    lv_obj_set_style_line_rounded(s_sweep_line, true, 0);

    /* Blip pool: a bright contact dot inside a dim halo, hidden until used. */
    for (int i = 0; i < XUI_BLIP_MAX; i++) {
        s_blip_halo[i] = lv_obj_create(s_radar);
        lv_obj_remove_style_all(s_blip_halo[i]);
        lv_obj_set_size(s_blip_halo[i], 15, 15);
        lv_obj_set_style_radius(s_blip_halo[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(s_blip_halo[i], 1, 0);
        lv_obj_set_style_border_color(s_blip_halo[i],
                                      lv_color_make(0, 150, 40), 0);
        lv_obj_set_style_bg_opa(s_blip_halo[i], LV_OPA_TRANSP, 0);
        lv_obj_add_flag(s_blip_halo[i], LV_OBJ_FLAG_HIDDEN);

        s_blip_dot[i] = lv_obj_create(s_radar);
        lv_obj_remove_style_all(s_blip_dot[i]);
        lv_obj_set_size(s_blip_dot[i], 7, 7);
        lv_obj_set_style_radius(s_blip_dot[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_blip_dot[i], lv_color_make(60, 255, 90), 0);
        lv_obj_set_style_bg_opa(s_blip_dot[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(s_blip_dot[i], LV_OBJ_FLAG_HIDDEN);

        s_blip_lbl[i] = lv_label_create(s_radar);
        lv_label_set_text(s_blip_lbl[i], "");
        lv_obj_set_style_text_font(s_blip_lbl[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(s_blip_lbl[i], lv_color_make(0, 230, 60), 0);
        lv_obj_add_flag(s_blip_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* RX flash at the scope's centre. */
    s_rx_dot = lv_obj_create(s_radar);
    lv_obj_remove_style_all(s_rx_dot);
    lv_obj_set_size(s_rx_dot, 14, 14);
    lv_obj_set_style_radius(s_rx_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_rx_dot, lv_color_make(120, 255, 120), 0);
    lv_obj_set_style_bg_opa(s_rx_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(s_rx_dot, LV_OPA_TRANSP, 0);
    lv_obj_center(s_rx_dot);

    /* Packets-heard figure, under the link rows on the left. */
    s_home_heard_label = lv_label_create(s_home);
    lv_label_set_text(s_home_heard_label, "0");
    lv_obj_set_style_text_font(s_home_heard_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_home_heard_label,
                                lv_color_make(255, 140, 0), 0);
    lv_obj_align(s_home_heard_label, LV_ALIGN_BOTTOM_LEFT, 0, -22);

    s_home_heard_caption = lv_label_create(s_home);
    lv_label_set_text(s_home_heard_caption, "Packets heard");
    lv_obj_set_style_text_font(s_home_heard_caption, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_home_heard_caption,
                                lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_align(s_home_heard_caption, LV_ALIGN_BOTTOM_LEFT, 0, -6);

    lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);

    /* ---- The flow table + content strip, hidden until shown ---- */
    s_flowtab = lv_table_create(scr);
    lv_obj_set_size(s_flowtab, s_w, s_flow_h);
    lv_obj_align(s_flowtab, LV_ALIGN_TOP_LEFT, 0, s_top_h);
    lv_obj_set_style_bg_color(s_flowtab, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_flowtab, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_flowtab, 0, 0);
    lv_obj_set_style_pad_all(s_flowtab, 0, 0);
    lv_obj_set_style_text_font(s_flowtab, &lv_font_montserrat_12,
                               LV_PART_ITEMS);
    lv_obj_set_style_text_color(s_flowtab, lv_color_white(), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(s_flowtab, lv_color_black(), LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(s_flowtab, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_border_width(s_flowtab, 1, LV_PART_ITEMS);
    lv_obj_set_style_border_color(s_flowtab, lv_color_make(40, 40, 40),
                                  LV_PART_ITEMS);
    lv_obj_set_style_pad_ver(s_flowtab, 4, LV_PART_ITEMS);
    lv_obj_set_style_pad_hor(s_flowtab, 4, LV_PART_ITEMS);
    lv_obj_set_scrollbar_mode(s_flowtab, LV_SCROLLBAR_MODE_OFF);
    lv_table_set_col_cnt(s_flowtab, 5);
    lv_table_set_row_cnt(s_flowtab, 1);
    /* Reference widths on a 320px screen: 70/54/90/54/52 -- the first fits
     * "X1RD89-7". Scale by width; the last column soaks up the rounding. */
    static const int ref_w[4] = { 70, 54, 90, 54 };
    int used = 0;
    for (int i = 0; i < 4; i++) {
        int cw = ref_w[i] * s_w / 320;
        lv_table_set_col_width(s_flowtab, i, cw);
        used += cw;
    }
    lv_table_set_col_width(s_flowtab, 4, s_w - used);
    lv_table_set_cell_value(s_flowtab, 0, 0, "From");
    lv_table_set_cell_value(s_flowtab, 0, 1, "To");
    lv_table_set_cell_value(s_flowtab, 0, 2, "Type");
    lv_table_set_cell_value(s_flowtab, 0, 3, "Dist");
    lv_table_set_cell_value(s_flowtab, 0, 4, "When");
    lv_obj_add_event_cb(s_flowtab, flow_draw_cb, LV_EVENT_DRAW_PART_BEGIN,
                        NULL);
    lv_obj_add_flag(s_flowtab, LV_OBJ_FLAG_HIDDEN);

    /* The reading strip: visibly its own box -- inset from the edges, a
     * grey border, and body-size text so it reads like content, not a bar. */
    s_flow_content = lv_label_create(scr);
    lv_label_set_text(s_flow_content, "");
    lv_obj_set_style_text_font(s_flow_content, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_flow_content, lv_color_white(), 0);
    lv_obj_set_style_bg_color(s_flow_content, lv_color_make(16, 22, 36), 0);
    lv_obj_set_style_bg_opa(s_flow_content, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_flow_content, 1, 0);
    lv_obj_set_style_border_color(s_flow_content,
                                  lv_color_make(90, 90, 90), 0);
    lv_obj_set_style_radius(s_flow_content, 4, 0);
    lv_obj_set_style_pad_all(s_flow_content, 5, 0);
    lv_label_set_long_mode(s_flow_content, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_flow_content, s_w - 8, FLOW_CONTENT_H - 4);
    lv_obj_align(s_flow_content, LV_ALIGN_TOP_LEFT, 4,
                 s_top_h + s_flow_h + 4);
    lv_obj_add_flag(s_flow_content, LV_OBJ_FLAG_HIDDEN);

    /* ---- Bottom bar (grey): button legend + device count ---- */
    lv_obj_t *bot = lv_obj_create(scr);
    lv_obj_set_size(bot, s_w, s_bot_h);
    lv_obj_set_style_bg_color(bot, lv_color_make(128, 128, 128), 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_set_style_pad_all(bot, 0, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);

    /* One label above each physical button: 20%, 50%, 80% of the width. */
    static const int key_frac[3] = { 20, 50, 80 };
    for (int i = 0; i < 3; i++) {
        s_key_label[i] = lv_label_create(bot);
        lv_label_set_text(s_key_label[i], "");
        lv_obj_set_style_text_font(s_key_label[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(s_key_label[i], lv_color_black(), 0);
        lv_obj_align(s_key_label[i], LV_ALIGN_CENTER,
                     s_w * key_frac[i] / 100 - s_w / 2, 0);
    }

    s_count_label = lv_label_create(bot);
    lv_label_set_text(s_count_label, "");
    lv_obj_set_style_text_font(s_count_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_count_label, lv_color_black(), 0);
    lv_obj_align(s_count_label, LV_ALIGN_RIGHT_MID, -6, 0);
}

/* ---- public API --------------------------------------------------------- */

esp_err_t xui_init(int width, int height, xui_flush_fn flush, void *ctx)
{
    if (width < 160 || height < 120 || !flush) return ESP_ERR_INVALID_ARG;
    s_w = width;
    s_h = height;
    s_flush = flush;
    s_flush_ctx = ctx;

    /* The reference layout is 320x240; every band scales with the height. */
    s_top_h = s_h * 28 / 240;
    s_bot_h = s_h * 22 / 240;
    s_center_h = s_h - s_top_h - s_bot_h;
    s_radar_size = s_center_h - 34;
    s_radar_r = s_radar_size / 2 - 4;
    s_home_col_w = s_w - 20 - s_radar_size - 36;
    s_flow_h = s_center_h - FLOW_CONTENT_H;

    lv_init();

    int buf_rows = s_h / 5;
    static lv_color_t *buf1;
    buf1 = heap_caps_malloc(s_w * buf_rows * sizeof(lv_color_t),
                            MALLOC_CAP_DMA);
    if (!buf1) buf1 = malloc(s_w * buf_rows * sizeof(lv_color_t));
    if (!buf1) return ESP_ERR_NO_MEM;
    lv_disp_draw_buf_init(&s_draw_buf, buf1, NULL, s_w * buf_rows);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = s_w;
    s_disp_drv.ver_res  = s_h;
    s_disp_drv.flush_cb = lcd_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    lv_theme_t *th = lv_theme_default_init(s_disp,
                                           lv_palette_main(LV_PALETTE_BLUE),
                                           lv_palette_main(LV_PALETTE_GREY),
                                           true,
                                           &lv_font_montserrat_14);
    lv_disp_set_theme(s_disp, th);

    build_ui();

    s_body[0] = 0; s_body_dirty = false;
    s_title[0] = 0; s_title_dirty = false;
    s_dev_count = 0; s_dev_dirty = false;
    s_last_tick_us = esp_timer_get_time();

    ESP_LOGI(TAG, "XPRS UI initialised (%dx%d) -- call xui_update() "
                  "from the UI task", s_w, s_h);
    return ESP_OK;
}

void xui_set_body(const char *text)
{
    if (!text) return;
    snprintf(s_body, sizeof s_body, "%s", text);
    s_body_dirty = true;
}

void xui_set_title(const char *text)
{
    if (!text) return;
    snprintf(s_title, sizeof s_title, "%s", text);
    s_title_dirty = true;
}

void xui_set_device_count(int count)
{
    s_dev_count = count;
    s_dev_dirty = true;
}

void xui_set_keys(const char *left, const char *mid, const char *right)
{
    const char *txt[3] = { left, mid, right };
    for (int i = 0; i < 3; i++) {
        if (!s_key_label[i]) continue;
        lv_label_set_text(s_key_label[i], txt[i] ? txt[i] : "");
        /* Re-centre: the label's width changed with its text. */
        static const int key_frac[3] = { 20, 50, 80 };
        lv_obj_align(s_key_label[i], LV_ALIGN_CENTER,
                     s_w * key_frac[i] / 100 - s_w / 2, 0);
    }
}

/* Header row in blue with white text, the selected row in a deep blue, and
 * alternating dark stripes elsewhere. */
static void flow_draw_cb(lv_event_t *e)
{
    lv_obj_draw_part_dsc_t *d = lv_event_get_draw_part_dsc(e);
    if (d->part != LV_PART_ITEMS) return;
    uint32_t row = d->id / s_tab_ncols;
    if (row == 0) {
        d->rect_dsc->bg_color = lv_color_make(30, 90, 200);
        d->rect_dsc->bg_opa = LV_OPA_COVER;
        d->label_dsc->color = lv_color_white();
    } else if (s_flow_sel >= 0 && row == (uint32_t)(s_flow_sel + 1)) {
        d->rect_dsc->bg_color = lv_color_make(0, 100, 40);
        d->rect_dsc->bg_opa = LV_OPA_COVER;
    } else if ((row & 1) == 0) {
        d->rect_dsc->bg_color = lv_color_make(22, 22, 22);
        d->rect_dsc->bg_opa = LV_OPA_COVER;
    }
}

void xui_show_table(bool show) { xui_show_flow(show); }

void xui_show_flow(bool show)
{
    if (!s_flowtab || !s_body_label) return;
    if (show) {
        lv_obj_clear_flag(s_flowtab, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_flow_content, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lv_obj_get_parent(s_body_label), LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_flowtab, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_flow_content, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *type_icon(const char *type)
{
    if (strcmp(type, "msg") == 0)      return LV_SYMBOL_ENVELOPE;
    if (strcmp(type, "status") == 0)   return LV_SYMBOL_BELL;
    if (strcmp(type, "position") == 0) return LV_SYMBOL_GPS;
    if (strcmp(type, "beacon") == 0)   return LV_SYMBOL_VOLUME_MAX;
    if (strcmp(type, "identity") == 0) return LV_SYMBOL_EYE_OPEN;
    if (strcmp(type, "observation") == 0) return LV_SYMBOL_EYE_OPEN;
    if (strcmp(type, "ping") == 0 ||
        strcmp(type, "pong") == 0)     return LV_SYMBOL_LOOP;
    if (strcmp(type, "receipt") == 0)  return LV_SYMBOL_OK;
    if (strcmp(type, "file") == 0)     return LV_SYMBOL_FILE;
    return LV_SYMBOL_RIGHT;
}

static void flow_show_content(void)
{
    if (!s_flow_content) return;
    if (s_flow_sel < 0 || s_flow_sel >= s_tab_n) {
        lv_label_set_text(s_flow_content,
                          s_tab_n ? "Scroll with the arrows for detail" : "");
        return;
    }
    lv_label_set_text(s_flow_content, s_tab_detail[s_flow_sel]);
}

void xui_table_setup(int ncols, const char *const headers[],
                     const int ref_w[])
{
    if (!s_flowtab) return;
    if (ncols < 1) ncols = 1;
    if (ncols > XUI_TAB_COLS) ncols = XUI_TAB_COLS;
    /* Drop the data rows BEFORE reshaping the columns: LVGL frees a row's
     * cells using the current column count, so shrinking/growing columns
     * while another panel's rows still sit in the table frees the wrong
     * pointers (LoadProhibited in lv_table_set_row_cnt). */
    if (ncols != s_tab_ncols) {
        lv_table_set_row_cnt(s_flowtab, 1);
        s_tab_n = 0;
    }
    s_tab_ncols = ncols;
    lv_table_set_col_cnt(s_flowtab, ncols);
    int used = 0;
    for (int i = 0; i < ncols; i++) {
        int cw = (i == ncols - 1) ? s_w - used : ref_w[i] * s_w / 320;
        lv_table_set_col_width(s_flowtab, i, cw);
        used += cw;
        lv_table_set_cell_value(s_flowtab, 0, i, headers[i]);
    }
}

void xui_table_rows(const xui_row_t *rows, int n)
{
    if (!s_flowtab) return;
    if (n > XUI_TAB_ROWS) n = XUI_TAB_ROWS;
    s_tab_n = n;
    lv_table_set_row_cnt(s_flowtab, n + 1);
    for (int i = 0; i < n; i++) {
        for (int c = 0; c < s_tab_ncols; c++)
            lv_table_set_cell_value(s_flowtab, i + 1, c, rows[i].cell[c]);
        snprintf(s_tab_detail[i], sizeof s_tab_detail[i], "%s",
                 rows[i].detail);
    }
    if (s_flow_sel >= n) s_flow_sel = n - 1;
    flow_show_content();
}

void xui_flow_rows(const xui_flow_t *rows, int n)
{
    static const char *const hdr[5] = { "From", "To", "Type", "Dist", "When" };
    static const int ref_w[5] = { 70, 54, 90, 54, 52 };  /* first fits X1RD89-7 */
    xui_table_setup(5, hdr, ref_w);

    /* Static: 8 rows of cells would not be kind to the UI task's stack. */
    static xui_row_t tr[XUI_TAB_ROWS];
    if (n > XUI_TAB_ROWS) n = XUI_TAB_ROWS;
    for (int i = 0; i < n; i++) {
        const xui_flow_t *r = &rows[i];
        snprintf(tr[i].cell[0], sizeof tr[i].cell[0], "%s", r->from);
        snprintf(tr[i].cell[1], sizeof tr[i].cell[1], "%s",
                 r->to[0] ? r->to : "all");
        /* One line per packet: long type names get cut, not wrapped. */
        snprintf(tr[i].cell[2], sizeof tr[i].cell[2], "%s %.8s",
                 type_icon(r->type), r->type);
        /* Distance reads better than dBm; the link's icon says how it came:
         * a bolt for ESP-NOW, the antenna for the LAN (no range there). */
        if (r->dist_m >= 0)
            snprintf(tr[i].cell[3], sizeof tr[i].cell[3],
                     LV_SYMBOL_CHARGE " ~%dm", (int)(r->dist_m + 0.5f));
        else
            snprintf(tr[i].cell[3], sizeof tr[i].cell[3], LV_SYMBOL_WIFI);
        if (r->age_s < 60)
            snprintf(tr[i].cell[4], sizeof tr[i].cell[4], "%lus",
                     (unsigned long)r->age_s);
        else if (r->age_s < 3600)
            snprintf(tr[i].cell[4], sizeof tr[i].cell[4], "%lum",
                     (unsigned long)(r->age_s / 60));
        else
            snprintf(tr[i].cell[4], sizeof tr[i].cell[4], "%luh",
                     (unsigned long)(r->age_s / 3600));
        snprintf(tr[i].detail, sizeof tr[i].detail, "%s", r->text);
    }
    xui_table_rows(tr, n);
}

void xui_flow_select(int idx) { xui_table_select(idx); }

void xui_table_select(int idx)
{
    if (idx >= s_tab_n) idx = s_tab_n - 1;
    if (idx < -1) idx = -1;
    s_flow_sel = idx;
    flow_show_content();
    if (s_flowtab) {
        /* Keep the selection in view: rows are ~25px; scroll so the selected
         * one sits inside the table's window. */
        if (idx >= 0) {
            int row_h = 25;
            int y_top = (idx + 1) * row_h;
            int view = s_flow_h - row_h;
            int want = y_top + row_h > view ? y_top + row_h - view : 0;
            lv_obj_scroll_to_y(s_flowtab, want, LV_ANIM_OFF);
        } else {
            lv_obj_scroll_to_y(s_flowtab, 0, LV_ANIM_OFF);
        }
        lv_obj_invalidate(s_flowtab);
    }
}

void xui_show_home(bool show)
{
    if (!s_home || !s_body_label) return;
    if (show) {
        lv_obj_clear_flag(s_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lv_obj_get_parent(s_body_label), LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_home, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(lv_obj_get_parent(s_body_label), LV_OBJ_FLAG_HIDDEN);
    }
}

void xui_home_row(int idx, const char *name, bool up, const char *detail)
{
    if (idx < 0 || idx >= XUI_HOME_ROWS || !s_home) return;
    lv_label_set_text(s_home_name[idx], name ? name : "");
    lv_label_set_text(s_home_detail[idx], detail ? detail : "");
    lv_obj_set_style_bg_color(s_home_dot[idx],
                              up ? lv_palette_main(LV_PALETTE_GREEN)
                                 : lv_palette_main(LV_PALETTE_GREY),
                              0);
}

void xui_home_heard(uint32_t heard)
{
    if (!s_home_heard_label) return;
    char buf[16];
    snprintf(buf, sizeof buf, "%lu", (unsigned long)heard);
    lv_label_set_text(s_home_heard_label, buf);
}

void xui_pulse(void)
{
    s_pulse_pending = true;
}

/* Log-scale radius: 1 m at the centre, ~100 m at the rim. */
static int radius_for(float meters)
{
    if (meters < 0) return s_radar_r * 45 / 100;        /* unknown: mid-ring */
    if (meters < 1) meters = 1;
    if (meters > 100) meters = 100;
    float f = log10f(meters) / 2.0f;                    /* 0..1 over 1..100 m */
    int r = 8 + (int)(f * (s_radar_r - 16));
    return r;
}

void xui_radar_blips(const xui_blip_t *blips, int n)
{
    if (!s_radar) return;
    if (n > XUI_BLIP_MAX) n = XUI_BLIP_MAX;

    /* First pass: place every contact (dot + halo), remember where. */
    int bx[XUI_BLIP_MAX], by[XUI_BLIP_MAX];
    for (int i = 0; i < XUI_BLIP_MAX; i++) {
        if (i >= n) {
            lv_obj_add_flag(s_blip_dot[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_blip_halo[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_blip_lbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        /* Bearing from the label: no direction is known, so give each
         * station a stable, label-derived angle -- it holds its spot on the
         * scope between updates instead of jumping. */
        uint32_t hsh = 5381;
        for (const char *c = blips[i].label; *c; c++)
            hsh = hsh * 33 + (uint8_t)*c;
        float ang = (float)(hsh % 360) * (2.0f * (float)M_PI / 360.0f);
        int r = radius_for(blips[i].meters);
        bx[i] = s_radar_size / 2 + (int)(sinf(ang) * r);
        by[i] = s_radar_size / 2 - (int)(cosf(ang) * r);

        lv_obj_clear_flag(s_blip_dot[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_blip_dot[i], bx[i] - 3, by[i] - 3);
        lv_obj_clear_flag(s_blip_halo[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_blip_halo[i], bx[i] - 7, by[i] - 7);
    }

    /* Second pass: the labels. A label belongs NEXT TO its contact -- try
     * the eight positions around the dot in preference order and take the
     * first that covers nothing (other contacts' halos, labels already
     * placed, the scope's centre where the RX flash lives). Only if every
     * spot is taken does it slide down as a last resort. */
    int lrx[XUI_BLIP_MAX], lry[XUI_BLIP_MAX];
    int lrw[XUI_BLIP_MAX], lrh[XUI_BLIP_MAX];
    for (int i = 0; i < n; i++) {
        char txt[24];
        if (blips[i].meters >= 0)
            snprintf(txt, sizeof txt, "%s\n~%dm", blips[i].label,
                     (int)(blips[i].meters + 0.5f));
        else
            snprintf(txt, sizeof txt, "%s", blips[i].label);
        lv_obj_clear_flag(s_blip_lbl[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_blip_lbl[i], txt);
        lv_obj_update_layout(s_blip_lbl[i]);
        int lw = lv_obj_get_width(s_blip_lbl[i]);
        int lh = lv_obj_get_height(s_blip_lbl[i]);

        /* Candidates around the dot: beside first, then under/over, then
         * the diagonals. The far side of the scope prefers the inner side
         * so the label stays inside the circle. */
        bool right_half = bx[i] > s_radar_size / 2;
        int cand[8][2] = {
            { right_half ? bx[i] - 10 - lw : bx[i] + 10, by[i] - lh / 2 },
            { right_half ? bx[i] + 10 : bx[i] - 10 - lw, by[i] - lh / 2 },
            { bx[i] - lw / 2, by[i] + 10 },
            { bx[i] - lw / 2, by[i] - 10 - lh },
            { bx[i] + 8, by[i] + 8 },
            { bx[i] - 8 - lw, by[i] + 8 },
            { bx[i] + 8, by[i] - 8 - lh },
            { bx[i] - 8 - lw, by[i] - 8 - lh },
        };

        int lx = cand[2][0], ly = cand[2][1];   /* fallback: below */
        for (int c = 0; c < 8; c++) {
            int tx = cand[c][0], ty = cand[c][1];
            if (tx < 2) tx = 2;
            if (tx + lw > s_radar_size - 2) tx = s_radar_size - 2 - lw;
            if (ty < 2) ty = 2;
            if (ty + lh > s_radar_size - 2) ty = s_radar_size - 2 - lh;
            bool free = true;
            int ccx = s_radar_size / 2 - 9;
            if (tx < ccx + 18 && ccx < tx + lw &&
                ty < ccx + 18 && ccx < ty + lh) free = false;
            for (int j = 0; free && j < n; j++) {
                if (j == i) continue;
                int jx = bx[j] - 8, jy = by[j] - 8;
                if (tx < jx + 16 && jx < tx + lw &&
                    ty < jy + 16 && jy < ty + lh) free = false;
            }
            for (int j = 0; free && j < i; j++) {
                if (tx < lrx[j] + lrw[j] && lrx[j] < tx + lw &&
                    ty < lry[j] + lrh[j] && lry[j] < ty + lh) free = false;
            }
            if (free) { lx = tx; ly = ty; goto placed; }
        }
        /* Nothing free around the dot: slide the below-position down. */
        for (int pass = 0; pass < n + XUI_BLIP_MAX; pass++) {
            bool moved = false;
            for (int j = 0; j < n; j++) {
                if (j == i) continue;
                int jx = bx[j] - 8, jy = by[j] - 8;
                if (lx < jx + 16 && jx < lx + lw &&
                    ly < jy + 16 && jy < ly + lh) { ly = jy + 17; moved = true; }
            }
            for (int j = 0; j < i; j++) {
                if (lx < lrx[j] + lrw[j] && lrx[j] < lx + lw &&
                    ly < lry[j] + lrh[j] && lry[j] < ly + lh) {
                    ly = lry[j] + lrh[j] + 1;
                    moved = true;
                }
            }
            if (!moved) break;
        }
        if (lx < 2) lx = 2;
        if (lx + lw > s_radar_size - 2) lx = s_radar_size - 2 - lw;
        if (ly + lh > s_radar_size - 2) ly = s_radar_size - 2 - lh;
        if (ly < 2) ly = 2;
placed:
        lv_obj_set_pos(s_blip_lbl[i], lx, ly);
        lrx[i] = lx; lry[i] = ly; lrw[i] = lw; lrh[i] = lh;
    }
}
