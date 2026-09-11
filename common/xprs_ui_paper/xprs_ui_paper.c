/*
 * xprs_ui_paper.c: the station UI interface (xprs_ui.h), on e-paper.
 *
 * xprs_app talks to every screen through the thirty-odd xui_* calls in
 * xprs_ui.h. On a 320x240 colour panel they land in xprs_ui and become seven
 * panels; on the T-Dongle's 160x80 strip they land in xprs_ui_mini and
 * become three rotating views. Here they become ONE black-and-white page
 * for a 200x200 e-paper panel: the room's temperature and humidity, who is
 * in reach, the newest messages, and whether the station is on the network.
 *
 * WHERE THE DATA COMES FROM. The app only fills in the panel it is showing,
 * and no single panel carries all of the above: the home panel has the
 * stations in reach, the chat panel has the messages. A board with this UI
 * therefore runs the hands-off tour (xapp_board_t.rotate), which walks from
 * home to stats to chat every 30 s, and every setter here PARKS what it is
 * given.
 * xui_set_panel(), called once per render pass after the setters, is what
 * says which panel the pass was for, and so which parked rows are messages
 * (the chat table and the device table are the same xui_table_rows() call).
 * The page always shows the latest of each, so it is at most one tour old.
 *
 * WHAT REACHES THE PANEL. See xprs_ui_paper.h: LVGL renders the whole page
 * into one frame, every pixel is thresholded to black or white in place,
 * and the board's flush is called once per real change, no more often than
 * XUP_MIN_COMMIT_MS. Nothing on the page changes for its own sake (no
 * seconds, no packet counter, no pulsing RX dot, distances in coarse steps),
 * because on this glass every change is a refresh somebody sees.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "lvgl.h"
#include "mbedtls/base64.h"

#include "xprs_station.h"
#include "xprs_ui.h"
#include "xprs_ui_paper.h"

static const char *TAG = "xui_paper";

/* Grey below this is ink. Montserrat is anti-aliased; at 128 the thin
 * strokes of a 12 px face break up, a little darker keeps them whole. */
#define XUP_INK_BELOW 150

#define XUP_DEVS 4      /* stations shown, two columns of two */
#define XUP_MSGS 2      /* newest messages shown */

/* ── The frame ─────────────────────────────────────────────────────────── */

static int s_w, s_h, s_row_bytes;
static xui_flush_fn s_flush;
static void *s_flush_ctx;
static bool s_flush_on = true;

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t s_drv;
static lv_disp_t *s_disp;
static lv_color_t *s_fb;       /* the whole screen, LVGL draws into it     */
static uint8_t *s_pack;        /* s_fb as one bit per pixel, 1 = white     */
static uint8_t *s_glass;       /* what the panel was last given            */
static bool s_changed;         /* s_pack differs from s_glass              */
static bool s_committed_once;
static uint32_t s_commit_ms;
static uint32_t s_commits;
static volatile bool s_dump_pending;
static uint64_t s_last_tick_us;
static uint32_t s_activity_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static inline bool is_ink(uint16_t v)
{
    unsigned r = (v >> 11) & 0x1F, g = (v >> 5) & 0x3F, b = v & 0x1F;
    unsigned r8 = (r << 3) | (r >> 2), g8 = (g << 2) | (g >> 4),
             b8 = (b << 3) | (b >> 2);
    return ((r8 * 77 + g8 * 150 + b8 * 29) >> 8) < XUP_INK_BELOW;
}

/* full_refresh is on, so every call is the whole screen. Threshold it in
 * place and pack it for the comparison. Black and white are 0x0000 and
 * 0xFFFF, which read the same in either byte order, so the board and the
 * screenshot both get exactly what the glass will show. */
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px)
{
    (void)area;
    uint16_t *p = (uint16_t *)px;
    memset(s_pack, 0, (size_t)s_row_bytes * s_h);
    for (int y = 0; y < s_h; y++) {
        uint8_t *row = s_pack + (size_t)y * s_row_bytes;
        for (int x = 0; x < s_w; x++, p++) {
            if (is_ink(*p)) {
                *p = 0x0000;
            } else {
                *p = 0xFFFF;
                row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
            }
        }
    }
    s_changed = memcmp(s_pack, s_glass, (size_t)s_row_bytes * s_h) != 0;
    lv_disp_flush_ready(drv);
}

static void commit(void)
{
    memcpy(s_glass, s_pack, (size_t)s_row_bytes * s_h);
    s_changed = false;
    s_committed_once = true;
    s_commit_ms = now_ms();
    s_commits++;
    if (s_flush) s_flush(0, 0, s_w - 1, s_h - 1, (const uint16_t *)s_fb,
                         s_flush_ctx);
}

/* One row of the glass as RGB565, for a screenshot. */
static void glass_row(int y, uint16_t *out)
{
    const uint8_t *row = s_glass + (size_t)y * s_row_bytes;
    for (int x = 0; x < s_w; x++)
        out[x] = (row[x >> 3] & (0x80 >> (x & 7))) ? 0xFFFF : 0x0000;
}

/* ── What the app has said, parked until the page is redrawn ───────────── */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_dirty = true;

static char s_call[16];

static bool     s_clim_ok;
static float    s_clim_t, s_clim_rh;
static uint32_t s_clim_seq, s_clim_seen;

static int  s_batt_pct = -1;
static bool s_batt_chg;

/* A station in reach and the distance step shown for it. A new step is only
 * shown once two readings in a row agree on it: an estimate sitting on the
 * line between ~5m and ~10m would otherwise flip on every beacon. */
typedef struct { char call[12]; int8_t step, pend; } pdev_t;
static pdev_t s_dev[XUI_BLIP_MAX];
static int   s_dev_n, s_reach = -1;

typedef struct { char from[10]; char when[8]; char text[120]; } pmsg_t;
static pmsg_t s_tab[XUP_MSGS];  /* parked from the last table pass */
static int   s_tab_n;
static pmsg_t s_msg[XUP_MSGS];  /* the chat panel's, once one was seen */
static int   s_msg_n = -1;     /* -1: no chat pass yet */

static bool s_wifi_up;
static char s_wifi_note[32];

/* ── Widgets ───────────────────────────────────────────────────────────── */

static lv_obj_t *s_lbl_call, *s_lbl_batt;
static lv_obj_t *s_lbl_temp, *s_deg, *s_lbl_unit, *s_lbl_rh, *s_lbl_rh_cap;
static lv_obj_t *s_lbl_reach;
static lv_obj_t *s_lbl_dev[XUP_DEVS];
static lv_obj_t *s_lbl_msg_head, *s_lbl_msg_text;   /* the newest message */
static lv_obj_t *s_lbl_msg_prev;                     /* the one before it  */
static lv_obj_t *s_lbl_net, *s_lbl_clock;
static int s_clock_min = -1;

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, lv_color_t col,
                       int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    lv_obj_set_pos(l, x, y);
    lv_label_set_text(l, "");
    return l;
}

static void rule(lv_obj_t *parent, int y)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, s_w - 8, 2);
    lv_obj_set_pos(r, 4, y);
    lv_obj_set_style_bg_color(r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
}

/* Only a real change is set: lv_label_set_text() invalidates even when the
 * text is the same, and here an invalidation is a refresh. */
static void set_text(lv_obj_t *l, const char *t)
{
    if (strcmp(lv_label_get_text(l), t) != 0) lv_label_set_text(l, t);
}

/*
 *   0 +------------------------------+
 *     | X3ABCD              [##] 87% |  header, inverted
 *  20 +------------------------------+
 *     | 23.4 C              45%      |  climate
 *     |                     humidity |
 *  62 +==============================+
 *     | 4 IN REACH                   |
 *     | X1ARKL ~5m     X16JK8 ~10m   |
 *     | X1WATT ~20m    X3S7S8 ~10m   |
 * 110 +==============================+
 *     | X1ARKL  5m                   |  newest message: who, when,
 *     | meet at the pier and bring   |  and two lines of what
 *     | the ...                      |
 *     | X16JK8: try two here         |  the one before, on one line
 * 181 +==============================+
 *     | (wifi) 192.168.178.140 14:32 |  footer
 * 200 +------------------------------+
 */
static void build(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_color_t ink = lv_color_black(), paper = lv_color_white();

    lv_obj_t *bar = lv_obj_create(scr);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, s_w, 20);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, ink, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    s_lbl_call = label(bar, &lv_font_montserrat_16, paper, 5, 1);
    s_lbl_batt = label(bar, &lv_font_montserrat_14, paper, 0, 2);
    lv_obj_align(s_lbl_batt, LV_ALIGN_TOP_RIGHT, -5, 2);

    s_lbl_temp = label(scr, &lv_font_montserrat_32, ink, 6, 23);
    s_deg = lv_obj_create(scr);
    lv_obj_remove_style_all(s_deg);
    lv_obj_set_size(s_deg, 8, 8);
    lv_obj_set_style_radius(s_deg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(s_deg, 2, 0);
    lv_obj_set_style_border_color(s_deg, ink, 0);
    lv_obj_set_style_border_opa(s_deg, LV_OPA_COVER, 0);
    s_lbl_unit = label(scr, &lv_font_montserrat_20, ink, 0, 0);
    lv_label_set_text(s_lbl_unit, "C");
    s_lbl_rh = label(scr, &lv_font_montserrat_20, ink, 0, 26);
    lv_obj_align(s_lbl_rh, LV_ALIGN_TOP_RIGHT, -6, 25);
    s_lbl_rh_cap = label(scr, &lv_font_montserrat_12, ink, 0, 46);
    lv_label_set_text(s_lbl_rh_cap, "humidity");
    lv_obj_align(s_lbl_rh_cap, LV_ALIGN_TOP_RIGHT, -6, 45);

    rule(scr, 62);
    s_lbl_reach = label(scr, &lv_font_montserrat_12, ink, 6, 66);
    for (int i = 0; i < XUP_DEVS; i++) {
        /* 12 px: at 14 a seven-character callsign and "~10m" do not fit
         * half of 200 px, and the second column lost its unit. */
        s_lbl_dev[i] = label(scr, &lv_font_montserrat_12, ink,
                             6 + (i % 2) * (s_w / 2), 81 + (i / 2) * 14);
        lv_label_set_long_mode(s_lbl_dev[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_width(s_lbl_dev[i], s_w / 2 - 8);
    }

    /* The newest message gets two lines: one line of 16 px held about
     * fifteen characters, which is rarely the point of a message. */
    rule(scr, 110);
    s_lbl_msg_head = label(scr, &lv_font_montserrat_12, ink, 6, 114);
    s_lbl_msg_text = label(scr, &lv_font_montserrat_14, ink, 6, 127);
    lv_label_set_long_mode(s_lbl_msg_text, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_msg_text, s_w - 12, 32);
    s_lbl_msg_prev = label(scr, &lv_font_montserrat_12, ink, 6, 162);
    lv_label_set_long_mode(s_lbl_msg_prev, LV_LABEL_LONG_DOT);
    lv_obj_set_size(s_lbl_msg_prev, s_w - 12, 15);

    rule(scr, 181);
    s_lbl_net = label(scr, &lv_font_montserrat_12, ink, 6, 185);
    lv_label_set_long_mode(s_lbl_net, LV_LABEL_LONG_DOT);
    lv_obj_set_height(s_lbl_net, 15);   /* one line; the width is set in apply() */
    s_lbl_clock = label(scr, &lv_font_montserrat_12, ink, 0, 185);
    lv_obj_align(s_lbl_clock, LV_ALIGN_TOP_RIGHT, -6, 185);
}

/* The wall clock, in the station's own offset (xst_tz: pinned, or found
 * out over the internet, daylight saving included), applied here rather
 * than through the process-wide TZ, which nothing on the board sets.
 * *utc is true while the zone is not known yet. False until NTP has set
 * the clock. */
static bool clock_now(struct tm *tm, bool *utc)
{
    time_t now = time(NULL);
    if (now < 1704067200) return false;         /* before 2024: no clock yet */
    bool known;
    now += xst_tz(&known);
    *utc = !known;
    gmtime_r(&now, tm);
    return true;
}

/* The walk-up hotspot's name, when it is up. The app does not say (the
 * home panel's rows are about the LAN), so ask the WiFi driver, which knows;
 * before WiFi has started it answers "not initialised" and this says no. */
static bool hotspot_ssid(char *out, size_t n)
{
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK ||
        (mode != WIFI_MODE_AP && mode != WIFI_MODE_APSTA))
        return false;
    wifi_config_t wc;
    if (esp_wifi_get_config(WIFI_IF_AP, &wc) != ESP_OK) return false;
    int len = wc.ap.ssid_len ? wc.ap.ssid_len : (int)strnlen((const char *)wc.ap.ssid, 32);
    snprintf(out, n, "%.*s", len, (const char *)wc.ap.ssid);
    return out[0] != 0;
}

/* A distance as the nearest of a few steps on a log scale. An RSSI estimate
 * wanders by a metre or two between beacons, and "~8m" becoming "~9m" would
 * be a refresh that tells nobody anything. -1 is unknown, 6 is "100m+". */
static const int k_step_m[] = { 2, 5, 10, 20, 50, 100 };

static int8_t dist_step(float m)
{
    if (m < 0) return -1;
    if (m >= 150) return 6;
    float lm = logf(m < 1 ? 1 : m);
    int8_t best = 0;
    for (int8_t i = 1; i < 6; i++)
        if (fabsf(lm - logf((float)k_step_m[i])) <
            fabsf(lm - logf((float)k_step_m[best])))
            best = i;
    return best;
}

static void dist_text(char *out, size_t n, int8_t step)
{
    if (step < 0)       snprintf(out, n, "?");
    else if (step >= 6) snprintf(out, n, "100m+");
    else                snprintf(out, n, "~%dm", k_step_m[step]);
}

static int by_call(const void *a, const void *b)
{
    return strcmp(((const pdev_t *)a)->call, ((const pdev_t *)b)->call);
}

static void apply(void)
{
    char buf[160];

    portENTER_CRITICAL(&s_mux);
    snprintf(buf, sizeof buf, "%s", s_call);
    bool clim_ok = s_clim_ok;
    float t = s_clim_t, rh = s_clim_rh;
    portEXIT_CRITICAL(&s_mux);

    set_text(s_lbl_call, buf[0] ? buf : "XPRS");

    if (s_batt_pct >= 0) {
        const char *sym = s_batt_chg ? LV_SYMBOL_CHARGE
                        : s_batt_pct > 80 ? LV_SYMBOL_BATTERY_FULL
                        : s_batt_pct > 55 ? LV_SYMBOL_BATTERY_3
                        : s_batt_pct > 30 ? LV_SYMBOL_BATTERY_2
                        : s_batt_pct > 10 ? LV_SYMBOL_BATTERY_1
                                          : LV_SYMBOL_BATTERY_EMPTY;
        snprintf(buf, sizeof buf, "%s %d%%", sym, s_batt_pct);
        set_text(s_lbl_batt, buf);
    } else {
        set_text(s_lbl_batt, "");
    }

    /* Hysteresis: a room does not change by a tenth of a degree in any way
     * worth a refresh, and the sensor's last digit does, all the time. */
    static float shown_t = NAN;
    static int shown_rh = -1;
    if (clim_ok) {
        if (isnan(shown_t) || fabsf(t - shown_t) >= 0.3f) shown_t = t;
        int r = (int)lroundf(rh);
        if (shown_rh < 0 || abs(r - shown_rh) >= 2) shown_rh = r;
        snprintf(buf, sizeof buf, "%.1f", (double)shown_t);
        set_text(s_lbl_temp, buf);
        snprintf(buf, sizeof buf, "%d%%", shown_rh);
        set_text(s_lbl_rh, buf);
    } else {
        shown_t = NAN;
        shown_rh = -1;
        set_text(s_lbl_temp, "--");
        set_text(s_lbl_rh, "--");
    }
    /* The degree sign is drawn, not typed: the built-in Montserrat covers
     * ASCII and LVGL's symbols, and U+00B0 is neither. */
    lv_obj_update_layout(s_lbl_temp);
    lv_obj_align_to(s_deg, s_lbl_temp, LV_ALIGN_OUT_RIGHT_TOP, 3, 5);
    lv_obj_align_to(s_lbl_unit, s_deg, LV_ALIGN_OUT_RIGHT_TOP, 1, -1);

    int reach = s_reach >= 0 ? s_reach : s_dev_n;
    if (reach == 0)       snprintf(buf, sizeof buf, "NOBODY IN REACH");
    else if (reach == 1)  snprintf(buf, sizeof buf, "1 STATION IN REACH");
    else                  snprintf(buf, sizeof buf, "%d STATIONS IN REACH", reach);
    set_text(s_lbl_reach, buf);
    /* The app's first four, which are the ones it ranks highest, but placed
     * by callsign: in the app's order they swap places whenever somebody
     * else was heard last, and every swap is a refresh. */
    pdev_t shown[XUP_DEVS];
    int ns = s_dev_n < XUP_DEVS ? s_dev_n : XUP_DEVS;
    memcpy(shown, s_dev, sizeof(pdev_t) * ns);
    qsort(shown, ns, sizeof shown[0], by_call);
    for (int i = 0; i < XUP_DEVS; i++) {
        if (i < ns) {
            char d[8];
            dist_text(d, sizeof d, shown[i].step);
            snprintf(buf, sizeof buf, "%.7s %s", shown[i].call, d);
        } else {
            buf[0] = 0;
        }
        set_text(s_lbl_dev[i], buf);
    }

    if (s_msg_n > 0) {
        snprintf(buf, sizeof buf, "%s  %s", s_msg[0].from, s_msg[0].when);
        set_text(s_lbl_msg_head, buf);
        set_text(s_lbl_msg_text, s_msg[0].text);
    } else {
        set_text(s_lbl_msg_head, s_msg_n == 0 ? "No messages yet" : "");
        set_text(s_lbl_msg_text, "");
    }
    if (s_msg_n > 1) {
        snprintf(buf, sizeof buf, "%s: %s", s_msg[1].from, s_msg[1].text);
        set_text(s_lbl_msg_prev, buf);
    } else {
        set_text(s_lbl_msg_prev, "");
    }

    struct tm tm;
    bool utc;
    if (clock_now(&tm, &utc)) {
        /* An unknown zone is said out loud: 07:40 on a wall in Berlin is
         * wrong unless it says what it is. */
        snprintf(buf, sizeof buf, "%02d:%02d%s", tm.tm_hour, tm.tm_min,
                 utc ? " UTC" : "");
        set_text(s_lbl_clock, buf);
    } else {
        set_text(s_lbl_clock, "");      /* no clock yet: say nothing */
    }

    /* The network line gets whatever the clock leaves, and ends in "..."
     * rather than running into it. Resized only when that changes: a new
     * width is an invalidation, and here an invalidation is a refresh. */
    static int net_w;
    lv_obj_update_layout(s_lbl_clock);
    int cw = lv_obj_get_width(s_lbl_clock);
    int w = s_w - 12 - (cw > 0 ? cw + 6 : 0);
    if (w != net_w) { net_w = w; lv_obj_set_width(s_lbl_net, w); }

    char ap[33];
    if (s_wifi_up)
        snprintf(buf, sizeof buf, LV_SYMBOL_WIFI " %s", s_wifi_note);
    else if (hotspot_ssid(ap, sizeof ap))
        /* No LAN: what a person holding a phone needs is the network to
         * join, said as the thing to do. */
        snprintf(buf, sizeof buf, "join %s", ap);
    else if (s_wifi_note[0])
        snprintf(buf, sizeof buf, "WiFi %s", s_wifi_note);
    else
        snprintf(buf, sizeof buf, "no WiFi");
    set_text(s_lbl_net, buf);
}

/* ── Bring-up ──────────────────────────────────────────────────────────── */

esp_err_t xui_init(int width, int height, xui_flush_fn flush, void *ctx)
{
    /* 256 wide at most: the screenshot paths copy rows into static
     * buffers of that size. */
    if (width < 200 || height < 200 || width > 256 || width % 8) {
        ESP_LOGE(TAG, "%dx%d: this page is drawn for a 200x200 panel",
                 width, height);
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_w = width;
    s_h = height;
    s_row_bytes = width / 8;
    s_flush = flush;
    s_flush_ctx = ctx;

    /* The frame is 80 KB at 200x200: PSRAM, because it is only ever touched
     * by the CPU (the board packs it to 5 KB before anything goes near
     * SPI). Internal RAM is the fallback, and on a board without PSRAM it
     * is a large ask that is better refused loudly than half-granted. */
    size_t fb_bytes = (size_t)width * height * sizeof(lv_color_t);
    size_t pk_bytes = (size_t)s_row_bytes * height;
    s_fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb) s_fb = heap_caps_malloc(fb_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_pack = heap_caps_malloc(pk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_pack) s_pack = malloc(pk_bytes);
    s_glass = heap_caps_malloc(pk_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_glass) s_glass = malloc(pk_bytes);
    if (!s_fb || !s_pack || !s_glass) {
        ESP_LOGE(TAG, "frame: %u + 2 x %u bytes refused (internal free %u)",
                 (unsigned)fb_bytes, (unsigned)pk_bytes,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return ESP_ERR_NO_MEM;
    }
    memset(s_glass, 0xFF, pk_bytes);

    lv_init();
    lv_disp_draw_buf_init(&s_draw_buf, s_fb, NULL, (uint32_t)width * height);
    lv_disp_drv_init(&s_drv);
    s_drv.hor_res = width;
    s_drv.ver_res = height;
    s_drv.flush_cb = flush_cb;
    s_drv.draw_buf = &s_draw_buf;
    s_drv.full_refresh = 1;
    s_disp = lv_disp_drv_register(&s_drv);
    lv_disp_set_theme(s_disp, NULL);

    build();
    s_last_tick_us = esp_timer_get_time();
    s_activity_ms = now_ms();
    ESP_LOGI(TAG, "e-paper page %dx%d, frame in %s", width, height,
             esp_ptr_external_ram(s_fb) ? "PSRAM" : "internal RAM");
    return ESP_OK;
}

/* ── The pump ──────────────────────────────────────────────────────────── */

static void framedump_now(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);   /* keep the stream clean */
    /* On a line of its own: another task may be half-way through printing
     * one, and framedump.py only knows the marker at the start of a line. */
    printf("\nFRAMEDUMP BEGIN %d %d\n", s_w, s_h);
    /* Ten rows a slice, each encoded in 480-byte pieces: multiples of 3, so
     * the pieces concatenate into one valid base64 string. */
    static uint16_t rows[10 * 256];
    static unsigned char b64[644];
    for (int y0 = 0; y0 < s_h; y0 += 10) {
        int n = s_h - y0 < 10 ? s_h - y0 : 10;
        for (int r = 0; r < n; r++) glass_row(y0 + r, rows + r * s_w);
        size_t total = (size_t)n * s_w * 2;
        printf("SLICE 0 %d %d %d %u\n", y0, s_w - 1, y0 + n - 1,
               (unsigned)(((total + 2) / 3) * 4));
        const unsigned char *raw = (const unsigned char *)rows;
        for (size_t off = 0; off < total; off += 480) {
            size_t len = total - off > 480 ? 480 : total - off, olen = 0;
            mbedtls_base64_encode(b64, sizeof b64, &olen, raw + off, len);
            fwrite(b64, 1, olen, stdout);
            fputc('\n', stdout);
        }
    }
    printf("FRAMEDUMP END\n");
    fflush(stdout);
    esp_log_level_set("*", ESP_LOG_INFO);
}

void xui_update(void)
{
    uint64_t now_us = esp_timer_get_time();
    uint32_t elapsed = (uint32_t)((now_us - s_last_tick_us) / 1000);
    if (elapsed) { lv_tick_inc(elapsed); s_last_tick_us = now_us; }

    /* The clock is the one thing that moves on its own, once a minute. */
    static uint32_t clock_check_ms;
    if (now_ms() - clock_check_ms >= 1000) {
        clock_check_ms = now_ms();
        struct tm tm;
        bool utc;
        int min = clock_now(&tm, &utc) ? tm.tm_min : -1;
        if (min != s_clock_min) { s_clock_min = min; s_dirty = true; }
    }

    portENTER_CRITICAL(&s_mux);
    if (s_clim_seq != s_clim_seen) { s_clim_seen = s_clim_seq; s_dirty = true; }
    portEXIT_CRITICAL(&s_mux);

    if (s_dirty) { s_dirty = false; apply(); }
    lv_timer_handler();

    uint32_t ms = now_ms();
    bool dump = s_dump_pending;
    if (s_flush_on && s_changed &&
        (!s_committed_once || dump || ms - s_commit_ms >= XUP_MIN_COMMIT_MS)) {
        commit();
        ESP_LOGI(TAG, "commit %lu", (unsigned long)s_commits);
    }
    if (dump) { s_dump_pending = false; framedump_now(); }
}

void xui_framedump(void) { s_dump_pending = true; }

esp_err_t xui_capture(xui_slice_fn cb, void *ctx, int *w, int *h)
{
    if (!s_disp) return ESP_ERR_INVALID_STATE;
    if (w) *w = s_w;
    if (h) *h = s_h;
    if (!cb) return ESP_OK;
    static uint16_t row[256];
    for (int y = 0; y < s_h; y++) {
        glass_row(y, row);
        cb(0, y, s_w - 1, y, row, ctx);
    }
    return ESP_OK;
}

void xui_flush_enable(bool on)
{
    /* Coming back from a blank: the glass kept its picture, but the frame
     * may have moved on, so let the next pass commit whatever differs. */
    s_flush_on = on;
}

/* ── What the app has to say ───────────────────────────────────────────── */

void xup_set_climate(bool ok, float temp_c, float rh_pct)
{
    portENTER_CRITICAL(&s_mux);
    s_clim_ok = ok;
    s_clim_t = temp_c;
    s_clim_rh = rh_pct;
    s_clim_seq++;
    portEXIT_CRITICAL(&s_mux);
}

void xui_set_call(const char *call)
{
    if (!call) return;
    portENTER_CRITICAL(&s_mux);
    snprintf(s_call, sizeof s_call, "%.15s", call);
    portEXIT_CRITICAL(&s_mux);
    s_dirty = true;
}

void xui_set_battery(int pct, bool charging)
{
    if (pct > 100) pct = 100;
    if (pct == s_batt_pct && charging == s_batt_chg) return;
    /* The ADC's noise moves the estimate a percent either way; follow the
     * cell, not the noise. */
    if (pct >= 0 && s_batt_pct >= 0 && charging == s_batt_chg &&
        abs(pct - s_batt_pct) < 2)
        return;
    s_batt_pct = pct;
    s_batt_chg = charging;
    s_dirty = true;
}

void xui_home_counts(int devices, uint32_t packets)
{
    (void)packets;      /* a counter that never stops is a refresh per pass */
    if (devices != s_reach) { s_reach = devices; s_dirty = true; }
}

void xui_set_device_count(int count)
{
    if (s_reach < 0 && count != s_dev_n) s_dirty = true;
}

void xui_radar_blips(const xui_blip_t *blips, int n)
{
    if (n < 0) n = 0;
    if (n > XUI_BLIP_MAX) n = XUI_BLIP_MAX;
    pdev_t next[XUI_BLIP_MAX];
    for (int i = 0; i < n; i++) {
        pdev_t *d = &next[i];
        snprintf(d->call, sizeof d->call, "%.11s", blips[i].label);
        int8_t st = dist_step(blips[i].meters);
        const pdev_t *old = NULL;
        for (int k = 0; k < s_dev_n && !old; k++)
            if (strcmp(s_dev[k].call, d->call) == 0) old = &s_dev[k];
        if (!old || st == old->step || st == old->pend) {
            d->step = d->pend = st;     /* new, unchanged, or confirmed */
        } else {
            d->step = old->step;        /* one reading: wait for a second */
            d->pend = st;
        }
    }
    memcpy(s_dev, next, sizeof(pdev_t) * n);
    s_dev_n = n;
    s_dirty = true;
}

void xui_home_row(int idx, const char *name, bool up, const char *detail,
                  const char *note)
{
    (void)idx;
    (void)detail;
    if (!name || strcmp(name, "WiFi / LAN") != 0) return;
    s_wifi_up = up;
    snprintf(s_wifi_note, sizeof s_wifi_note, "%.31s", note ? note : "");
    s_dirty = true;
}

/* The chat table (a board without a keyboard): column 0 is the callsign,
 * column 2 the age, and the row's detail carries the whole message where
 * column 1 has only its first nineteen characters behind a room glyph. */
void xui_table_rows(const xui_row_t *rows, int n)
{
    if (n < 0) n = 0;
    if (n > XUP_MSGS) n = XUP_MSGS;
    for (int i = 0; i < n; i++) {
        pmsg_t *m = &s_tab[i];
        snprintf(m->from, sizeof m->from, "%.9s", rows[i].cell[0]);
        snprintf(m->when, sizeof m->when, "%.7s", rows[i].cell[2]);
        const char *txt = rows[i].detail;
        size_t fl = strlen(m->from);
        if (fl && strncmp(txt, m->from, fl) == 0 && strncmp(txt + fl, ": ", 2) == 0)
            txt += fl + 2;
        snprintf(m->text, sizeof m->text, "%.119s", txt);
    }
    s_tab_n = n;
}

/* The interactive chat panel, on a board with a keyboard: oldest first. */
void xui_chat_msgs(const xui_msg_t *msgs, int n, const char *header)
{
    (void)header;
    if (n < 0) n = 0;
    int k = 0;
    for (int i = n - 1; i >= 0 && k < XUP_MSGS; i--, k++) {
        pmsg_t *m = &s_tab[k];
        snprintf(m->from, sizeof m->from, "%.9s", msgs[i].from[0] ? msgs[i].from : "me");
        snprintf(m->when, sizeof m->when, "%.7s", msgs[i].when);
        snprintf(m->text, sizeof m->text, "%.119s", msgs[i].text);
    }
    s_tab_n = k;
}

void xui_set_panel(int idx)
{
    if (idx != XUI_PANEL_CHAT) return;
    if (s_msg_n == s_tab_n && memcmp(s_msg, s_tab, sizeof(pmsg_t) * s_tab_n) == 0)
        return;
    memcpy(s_msg, s_tab, sizeof(pmsg_t) * s_tab_n);
    s_msg_n = s_tab_n;
    s_dirty = true;
}

/* ── Idleness ──────────────────────────────────────────────────────────── */

void xui_activity(void) { s_activity_ms = now_ms(); }
uint32_t xui_idle_ms(void) { return now_ms() - s_activity_ms; }

/* ── What one page of e-paper does not have ────────────────────────────────
 *
 * Drawn by the seven-panel UI and with nowhere to go here, or deliberately
 * left off because it would change the page every few seconds. No-ops, not
 * errors: the app is board-independent and calls every one of them.
 *
 *   body/title/keys/note  one page, no panels to name, no legend to draw
 *   show_*                there is one view and it is always shown
 *   tables beyond chat    Reachable, Traffic, This device and Settings are
 *                         not on the tour; the home panel's blips are the
 *                         stations in reach
 *   stats                 three charts do not fit beside everything else
 *   pulse                 a flashing RX dot is a refresh per packet
 *   splash                the glass keeps its last picture through a boot,
 *                         which says more than a logo would
 *   touch/events          no panel over this glass
 */
void xui_set_body(const char *text)  { (void)text; }
void xui_set_title(const char *text) { (void)text; }
void xui_set_note(const char *text)  { (void)text; }
void xui_set_keys(const char *l, const char *m, const char *r)
{ (void)l; (void)m; (void)r; }

void xui_show_home(bool show)  { (void)show; }
void xui_show_table(bool show) { (void)show; }
void xui_show_flow(bool show)  { (void)show; }
void xui_show_stats(bool show) { (void)show; }
void xui_show_chat(bool show)  { (void)show; }

void xui_table_setup(int ncols, const char *const headers[], const int ref_w[])
{ (void)ncols; (void)headers; (void)ref_w; }
void xui_table_select(int idx) { (void)idx; }
void xui_flow_rows(const xui_flow_t *rows, int n) { (void)rows; (void)n; }
void xui_flow_select(int idx)  { (void)idx; }
void xui_stats_set(int idx, const char *title, const uint16_t *vals, int n)
{ (void)idx; (void)title; (void)vals; (void)n; }

void xui_chat_rooms(const xui_room_t *rooms, int n, int sel)
{ (void)rooms; (void)n; (void)sel; }
void xui_chat_input(const char *text, bool focused)
{ (void)text; (void)focused; }

void xui_pulse(void) { }

void xui_splash_show(void) { }
void xui_splash_status(const char *what) { (void)what; }
bool xui_splash_dismiss(void) { return true; }   /* was never up */

void xui_touch_enable(xui_touch_fn fn) { (void)fn; }
bool xui_ev_pop(xui_ev_t *out) { (void)out; return false; }
