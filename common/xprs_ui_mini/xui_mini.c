/*
 * xui_mini.c -- the station UI interface (xprs_ui.h), drawn on a strip.
 *
 * xprs_app is one program on every board, and it talks to a screen through
 * the thirty-odd xui_* calls in xprs_ui.h. On the M5Stack and the T-Deck
 * those land in xprs_ui, which has 320x240 to spend and gives each of the
 * seven panels its own object tree. The T-Dongle has 160x80. xui_init()
 * over there refuses anything under 160x120 rather than draw something
 * illegible, and it is right to.
 *
 * What the T-Dongle already had is xprs_ui_mini: three views -- DEVICES,
 * STATS, CHAT -- built for that strip and rotating hands-off, because the
 * board's one button (BOOT) is barely reachable in most mountings. This
 * file is the whole of what makes that UI answer to the shared interface,
 * so the T-Dongle runs the same station as every other board and keeps the
 * screen that was drawn for it.
 *
 * THE CONDENSING RULE. Seven panels do not fit in three views, so panels
 * arrive here and are folded:
 *
 *   HOME, REACHABLE, TRAFFIC, THIS_DEVICE, SETTINGS  ->  DEVICES
 *   CHAT                                             ->  CHAT
 *   STATS                                            ->  STATS
 *
 * The fold is decided by xui_set_panel(), not by guessing from the data:
 * the chat table and the device table are the same xui_table_rows() call
 * and nothing else tells them apart. The app calls it once per render
 * pass, after the setters, which is why every setter here only PARKS its
 * rows and xui_set_panel() is what pushes them at the right view.
 *
 * WHAT A STRIP CANNOT SHOW is a no-op and says so once, in the list at the
 * foot of this file: the radar, the button legend (there are no buttons to
 * label), the table's header row and detail strip, the composer, the
 * splash, touch. None of them is an error; a caller that needs them is on
 * the wrong screen and would have been told by xui_init().
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "esp_timer.h"
#include "lvgl.h"

#include "xprs_ui.h"
#include "xprs_ui_mini.h"

/* ── The flush gate ──────────────────────────────────────────────────────
 *
 * xui_flush_enable(false) is how the app puts the panel to sleep: LVGL
 * keeps running so touch and timers still tick, but nothing reaches the
 * glass. xprs_ui_mini has no such switch, so the flush the board gave us
 * is held here and the mini UI is handed a gate in its place. */
static xui_flush_fn s_flush;
static bool s_flush_on = true;

static void flush_gate(int x1, int y1, int x2, int y2,
                       const uint16_t *px, void *ctx)
{
    if (s_flush_on && s_flush) s_flush(x1, y1, x2, y2, px, ctx);
}

/* Cutting a field to fit is what a strip DOES -- a callsign column nine
 * characters wide is the whole design -- so every copy below writes its
 * precision out rather than letting snprintf truncate silently, which the
 * compiler is right to refuse.
 */

/* ── Parked rows ─────────────────────────────────────────────────────────
 *
 * Every setter writes here and nothing is pushed until xui_set_panel()
 * says which view the pass was for. Two arrays, because a pass fills one
 * or the other and the view decides which one matters. */
static xum_dev_t  s_dev[XUM_DEV_ROWS];
static int        s_dev_n;
static xum_chat_t s_chat[XUM_CHAT_ROWS];
static int        s_chat_n;

/* Three series and their length, parked by xui_stats_set(0..2). */
static uint16_t s_series[3][XUM_STATS_POINTS];
static int      s_series_n;

static uint32_t s_activity_ms;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ── Bring-up ────────────────────────────────────────────────────────────── */

esp_err_t xui_init(int width, int height, xui_flush_fn flush, void *ctx)
{
    s_flush = flush;
    s_flush_on = true;
    s_activity_ms = now_ms();
    return xum_init(width, height, flush ? flush_gate : NULL, ctx);
}

void xui_update(void) { xum_update(); }

void xui_framedump(void) { xum_framedump(); }

void xui_flush_enable(bool on)
{
    /* Coming back from a blank: the mini UI has been drawing into a
     * framebuffer nobody was reading, so make LVGL repaint the lot rather
     * than trust its damage list, which was honoured while the gate was
     * shut. */
    if (on && !s_flush_on) lv_obj_invalidate(lv_scr_act());
    s_flush_on = on;
}

/* ── What the app has to say ────────────────────────────────────────────── */

void xui_set_device_count(int count) { xum_set_count(count); }

void xui_home_counts(int devices, uint32_t packets)
{
    (void)packets;              /* the top bar has room for one figure */
    xum_set_count(devices);
}

/* The radar's blips are the home panel's list of who is in reach: a
 * callsign and an estimated distance, which is two thirds of a DEVICES
 * row. The bearer is not in a blip and is left blank rather than
 * invented. */
void xui_radar_blips(const xui_blip_t *blips, int n)
{
    if (n > XUM_DEV_ROWS) n = XUM_DEV_ROWS;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        snprintf(s_dev[i].call, sizeof s_dev[i].call, "%.9s", blips[i].label);
        s_dev[i].bearer[0] = 0;
        s_dev[i].dist_m = blips[i].meters < 0 ? -1
                                              : (int)(blips[i].meters + 0.5f);
        s_dev[i].age_s = 0;
    }
    s_dev_n = n;
}

/* Traffic: from / link / distance, which is a DEVICES row exactly. */
void xui_flow_rows(const xui_flow_t *rows, int n)
{
    if (n > XUM_DEV_ROWS) n = XUM_DEV_ROWS;
    if (n < 0) n = 0;
    for (int i = 0; i < n; i++) {
        snprintf(s_dev[i].call, sizeof s_dev[i].call, "%s", rows[i].from);
        snprintf(s_dev[i].bearer, sizeof s_dev[i].bearer, "%s", rows[i].link);
        s_dev[i].dist_m = rows[i].dist_m < 0 ? -1
                                             : (int)(rows[i].dist_m + 0.5f);
        s_dev[i].age_s = (int)rows[i].age_s;
    }
    s_dev_n = n;
}

/* Strip a leading LVGL symbol glyph and the space after it. The chat table
 * marks each row's room with one (a house for scope:local, an envelope for
 * a 1:1, a pin for the global default); the strip has its own one-character
 * marks, so the glyph is read for its MEANING and then removed. */
static const char *chat_kind_split(const char *cell, uint8_t *kind)
{
    *kind = 0;
    if (!cell) return "";
    if (strncmp(cell, LV_SYMBOL_HOME, strlen(LV_SYMBOL_HOME)) == 0) {
        *kind = 1;
        cell += strlen(LV_SYMBOL_HOME);
    } else if (strncmp(cell, LV_SYMBOL_ENVELOPE,
                       strlen(LV_SYMBOL_ENVELOPE)) == 0) {
        *kind = 2;
        cell += strlen(LV_SYMBOL_ENVELOPE);
    } else if (strncmp(cell, LV_SYMBOL_GPS, strlen(LV_SYMBOL_GPS)) == 0) {
        cell += strlen(LV_SYMBOL_GPS);
    }
    while (*cell == ' ') cell++;
    return cell;
}

/*
 * The generic table, which on this screen is either people or messages.
 *
 * Both are parked, because the pass has not yet said which panel it was
 * for. Column 0 is the callsign on every table the app builds; column 1 is
 * the bearer on the device tables and the message on the chat one, and
 * both parks are cheap.
 *
 * On This-device and Settings the same two columns are an item and its
 * value ("Uptime  00:12:44"), which reads perfectly well in the DEVICES
 * slot -- those panels are not on this board's rotation anyway.
 */
void xui_table_rows(const xui_row_t *rows, int n)
{
    int nd = n > XUM_DEV_ROWS ? XUM_DEV_ROWS : (n < 0 ? 0 : n);
    for (int i = 0; i < nd; i++) {
        snprintf(s_dev[i].call, sizeof s_dev[i].call, "%.9s", rows[i].cell[0]);
        snprintf(s_dev[i].bearer, sizeof s_dev[i].bearer, "%.6s",
                 rows[i].cell[1]);
        /* "~123m" in the distance column, "-" when the link cannot say. */
        s_dev[i].dist_m = rows[i].cell[2][0] == '~' ? atoi(rows[i].cell[2] + 1)
                                                    : -1;
        s_dev[i].age_s = atoi(rows[i].cell[3]);
    }
    s_dev_n = nd;

    int nc = n > XUM_CHAT_ROWS ? XUM_CHAT_ROWS : (n < 0 ? 0 : n);
    for (int i = 0; i < nc; i++) {
        uint8_t kind;
        const char *text = chat_kind_split(rows[i].cell[1], &kind);
        snprintf(s_chat[i].from, sizeof s_chat[i].from, "%.9s", rows[i].cell[0]);
        snprintf(s_chat[i].text, sizeof s_chat[i].text, "%.63s", text);
        s_chat[i].kind = kind;
    }
    s_chat_n = nc;
}

/* The interactive chat panel, on a board with a keyboard. The T-Dongle has
 * none, so its chat arrives as the table above -- but a future strip board
 * that can be typed on gets this for free. */
void xui_chat_msgs(const xui_msg_t *msgs, int n, const char *header)
{
    (void)header;
    if (n > XUM_CHAT_ROWS) n = XUM_CHAT_ROWS;
    if (n < 0) n = 0;
    /* The app hands these oldest-first; xum_chat wants newest-first and
     * reverses them itself. */
    for (int i = 0; i < n; i++) {
        const xui_msg_t *m = &msgs[n - 1 - i];
        snprintf(s_chat[i].from, sizeof s_chat[i].from, "%.9s",
                 m->from[0] ? m->from : "me");
        snprintf(s_chat[i].text, sizeof s_chat[i].text, "%.63s", m->text);
        s_chat[i].kind = m->outgoing ? 2 : 0;
    }
    s_chat_n = n;
}

void xui_stats_set(int idx, const char *title, const uint16_t *vals, int n)
{
    (void)title;                /* the strip has no room for three headings */
    if (idx < 0 || idx > 2) return;
    if (n > XUM_STATS_POINTS) n = XUM_STATS_POINTS;
    if (n < 0) n = 0;
    memcpy(s_series[idx], vals, (size_t)n * sizeof *vals);
    s_series_n = n;             /* all three carry the same length */
}

/* ── The fold ────────────────────────────────────────────────────────────
 *
 * Called once per render pass, after the setters. This is where the seven
 * panels become three views and the parked rows are pushed at whichever
 * one is now up. */
void xui_set_panel(int idx)
{
    switch (idx) {
    case XUI_PANEL_CHAT:
        xum_chat(s_chat, s_chat_n);
        xum_show(XUM_VIEW_CHAT);
        break;
    case XUI_PANEL_STATS:
        xum_stats(s_series[0], s_series[1], s_series[2], s_series_n, NULL);
        xum_show(XUM_VIEW_STATS);
        break;
    default:
        xum_devices(s_dev, s_dev_n);
        xum_show(XUM_VIEW_DEVICES);
        break;
    }
}

/* ── Idleness ───────────────────────────────────────────────────────────
 *
 * The app blanks the screen after an idle period and wakes it on a key.
 * With no touch panel here there is nothing LVGL could notice by itself,
 * so this is exactly what the app tells it. */
void xui_activity(void) { s_activity_ms = now_ms(); }
uint32_t xui_idle_ms(void) { return now_ms() - s_activity_ms; }

/* ── What a 160x80 strip does not have ───────────────────────────────────
 *
 * Each of these is drawn by the seven-panel UI and has nowhere to go here.
 * They are no-ops rather than errors: the app is board-independent by
 * design and must be able to call every one of them on every board.
 *
 *   body/title/call/keys  the strip draws its own top bar, and there are
 *                         no buttons under the screen to label
 *   show_*                the view is chosen by xui_set_panel() above,
 *                         from the panel, not from four booleans
 *   table_setup/select    no header row, no detail strip, no selection --
 *                         five rows of text is the whole widget
 *   pulse                 the top bar's device count is the only indicator
 *   splash                three lines of logo would cover the screen for
 *                         five seconds; the boot log is on the cable
 *   touch/events          no panel over this glass
 */
void xui_set_body(const char *text)  { (void)text; }
void xui_set_title(const char *text) { (void)text; }
void xui_set_call(const char *call)  { (void)call; }
void xui_set_keys(const char *l, const char *m, const char *r)
{ (void)l; (void)m; (void)r; }

void xui_show_home(bool show)  { (void)show; }
void xui_show_table(bool show) { (void)show; }
void xui_show_flow(bool show)  { (void)show; }
void xui_show_stats(bool show) { (void)show; }
void xui_show_chat(bool show)  { (void)show; }

void xui_home_row(int idx, const char *name, bool up, const char *detail,
                  const char *note)
{ (void)idx; (void)name; (void)up; (void)detail; (void)note; }

void xui_table_setup(int ncols, const char *const headers[], const int ref_w[])
{ (void)ncols; (void)headers; (void)ref_w; }
void xui_table_select(int idx) { (void)idx; }
void xui_flow_select(int idx)  { (void)idx; }

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
