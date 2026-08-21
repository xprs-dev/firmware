/* xprs_ui_mini.c -- see the header. The flush callback, framedump and
 * chart plumbing are the proven blocks from xprs_ui.c, shrunk to one
 * font and 80 pixels of height. */
#include "xprs_ui_mini.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "mbedtls/base64.h"

static const char *TAG = "xum";

#define TOP_H     13
#define BUF_ROWS  20

static int s_w, s_h;
static xum_flush_fn s_flush;
static void *s_flush_ctx;

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_disp_t         *s_disp;

/* Top bar */
static lv_obj_t *s_top;
static lv_obj_t *s_title_label;      /* view name + uptime */
static lv_obj_t *s_count_label;      /* devices in range + pause glyph */
static bool      s_held;
static int       s_count = -1;

/* Bodies, one per view */
static lv_obj_t *s_body[XUM_VIEW_COUNT];
static lv_obj_t *s_dev_label;
static lv_obj_t *s_chat_label;
static lv_obj_t *s_stats_chart[3];
static lv_obj_t *s_stats_tag[3];
static lv_chart_series_t *s_stats_series[3];
static lv_coord_t s_stats_vals[3][XUM_STATS_POINTS];
static lv_obj_t *s_stats_wait;

static int s_view = XUM_VIEW_DEVICES;

/* ---- LVGL flush callback (xprs_ui.c pattern, verbatim) ------------------ */

static volatile bool s_dump_pending;
static bool s_dump_active;

static void lcd_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area,
                         lv_color_t *color_p)
{
    uint32_t size = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    uint16_t *px = (uint16_t *)color_p;

    /* LVGL keeps RGB565 little-endian; the panel wants big-endian. */
    for (uint32_t i = 0; i < size; i++) {
        px[i] = (px[i] >> 8) | (px[i] << 8);
    }

    /* Debug frame dump: every slice of this refresh onto the UART as
     * base64, in pieces small enough to never strain the heap. */
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

void xum_framedump(void)
{
    s_dump_pending = true;
}

/* ---- build ---------------------------------------------------------------*/

static lv_obj_t *body_make(lv_obj_t *scr)
{
    lv_obj_t *b = lv_obj_create(scr);
    lv_obj_set_size(b, s_w, s_h - TOP_H);
    lv_obj_set_pos(b, 0, TOP_H);
    lv_obj_set_style_bg_color(b, lv_color_black(), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 0, 0);
    lv_obj_set_style_pad_all(b, 2, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    return b;
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);

    /* Top bar: the station's orange, same as the big UI. */
    s_top = lv_obj_create(scr);
    lv_obj_set_size(s_top, s_w, TOP_H);
    lv_obj_set_pos(s_top, 0, 0);
    lv_obj_set_style_bg_color(s_top, lv_color_make(0xE8, 0x7A, 0x00), 0);
    lv_obj_set_style_border_width(s_top, 0, 0);
    lv_obj_set_style_radius(s_top, 0, 0);
    lv_obj_set_style_pad_all(s_top, 0, 0);
    lv_obj_clear_flag(s_top, LV_OBJ_FLAG_SCROLLABLE);

    s_title_label = lv_label_create(s_top);
    lv_label_set_text(s_title_label, "XPRS");
    lv_obj_set_style_text_font(s_title_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_black(), 0);
    lv_obj_align(s_title_label, LV_ALIGN_LEFT_MID, 3, 0);

    s_count_label = lv_label_create(s_top);
    lv_label_set_text(s_count_label, "");
    lv_obj_set_style_text_font(s_count_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_count_label, lv_color_black(), 0);
    lv_obj_align(s_count_label, LV_ALIGN_RIGHT_MID, -3, 0);

    /* DEVICES */
    s_body[XUM_VIEW_DEVICES] = body_make(scr);
    s_dev_label = lv_label_create(s_body[XUM_VIEW_DEVICES]);
    lv_label_set_text(s_dev_label, "--");
    lv_obj_set_style_text_font(s_dev_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_dev_label, lv_color_white(), 0);
    lv_obj_set_width(s_dev_label, s_w - 6);
    lv_obj_set_pos(s_dev_label, 1, 0);

    /* STATS: three thin bar charts with a one-letter tag each. */
    s_body[XUM_VIEW_STATS] = body_make(scr);
    static const char *const tag[3] = { "D", "RX", "TX" };
    static const lv_palette_t col[3] = { LV_PALETTE_ORANGE,
                                         LV_PALETTE_GREEN, LV_PALETTE_BLUE };
    int slot_h = (s_h - TOP_H - 4) / 3;                  /* ~21 px each */
    for (int i = 0; i < 3; i++) {
        lv_obj_t *ch = lv_chart_create(s_body[XUM_VIEW_STATS]);
        s_stats_chart[i] = ch;
        lv_obj_set_size(ch, s_w - 22, slot_h - 1);
        lv_obj_set_pos(ch, 16, i * slot_h);
        lv_obj_set_style_bg_color(ch, lv_color_make(10, 10, 10), 0);
        lv_obj_set_style_border_width(ch, 1, 0);
        lv_obj_set_style_border_color(ch, lv_color_make(50, 50, 50), 0);
        lv_obj_set_style_radius(ch, 1, 0);
        lv_obj_set_style_pad_all(ch, 1, 0);
        lv_obj_set_style_pad_column(ch, 1, LV_PART_ITEMS);
        lv_chart_set_type(ch, LV_CHART_TYPE_BAR);
        lv_chart_set_point_count(ch, XUM_STATS_POINTS);
        lv_chart_set_div_line_count(ch, 0, 0);
        s_stats_series[i] = lv_chart_add_series(ch, lv_palette_main(col[i]),
                                                LV_CHART_AXIS_PRIMARY_Y);
        lv_chart_set_ext_y_array(ch, s_stats_series[i], s_stats_vals[i]);

        s_stats_tag[i] = lv_label_create(s_body[XUM_VIEW_STATS]);
        lv_label_set_text(s_stats_tag[i], tag[i]);
        lv_obj_set_style_text_font(s_stats_tag[i], &lv_font_montserrat_10, 0);
        lv_obj_set_style_text_color(s_stats_tag[i],
                                    lv_palette_main(col[i]), 0);
        lv_obj_set_pos(s_stats_tag[i], 0, i * slot_h + (slot_h - 12) / 2);
    }
    s_stats_wait = lv_label_create(s_body[XUM_VIEW_STATS]);
    lv_label_set_text(s_stats_wait, "waiting for time");
    lv_obj_set_style_text_font(s_stats_wait, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_stats_wait,
                                lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
    lv_obj_align(s_stats_wait, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(s_stats_wait, LV_OBJ_FLAG_HIDDEN);

    /* CHAT */
    s_body[XUM_VIEW_CHAT] = body_make(scr);
    s_chat_label = lv_label_create(s_body[XUM_VIEW_CHAT]);
    lv_label_set_text(s_chat_label, "no messages yet");
    lv_obj_set_style_text_font(s_chat_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(s_chat_label, lv_color_white(), 0);
    lv_obj_set_width(s_chat_label, s_w - 6);
    lv_obj_set_pos(s_chat_label, 1, 0);

    lv_obj_clear_flag(s_body[XUM_VIEW_DEVICES], LV_OBJ_FLAG_HIDDEN);
}

/* ---- public API ----------------------------------------------------------*/

esp_err_t xum_init(int width, int height, xum_flush_fn flush, void *ctx)
{
    if (width < 120 || height < 64 || !flush) return ESP_ERR_INVALID_ARG;
    s_w = width;
    s_h = height;
    s_flush = flush;
    s_flush_ctx = ctx;

    lv_init();

    /* Partial draw buffer: BUF_ROWS rows, not the whole frame -- on a
     * 15 KB-free board the difference is the whole feature. */
    static lv_color_t *buf1;
    size_t px = (size_t)s_w * BUF_ROWS;
    buf1 = heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_DMA);
    /* INTERNAL, not a bare malloc: under CONFIG_SPIRAM_USE_MALLOC that
     * would be allowed to answer out of PSRAM, and this buffer is handed
     * to the SPI DMA engine. Identical on a board without PSRAM. */
    if (!buf1) buf1 = heap_caps_malloc(px * sizeof(lv_color_t),
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf1) return ESP_ERR_NO_MEM;
    lv_disp_draw_buf_init(&s_draw_buf, buf1, NULL, px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = s_w;
    s_disp_drv.ver_res = s_h;
    s_disp_drv.flush_cb = lcd_flush_cb;
    s_disp_drv.draw_buf = &s_draw_buf;
    s_disp = lv_disp_drv_register(&s_disp_drv);

    lv_theme_t *th = lv_theme_default_init(s_disp,
                                           lv_palette_main(LV_PALETTE_ORANGE),
                                           lv_palette_main(LV_PALETTE_GREY),
                                           true, &lv_font_montserrat_10);
    lv_disp_set_theme(s_disp, th);

    build_ui();
    ESP_LOGI(TAG, "mini UI up (%dx%d, %u B draw buffer)", s_w, s_h,
             (unsigned)(px * sizeof(lv_color_t)));
    return ESP_OK;
}

static uint64_t s_last_tick_us;
static uint32_t s_uptime_last;

void xum_update(void)
{
    /* The managed LVGL component does not see an lv_conf.h, so the tick
     * advances by hand (same as xprs_ui / tdongle_ui). */
    uint64_t now_us = esp_timer_get_time();
    uint32_t elapsed_ms = (uint32_t)((now_us - s_last_tick_us) / 1000);
    if (elapsed_ms > 0) {
        lv_tick_inc(elapsed_ms);
        s_last_tick_us = now_us;
    }

    /* Title once a second: view name + uptime. */
    uint32_t up = (uint32_t)(now_us / 1000000);
    if (up != s_uptime_last) {
        s_uptime_last = up;
        static const char *const names[XUM_VIEW_COUNT] = {
            "Devices", "Stats", "Chat"
        };
        char t[40];
        snprintf(t, sizeof t, "%s%s  %02lu:%02lu:%02lu",
                 names[s_view], s_held ? " ||" : "",
                 (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
                 (unsigned long)(up % 60));
        lv_label_set_text(s_title_label, t);

        /* Once a minute, say how much of the LVGL pool this screen
         * actually uses. The pool is static RAM taken from the heap's
         * total before anything else runs, and on this board that heap is
         * the scarcest thing there is -- so it gets sized from this line
         * and never from a guess. docs/esp32.md says the same, and says
         * it because a guess in the other direction once cost the
         * M5Stack a 70-second reboot loop. */
        if ((up % 60) == 0) {
            lv_mem_monitor_t mm;
            lv_mem_monitor(&mm);
            ESP_LOGI(TAG, "lvmem free=%u (largest %u) frag=%u%% used=%u%%",
                     (unsigned)mm.free_size, (unsigned)mm.free_biggest_size,
                     (unsigned)mm.frag_pct, (unsigned)mm.used_pct);
        }
    }

    lv_timer_handler();

    if (s_dump_pending) {
        s_dump_pending = false;
        esp_log_level_set("*", ESP_LOG_NONE);   /* keep the stream clean */
        printf("FRAMEDUMP BEGIN %d %d\n", s_w, s_h);
        s_dump_active = true;
        lv_obj_invalidate(lv_scr_act());
        lv_refr_now(s_disp);
        s_dump_active = false;
        printf("FRAMEDUMP END\n");
        esp_log_level_set("*", ESP_LOG_INFO);
    }
}

void xum_show(int view)
{
    if (view < 0 || view >= XUM_VIEW_COUNT) return;
    for (int i = 0; i < XUM_VIEW_COUNT; i++) {
        if (i == view) lv_obj_clear_flag(s_body[i], LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(s_body[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_view = view;
    s_uptime_last = 0;      /* refresh the title now */
}

int xum_view(void) { return s_view; }

void xum_set_held(bool held)
{
    s_held = held;
    s_uptime_last = 0;
}

void xum_devices(const xum_dev_t *rows, int n)
{
    if (!s_dev_label) return;
    char buf[XUM_DEV_ROWS * 40 + 8];
    int o = 0;
    if (n > XUM_DEV_ROWS) n = XUM_DEV_ROWS;
    for (int i = 0; i < n; i++) {
        char d[16];
        if (rows[i].dist_m >= 0)
            snprintf(d, sizeof d, "~%dm", rows[i].dist_m);
        else
            snprintf(d, sizeof d, "-");
        o += snprintf(buf + o, sizeof buf - o, "%s%-9s %-6s %s",
                      i ? "\n" : "", rows[i].call, rows[i].bearer, d);
        if (o >= (int)sizeof buf - 1) break;
    }
    if (!n) snprintf(buf, sizeof buf, "nobody in range");
    lv_label_set_text(s_dev_label, buf);
}

void xum_stats(const uint16_t *dev, const uint16_t *rx, const uint16_t *tx,
               int n, const char *suffix)
{
    (void)suffix;
    if (n > XUM_STATS_POINTS) n = XUM_STATS_POINTS;
    const uint16_t *src[3] = { dev, rx, tx };
    if (!n) {
        lv_obj_clear_flag(s_stats_wait, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 3; i++)
            lv_obj_add_flag(s_stats_chart[i], LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_add_flag(s_stats_wait, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 3; i++) {
        lv_obj_clear_flag(s_stats_chart[i], LV_OBJ_FLAG_HIDDEN);
        uint16_t maxv = 0;
        for (int k = 0; k < n; k++) if (src[i][k] > maxv) maxv = src[i][k];
        int pad = XUM_STATS_POINTS - n;
        for (int k = 0; k < XUM_STATS_POINTS; k++)
            s_stats_vals[i][k] = (k < pad) ? 0
                                 : (lv_coord_t)src[i][k - pad];
        uint16_t range = maxv < 4 ? 4 : maxv + maxv / 4 + 1;
        lv_chart_set_range(s_stats_chart[i], LV_CHART_AXIS_PRIMARY_Y, 0,
                           range);
        lv_chart_refresh(s_stats_chart[i]);
    }
}

void xum_chat(const xum_chat_t *rows, int n)
{
    if (!s_chat_label) return;
    char buf[XUM_CHAT_ROWS * 80 + 8];
    int o = 0;
    if (n > XUM_CHAT_ROWS) n = XUM_CHAT_ROWS;
    /* rows arrive newest first; show them oldest first, newest at the
     * bottom, the way a chat reads. */
    for (int i = n - 1; i >= 0; i--) {
        const char *k = rows[i].kind == 1 ? "*"        /* scope:local */
                        : rows[i].kind == 2 ? ">"      /* a 1:1 */
                                            : "";      /* global */
        o += snprintf(buf + o, sizeof buf - o, "%s%s%s: %.48s",
                      (i == n - 1) ? "" : "\n", k, rows[i].from,
                      rows[i].text);
        if (o >= (int)sizeof buf - 1) break;
    }
    if (!n) snprintf(buf, sizeof buf, "no messages yet");
    lv_label_set_text(s_chat_label, buf);
}

void xum_set_count(int devices)
{
    if (devices == s_count || !s_count_label) return;
    s_count = devices;
    char b[16];
    snprintf(b, sizeof b, "%d dev", devices);
    lv_label_set_text(s_count_label, b);
}
