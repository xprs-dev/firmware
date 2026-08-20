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

/* Stats: three stacked hourly bar charts. */
static lv_obj_t  *s_stats;
static lv_obj_t  *s_stats_chart[XUI_STATS_CHARTS];
static lv_obj_t  *s_stats_title[XUI_STATS_CHARTS];
static lv_chart_series_t *s_stats_series[XUI_STATS_CHARTS];
static lv_coord_t s_stats_vals[XUI_STATS_CHARTS][XUI_STATS_POINTS];

/* Chat: the station's web page, drawn natively. Its palette, so the two
 * faces of one station look like one station. */
#define XUI_C_ACCENT  lv_color_make(255, 168, 106)   /* #ffa86a */
#define XUI_C_PAGE    lv_color_make(16, 16, 16)      /* #101010 */
#define XUI_C_TEXT    lv_color_make(240, 240, 240)   /* #f0f0f0 */
#define XUI_C_MUTED   lv_color_make(136, 136, 136)   /* #888888 */
#define XUI_C_LINE    lv_color_make(44, 43, 42)      /* the hairline, flat */
#define XUI_C_IN      lv_color_make(27, 27, 27)      /* #1b1b1b */
#define XUI_C_OUT     lv_color_make(42, 28, 16)      /* #2a1c10 */
#define XUI_C_HEAD    lv_color_make(85, 85, 85)      /* #555555 */

#define XUI_ROOM_H     16    /* a rail row */
#define XUI_HEAD_H     16    /* the room header over the conversation */
#define XUI_COMPOSE_H  22    /* the composer band */

static lv_obj_t *s_chat;
static lv_obj_t *s_chat_rail;
static lv_obj_t *s_room_row[XUI_CHAT_ROOMS];
static lv_obj_t *s_room_lbl[XUI_CHAT_ROOMS];
static lv_obj_t *s_chat_head;
static lv_obj_t *s_chat_list;
static lv_obj_t *s_chat_input;
static lv_obj_t *s_chat_input_lbl;
static lv_obj_t *s_chat_send;
static int s_rail_w;
static int s_msgs_h;

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
     * base64, so a host script can reassemble a screenshot. Encoded in small
     * chunks (multiples of 3 bytes, so the pieces concatenate into one valid
     * base64 string) -- an earlier draft malloc'd the whole 40 KB slice and
     * died silently the day the free heap shrank below it. */
    if (s_dump_active) {
        size_t total = size * 2;
        size_t b64_total = ((total + 2) / 3) * 4;
        printf("SLICE %d %d %d %d %u\n",
               area->x1, area->y1, area->x2, area->y2, (unsigned)b64_total);
        static unsigned char b64[644];
        const unsigned char *raw = (const unsigned char *)px;
        for (size_t off = 0; off < total; off += 480) {
            size_t n = total - off > 480 ? 480 : total - off;
            size_t olen = 0;
            mbedtls_base64_encode(b64, sizeof b64, &olen, raw + off, n);
            fwrite(b64, 1, olen, stdout);
            fputc('\n', stdout);
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
        lv_mem_monitor_t mm;
        lv_mem_monitor(&mm);
        ESP_LOGI(TAG, "dump: scr=%p disp=%p lvmem free=%u frag=%u%% "
                 "biggest=%u", (void *)lv_scr_act(), (void *)s_disp,
                 (unsigned)mm.free_size, mm.frag_pct,
                 (unsigned)mm.free_biggest_size);
        esp_log_level_set("*", ESP_LOG_NONE);   /* keep the stream clean */
        printf("FRAMEDUMP BEGIN %d %d\n", s_w, s_h);
        s_dump_active = true;
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(s_disp);
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

    /* ---- The stats panel: three hourly bar charts, hidden until shown --- */
    s_stats = lv_obj_create(scr);
    lv_obj_remove_style_all(s_stats);
    lv_obj_set_size(s_stats, s_w, s_center_h);
    lv_obj_align(s_stats, LV_ALIGN_TOP_LEFT, 0, s_top_h);
    lv_obj_set_style_bg_color(s_stats, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_stats, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_stats, LV_OBJ_FLAG_SCROLLABLE);

    static const lv_color_t chart_col[XUI_STATS_CHARTS] = {
        LV_COLOR_MAKE(255, 140, 0),    /* devices: the top bar's orange */
        LV_COLOR_MAKE(0, 220, 60),     /* received: radar green */
        LV_COLOR_MAKE(70, 150, 255),   /* sent: the flow header's blue */
    };
    int slot_h = s_center_h / XUI_STATS_CHARTS;
    for (int i = 0; i < XUI_STATS_CHARTS; i++) {
        s_stats_title[i] = lv_label_create(s_stats);
        lv_label_set_text(s_stats_title[i], "");
        lv_obj_set_style_text_font(s_stats_title[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(s_stats_title[i],
                                    lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
        lv_obj_set_pos(s_stats_title[i], 8, i * slot_h + 2);

        lv_obj_t *ch = lv_chart_create(s_stats);
        s_stats_chart[i] = ch;
        lv_obj_set_size(ch, s_w - 16, slot_h - 18);
        lv_obj_set_pos(ch, 8, i * slot_h + 14);
        lv_obj_set_style_bg_color(ch, lv_color_make(10, 10, 10), 0);
        lv_obj_set_style_border_width(ch, 1, 0);
        lv_obj_set_style_border_color(ch, lv_color_make(50, 50, 50), 0);
        lv_obj_set_style_radius(ch, 2, 0);
        lv_obj_set_style_pad_all(ch, 2, 0);
        lv_obj_set_style_pad_column(ch, 1, LV_PART_ITEMS);
        lv_chart_set_type(ch, LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(ch, XUI_STATS_POINTS);
        lv_chart_set_div_line_count(ch, 3, 0);
        lv_obj_set_style_line_color(ch, lv_color_make(35, 35, 35), 0);
        s_stats_series[i] = lv_chart_add_series(ch, chart_col[i],
                                                LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_ext_y_array(ch, s_stats_series[i], s_stats_vals[i]);
    }
    lv_obj_add_flag(s_stats, LV_OBJ_FLAG_HIDDEN);

    /* ---- Chat: rail | conversation | composer ----
     *
     * The proportions come from the station's own web page. The rail is
     * narrow on purpose: it must stay legible without taking the room the
     * bubbles need, and at 320 px wide that is about a quarter. */
    s_chat = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chat);
    lv_obj_set_size(s_chat, s_w, s_center_h);
    lv_obj_align(s_chat, LV_ALIGN_TOP_LEFT, 0, s_top_h);
    lv_obj_set_style_bg_color(s_chat, XUI_C_PAGE, 0);
    lv_obj_set_style_bg_opa(s_chat, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE);

    /* The rail. */
    s_chat_rail = lv_obj_create(s_chat);
    lv_obj_remove_style_all(s_chat_rail);
    lv_obj_set_size(s_chat_rail, s_rail_w, s_center_h);
    lv_obj_set_pos(s_chat_rail, 0, 0);
    lv_obj_set_style_bg_color(s_chat_rail, XUI_C_PAGE, 0);
    lv_obj_set_style_bg_opa(s_chat_rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_chat_rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(s_chat_rail, 1, 0);
    lv_obj_set_style_border_color(s_chat_rail, XUI_C_LINE, 0);
    lv_obj_set_style_pad_all(s_chat_rail, 0, 0);
    lv_obj_clear_flag(s_chat_rail, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < XUI_CHAT_ROOMS; i++) {
        s_room_row[i] = lv_obj_create(s_chat_rail);
        lv_obj_remove_style_all(s_room_row[i]);
        lv_obj_set_size(s_room_row[i], s_rail_w - 1, XUI_ROOM_H);
        lv_obj_set_style_bg_opa(s_room_row[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_side(s_room_row[i], LV_BORDER_SIDE_LEFT, 0);
        lv_obj_set_style_border_color(s_room_row[i], XUI_C_ACCENT, 0);
        lv_obj_set_style_border_width(s_room_row[i], 0, 0);
        lv_obj_clear_flag(s_room_row[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(s_room_row[i], LV_OBJ_FLAG_HIDDEN);

        s_room_lbl[i] = lv_label_create(s_room_row[i]);
        lv_label_set_text(s_room_lbl[i], "");
        lv_obj_set_style_text_font(s_room_lbl[i], &lv_font_montserrat_12, 0);
        lv_obj_align(s_room_lbl[i], LV_ALIGN_LEFT_MID, 6, 0);
    }

    /* The room header, over the conversation. */
    s_chat_head = lv_label_create(s_chat);
    lv_label_set_text(s_chat_head, "");
    lv_obj_set_style_text_font(s_chat_head, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_chat_head, XUI_C_ACCENT, 0);
    lv_obj_set_pos(s_chat_head, s_rail_w + 6, 2);

    /* The conversation itself: a scrolling column of bubbles. */
    s_chat_list = lv_obj_create(s_chat);
    lv_obj_remove_style_all(s_chat_list);
    lv_obj_set_size(s_chat_list, s_w - s_rail_w, s_msgs_h);
    lv_obj_set_pos(s_chat_list, s_rail_w, XUI_HEAD_H);
    lv_obj_set_style_bg_color(s_chat_list, XUI_C_PAGE, 0);
    lv_obj_set_style_bg_opa(s_chat_list, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_chat_list, 4, 0);
    lv_obj_set_style_pad_row(s_chat_list, 3, 0);
    lv_obj_set_flex_flow(s_chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_chat_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat_list, LV_SCROLLBAR_MODE_OFF);

    /* The composer. */
    s_chat_input = lv_obj_create(s_chat);
    lv_obj_remove_style_all(s_chat_input);
    lv_obj_set_size(s_chat_input, s_w - s_rail_w - 8, XUI_COMPOSE_H - 6);
    lv_obj_set_pos(s_chat_input, s_rail_w + 4, XUI_HEAD_H + s_msgs_h + 2);
    lv_obj_set_style_bg_opa(s_chat_input, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_input, 1, 0);
    lv_obj_set_style_border_color(s_chat_input, XUI_C_LINE, 0);
    lv_obj_set_style_radius(s_chat_input, 4, 0);
    lv_obj_set_style_pad_all(s_chat_input, 0, 0);
    lv_obj_clear_flag(s_chat_input, LV_OBJ_FLAG_SCROLLABLE);

    s_chat_input_lbl = lv_label_create(s_chat_input);
    lv_label_set_text(s_chat_input_lbl, "");
    lv_obj_set_style_text_font(s_chat_input_lbl, &lv_font_montserrat_12, 0);
    lv_label_set_long_mode(s_chat_input_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(s_chat_input_lbl, s_w - s_rail_w - 36);
    lv_obj_align(s_chat_input_lbl, LV_ALIGN_LEFT_MID, 5, 0);

    /* The send glyph, the web page's single '>' in accent. */
    s_chat_send = lv_label_create(s_chat_input);
    lv_label_set_text(s_chat_send, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(s_chat_send, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_chat_send, XUI_C_ACCENT, 0);
    lv_obj_align(s_chat_send, LV_ALIGN_RIGHT_MID, -6, 0);

    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_HIDDEN);

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
    /* The chat rail: about a quarter of the width, which at 320 is the
     * narrowest a callsign stays readable at. */
    s_rail_w = s_w * 84 / 320;
    s_msgs_h = s_center_h - XUI_HEAD_H - XUI_COMPOSE_H;

    lv_init();

    /* An eighth of the screen: refresh crosses the panel in a few strokes
     * and the ~11 KB saved is what lets the indexer's writer task start on
     * a busy station. */
    int buf_rows = s_h / 8;
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

void xui_show_stats(bool show)
{
    if (!s_stats || !s_body_label) return;
    if (show) {
        lv_obj_clear_flag(s_stats, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lv_obj_get_parent(s_body_label), LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_stats, LV_OBJ_FLAG_HIDDEN);
    }
}

void xui_stats_set(int idx, const char *title, const uint16_t *vals, int n)
{
    if (idx < 0 || idx >= XUI_STATS_CHARTS || !s_stats_chart[idx]) return;
    if (n > XUI_STATS_POINTS) n = XUI_STATS_POINTS;

    uint16_t maxv = 0;
    for (int i = 0; i < n; i++) if (vals[i] > maxv) maxv = vals[i];

    /* Oldest first, right-aligned so "now" is always the rightmost bar. */
    int pad = XUI_STATS_POINTS - n;
    for (int i = 0; i < XUI_STATS_POINTS; i++)
        s_stats_vals[idx][i] = (i < pad) ? 0 : (lv_coord_t)vals[i - pad];

    /* Headroom keeps a full-scale hour from touching the frame. */
    uint16_t range = maxv < 4 ? 4 : maxv + maxv / 4 + 1;
    lv_chart_set_range(s_stats_chart[idx], LV_CHART_AXIS_PRIMARY_Y, 0, range);

    char buf[64];
    snprintf(buf, sizeof buf, "%s   (peak %u)", title, (unsigned)maxv);
    lv_label_set_text(s_stats_title[idx], buf);

    lv_chart_refresh(s_stats_chart[idx]);
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

/* ---- The chat panel ----------------------------------------------------- */

void xui_show_chat(bool show)
{
    if (!s_chat) return;
    if (show) {
        lv_obj_clear_flag(s_chat, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(lv_obj_get_parent(s_body_label), LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_chat, LV_OBJ_FLAG_HIDDEN);
    }
}

void xui_chat_rooms(const xui_room_t *rooms, int n, int sel)
{
    if (!s_chat_rail) return;
    if (n > XUI_CHAT_ROOMS) n = XUI_CHAT_ROOMS;
    if (n < 0) n = 0;

    for (int i = 0; i < XUI_CHAT_ROOMS; i++) {
        if (i >= n) {
            lv_obj_add_flag(s_room_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const xui_room_t *r = &rooms[i];
        lv_obj_clear_flag(s_room_row[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_room_row[i], 0, i * XUI_ROOM_H);
        /* The unread mark is a character, not an object: at 16 px a row it
         * reads the same and costs nothing from LVGL's pool. */
        if (r->unread && !r->heading)
            lv_label_set_text_fmt(s_room_lbl[i], "%s %s", r->name,
                                  LV_SYMBOL_BELL);
        else
            lv_label_set_text(s_room_lbl[i], r->name);

        if (r->heading) {
            /* A section label is not a destination: no rule, no highlight,
             * and the selection steps over it. */
            lv_obj_set_style_text_color(s_room_lbl[i], XUI_C_HEAD, 0);
            lv_obj_set_style_text_font(s_room_lbl[i], &lv_font_montserrat_10, 0);
            lv_obj_set_style_bg_opa(s_room_row[i], LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(s_room_row[i], 0, 0);
            continue;
        }

        lv_obj_set_style_text_font(s_room_lbl[i], &lv_font_montserrat_12, 0);
        if (i == sel) {
            lv_obj_set_style_text_color(s_room_lbl[i], XUI_C_ACCENT, 0);
            lv_obj_set_style_border_width(s_room_row[i], 2, 0);
            lv_obj_set_style_bg_color(s_room_row[i], XUI_C_OUT, 0);
            lv_obj_set_style_bg_opa(s_room_row[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_text_color(s_room_lbl[i], XUI_C_MUTED, 0);
            lv_obj_set_style_border_width(s_room_row[i], 0, 0);
            lv_obj_set_style_bg_opa(s_room_row[i], LV_OPA_TRANSP, 0);
        }
    }
}

void xui_chat_msgs(const xui_msg_t *msgs, int n, const char *header)
{
    if (!s_chat_list) return;
    lv_label_set_text(s_chat_head, header ? header : "");

    /* Rebuild rather than diff. The list is short by construction and a
     * bubble's height depends on how its text wrapped, so there is no
     * cheap way to know an existing one still fits. */
    lv_obj_clean(s_chat_list);
    if (n > XUI_CHAT_MSGS) {
        msgs += n - XUI_CHAT_MSGS;      /* keep the newest */
        n = XUI_CHAT_MSGS;
    }

    const int inner = s_w - s_rail_w - 8;
    const int maxw = inner * 85 / 100;   /* the web page's max-width: 85% */

    for (int i = 0; i < n; i++) {
        const xui_msg_t *m = &msgs[i];

        lv_obj_t *wrap = lv_obj_create(s_chat_list);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_width(wrap, inner);
        lv_obj_set_height(wrap, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_all(wrap, 0, 0);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(wrap, LV_FLEX_FLOW_COLUMN);
        /* In a column the cross axis is the horizontal one, and BOTH the
         * item placement and the track's own placement have to be told
         * which side to sit on -- setting only the first leaves the track
         * itself at the left, taking the bubble with it. */
        lv_flex_align_t side = m->outgoing ? LV_FLEX_ALIGN_END
                                           : LV_FLEX_ALIGN_START;
        lv_obj_set_flex_align(wrap, LV_FLEX_ALIGN_START, side, side);

        /* Incoming names itself above the bubble; outgoing puts its time
         * underneath and never repeats our own callsign. */
        if (!m->outgoing) {
            lv_obj_t *meta = lv_label_create(wrap);
            lv_label_set_text_fmt(meta, "%s  %s", m->from, m->when);
            lv_obj_set_style_text_font(meta, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(meta, XUI_C_ACCENT, 0);
        }

        lv_obj_t *bub = lv_obj_create(wrap);
        lv_obj_remove_style_all(bub);
        lv_obj_set_width(bub, LV_SIZE_CONTENT);
        lv_obj_set_height(bub, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(bub, m->outgoing ? XUI_C_OUT : XUI_C_IN, 0);
        lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bub, 8, 0);
        lv_obj_set_style_pad_hor(bub, 6, 0);
        lv_obj_set_style_pad_ver(bub, 3, 0);
        lv_obj_set_style_border_width(bub, 1, 0);
        lv_obj_set_style_border_color(bub, m->outgoing ? XUI_C_ACCENT
                                                       : XUI_C_LINE, 0);
        lv_obj_set_style_border_opa(bub, m->outgoing ? LV_OPA_40
                                                     : LV_OPA_COVER, 0);
        lv_obj_clear_flag(bub, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *t = lv_label_create(bub);
        lv_label_set_text(t, m->text);
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        /* The LABEL is what is bounded, and the bubble hugs it. Sizing the
         * bubble to its content while the label asks for a percentage OF
         * the bubble is circular, and LVGL resolves the circle by making
         * both as narrow as one character -- a tall thin ribbon down the
         * screen, which is exactly what the first build drew. */
        lv_obj_set_width(t, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(t, maxw - 14, 0);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(t, XUI_C_TEXT, 0);

        if (m->outgoing) {
            lv_obj_t *meta = lv_label_create(wrap);
            lv_label_set_text(meta, m->when);
            lv_obj_set_style_text_font(meta, &lv_font_montserrat_10, 0);
            lv_obj_set_style_text_color(meta, XUI_C_MUTED, 0);
        }
    }

    /* Newest at the bottom, which is where a conversation is read from. */
    lv_obj_update_layout(s_chat_list);
    lv_obj_scroll_to_y(s_chat_list, LV_COORD_MAX, LV_ANIM_OFF);
}

void xui_chat_input(const char *text, const char *placeholder, bool focused)
{
    if (!s_chat_input_lbl) return;
    bool empty = !text || !text[0];
    lv_label_set_text(s_chat_input_lbl,
                      empty ? (placeholder ? placeholder : "") : text);
    lv_obj_set_style_text_color(s_chat_input_lbl,
                                empty ? XUI_C_MUTED : XUI_C_TEXT, 0);
    lv_obj_set_style_border_color(s_chat_input,
                                  focused ? XUI_C_ACCENT : XUI_C_LINE, 0);
}
