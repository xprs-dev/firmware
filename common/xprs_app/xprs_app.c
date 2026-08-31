/*
 * The XPRS station: the program every board runs, with the board taken out.
 *
 * It beacons on ESP-NOW and the LAN, repeats what it hears, keeps everything
 * in an answerable index, prints every packet with the signal it arrived at,
 * and shows the lot on an eight-panel dashboard. What a board contributes is
 * a screen and a way to press something -- see xprs_app.h.
 *
 * This was models/m5stack-core/firmware/src/main.c until the T-Deck wanted the
 * same station. Nothing here changed in the move except the board seams.
 *
 * THE CHANNEL IS THE WHOLE TRICK. ESP-NOW rides whatever channel the WiFi
 * station is on, and two devices on different channels hear nothing from each
 * other with no error anywhere. So a node associates to the SAME access
 * point as the station it wants to meet, which lands it on the same channel
 * without anybody having to guess one. With no credentials it stays
 * unassociated and pins the channel the board named instead -- fine between
 * two idle boards, useless against one that is associated somewhere else.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "xprs.h"
#include "xprsnow.h"
#include "xprslan.h"
#include "xprsble.h"
#include "xprslora.h"
#include "xprsrns.h"
#include "xprsid.h"
#include "xprssig.h"
#include "xprschan.h"
#include "nostr_keys.h"
#include "bech32.h"

#include "xprs_app.h"

/* The board this station is running on. Set once by xapp_run(), read
 * everywhere; never NULL after that. */
static const xapp_board_t *s_board;

/* The credentials actually used: config.ini overrides the board's
 * compiled-in secrets, so a device can be repointed without a rebuild. */
static char s_ssid[32];   /* sized to wifi_config_t */
static char s_pass[64];

#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "xprs_station.h"
#include "xprs_ui.h"
#include "xprs_config.h"
#include "xprs_api.h"
#include "xprs_health.h"
#include "xprs_diag.h"

/* What this station is supposed to have running (xprs_health.h). One
 * roster, consumed by the boot log, the heartbeat and the OTA rollback
 * self-test alike, so "healthy enough to keep this firmware" and "healthy
 * enough to stop complaining" cannot drift apart. */
#define XH_HTTP  "http api"
#define XH_LAN   "lan bearer"
#define XH_NOW   "esp-now"
#define XH_ADDR  "wifi address"
#define XH_INDEX "archive"

/* What a board is documented to boot with; see docs/esp32.md. The floor
 * catches a step change -- a setting that stopped being applied -- not
 * ordinary drift, so it sits below the transient and above the failure.
 *
 * 6,000 was measured on the M5Stack on 2026-08-21 at
 * CONFIG_SDCARD_MAX_FILES=4: about 12 KB free once the archive had opened
 * its files. It was the only figure here for a while and every board was
 * judged by it, which is wrong in both directions -- a roomier board's
 * regression goes unnoticed and a tighter one is condemned for being
 * itself. A board that knows its own number says so (xprs_app.h); this is
 * what the rest get. */
#define XAPP_HEAP_FLOOR_DEFAULT 6000

static uint32_t heap_floor(void);   /* the board's, or the default above */


#include "xprs_auth.h"
#include "xprs_ota.h"
#include "xprs_hotspot.h"
#include "xprsindex.h"
#include "xgossip.h"
#include "xcadence.h"
#include "rns_tcp.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_core_dump.h"
#include "esp_system.h"
#include "xprs_psram.h"

static const char *TAG = "xprs";

/* Released once the config and the publisher key exist; see the call site.
 * Weak so boards without common/xprs_script still link. */
__attribute__((weak)) void xs_app_ready(void) { }

/* One boot-trace line, and it reports INTERNAL memory on purpose.
 *
 * docs/esp32.md's whole heap-by-boot-stage method rests on these marks, and
 * esp_get_free_heap_size() quietly stopped being the right number the day a
 * board grew PSRAM: it counts eight megabytes that a task stack, a DMA
 * buffer, or anything touched while the flash cache is down can never use.
 * The T-Deck proved it by reporting 8,367,348 bytes free while the HTTP
 * server could not get a 6,144-byte stack.
 *
 * So: internal free, internal largest block, internal minimum-ever -- the
 * three numbers that actually decide whether the next subsystem starts --
 * and the PSRAM total alongside, separately, where it cannot be mistaken
 * for headroom it is not. Identical output on a board without PSRAM. */
static uint32_t heap_floor(void)
{
    return s_board->heap_floor ? (uint32_t)s_board->heap_floor
                               : XAPP_HEAP_FLOOR_DEFAULT;
}

static void heap_mark(const char *stage)
{
    unsigned internal = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    unsigned largest  =
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    unsigned min_ever =
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
#if CONFIG_SPIRAM
    unsigned psram = (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGI(TAG, "heap %s: internal %u (largest %u, min %u) + psram %u",
             stage, internal, largest, min_ever, psram);
#else
    ESP_LOGI(TAG, "heap %s: internal %u (largest %u, min %u)",
             stage, internal, largest, min_ever);
#endif
}

/* This station. Derived from the MAC so two boards never collide, unless NVS
 * carries one an operator chose. X5 marks it as an experimental station rather
 * than claiming an X1/X3 form that means something. */
static char s_call[10];

static void derive_callsign(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    static const char *b32 = "ACDEFGHJKLMNPQRSTUVWXYZ23456789";  /* no B/I/O/1 */
    uint32_t v = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
    s_call[0] = 'X'; s_call[1] = '5';
    for (int i = 0; i < 4; i++) { s_call[2 + i] = b32[v % 30]; v /= 30; }
    s_call[6] = 0;
}

/* ── Signing, and whose word we take ────────────────────────────────────── */

/* One key per station we have heard a t:identity from (§9.3), first speaker
 * wins. This existed so §23.7 had somebody it could believe, and four was
 * enough for that -- but the same table decides who is a KNOWN asker and who
 * is a stranger when history is metered (31.2: six replays an hour against
 * two), so on a station that many peers ask, a table this small quietly
 * demotes the fifth onward to a stranger's budget and keeps them there.
 * 36.9.4 asks a super to be budgeted for being leaned on; that starts with
 * being able to remember who is asking. 42 bytes an entry. */
#define PEERKEYS_MAX 32
static struct { char call[10]; uint8_t pub[32]; } s_peers[PEERKEYS_MAX];
static int s_peers_n;

static const uint8_t *peer_key(const char *call)
{
    for (int i = 0; i < s_peers_n; i++) {
        if (strcasecmp(s_peers[i].call, call) == 0) return s_peers[i].pub;
    }
    return NULL;
}

/* Verified against the very key it publishes: a station claiming a key must
 * show it holds it, and the packet is signed with it. */
static void identity_heard(const xprs_t *p)
{
    char call[10], npub[80];
    if (!xprs_get_str(p, "f", call, sizeof call)) return;
    if (!xprs_get_str(p, "k", npub, sizeof npub)) return;
    if (strncmp(npub, "npub1", 5) != 0 || peer_key(call)) return;
    if (s_peers_n >= PEERKEYS_MAX) return;

    char hrp[8];
    uint8_t data[64];
    size_t dlen = sizeof data;
    if (bech32_decode(npub, hrp, data, &dlen) != ESP_OK) return;
    if (dlen != 32 || strcmp(hrp, "npub") != 0) return;
    if (!xprsid_verify(p, data)) {
        ESP_LOGW(TAG, "%s does not sign for the key it published", call);
        return;
    }
    snprintf(s_peers[s_peers_n].call, sizeof s_peers[0].call, "%s", call);
    memcpy(s_peers[s_peers_n].pub, data, 32);
    s_peers_n++;
    ESP_LOGI(TAG, "learned %s signs with %02x%02x%02x%02x...", call,
             data[0], data[1], data[2], data[3]);
}

/* What a signature costs on the wire: " sig:" and 60 base85 characters
 * (9.1). Anything composing a packet it means to sign has to leave this
 * much, because xprsid_sign declines silently rather than truncating. */
#define SIG_ROOM (5 + XPRSSIG_B85_LEN)

static int sign_wire(char *wire, int len, int cap)
{
    const nostr_keys_t *k = nostr_keys_get();
    return k ? xprsid_sign(wire, len, cap, k->private_key) : len;
}

/* ── The rotating log: every ESP_LOG line, kept for the post-mortem ─────── */

/* The hook runs on WHOEVER logs, so it does only what is free: format into
 * a small buffer and copy into this ring under a spinlock. idx_task -- the
 * only task allowed near the storage -- drains it to /idx/log/cur.txt and
 * rotates to prev.txt at 64 KB, so a freeze or a crash leaves at most two
 * files and the truth of the last minutes on the flash. */
#define LOGRING_N 6144
static char s_logring[LOGRING_N];
static volatile int s_logring_w, s_logring_r;
/* Bytes the ring had to refuse. Boot is the noisy part and the storage it
 * drains to is not mounted yet, so this is where a hole appears -- and a
 * hole nobody is told about is the failure that costs a day. */
static volatile uint32_t s_logring_lost;
static portMUX_TYPE s_logring_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_log_orig;

static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int out = s_log_orig ? s_log_orig(fmt, ap) : vprintf(fmt, ap);

    /* Stamp the line: epoch seconds under a synced clock, +ms-since-boot
     * before that -- what makes /api/log machine readable (API-HTTP.md). */
    char line[176];
    int n;
    uint32_t ep = xst_epoch_now();
    if (ep)
        n = snprintf(line, sizeof line, "%lu ", (unsigned long)ep);
    else
        n = snprintf(line, sizeof line, "+%lu ",
                     (unsigned long)(esp_timer_get_time() / 1000));
    int m = vsnprintf(line + n, sizeof line - n, fmt, ap2);
    va_end(ap2);
    if (m <= 0) return out;
    n += m;
    if (n >= (int)sizeof line) {
        /* Truncated: the newline went with the tail. Put one back or the
         * next entry glues itself onto this line in the file. */
        n = sizeof line - 1;
        line[n - 1] = '\n';
    }

    /* The UART likes its colours; a log file does not. Strip the ANSI
     * escapes and carriage returns on the way into the ring. */
    portENTER_CRITICAL(&s_logring_mux);
    bool in_esc = false;
    for (int i = 0; i < n; i++) {
        char c = line[i];
        if (in_esc) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
                in_esc = false;
            continue;
        }
        if (c == 0x1b) { in_esc = true; continue; }
        if (c == '\r') continue;
        int nw = (s_logring_w + 1) % LOGRING_N;
        if (nw == s_logring_r) {               /* full: drop the rest */
            s_logring_lost += (uint32_t)(n - i);
            break;
        }
        s_logring[s_logring_w] = c;
        s_logring_w = nw;
    }
    portEXIT_CRITICAL(&s_logring_mux);
    xdiag_log_line(line, n);           /* the RTC tail that outlives a crash */
    return out;
}

/* How full the ring is, read without the lock: idx_task uses it to decide
 * whether to drain early, and an answer one line stale is harmless. */
static int log_fill(void)
{
    int w = s_logring_w, r = s_logring_r;
    return w >= r ? w - r : LOGRING_N - r + w;
}

#define LOG_ROTATE_BYTES (64 * 1024)
static FILE *s_logfile;
static long  s_logfile_len;

/* idx_task only. Append what the ring holds, rotate at the cap.
 *
 * [sync] false is the drain that only exists to stop the ring overflowing:
 * it gets the bytes out of RAM and leaves them to the filesystem's own
 * cadence. The periodic drain syncs, so a freeze still leaves the last ten
 * seconds on the flash -- which is the promise this log makes. */
static void log_drain(bool sync)
{
    char chunk[512];
    for (;;) {
        int n = 0;
        portENTER_CRITICAL(&s_logring_mux);
        while (s_logring_r != s_logring_w && n < (int)sizeof chunk) {
            chunk[n++] = s_logring[s_logring_r];
            s_logring_r = (s_logring_r + 1) % LOGRING_N;
        }
        portEXIT_CRITICAL(&s_logring_mux);
        if (n == 0) break;

        if (!s_logfile) {
            s_logfile = fopen("/idx/log/cur.txt", "ab");
            if (!s_logfile) return;
            s_logfile_len = ftell(s_logfile);
        }
        fwrite(chunk, 1, n, s_logfile);
        s_logfile_len += n;
    }
    /* Say what was lost, in the file, where the hole is. */
    uint32_t lost = s_logring_lost;
    if (lost && s_logfile) {
        s_logring_lost -= lost;
        char note[120];
        uint32_t ep = xst_epoch_now();
        /* The line the ring cut short lost its newline with its tail, so
         * start one: without this the note reads as the end of a sentence
         * it has nothing to do with. */
        int n = snprintf(note, sizeof note, "\n");
        n += ep ? snprintf(note + n, sizeof note - n, "%lu ", (unsigned long)ep)
                : snprintf(note + n, sizeof note - n, "+%lu ",
                           (unsigned long)(esp_timer_get_time() / 1000));
        n += snprintf(note + n, sizeof note - n,
                      "W log: %lu bytes never reached this file -- the ring "
                      "filled faster than it drained\n", (unsigned long)lost);
        fwrite(note, 1, (size_t)n, s_logfile);
        s_logfile_len += n;
    }

    if (s_logfile) {
        fflush(s_logfile);
        if (sync) fsync(fileno(s_logfile));
        if (s_logfile_len >= LOG_ROTATE_BYTES) {
            fclose(s_logfile);
            s_logfile = NULL;
            remove("/idx/log/prev.txt");
            rename("/idx/log/cur.txt", "/idx/log/prev.txt");
        }
    }
}

/* ── The index: everything heard, kept and answerable (XPRS 36) ─────────── */

/* The store lives on a wear-levelled FAT partition filling the spare flash.
 * NOTHING here runs on a receive path: hearing a packet only copies it into
 * this ring, and idx_task (core 1) does every write, query and reply --
 * flash work on the radio cores is how stations go deaf. */
#define IDXQ_N 12
static struct {
    char    wire[XPRSIDX_WIRE_MAX + 1];
    int16_t len;
    int8_t  rssi;
    uint8_t bearer;   /* xprsidx_bearer_t */
    bool    own;      /* we sent it: archived as outgoing, dated now */
} s_idxq[IDXQ_N];
static volatile int s_idxq_w, s_idxq_r;   /* single writer set, single reader */
static uint32_t s_idxq_dropped;

static xprsidx_t *s_index;
static xprs_api_cfg_t s_api_cfg;    /* filled below; idx_task publishes into it */

/*
 * History asks waiting for idx_task.
 *
 * One replay in flight protects the channel (31.4), and for an ordinary
 * archiver that is still the rule: depth one. But 36.9.4 lists `concurrent`
 * among the things claiming `super` commits a station to -- "one replay at a
 * time, with a page taking tens of seconds to air, means the second asker of
 * any minute is refused" -- and a page here takes about 19.5 s. So a super
 * queues a few and works through them.
 *
 * The other half matters more than the depth: an ask that does not fit used
 * to be dropped where it arrived, with no answer at all. Silence is the one
 * response the asker cannot act on -- it reads as a dead station, not as a
 * busy one, and 36.10.2's cadence backs off on a 429 and gives up on
 * nothing. So the overflow is remembered and answered 429 on idx_task, which
 * is where this station is allowed to transmit from.
 */
static bool idx_is_super(void);        /* defined with the budgets below */

#define ASKQ_MAX 4
static struct {
    char wire[XPRSIDX_WIRE_MAX + 1];
    int  len;
    char bearer[7];
} s_askq[ASKQ_MAX], s_ask_over;
static volatile uint8_t s_askq_w, s_askq_r, s_askq_n;
static volatile bool s_ask_over_pending;
static volatile bool s_wipe_req;   /* Settings asked for the archive to go */

static void idx_enqueue(const char *wire, int len, int rssi, uint8_t bearer)
{
    if (!s_index || !xcfg_get_bool("index_on", true)) return;
    if (len > XPRSIDX_WIRE_MAX) return;
    int w = s_idxq_w, nw = (w + 1) % IDXQ_N;
    if (nw == s_idxq_r) { s_idxq_dropped++; return; }   /* full: drop, count */
    memcpy(s_idxq[w].wire, wire, len);
    s_idxq[w].wire[len] = 0;
    s_idxq[w].len = (int16_t)len;
    s_idxq[w].rssi = (int8_t)(rssi < -127 ? -127 : rssi);
    s_idxq[w].bearer = bearer;
    s_idxq[w].own = false;
    s_idxq_w = nw;
}

/* Our own packet, for the archive, the same way: queued for idx_task. It
 * used to be written straight from api_send_wire() on the httpd task, and
 * an index that is mid-replay holds its lock for seconds -- the send door
 * had put the packet on the air in 0.4 s and then sat on the request until
 * the client gave up, and a second request queued behind it never ran. */
static void idx_enqueue_own(const char *wire, int len, uint8_t bearer)
{
    if (!s_index || !xcfg_get_bool("index_on", true)) return;
    if (len > XPRSIDX_WIRE_MAX) return;
    int w = s_idxq_w, nw = (w + 1) % IDXQ_N;
    if (nw == s_idxq_r) { s_idxq_dropped++; return; }
    memcpy(s_idxq[w].wire, wire, len);
    s_idxq[w].wire[len] = 0;
    s_idxq[w].len = (int16_t)len;
    s_idxq[w].rssi = 0;
    s_idxq[w].bearer = bearer;
    s_idxq[w].own = true;
    s_idxq_w = nw;
}

static uint8_t bearer_code(const char *name)
{
    if (strcmp(name, "espnow") == 0) return XI_B_ESPNOW;
    if (strcmp(name, "lan") == 0)    return XI_B_LAN;
    if (strcmp(name, "lora") == 0)   return XI_B_LORA;
    if (strcmp(name, "ble") == 0)    return XI_B_BLE;
    if (strcmp(name, "rns") == 0)    return XI_B_RNS;
    return XI_B_UNKNOWN;
}

/* What the index calls to decide whether a stored packet is really from who
 * it says. Runs on idx_task, never on a radio task. Three answers: 1 checks
 * out, -1 fails against a key we hold, 0 cannot tell. */
static int index_verifier(const char *wire, int len, const char *from)
{
    const uint8_t *key = peer_key(from);
    if (!key) return 0;
    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return 0;
    return xprsid_verify(&p, key) ? 1 : -1;
}

/* ── What we hear ───────────────────────────────────────────────────────── */

static uint32_t s_heard_count;

static uint32_t s_last_rx_ms;
static char s_last_call[10];

/* The flow: the last packets heard, newest first, for the screen. Type and
 * origin only -- the screen never shows message content. */
#define FLOW_MAX 9
typedef struct {
    char     call[10];
    char     to[10];           /* d: recipient; empty = broadcast */
    char     type[13];
    char     link[7];
    int      rssi;
    uint32_t ms;
    char     text[160];        /* m: payload, or the raw wire for the rest */
} flow_t;
static flow_t s_flow[FLOW_MAX];
static int s_flow_n;


/* LVGL's FontAwesome glyph bytes, without dragging lvgl.h into main.c:
 * HOME f015, GPS f124, ENVELOPE f0e0 -- the same values xprs_ui renders. */
#define LV_SYMBOL_HOME     "\xEF\x80\x95"
#define LV_SYMBOL_GPS      "\xEF\x84\xA4"
#define LV_SYMBOL_ENVELOPE "\xEF\x83\xA0"


static int s_tz_off;      /* seconds east of UTC, from config (Node clock) */
static int s_sel[7];      /* per-panel selected row (7 = UI_PANEL_COUNT) */
static int s_list_n;      /* rows the current panel rendered; the key
                           * handlers bound the selection to it, because
                           * "render clamps it later" was never true for
                           * Settings -- see the note at the clamp below */
/* Settings is modal: A stays the Menu key until the arrows dive into the
 * list, then it becomes OK; climbing back out above the top row hands the
 * Menu key back. */
static bool s_set_focus;
/* Touch state. s_compose_focus is the caret: on a keyboard board typing
 * always lands in the composer, this only says whether it is drawn focused.
 * s_rail_room maps a tapped rail row back to a room id (-1 = a heading).
 * s_bar is what each bottom-bar slot DOES on a touch board; on a button
 * board the slots are legends for physical keys and the actions are unused. */
static bool s_compose_focus = true;
static int  s_rail_room[XUI_CHAT_ROOMS];
enum { BAR_NONE = 0, BAR_KEY_HOME, BAR_KEY_NEXT, BAR_KEY_PREV, BAR_KEY_DOWN,
       BAR_OK, BAR_NEXT_PANEL };
static struct { const char *txt; int act; } s_bar[3];
static void set_bar(void);
static void settings_ok(int row);

/* What the user must press to change the selected setting. A board with a
 * keyboard says "Enter", because there a trackball click is too easy to make
 * by accident; a board with only buttons says "OK", which is its middle
 * button. The help text under each row has to agree with the bottom bar, or
 * the station is telling the user to press something that does nothing. */
static const char *ok_key(void)
{
    return (s_board && s_board->raw_key) ? "Enter" : "OK";
}
static uint32_t now_ms(void);
static bool s_display_up;   /* the panel and LVGL are alive */

/*
 * Name the step that is starting, on the splash and in the log.
 *
 * Also drains stdin for 'S': ui_task is what normally answers a framedump
 * request, and during boot it does not exist yet -- without this there is no
 * way to photograph the splash WHILE the station is still coming up, which
 * is the only evidence that distinguishes a splash from a picture of a
 * finished boot.
 */
static void splash_step(const char *what)
{
    for (int i = 0; i < 8; i++) {
        int c = getchar();
        if (c <= 0) break;
        if (c == 'S' || c == 's') xui_framedump();
    }
    ESP_LOGI(TAG, "splash: %s", what);
    xui_splash_status(what);
}

/* Keyboard backlight: lit by a keypress, out after this many ms of none. */
#define KB_LIGHT_MS 5000
static bool     s_kb_lit;
static uint32_t s_kb_off_ms;

/* Battery. Six samples ten seconds apart: one minute of trend. The state
 * is what the trend says, not the voltage -- a full battery on USB and a
 * full battery just unplugged read the same millivolts. */
#define BAT_RING 6
#define BAT_SAMPLE_MS 10000
#define BAT_TREND_MV 40      /* outside the ADC's own wander */
#define BAT_USB_MV   4300    /* above any lithium cell: USB is present */
enum { BAT_UNKNOWN = 0, BAT_CHARGING, BAT_DISCHARGING };
static int      s_bat_mv = -1;
static int      s_bat_ring[BAT_RING];
static int      s_bat_n;              /* samples taken, saturates at BAT_RING */
static int      s_bat_state = BAT_UNKNOWN;
static uint32_t s_bat_next_ms;
static bool     s_screen_off;
static bool     s_touch_pressed;      /* a finger went down this tick */

static const char *bat_state_name(void)
{
    return s_bat_state == BAT_CHARGING ? "charging"
         : s_bat_state == BAT_DISCHARGING ? "discharging" : "unknown";
}

static void battery_tick(void)
{
    if (!s_board->battery_mv) return;
    uint32_t now = now_ms();
    if ((int32_t)(now - s_bat_next_ms) < 0) return;
    s_bat_next_ms = now + BAT_SAMPLE_MS;
    int mv = s_board->battery_mv();
    if (mv < 0) return;
    s_bat_mv = mv;
    for (int i = BAT_RING - 1; i > 0; i--) s_bat_ring[i] = s_bat_ring[i - 1];
    s_bat_ring[0] = mv;
    if (s_bat_n < BAT_RING) s_bat_n++;

    int was = s_bat_state;
    /* First the hard fact: a lithium cell never exceeds ~4.2 V, so a node
     * reading above 4.3 V is being held up by USB. The trend cannot be
     * trusted there -- measured on a bench T-Deck, the ADC wandered between
     * 4456 and 4568 mV on USB, which a 15 mV threshold read as "discharging"
     * and put the screen out. Below that, the trend decides, with a band
     * wide enough to sit outside that noise, and only once a full minute of
     * samples exists. */
    if (mv >= BAT_USB_MV) {
        s_bat_state = BAT_CHARGING;
    } else if (s_bat_n >= BAT_RING) {
        int delta = s_bat_ring[0] - s_bat_ring[BAT_RING - 1];
        if (delta <= -BAT_TREND_MV)      s_bat_state = BAT_DISCHARGING;
        else if (delta >= BAT_TREND_MV)  s_bat_state = BAT_CHARGING;
        /* flat and below the USB line: whatever it was, unchanged */
    }
    if (s_bat_state != was)
        ESP_LOGI(TAG, "battery %d mV %s", mv, bat_state_name());
    else
        ESP_LOGD(TAG, "battery %d mV %s", mv, bat_state_name());
}

static void screen_wake(const char *why)
{
    if (!s_screen_off) return;
    s_screen_off = false;
    if (s_board->screen_power) s_board->screen_power(true);
    xui_flush_enable(true);
    xui_activity();
    ESP_LOGI(TAG, "screen: wake (%s)", why);
}

static void screen_tick(void)
{
    if (s_screen_off || !s_board->screen_power) return;
    int off_s = atoi(xcfg_get("screen_off_s", "120"));
    if (off_s <= 0) return;                         /* 0 = never */
    if (s_bat_state != BAT_DISCHARGING) return;     /* on power: stay lit */
    if (xui_idle_ms() < (uint32_t)off_s * 1000u) return;
    s_screen_off = true;
    xui_flush_enable(false);
    s_board->screen_power(false);
    ESP_LOGI(TAG, "screen: off after %d s idle on battery", off_s);
}

static void kb_light_tick(int kb)
{
    if (!s_board->kb_backlight) return;
    uint32_t now = now_ms();
    if (kb > 0) {
        if (!s_kb_lit) { s_board->kb_backlight(true); s_kb_lit = true;
                         ESP_LOGD(TAG, "kbd light 1"); }
        s_kb_off_ms = now + KB_LIGHT_MS;
    } else if (s_kb_lit && (int32_t)(now - s_kb_off_ms) >= 0) {
        s_board->kb_backlight(false); s_kb_lit = false;
        ESP_LOGD(TAG, "kbd light 0");
    }
}

static int api_status_json(char *buf, size_t cap)
{
    /* Cached values only -- this runs on the HTTP task. */
    return snprintf(buf, cap,
        "\"battery\":{\"mv\":%d,\"state\":\"%s\"},\"screen\":\"%s\"",
        s_bat_mv, bat_state_name(), s_screen_off ? "off" : "on");
}
static int s_stats_view;  /* Stats panel: 0 = 10 min, 1 = hour, 2 = day */
/* Rotate: C on the home panel starts a 30 s tour of Home, Stats, Chat;
 * any button ends it and hands the wheel back. */
static bool s_rotate;
static uint64_t s_rotate_next_us;      /* total ever, ring position = s_flow_n % FLOW_MAX */

/*
 * Catching up with other archivers (36.10), one row per peer.
 *
 * What was here was a single parked ask on a fixed ten-minute clock, and it
 * could not page: the ask carried only `since:`, and a `code:206` -- the
 * responder saying "there is more" -- was never acted on anywhere in this
 * firmware. It was only ever SENT. So a board that had been away for a day
 * asked once, was handed twelve records, and stopped; the rest of the hole
 * stayed a hole until somebody power-cycled it into asking again.
 *
 * Now each peer has its own cadence (36.10.2, the ladder in xcadence.c) and
 * its own continuation mark. A 206 is followed with `until:` set to the
 * oldest record the page brought, which walks the window backwards until the
 * peer says 200.
 *
 * `resume` is checked for PROGRESS. A responder that answers the same
 * `until:` with the same page is stalled, not busy, and the difference
 * matters: read as activity it would be asked faster and faster forever,
 * which is the load 36.10.2 exists to prevent. Two pages that do not move
 * the mark end the chain and count as quiet.
 */
#define CU_PEERS 6
typedef struct {
    char     call[10];
    char     bearer[7];
    uint32_t interval_s;      /* what the ladder says, for this peer */
    uint32_t next_ask_s;      /* uptime seconds; 0 = never asked */
    uint32_t last_news_s;     /* uptime when it last had something to say */
    uint32_t since;           /* the window this chain is filling */
    uint32_t resume;          /* the 206 continuation mark, 0 = fresh ask */
    uint32_t oldest_seen;     /* oldest ts of the page now arriving */
    uint32_t ask_at_s;        /* uptime the outstanding ask went out */
    uint16_t rows;            /* records this peer served since the ask */
    uint8_t  stalls;          /* continuations that moved nothing */
    bool     super;           /* it announced serve:archive,super */
    bool     await;           /* an ask is out and unanswered */
    bool     used;
} cu_peer_t;
static cu_peer_t s_cu_peers[CU_PEERS];

/* The one ask waiting for idx_task, which owns the signing stack. */
static struct {
    char call[10];
    char bearer[7];
    uint32_t since;
    uint32_t until;
    volatile bool pending;
} s_cu;

static cu_peer_t *cu_find(const char *call, bool create)
{
    cu_peer_t *free_row = NULL, *oldest = &s_cu_peers[0];
    for (int i = 0; i < CU_PEERS; i++) {
        cu_peer_t *r = &s_cu_peers[i];
        if (r->used && strcasecmp(r->call, call) == 0) return r;
        if (!r->used && !free_row) free_row = r;
        if (r->used && r->next_ask_s < oldest->next_ask_s) oldest = r;
    }
    if (!create) return NULL;
    cu_peer_t *r = free_row ? free_row : oldest;
    memset(r, 0, sizeof *r);
    snprintf(r->call, sizeof r->call, "%s", call);
    r->interval_s = xcadence_initial();
    r->used = true;
    return r;
}

/*
 * The `since:` a chain starts from.
 *
 * Normally the newest record already on disk when this station booted -- the
 * hole begins where the archive stopped (36.10). But an EMPTY store used to
 * mean "nothing missed", and that is exactly backwards: a station whose
 * archive has just been wiped, or which has never had one, has missed
 * everything there is. It sat next to a super-archiver holding eighteen
 * thousand records and never asked it a single question.
 *
 * A bounded bootstrap instead: a day back. The peer's budgets and the 206
 * chain decide how fast that arrives, and one day is a hole a board can
 * actually fill rather than a request to replay the world.
 */
#define CU_BOOTSTRAP_SEC (24u * 3600u)

static uint32_t cu_since_floor(void)
{
    const uint32_t newest = s_index ? xprsindex_boot_newest_ts(s_index) : 0;
    if (newest) return newest;
    const uint32_t now = xst_epoch_now();
    return now > CU_BOOTSTRAP_SEC ? now - CU_BOOTSTRAP_SEC : 0;
}

/*
 * The peer whose page is arriving, if any.
 *
 * A replay carries the ORIGINAL AUTHOR's `f:`, not the replaying station's --
 * that is the whole point of 36.2, the author's bytes and the author's
 * signature. So rows cannot be attributed by looking at who signed them, and
 * attributing them that way is what made a chain die after one page here: the
 * dongle replayed twenty thousand records written by other stations, none of
 * them counted, `oldest_seen` stayed 0, and the 206 fell through to "nothing
 * new" and ended the chain as quiet. Only one ask is in flight at a time, so
 * there is exactly one peer a page can belong to.
 */
static cu_peer_t *cu_awaiting(void)
{
    for (int i = 0; i < CU_PEERS; i++)
        if (s_cu_peers[i].used && s_cu_peers[i].await) return &s_cu_peers[i];
    return NULL;
}

/* How old a record must be to count as REPLAYED rather than as live traffic
 * that happened to arrive mid-page. The phone uses the same minute. */
#define CU_REPLAY_AGE_SEC 60

/* A board archives whether or not anybody is looking at it, so the phone's
 * hidden floor does not map: what bounds this station is the PEER's budget,
 * never a screen. */
static void cu_answered(cu_peer_t *r, xc_answer_t a, uint32_t now_s)
{
    const uint32_t silent = r->last_news_s ? now_s - r->last_news_s : 0;
    r->interval_s = xcadence_next(r->interval_s, a,
                                  r->super ? XC_FAST : XC_ORDINARY,
                                  true, silent);
    r->next_ask_s = now_s + xcadence_jitter(r->interval_s, now_s);
    r->await = false;
    r->rows = 0;
    r->oldest_seen = 0;
    if (a == XC_NEWS) r->last_news_s = now_s;
}

/* Marks the room a heard saying belongs to as having something new in it.
 * Defined with the rest of the chat panel, below. */
static void chat_note_unread(const xprs_t *p);

/* The card is the one thing an install must not fight: an erase of the
 * other slot while the index is mid-write is how a station comes back with
 * a corrupt archive on top of a corrupt update. */
/* Stand the station down for the length of an install.
 *
 * Pausing the index writer is not the expensive part. This board reaches
 * steady state with about 7 KB of free heap, and an image it fetches for
 * itself needs an HTTP client, a 2 KB manifest buffer and a socket --
 * docs/device.md puts the floor at roughly 25 KB. The first attempt timed
 * out at eight seconds without the server ever seeing the request.
 *
 * The hotspot looked like the answer and is not. The boot trace says
 * `heap after hotspot: 30,448`, down from 139,440, so the SoftAP and its
 * DHCP server and netif cost about 109 KB -- but dropping to WIFI_MODE_STA
 * gives back only 3,312 of them (measured: 7,628 -> 10,940). The rest was
 * claimed when the interface was created and is not returned by a mode
 * change; freeing it would mean tearing the netif down, which is a
 * different and much more invasive change. Three kilobytes is still worth
 * having for the duration, and the AP comes back afterwards -- including
 * on the failure path, because a refused image must not cost the station
 * its hotspot until someone power-cycles it.
 *
 * It is not enough to fetch with. At about 11 KB free this board is still
 * under the ~25 KB floor in docs/device.md, and esp_https_ota_begin()
 * answers ESP_ERR_NO_MEM. Both shipping boards are therefore given their
 * images rather than fetching them; see device.md 6.2. */
static bool s_ap_stood_down;

static void ota_quiesce(bool quiet)
{
    if (s_index) xprsindex_pause_writes(s_index, quiet);

    unsigned before = (unsigned)esp_get_free_heap_size();
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        if (quiet && mode == WIFI_MODE_APSTA) {
            if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
                s_ap_stood_down = true;
                ESP_LOGW(TAG, "hotspot down for the install: heap %u -> %u",
                         (unsigned)before,
                         (unsigned)esp_get_free_heap_size());
            }
        } else if (!quiet && s_ap_stood_down) {
            s_ap_stood_down = false;
            if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK)
                ESP_LOGI(TAG, "hotspot back up");
        }
    }

    if (quiet) ESP_LOGW(TAG, "storage paused: an update is being installed");
    else       ESP_LOGI(TAG, "storage resumed");
}

/* A cmd:update waiting for the task that may afford to verify it. */
static struct {
    char wire[XPRSIDX_WIRE_MAX + 1];
    int  len;
    char bearer[8];
    volatile bool pending;
} s_upd;

/* A `q:identity` ask (18.1) waiting for idx_task, which owns the signing
 * stack. Answering costs one advert and saves the asker half an hour of
 * being unable to verify anything this station says -- including the very
 * hears: observations its gossip runs on. */
static volatile bool s_qid_pending;

/* A recipient just heard directly, whose held mail idx_task should try to
 * deliver (XPRS.md 36.8.1). Parked like every other job that costs storage
 * or curve time: the radio task decides WHETHER, idx_task does the work. */
static struct {
    char call[10];
    char bearer[8];
    volatile bool pending;
} s_rel;

/* ── 13.7: receipts ─────────────────────────────────────────────────────
 *
 * Until this existed the station heard its peers' `t:receipt s:ack` and threw
 * them away, so held mail had no terminal state: the 36.8.1 release was
 * stopped by REL_THROTTLE_SEC, a clock, and re-aired the same packet every
 * ten minutes for as long as it was held. Measured on the bench, one message
 * from 18:27 was re-aired four times in a twenty-minute sample and would have
 * gone on for as long as the board stayed up.
 *
 * Both directions are secp256k1 work -- verifying one, signing one -- so both
 * are PARKED here and done on idx_task, the same discipline cmd:update and
 * the diagnostics asks already follow. The radio task decides whether.
 */

/* Messages whose receipt we have seen and verified: stop holding them.
 *
 * Not a purge of the archive -- 13.3 says the ack "releases carriers still
 * holding a copy", and this station's archive is not its custody queue. The
 * index keeps the packet, as an index should; this ring is what stops it
 * being re-aired at somebody who already has it. */
#define ACKED_MAX 48
static struct {
    char id[XPRS_ID_LEN];
    uint32_t t_ms;
} s_acked[ACKED_MAX];
static uint8_t s_acked_pos;

static void acked_note(const char *id)
{
    for (int i = 0; i < ACKED_MAX; i++)
        if (strcmp(s_acked[i].id, id) == 0) return;
    snprintf(s_acked[s_acked_pos].id, XPRS_ID_LEN, "%s", id);
    s_acked[s_acked_pos].t_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_acked_pos = (uint8_t)((s_acked_pos + 1) % ACKED_MAX);
}

static bool acked_has(const char *id)
{
    for (int i = 0; i < ACKED_MAX; i++)
        if (s_acked[i].id[0] && strcmp(s_acked[i].id, id) == 0) return true;
    return false;
}

/* 13.7.1's one condition: "a direct message has already passed between the two
 * callsigns, in either direction". The first message from a stranger is not
 * acknowledged -- it is what establishes the pair -- and every one after it
 * is. A ring, because a board does not correspond with the world. */
#define EXCH_MAX 12
static char s_exch[EXCH_MAX][10];
static uint8_t s_exch_pos;

static bool exchanged_with(const char *call)
{
    for (int i = 0; i < EXCH_MAX; i++)
        if (s_exch[i][0] && strcasecmp(s_exch[i], call) == 0) return true;
    return false;
}

static void exchanged_note(const char *call)
{
    if (exchanged_with(call)) return;
    snprintf(s_exch[s_exch_pos], sizeof s_exch[0], "%s", call);
    s_exch_pos = (uint8_t)((s_exch_pos + 1) % EXCH_MAX);
}

/* One slot each way. A dropped job is not a lost fact: an unacknowledged
 * message is re-sent by its sender's own ladder, and a receipt we missed
 * arrives again with the next copy. A queue would buy nothing but RAM. */
static struct {
    char wire[XPRS_MAX_WIRE + 1];
    int  len;
    char bearer[8];
    volatile bool pending;
} s_rcpt_in, s_rcpt_out;

/* One delivery attempt per recipient per period, however chatty their
 * beacons: the trigger is cheap, the replay is airtime. */
#define REL_THROTTLE_SEC 600
static struct { char call[10]; uint32_t at_s; } s_rel_seen[16];

/*
 * Gossip (36.9.4): who else heard whom. The store is xprs_gossip, on the same
 * volume as the archive.
 *
 * This was 32 rows of volatile RAM with no layers and no walls -- and, worse,
 * it took an unsigned observation's word for it, so anybody in earshot could
 * write this station's idea of where a callsign lives by beaconing a hears:
 * list nobody signed. The component keeps the L2/L3 split, credits the signer,
 * meters each observer, and survives the reboot that used to empty it.
 */
static xgossip_t *s_goss;
static bool verified(const xprs_t *p);     /* the signature verdict, below */

static int goss_try(const char *call, const char *self, char *out, int cap)
{
    return xgossip_try_candidates(s_goss, call, self, out, cap);
}

/* One answer to a command, on the bearer it arrived on -- a reply aired
 * somewhere else is a reply the asker never hears. Signed, because a
 * result is evidence and an unsigned one proves nothing (9.1). */
static void ota_answer(const char *to, const char *bearer, const char *id,
                       int code, const char *msg)
{
    if (!to || !to[0] || !id || !id[0]) return;
    char ts[24] = "";
    time_t t = time(NULL);
    if (t > 1700000000) {
        struct tm tmv;
        gmtime_r(&t, &tmv);
        strftime(ts, sizeof ts, "%Y-%m-%d_%H:%M:%S", &tmv);
    }
    char w[XPRSIDX_WIRE_MAX + 1];
    int n = snprintf(w, sizeof w, "t:result f:%s d:%s%s%s r:%s code:%d",
                     s_call, to, ts[0] ? " ts:" : "", ts[0] ? ts : "",
                     id, code);
    if (msg && msg[0] && n > 0 && n < (int)sizeof w)
        n += snprintf(w + n, sizeof w - n, " m:%s", msg);
    if (n <= 0 || n >= (int)sizeof w) return;
    n = sign_wire(w, n, sizeof w);
    if (strcmp(bearer, "espnow") == 0) xprsnow_send(w, n);
    else                               xprslan_send(w, n);
}

static void seen_note(const char *wire, int len, const char *bearer, int rssi)
{
    xprs_t sp;
    char call[10];
    if (!xprs_parse(wire, len, &sp)) return;
    if (!xprs_get_str(&sp, "f", call, sizeof call)) return;
    if (strcasecmp(call, s_call) == 0) return;   /* our own echo */

    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    s_last_rx_ms = now;
    snprintf(s_last_call, sizeof s_last_call, "%s", call);

    /* Every heard packet is offered to the index; ping/pong and duplicates
     * are its problem to refuse, not ours to guess. */
    idx_enqueue(wire, len, rssi, bearer_code(bearer));

    /* A history ask addressed to us (or to the whole channel) is copied for
     * idx_task; everything with a cost happens there. */
    do {
        char type[16], cmd[16], dst[16];
        xprs_type(&sp, type, sizeof type);
        if (strcmp(type, "command") != 0) break;
        if (!xprs_get_str(&sp, "cmd", cmd, sizeof cmd) ||
            strcmp(cmd, "history") != 0) break;
        if (xprs_get_str(&sp, "d", dst, sizeof dst)) {
            char *dash = strchr(dst, '-');
            if (dash) *dash = 0;              /* base callsign, section 3.1 */
            char base[16];
            snprintf(base, sizeof base, "%s", s_call);
            dash = strchr(base, '-');
            if (dash) *dash = 0;
            if (strcasecmp(dst, base) != 0) break;
        }
        const int depth = idx_is_super() ? ASKQ_MAX : 1;
        if (s_askq_n >= depth) {
            /* Full. Keep the newest for a 429 rather than saying nothing;
             * an earlier overflow still waiting is simply overwritten,
             * because one honest refusal per busy spell is the point. */
            memcpy(s_ask_over.wire, wire, len);
            s_ask_over.wire[len] = 0;
            s_ask_over.len = len;
            snprintf(s_ask_over.bearer, sizeof s_ask_over.bearer, "%s", bearer);
            s_ask_over_pending = true;        /* published last */
            break;
        }
        const uint8_t w = s_askq_w;
        memcpy(s_askq[w].wire, wire, len);
        s_askq[w].wire[len] = 0;
        s_askq[w].len = len;
        snprintf(s_askq[w].bearer, sizeof s_askq[w].bearer, "%s", bearer);
        s_askq_w = (uint8_t)((w + 1) % ASKQ_MAX);
        s_askq_n++;                           /* published last */
    } while (0);

    /* An update command (25.8). It is an actuation, so it goes through the
     * gate before anything else: unsigned or unverifiable dies here without
     * an answer, and a stranger we can identify gets a refusal rather than
     * silence. Everything that costs time happens on the updater's task. */
    do {
        char type[16], cmd[16];
        xprs_type(&sp, type, sizeof type);
        if (strcmp(type, "command") != 0) break;
        if (!xprs_get_str(&sp, "cmd", cmd, sizeof cmd) ||
            strcmp(cmd, "update") != 0) break;

        /* Park it, verify it on idx_task. A signature check is secp256k1:
         * kilobytes of stack and milliseconds of work, and this runs on a
         * bearer callback. esp32.md has a section on exactly this mistake
         * -- the receive task decides WHETHER (parse, dedupe: all RAM) and
         * a core-1 task does the signature and the reply. */
        char dst[16] = "";
        if (!xprs_get_str(&sp, "d", dst, sizeof dst) || !dst[0]) break;
        if (strncasecmp(dst, s_call, strlen(s_call)) != 0) break;
        if (s_upd.pending) break;
        memcpy(s_upd.wire, wire, len);
        s_upd.wire[len] = 0;
        s_upd.len = len;
        snprintf(s_upd.bearer, sizeof s_upd.bearer, "%s", bearer);
        s_upd.pending = true;
        ESP_LOGW(TAG, "cmd:update parked from %s", bearer);
    } while (0);

    /* The diagnostics asks (cmd:zdiag/zcore/zlog, xprs_diag): parked the
     * same way, verified and answered on idx_task. */
    xdiag_park_parsed(&sp, wire, len, bearer);

    /* 13.7: a receipt heard. Park it -- the signature decides whether it
     * means anything, and that is curve work for idx_task. An unverifiable
     * one changes nothing, which is the whole reason it must be checked:
     * 13.7.1 calls a forged ack "a way to delete a message the attacker
     * never held". */
    do {
        char type[16], r[XPRS_ID_LEN];
        xprs_type(&sp, type, sizeof type);
        if (strcmp(type, "receipt") != 0) break;
        if (!xprs_get_str(&sp, "r", r, sizeof r) || !r[0]) break;
        if (s_rcpt_in.pending) break;
        memcpy(s_rcpt_in.wire, wire, len);
        s_rcpt_in.wire[len] = 0;
        s_rcpt_in.len = len;
        s_rcpt_in.pending = true;
    } while (0);

    /* 13.7.1: a direct message addressed to us, from a station we have
     * corresponded with, is acknowledged without anybody asking. The
     * exclusions are the table in that section -- no `d:` is a broadcast, a
     * receipt would never terminate, and a stranger has agreed to neither
     * the airtime nor the disclosure that answering costs. */
    do {
        char type[16], dst[16];
        xprs_type(&sp, type, sizeof type);
        if (strcmp(type, "message") != 0) break;
        if (!xprs_get_str(&sp, "d", dst, sizeof dst) || !dst[0]) break;
        char *dash = strchr(dst, '-');
        if (dash) *dash = 0;                  /* base callsign, section 3.1 */
        char base[16];
        snprintf(base, sizeof base, "%s", s_call);
        dash = strchr(base, '-');
        if (dash) *dash = 0;
        if (strcasecmp(dst, base) != 0) break;   /* not ours to acknowledge */

        if (exchanged_with(call) && !s_rcpt_out.pending) {
            memcpy(s_rcpt_out.wire, wire, len);
            s_rcpt_out.wire[len] = 0;
            s_rcpt_out.len = len;
            snprintf(s_rcpt_out.bearer, sizeof s_rcpt_out.bearer, "%s", bearer);
            s_rcpt_out.pending = true;
        }
        /* Recorded AFTER the test, so the first message establishes the pair
         * and the second is the first one acknowledged. */
        exchanged_note(call);
    } while (0);

    /* q:identity (18.1): publish the key binding on request. */
    do {
        char type[16], q[12], dst[16];
        xprs_type(&sp, type, sizeof type);
        if (strcmp(type, "command") != 0) break;
        if (!xprs_get_str(&sp, "q", q, sizeof q) ||
            strcmp(q, "identity") != 0) break;
        if (xprs_get_str(&sp, "d", dst, sizeof dst) &&
            strncasecmp(dst, s_call, strlen(s_call)) != 0) break;
        s_qid_pending = true;
    } while (0);

    /* ── 36.8.1: deliver held mail the moment its recipient is heard ────
     * The trigger is the packet itself, never a poll. Cheap here (a ring
     * compare); the index query and the paced re-air run on idx_task. */
    do {
        /* call[10]: the ring rows are 10 wide, and a callsign that long is
         * already past section 3's shape -- truncate at parse, not at copy. */
        char from[10] = "", via[8];
        if (!xprs_get_str(&sp, "f", from, sizeof from) || !from[0]) break;
        if (strcasecmp(from, s_call) == 0) break;
        if (xprs_get_str(&sp, "via", via, sizeof via)) break; /* direct only */
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);

        /* Our own radio is its own witness (36.9.4): no via:, no signature
         * needed, and it is what makes a callsign this station's business
         * at all. Debounced inside the component -- every beacon from every
         * neighbour arrives here. */
        xgossip_note_direct(s_goss, from, s_call, bearer, now_s);
        int free_slot = 0;
        bool throttled = false;
        for (int i = 0; i < 16; i++) {
            if (strcasecmp(s_rel_seen[i].call, from) == 0) {
                throttled = now_s - s_rel_seen[i].at_s < REL_THROTTLE_SEC;
                free_slot = i;
                break;
            }
            if (s_rel_seen[i].at_s <= s_rel_seen[free_slot].at_s) free_slot = i;
        }
        if (throttled || s_rel.pending) break;
        snprintf(s_rel_seen[free_slot].call, sizeof s_rel_seen[free_slot].call,
                 "%s", from);
        s_rel_seen[free_slot].at_s = now_s;
        snprintf(s_rel.call, sizeof s_rel.call, "%s", from);
        snprintf(s_rel.bearer, sizeof s_rel.bearer, "%s", bearer);
        s_rel.pending = true;                 /* published last */
    } while (0);

    /* ── Gossip intake (36.9.4): who else heard whom ───────────────────
     * hears: is directly-heard-only by 10.6.3, so each listed callsign
     * pairs with the OBSERVER as its gateway.
     *
     * The quota is checked BEFORE the signature. A verify is a curve
     * operation on whichever radio task heard the packet, a neighbour
     * beacons its observation every few seconds, and the quota admits one
     * per interval anyway -- so verifying the ones it is about to refuse is
     * heat bought for nothing. Same argument, same order, as the phone's
     * ingest funnel. */
    do {
        /* 208 and 25, not 96 and 8: 10.6.3 says "about twenty-five callsigns
         * fit a packet", and this is the intake for exactly such a packet.
         * At 96 bytes a busy neighbour's list was cut to a third before the
         * gossip store ever saw it -- the truncation happened HERE, silently,
         * after the sender had already paid to transmit the whole thing.
         * Matches the render buffer at the beacon end, which is 208. */
        char type[16], from[10], hears[208], link[8];
        xprs_type(&sp, type, sizeof type);
        if (strcmp(type, "observation") != 0) break;
        if (!xprs_get_str(&sp, "f", from, sizeof from) || !from[0]) break;
        if (strcasecmp(from, s_call) == 0) break;
        if (!xprs_get_str(&sp, "hears", hears, sizeof hears)) break;
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
        if (!xgossip_would_accept(s_goss, from, now_s)) break;

        /* link: names the radio the claim is about (10.6.1). Without one,
         * the bearer it reached us on is the honest guess -- but only a
         * short-range bearer writes the durable layer either way. */
        if (!xprs_get_str(&sp, "link", link, sizeof link))
            snprintf(link, sizeof link, "%s", bearer);

        const char *list[XST_HEARS_MAX];
        char *tok = hears, *next;
        int n = 0;
        while (tok && *tok && n < XST_HEARS_MAX) {
            next = strchr(tok, ',');
            if (next) *next = 0;
            if (*tok) list[n++] = tok;
            tok = next ? next + 1 : NULL;
        }
        /* An unsigned or unverifiable claim feeds nothing: the component
         * counts the refusal so /api can show it being refused. */
        xgossip_note_hears(s_goss, from, list, n, link, verified(&sp), now_s);
    } while (0);

    /* A serve:archive announcement from a station that was away: ask it
     * for the window we missed (XPRS.md 36.10), before ingesting makes it
     * look freshly heard. A meeting is DIRECT -- a relayed announcement is
     * not a peer in range. The reply is ordinary heard traffic. */
    do {
        char type[16];
        xprs_type(&sp, type, sizeof type);
        if (strcmp(type, "service") != 0) break;
        char sv[40];
        if (!xprs_get_str(&sp, "serve", sv, sizeof sv)) break;
        if (!strstr(sv, "archive")) break;
        if (xprs_get(&sp, "via", NULL) != NULL) break;
        if (!xst_epoch_now()) break;              /* no clock, no since: */
        const uint32_t newest = cu_since_floor();
        if (!newest) break;                        /* no clock, no window */

        cu_peer_t *r = cu_find(call, true);
        if (!r) break;
        /* `serve:archive,super` is what earns the fast floor: only raised
         * budgets can serve a fast caller (36.9.4), and asking an ordinary
         * peer faster steals its whole cross-caller allowance. */
        r->super = strstr(sv, "super") != NULL;
        snprintf(r->bearer, sizeof r->bearer, "%s", bearer);

        const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
        if (r->await) break;                    /* one chain per peer */
        if (r->next_ask_s && now_s < r->next_ask_s) break;
        if (!r->next_ask_s && !xst_catchup_due(call, 600)) break;

        /* Park it. Signing is several KB of secp256k1 stack and this runs
         * on the 5 KB bearer task -- the first build here PANIC'd xprslan.
         * idx_task (8 KB) builds and airs the ask. */
        if (s_cu.pending) break;
        if (!r->resume) r->since = newest;
        snprintf(s_cu.call, sizeof s_cu.call, "%s", call);
        snprintf(s_cu.bearer, sizeof s_cu.bearer, "%s", bearer);
        s_cu.since = r->since;
        s_cu.until = r->resume;
        s_cu.pending = true;
        r->await = true;
        r->ask_at_s = now_s;
        r->rows = 0;
        r->oldest_seen = 0;
    } while (0);

    /* ── The other half of 36.10: what came back ───────────────────────
     * A replay arrives as ordinary heard traffic -- the author's wires, not
     * the peer's -- so what marks it as OUR page is that the peer we asked
     * is still awaiting an answer. Count the rows and keep the oldest ts:
     * that ts is the `until:` a 206 continues from. */
    do {
        char rfrom[10];
        if (!xprs_get_str(&sp, "f", rfrom, sizeof rfrom) || !rfrom[0]) break;
        const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);

        char rtype[16];
        xprs_type(&sp, rtype, sizeof rtype);
        if (strcmp(rtype, "result") == 0) {
            /* The answer to our own ask, addressed to us. */
            char rto[10], code[8];
            cu_peer_t *r = cu_find(rfrom, false);
            if (!r || !r->await) break;
            if (!xprs_get_str(&sp, "d", rto, sizeof rto)) break;
            if (strncasecmp(rto, s_call, strlen(rto)) != 0) break;
            if (!xprs_get_str(&sp, "code", code, sizeof code)) break;
            const int c = atoi(code);
            if (c == 202) break;                /* "starting": not an answer */

            if (c == 429) {
                ESP_LOGI(TAG, "catch-up: %s refused us (429)", rfrom);
                r->resume = 0;
                r->stalls = 0;
                cu_answered(r, XC_REFUSED, now_s);
                break;
            }
            if (c == 206 && r->oldest_seen) {
                /* A continuation must MAKE PROGRESS (25.2.1). A responder
                 * that answers the same window with the same page is
                 * stalled, and reading that as a busy room would have us
                 * ask it faster and faster forever. */
                if (r->resume && r->oldest_seen >= r->resume) {
                    ESP_LOGW(TAG, "catch-up: %s made no progress at %lu - "
                             "ending the chain", rfrom,
                             (unsigned long)r->resume);
                    r->resume = 0;
                    r->stalls++;
                    cu_answered(r, XC_QUIET, now_s);
                    break;
                }
                r->resume = r->oldest_seen;
                r->stalls = 0;
                /* More is held: come straight back for it rather than
                 * waiting out the cadence. The cadence paces CHAINS, not
                 * the pages inside one. */
                r->last_news_s = now_s;
                r->await = false;
                r->rows = 0;
                r->oldest_seen = 0;
                r->next_ask_s = now_s;
                ESP_LOGI(TAG, "catch-up: %s has more, continuing until %lu",
                         rfrom, (unsigned long)r->resume);
                break;
            }
            /* 200 with rows, 200 empty, 404, or a 206 that brought nothing. */
            const bool news = r->rows > 0;
            r->resume = 0;
            cu_answered(r, news ? XC_NEWS : XC_QUIET, now_s);
            ESP_LOGI(TAG, "catch-up: %s answered %d with %u row(s); "
                     "next in %lus", rfrom, c, (unsigned)(news ? r->rows : 0),
                     (unsigned long)r->interval_s);
            break;
        }

        cu_peer_t *r = cu_awaiting();
        if (!r) break;
        char tsbuf[24];
        if (!xprs_get_str(&sp, "ts", tsbuf, sizeof tsbuf)) break;
        const uint32_t ts = xprsindex_ts_to_epoch(tsbuf, (int)strlen(tsbuf));
        if (!ts) break;
        /* Live traffic arriving mid-page is not part of the page. */
        const uint32_t ep = xst_epoch_now();
        if (ep && ep - ts < CU_REPLAY_AGE_SEC) break;
        if (r->rows < 0xFFFF) r->rows++;
        if (!r->oldest_seen || ts < r->oldest_seen) r->oldest_seen = ts;
    } while (0);

    /* Chat ring + rx/device stats + devices list live in xprs_station now,
     * shared with every board that has a screen. */
    xst_ingest_parsed(&sp, bearer, rssi);
    chat_note_unread(&sp);

    flow_t *fl = &s_flow[s_flow_n % FLOW_MAX];
    snprintf(fl->call, sizeof fl->call, "%s", call);
    if (!xprs_get_str(&sp, "d", fl->to, sizeof fl->to)) fl->to[0] = 0;
    xprs_type(&sp, fl->type, sizeof fl->type);
    /* What the Flow panel shows when the row is selected: the message text
     * when there is one, the raw packet for everything else. */
    if (!xprs_get_str(&sp, "m", fl->text, sizeof fl->text))
        snprintf(fl->text, sizeof fl->text, "%.*s",
                 len > 150 ? 150 : len, wire);
    snprintf(fl->link, sizeof fl->link, "%s", bearer);
    fl->rssi = rssi;
    fl->ms = now;
    s_flow_n++;

}

/* ── Network state the UI shows ─────────────────────────────────────────── */

static char s_ip_str[20];                 /* empty until GOT_IP */
static volatile bool s_inet_up;           /* last probe verdict */
static volatile bool s_inet_known;        /* at least one probe finished */

/*
 * THE CAROUSEL: the last ten messages, said again while nobody is talking.
 *
 * A relay only helps whoever was listening at the time. Bluetooth is tens
 * of metres and a phone walks; the packet that crossed the room two minutes
 * ago never reached the person who has just come within range, and nothing
 * in a flood asks for it again. So this station keeps the ten most recent
 * packets worth hearing and repeats one whenever a bearer has been quiet
 * for a while -- verbatim, via: and all, so a station that already relayed
 * it is in its own path and stays silent while a new arrival hears it for
 * the first time.
 *
 * PRIORITY IS THE POINT. Live traffic is never delayed by this: an echo
 * only goes out on a bearer that has heard and said nothing for
 * ECHO_QUIET_MS, xb_echo() refuses when the packet is already queued, and
 * the re-air queue is drained before anything here is considered. New
 * packets go out first and then join the ring, at the front.
 *
 * What is worth repeating is what a person would miss: messages, and the
 * two shouts. Beacons, observations and identities are said again by their
 * authors on a timer anyway, and repeating those would be a station talking
 * about itself instead of carrying somebody else's words.
 */
#define ECHO_N          10
#define ECHO_QUIET_MS   12000u   /* a bearer this silent has room     */
#define ECHO_GAP_MS     20000u   /* and this long between our echoes  */

static XPRS_PSRAM_BSS struct {
    char     wire[XPRS_MAX_WIRE + 1];
    int      len;
    char     id[XPRS_ID_LEN];
} s_echo[ECHO_N];
static int s_echo_n;             /* how many slots are filled   */
static int s_echo_w;             /* next slot to overwrite      */
static int s_echo_cursor;        /* where the carousel is       */

static bool echo_worth_repeating(const char *wire, int len)
{
    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return false;
    char t[16] = "";
    xprs_type(&p, t, sizeof t);
    return strcmp(t, "message") == 0 || strcmp(t, "sos") == 0 ||
           strcmp(t, "warning") == 0;
}

static void echo_keep(const char *wire, int len)
{
    if (len <= 0 || len > XPRS_MAX_WIRE) return;
    if (!echo_worth_repeating(wire, len)) return;

    char id[XPRS_ID_LEN];
    if (!xprs_id_of(wire, len, id)) return;
    for (int i = 0; i < s_echo_n; i++)
        if (strcmp(s_echo[i].id, id) == 0) return;      /* already held */

    memcpy(s_echo[s_echo_w].wire, wire, (size_t)len);
    s_echo[s_echo_w].wire[len] = 0;
    s_echo[s_echo_w].len = len;
    snprintf(s_echo[s_echo_w].id, sizeof s_echo[s_echo_w].id, "%s", id);
    s_echo_w = (s_echo_w + 1) % ECHO_N;
    if (s_echo_n < ECHO_N) s_echo_n++;
}

/* One step of the carousel, called about once a second from status_task. */
static void echo_tick(void)
{
    if (!s_echo_n || !xcfg_get_bool("echo_on", true)) return;

    /* Both thresholds are the operator's: a bench with four stations on it
     * is never quiet for twelve seconds and the carousel correctly never
     * speaks, which is also how you prove it works -- `cfg set
     * echo_quiet_ms 2000` and it does. */
    const char *q = xcfg_get("echo_quiet_ms", NULL);
    const char *g = xcfg_get("echo_gap_ms", NULL);
    uint32_t quiet = q && q[0] ? (uint32_t)atoi(q) : ECHO_QUIET_MS;
    uint32_t gap   = g && g[0] ? (uint32_t)atoi(g) : ECHO_GAP_MS;

    uint32_t now = now_ms();
    static uint32_t next_ms[4];          /* lan, espnow, lora, ble */
    struct {
        uint32_t (*idle)(uint32_t);
        void     (*echo)(const char *, int);
    } bearers[4] = {
        { xprslan_idle_ms,  xprslan_echo  },
        { xprsnow_idle_ms,  xprsnow_echo  },
        { xprslora_idle_ms, xprslora_echo },
        { xprsble_idle_ms,  xprsble_echo  },
    };

    for (int i = 0; i < 4; i++) {
        if ((int32_t)(now - next_ms[i]) < 0) continue;
        if (bearers[i].idle(now) < quiet) continue;

        /* Oldest first, so the ring is walked rather than the newest
         * repeated forever; the newest is also the one most likely to have
         * just been heard by everybody present. */
        int slot = s_echo_cursor % s_echo_n;
        s_echo_cursor = (s_echo_cursor + 1) % s_echo_n;
        static const char *const names[4] = { "lan", "espnow", "lora", "ble" };
        ESP_LOGI(TAG, "echo   %6s %3dB  %s", names[i], s_echo[slot].len,
                 s_echo[slot].wire);
        bearers[i].echo(s_echo[slot].wire, s_echo[slot].len);
        next_ms[i] = now + gap;
    }
}

/*
 * THE BRIDGE, in one place.
 *
 * A station with more than one radio exists to join them: what arrives on
 * any bearer is offered to every OTHER bearer it has, and re-aired on the
 * one it came from when this station is a digipeater. That is what carries
 * a phone's beacon from Bluetooth onto LoRa, over a valley, and back onto
 * Bluetooth beside somebody else's phone -- the leg that was missing until
 * 2026-08-31, when a packet that had come a kilometre reached the LAN and
 * ESP-NOW and stopped, one hop short of the phones it was for.
 *
 * Nothing here decides whether a packet SHOULD travel: xb_offer() refuses a
 * packet already in this bearer's heard ring, one that already carries this
 * station in via:, and one whose hop budget (13.1) is spent, and the
 * duplicate rings on every station downstream do the rest. The switches
 * below are the operator's, not the protocol's:
 *
 *   bridge_on  (default yes) carry between bearers -- what this is
 *   igate_on   (default yes) the LAN leg specifically, because that one
 *              reaches the internet and is the one an operator may want
 *              shut without going deaf on the radios
 *   digi_on    (default yes) re-air on the medium it was heard on
 *   digi_ble_on(default yes) the same for Bluetooth, kept separate because
 *              BLE is cheap and local where LoRa is duty-cycled
 */
typedef enum { FROM_LAN, FROM_NOW, FROM_LORA, FROM_BLE, FROM_RNS } from_t;

static void bridge_out(const char *wire, int len, from_t from)
{
    echo_keep(wire, len);

    bool bridge = xcfg_get_bool("bridge_on", true);
    bool igate  = xcfg_get_bool("igate_on", true);

    /* Onto the other bearers. */
    if (from != FROM_LAN  && igate)  xprslan_offer(wire, len);
    if (from != FROM_NOW  && bridge) xprsnow_offer(wire, len);
    if (from != FROM_LORA && bridge) xprslora_offer(wire, len);
    if (from != FROM_BLE  && bridge) xprsble_offer(wire, len);
    if (from != FROM_RNS  && bridge) xprsrns_send(wire, len);

    /* And again on the one it came from, for stations past the sender's
     * reach but inside ours. */
    if (from == FROM_NOW  && xcfg_get_bool("digi_on", true))
        xprsnow_digipeat(wire, len);
    if (from == FROM_LORA && xcfg_get_bool("digi_on", true))
        xprslora_digipeat(wire, len);
    if (from == FROM_BLE  && xcfg_get_bool("digi_ble_on", true))
        xprsble_digipeat(wire, len);
}

static void on_espnow(const char *wire, int len, const uint8_t mac[6], int rssi)
{
    s_heard_count++;
    seen_note(wire, len, "espnow", rssi);
    xprs_t p;
    if (xprs_parse(wire, len, &p)) {
        char type[16];
        xprs_type(&p, type, sizeof type);
        if (strcmp(type, "identity") == 0) identity_heard(&p);
        /* §23.7 is handled in heard_espnow(), not here — see the note there. */
        if (strcmp(type, "channel") == 0 || strcmp(type, "receipt") == 0) return;
    }
    ESP_LOGI(TAG, "espnow %02x:%02x:%02x:%02x:%02x:%02x %4d dBm %3dB  %s",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], rssi, len, wire);
    bridge_out(wire, len, FROM_NOW);
}

/* Every hearing on ESP-NOW, duplicates included.
 *
 * §23.7's step 4 is the same signed acceptance aired a second time on the
 * working channel, and the ordinary receive path drops a packet whose
 * identifier it has already heard. This callback is the one that does not. */
static void heard_espnow(const char *id, const char *wire, int len)
{
    (void)id;
    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return;
    char type[16];
    xprs_type(&p, type, sizeof type);
    if (strcmp(type, "channel") == 0 || strcmp(type, "receipt") == 0) {
        xprschan_on_packet(&p, wire, len);
    }
}

static void on_lan(const char *wire, int len, uint32_t ip)
{
    seen_note(wire, len, "lan", 0);
    /* A key published on the LAN is as good as one published on a radio, and
     * a station we only ever meet here -- a desktop, typically -- had no other
     * way to tell us how it signs. Leaving this out made every one of its
     * packets unverifiable while its identity sat in the log unread. */
    xprs_t lp;
    if (xprs_parse(wire, len, &lp)) {
        char ltype[16];
        xprs_type(&lp, ltype, sizeof ltype);
        if (strcmp(ltype, "identity") == 0) identity_heard(&lp);
    }
    ESP_LOGI(TAG, "lan    %u.%u.%u.%u %3dB  %s",
             (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF),
             len, wire);
    bridge_out(wire, len, FROM_LAN);
}

/*
 * One frame off the BLE5 air (docs/ble5.md §2). Subtypes other than XPRS on
 * this radio belong to other software -- Reticulum 0x55, the compact APRS
 * frame 0x41, the route beacon 0x4D -- and are not this station's business.
 *
 * Runs on xb_rx_task (xprsble.c), not on the controller's task: queue_ad()
 * copies the AD and posts it, so this side has a 6144-byte stack of its own
 * and may verify a signature or hand a wire to another bearer. Offering is
 * cheap in any case -- xb_offer() dedups, appends via: and copies into a
 * preallocated slot; the transmit happens later on the owning bearer's tick.
 */
static void on_ble(uint8_t subtype, const uint8_t *payload, int len, int rssi)
{
    if (subtype != XPRSBLE_SUB_XPRS || len <= 0 || len > XPRS_MAX_WIRE) return;
    char wire[XPRS_MAX_WIRE + 1];
    memcpy(wire, payload, (size_t)len);
    wire[len] = 0;

    s_heard_count++;
    seen_note(wire, len, "ble", rssi);
    xprs_t p;
    if (xprs_parse(wire, len, &p)) {
        char type[16];
        xprs_type(&p, type, sizeof type);
        if (strcmp(type, "identity") == 0) identity_heard(&p);
    }
    ESP_LOGI(TAG, "ble    %19d dBm %3dB  %s", rssi, len, wire);
    /* iGate: what arrived on the BLE air goes toward the LAN and whatever
     * internet sits behind it -- the same rule ESP-NOW and LoRa already
     * follow. Whether it may be relayed at all is xprs_append_via's call
     * inside the bearer: it refuses when we are already in via: (13.2) or
     * the type's hop budget is spent (13.1).
     *
     * Without this a Bluetooth-only station is unreachable from anywhere
     * else, because BLE is the one bearer this station hears and never
     * repeats -- see docs/diagrams/relay-gateway-flow.md. */
    bridge_out(wire, len, FROM_BLE);
}

/* A wire off the Reticulum uplink (xprsrns.h). Same funnel as every radio;
 * an internet byte has no signal strength to report. */
static void on_rns(const char *wire, int len)
{
    s_heard_count++;
    seen_note(wire, len, "rns", 0);
    /* Same as the LAN: a peer met only over the internet must still be able
     * to publish the key it signs with (36.12.2's identity lane is the one
     * thing guaranteed to cross a shared transport). */
    xprs_t rp;
    if (xprs_parse(wire, len, &rp)) {
        char rtype[16];
        xprs_type(&rp, rtype, sizeof rtype);
        if (strcmp(rtype, "identity") == 0) identity_heard(&rp);
    }
    /* An internet byte reaches the radios like any other. */
    bridge_out(wire, len, FROM_RNS);
}

/* The last LoRa packet, for the strip's top bar: a board with a radio and
 * three lines of screen is one somebody points an antenna with, and the
 * number that tells them whether they pointed it right is not otherwise
 * visible without a laptop. */
static int      s_lora_rssi;
static uint32_t s_lora_rssi_ms;

static void on_lora(const char *wire, int len, int rssi)
{
    s_heard_count++;
    s_lora_rssi = rssi;
    s_lora_rssi_ms = now_ms();
    seen_note(wire, len, "lora", rssi);
    xprs_t p;
    if (xprs_parse(wire, len, &p)) {
        char type[16];
        xprs_type(&p, type, sizeof type);
        if (strcmp(type, "identity") == 0) identity_heard(&p);
    }
    ESP_LOGI(TAG, "lora   %19d dBm %3dB  %s", rssi, len, wire);
    /* What arrived over kilometres goes to every other bearer this station
     * has -- the LAN, ESP-NOW, Bluetooth -- and back onto LoRa itself when
     * this station digipeats. */
    bridge_out(wire, len, FROM_LORA);
}

/* ── What we say ────────────────────────────────────────────────────────── */

/* t:observation f:<call> link:espnow peers:<n> — §10.6, and §10.6.1's rule that
 * a reading belongs to the bearer it names, which is why this says espnow and
 * the LAN beacon below says lan. */
/* One beacon body, per bearer: peers: is the FULL count of directly-heard
 * CALLSIGNS on that bearer (the corrected quantity -- the address count the
 * bearers keep is a different number wearing the same name), and hears:
 * lists as many of them as fit (10.6.3, truncation per 10.6.4). hears: is
 * what makes this station's gossip (36.9.4) worth listening to.
 *
 * `zhq:` rides beside it when there is room: one digit a neighbour, same
 * order, same count, 9 loud to 0 barely there. It is private vocabulary under
 * the z prefix (4.9) until the packets have been shown and agreed, and it is
 * the first thing dropped when the packet gets tight -- a cut list of names is
 * worth more than a full list of signal nobody can name.
 *
 * [sign_room] is the bytes a signature will want afterwards, and it is not
 * optional arithmetic: `xprsid_sign` returns the wire UNSIGNED when it will
 * not fit, silently, and 36.9.4 says an unsigned claim feeds no gossip. A
 * beacon that grew past the room for its own signature used to become a
 * statement nobody was allowed to believe. */
static int observation_beacon(const char *bearer, char *out, int cap,
                              int sign_room)
{
    if (!s_call[0]) return 0;
    int room = (cap - 1 < XPRS_MAX_WIRE) ? cap - 1 : XPRS_MAX_WIRE;

    /* What the header costs, before it is written. peers: is charged three
     * digits because the count is not known until the list has been built,
     * and one spare byte is cheaper than rendering twice. */
    int fixed = (int)(sizeof "t:observation f:" - 1) + (int)strlen(s_call)
              + (int)(sizeof " link:" - 1) + (int)strlen(bearer)
              + (int)(sizeof " peers:" - 1) + 3;
    int budget = room - fixed - sign_room;

    char hears[208];
    char q[XST_SEEN_MAX + 1];
    int total = 0;
    int hn = xst_hears_render(bearer, 600, budget, hears, sizeof hears,
                              &total, q, sizeof q);

    int n = snprintf(out, (size_t)cap, "t:observation f:%s link:%s peers:%d",
                     s_call, bearer, total);
    if (hn > 0 && n > 0 && n < cap)
        n += snprintf(out + n, (size_t)(cap - n), " hears:%s", hears);
    if (hn > 0 && q[0] && n > 0 && n < cap)
        n += snprintf(out + n, (size_t)(cap - n), " zhq:%s", q);
    ESP_LOGI(TAG, "obs %s: peers=%d hn=%d zhq=%s bytes=%d budget=%d", bearer,
             total, hn, q[0] ? q : "-", n, budget);
    return n;
}

/* The bearers' own beacons are not signed (xb_beacon_tick airs what it is
 * handed), so all of the packet is theirs to fill. */
static int espnow_beacon(char *out, int cap)
{
    return observation_beacon("espnow", out, cap, 0);
}

static int lan_beacon(char *out, int cap)
{
    return observation_beacon("lan", out, cap, 0);
}

/*
 * The BLE5 beacon, aired from the status tick rather than by the bearer.
 *
 * ESP-NOW and the LAN each own a task with a beacon timer in it; the BLE
 * component is the radio and nothing else, deliberately, so the cadence lives
 * with the station. §10.6.1's rule still holds -- a reading belongs to the
 * bearer it names -- so this says link:ble and no other beacon may.
 *
 * This is the one a phone hears. Nothing else this station transmits reaches a
 * device with no access point and no pairing.
 */
static void air_ble_beacon(void)
{
    if (!s_call[0] || !xprsble_is_active()) return;
    char wire[XPRS_MAX_WIRE + 1];
    /* An advert holds less than a packet does, so the beacon is composed to
     * the radio's limit rather than the format's. */
    int n = observation_beacon("ble", wire, XPRSBLE_WIRE_MAX + 1, SIG_ROOM);
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = sign_wire(wire, n, (int)sizeof wire);
    if (!xprsble_send(wire, n))
        ESP_LOGW(TAG, "BLE5 beacon refused by the radio");
}

/* A SIGNED observation per wired bearer, from the status task -- the one
 * task sized for the signature (see its 6 KB note). The bearers' own
 * unsigned beacons keep their job (they pump the re-air queues); these are
 * the copies whose hears: a stranger's gossip may believe (36.9.4: an
 * unsigned claim feeds nothing). Every minute, and only when there is
 * something to claim -- an empty hears: signed is airtime for nobody. */
static void air_signed_observations(void)
{
    if (!s_call[0]) return;

    /*
     * Cadence by density, because the payload was never the problem: this
     * function airs one observation PER BEARER over EVERY lane, so nine
     * signed packets can leave per turn before any of them grows a byte.
     * Multiply that by everybody in a busy street and the reachability
     * claims cost more airtime than they inform.
     *
     * So a station in a crowd speaks less often -- and it is the one whose
     * claim is least surprising, since a dozen neighbours all hear each
     * other. A station alone on a hill keeps the full minute, which is where
     * the news actually is. Sixty seconds a turn, one turn per four
     * neighbours, never slower than eight minutes.
     */
    static int s_wait;                     /* turns still to sit out */
    if (s_wait > 0) { s_wait--; return; }
    int mult = 1 + xst_devices_in_range(600) / 4;
    if (mult > 8) mult = 8;
    s_wait = mult - 1;
    /* One observation PER BEARER (link: names the radio the claim is
     * about, 10.6.1) -- and each is fanned over EVERY transmit lane,
     * because a reachability claim is gossip (36.9.4) and gossip travels:
     * the worked example of 36.6 is exactly a ble observation fetched over
     * the internet. Without the fan-out, "X3R8XX hears X1A67X on ble" was
     * a fact only other BLE listeners could ever learn. */
    static const char *k_bearers[] = { "ble", "lan", "espnow" };
    for (int i = 0; i < 3; i++) {
        char wire[XPRS_MAX_WIRE + 1];
        int n = observation_beacon(k_bearers[i], wire, (int)sizeof wire,
                                   SIG_ROOM);
        if (n <= 0 || n > XPRS_MAX_WIRE) continue;
        if (!strstr(wire, " hears:")) continue;
        n = sign_wire(wire, n, (int)sizeof wire);
        xprslan_send(wire, n);
        xprsnow_send(wire, n);
        if (xprsrns_is_up()) xprsrns_send(wire, n);
    }
}

/* ── Meeting on a working channel (§23.7) ───────────────────────────────── */

/*
 * ts: under a synced clock, `epoch:<boots>.<uptime>` otherwise (10.7).
 *
 * The comment here used to say this board had no SNTP. It has had it for some
 * time -- esp_sntp.h is included above and wifi_up() starts it -- so the
 * everyday form is now a real timestamp, and epoch: is what covers the
 * minutes before the first sync and the times there is no network at all.
 *
 * The boot counter is the half that was missing. 10.7 dates a clockless
 * station's packets by <boots>.<uptime> precisely so a receiver can ORDER
 * them: without the boots ordinal every reboot restarts the uptime and
 * yesterday's packet and today's are indistinguishable. This sent a hardcoded
 * zero, so every packet this station ever aired without a clock claimed to be
 * from the same boot as every other. The dongle has kept the counter in NVS
 * all along; this is the same thing, in the same shape.
 */
static uint32_t s_boot_epoch;

static void boot_epoch_init(void)
{
    nvs_handle_t h;
    if (nvs_open("xprscfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u32(h, "bootcnt", &s_boot_epoch);       /* absent -> 0 */
        s_boot_epoch++;
        nvs_set_u32(h, "bootcnt", s_boot_epoch);
        nvs_commit(h);
        nvs_close(h);
    } else {
        s_boot_epoch = 1;
    }
}

static void time_field(char *out, int cap)
{
    const uint32_t ep = xst_epoch_now();
    if (ep) {
        struct tm tmv;
        time_t t = (time_t)ep;
        gmtime_r(&t, &tmv);
        char ts[24];
        strftime(ts, sizeof ts, "%Y-%m-%d_%H:%M:%S", &tmv);
        snprintf(out, cap, "ts:%s", ts);
        return;
    }
    snprintf(out, cap, "epoch:%u.%u", (unsigned)s_boot_epoch,
             (unsigned)(esp_timer_get_time() / 1000000ULL));
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
/* The boots ordinal of 10.7, for whatever asks the ops table for it. Zero
 * until boot_epoch_init() has read NVS, which happens before anything airs. */
static uint32_t boot_epoch(void) { return s_boot_epoch; }

static bool verified(const xprs_t *p)
{
    char from[10];
    if (!xprs_get_str(p, "f", from, sizeof from)) return false;
    const uint8_t *k = peer_key(from);
    return k && xprsid_verify(p, k);
}

static bool may_move(void) { return true; }   /* this board gateways nothing */
static void air_identity(void);               /* defined below, used by the ops */

static void on_working(const char *peer, uint8_t channel, bool lr)
{
    ESP_LOGW(TAG, "on channel %u with %s%s", channel, peer,
             lr ? " (long range)" : "");
}

static void on_home(const char *peer, bool worked)
{
    ESP_LOGW(TAG, "home again after %s (%s)", peer[0] ? peer : "nobody",
             worked ? "the pair met" : "nothing happened");
}

static const xc_ops_t k_chan_ops = {
    .sign = sign_wire,
    .verified = verified,
    .air = xprsnow_send,
    .now_ms = now_ms,
    .time_field = time_field,
    .epoch = boot_epoch,
    .hold_reconnect = NULL,      /* handled in the event handler above */
    .announce_identity = air_identity,
    .may_move = may_move,
    .settle = xprsnow_settle,
    .trace = xprsnow_set_trace,
    .on_working = on_working,
    .on_home = on_home,
};

/* §9.3: publish the key, or nobody can check a thing we say — including the
 * acceptance §23.7 leans on. */
static void air_identity(void)
{
    const char *npub = nostr_keys_get_npub();
    if (!npub || !npub[0]) return;
    char wire[XPRS_MAX_WIRE + 1], ts[24];
    time_field(ts, sizeof ts);
    int n = snprintf(wire, sizeof wire, "t:identity f:%s %s k:%s",
                     s_call, ts, npub);
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = sign_wire(wire, n, (int)sizeof wire);
    xprsnow_send(wire, n);
    /* And on BLE, or a phone can never check a signature of ours and meters us
     * as a stranger for good -- two history replays an hour instead of six.
     * The same argument is why this goes out on the LAN and on Reticulum: a
     * peer that only ever meets this station over the internet had no way at
     * all to learn its key, so every ask it made was metered as a stranger's
     * and every reply it got was unverifiable. */
    if (xprsble_is_active()) xprsble_send(wire, n);
    xprslan_send(wire, n);
    if (xprsrns_is_up()) xprsrns_send(wire, n);
    /* And keep it as the hello the bearer airs the moment it (re)connects to
     * a hub: 36.12.2's identity-announcement lane is the one thing guaranteed
     * to cross a shared transport, and a peer that joins after us would
     * otherwise wait for our next ordinary wire to learn we exist. */
    xprsrns_set_hello(wire, n);
}

/* ── Status, every 15 s, the same shape the dongle prints ───────────────── */

static void status_task(void *arg)
{
    (void)arg;
    int n = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500));
        /* §23.7's deadline: the one thing that guarantees a station that moved
         * comes back. Cheap, so it runs on the fast tick, not the log's. */
        xprschan_tick();
        /* And the carousel, which only ever speaks into a silence. */
        echo_tick();
        /*
         * Section 18.1: "every 30 minutes is a reasonable interval on a quiet
         * channel". This was every 60 s, and because each announcement carries
         * a fresh ts:/epoch: every one was a NEW record in every archive that
         * heard it -- measured on a bench station, 120 of the newest 200
         * records were one neighbour's identity and not one was a message. A
         * key binding does not change; saying so once a minute buys nothing
         * and costs everybody's store.
         *
         * First airing stays early (n small) so a station that just came up is
         * findable without a half-hour wait -- it is exactly the one its
         * neighbours have not heard of.
         */
        n++;
        if (n == 60 || n % 3600 == 0) air_identity();     /* 30 s, then 30 min */
        /* The BLE5 beacon, every 30 s. A phone scans in bursts and a station it
         * has not heard is a station it cannot ask, so this is the one cadence
         * that decides whether an off-grid device catches up at all. */
        if (n % 60 == 0) air_ble_beacon();
        /* The signed hears: observations, same cadence, same task. */
        if (n % 120 == 60) air_signed_observations();

        /* Rollback self-test (25.8). A new image is on probation until it
         * has held together for two minutes: the API listening, a bearer
         * up, an address when WiFi is wanted, storage mounted, and no panic
         * behind us. Two minutes rather than thirty seconds because "the
         * radios came up and then the heap ran out on the first signature"
         * is exactly the failure this exists to catch. */
        if (n == 240) {
            /* One definition, not a second copy of the same predicate:
             * the roster below is what the log and the heartbeat judge
             * too. A panic behind us is the one extra condition, because
             * it is about the boot rather than about what is running. */
            bool healthy = xh_all_ok() && esp_reset_reason() != ESP_RST_PANIC;
            if (healthy) xota_mark_healthy();
            else         xota_mark_unhealthy();
        }
        if (n % 30) continue;                     /* the rest every 15 s */

        uint32_t rx = 0, tx = 0, cancelled = 0, dropped = 0;
        uint32_t issued = 0, done = 0, failed = 0;
        xprsnow_stats(&rx, &tx, &cancelled, &dropped);
        /* `tx` counts what this station decided to say; `done`/`fail` count what
         * the radio actually did with it. They were the same number until the
         * send callback existed, and the difference is where a rendezvous that
         * "sent" its acceptance and was not heard shows up. */
        xprsnow_tx_stats(&issued, &done, &failed);
        /* Re-read the parts that can die after boot, not only fail to
         * start. Then say so only when the picture changes. */
        xh_set(XH_HTTP, xprs_api_httpd() != NULL);
        xh_set(XH_LAN, xprslan_is_active() || !xcfg_get_bool("wifi_on", true));
        xh_set(XH_NOW, xprsnow_is_active() || !xcfg_get_bool("espnow_on", true));
        xh_set(XH_ADDR, !xcfg_get_bool("wifi_on", true) || s_ip_str[0] != 0);
        xh_set(XH_INDEX, s_index != NULL || !xcfg_get_bool("index_on", true));
        xh_report(false);
        xh_heap_floor(heap_floor());

        ESP_LOGW(TAG, "alive %us heap=%u/%u call=%s ch=%u espnow rx=%u tx=%u "
                      "cancel=%u drop=%u sent=%u/%u fail=%u peers=%d heard=%u",
                 (unsigned)(esp_timer_get_time() / 1000000ULL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                 s_call,
                 xprsnow_channel(), (unsigned)rx, (unsigned)tx,
                 (unsigned)cancelled, (unsigned)dropped,
                 (unsigned)done, (unsigned)issued, (unsigned)failed,
                 xprsnow_peer_count(600), (unsigned)s_heard_count);
    }
}

/* ── WiFi ───────────────────────────────────────────────────────────────── */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id,
                          void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Leaving the access point's channel on purpose (§23.7) looks exactly
         * like losing the link. Reconnecting here would drag us home before the
         * far side arrives, which is what it did the first time. */
        if (xprschan_busy()) return;
        ESP_LOGW(TAG, "wifi disconnected — retrying");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof s_ip_str, IPSTR, IP2STR(&e->ip_info.ip));
        ESP_LOGI(TAG, "wifi up on channel %u, ip %s", xprsnow_channel(),
                 s_ip_str);
    }
}

static void wifi_up(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    if (s_ssid[0]) {
        wifi_config_t wc = {0};
        snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", s_ssid);
        snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", s_pass);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_LOGI(TAG, "associating to \"%s\" — this is what puts us on the "
                      "other station's channel", s_ssid);
    } else {
        ESP_LOGW(TAG, "no WiFi credentials: staying unassociated on channel %d. "
                      "That only meets another unassociated board — an "
                      "associated station is on its access point's channel.",
                 s_board->espnow_channel);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!s_ssid[0]) {
        ESP_ERROR_CHECK(esp_wifi_set_channel(s_board->espnow_channel,
                                             WIFI_SECOND_CHAN_NONE));
    }
}

/* ── Internet probe ─────────────────────────────────────────────────────── */

/* "Internet: up" means a TCP handshake to a stable public address completed
 * just now — not that WiFi has an IP, which only proves the access point
 * exists. Once a minute, 3 s timeout, and only while an address is held. */
static void inet_probe_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (!s_ip_str[0]) {
            s_inet_known = false;
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        bool up = false;
        if (fd >= 0) {
            struct timeval tv = { .tv_sec = 3, .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
            struct sockaddr_in dst = {
                .sin_family = AF_INET,
                .sin_port = htons(443),
            };
            dst.sin_addr.s_addr = inet_addr("1.1.1.1");
            up = connect(fd, (struct sockaddr *)&dst, sizeof dst) == 0;
            close(fd);
        }
        s_inet_up = up;
        s_inet_known = true;
        vTaskDelay(pdMS_TO_TICKS(60000));
    }
}

/* ── The screen ─────────────────────────────────────────────────────────── */

/* The T-Dongle's dashboard, on a bigger panel: same bands, same rule —
 * metadata only, never message content. What moves through it is the
 * board's XAPP_KEY_* intents (xprs_app.h), not any particular button. */
/* Seven panels, in the order a person reaches for them: the scope first,
 * then talking, then the numbers, then the lists, then the machine itself.
 * (Traffic's raw counters left the rotation -- /api/status still has them.) */
#define UI_PANEL_COUNT 7
/* The UI is told which of these is up, and a screen with fewer views
 * than there are panels folds them by that number. Both sides have to
 * be counting the same panels. */
_Static_assert(UI_PANEL_COUNT == XUI_PANEL_COUNT,
               "panel numbering must match xprs_ui.h");
#define UI_INRANGE_SEC 300

static int s_panel;

/* ── The chat panel: rooms, what is typed, and what leaves ───────────────
 *
 * Only for a board that offers raw_key -- see xprs_app.h. Everything here
 * is the station's own web page rendered natively, including the rooms:
 * Local is scope:local, Global is the default scope, Social is t:status,
 * and a callsign is a 1:1 addressed with d:.
 */
typedef enum { RM_LOCAL = 0, RM_GLOBAL, RM_SOCIAL, RM_FIXED } room_kind_t;

/* Six, because the rail holds twelve rows and three headings and the three
 * fixed rooms have already taken six of them. A peer that cannot be shown
 * cannot be chosen either, so there is no point remembering more. */
#define CHAT_PEERS_MAX 6

static bool chat_active(void)   /* the interactive panel, not the table */
{
    return s_panel == 1 && s_board && s_board->raw_key;
}

static int  s_room;                       /* 0..2 fixed, else a peer index */
static char s_peer[CHAT_PEERS_MAX][10];   /* callsigns offered for a 1:1  */
static int  s_peer_n;

/* ONE scratch copy of the chat ring, shared by everything on the UI task
 * that needs to walk it -- the rail's peer list and the bubbles.
 *
 * Forty rows is 6.5 KB. On the stack it overflowed ui_task; as two separate
 * function statics it was 13 KB of a board whose whole free heap is under
 * twenty, and the M5Stack answered ESP_ERR_HTTPD_TASK and drew a black
 * screen for want of it. The UI is a single task, so one copy is all there
 * has ever been a need for. */
static XPRS_PSRAM_BSS xst_chat_t s_chat_scratch[XST_CHAT_MAX];
static bool s_room_unread[RM_FIXED + CHAT_PEERS_MAX];
static char s_compose[121];               /* what has been typed          */
static int  s_compose_n;

/* One slot, filled by the UI task and drained by idx_task. Signing is
 * several KB of secp256k1 stack and ui_task has 6 KB, which is the same
 * trap that once PANIC'd the bearer task -- so the UI composes the text
 * and somebody with room to stand signs it. */
static struct {
    volatile bool full;
    char text[121];
    char to[10];        /* empty unless a 1:1 */
    uint8_t kind;       /* room_kind_t for the fixed rooms, RM_FIXED = dm */
} s_outbox;

/* The base callsign: X1A67X-2 and X1A67X are one person, and a rail that
 * listed both would be lying about how many conversations there are. */
static void base_call(const char *in, char *out, int cap)
{
    int i = 0;
    for (; in[i] && in[i] != '-' && i < cap - 1; i++) out[i] = in[i];
    out[i] = 0;
}

/* Which room a stored saying belongs to, or -1 for somebody else's 1:1 --
 * which is not ours to show. Mirrors roomOf() in the web page. */
static int room_of(const xst_chat_t *c)
{
    if (c->kind == 3) return RM_SOCIAL;
    if (c->kind == 2) {
        char me[10], f[10], t[10];
        base_call(s_call, me, sizeof me);
        base_call(c->from, f, sizeof f);
        base_call(c->to, t, sizeof t);
        const char *peer = NULL;
        if (strcasecmp(t, me) == 0) peer = f;
        else if (strcasecmp(f, me) == 0) peer = t;
        if (!peer || !peer[0]) return -1;
        for (int i = 0; i < s_peer_n; i++)
            if (strcasecmp(s_peer[i], peer) == 0) return RM_FIXED + i;
        return -1;
    }
    return c->kind == 1 ? RM_LOCAL : RM_GLOBAL;
}

/* The rail's peers: everyone within reach, so a conversation can be
 * started with somebody who has not spoken yet, plus anyone we have
 * already exchanged with even if they have since gone quiet. */
static void chat_refresh_peers(void)
{
    char me[10];
    base_call(s_call, me, sizeof me);
    s_peer_n = 0;

    xst_dev_t dev[XST_SEEN_MAX];
    /* An hour, not UI_INRANGE_SEC's five minutes: somebody who spoke half
     * an hour ago is still worth being able to answer. */
    int dn = xst_devices(dev, XST_SEEN_MAX, 3600);
    for (int i = 0; i < dn && s_peer_n < CHAT_PEERS_MAX; i++) {
        char b[10];
        base_call(dev[i].call, b, sizeof b);
        if (!b[0] || strcasecmp(b, me) == 0) continue;
        /* A group is not a person: only a callsign can hold a 1:1. */
        if (!xprs_is_station(b, (int)strlen(b))) continue;
        bool seen = false;
        for (int j = 0; j < s_peer_n; j++)
            if (strcasecmp(s_peer[j], b) == 0) { seen = true; break; }
        if (!seen) snprintf(s_peer[s_peer_n++], 10, "%s", b);
    }

    xst_chat_t *rows = s_chat_scratch;
    int cn = xst_chat(rows, XST_CHAT_MAX);
    for (int i = 0; i < cn && s_peer_n < CHAT_PEERS_MAX; i++) {
        if (rows[i].kind != 2) continue;
        char f[10], t[10];
        base_call(rows[i].from, f, sizeof f);
        base_call(rows[i].to, t, sizeof t);
        const char *peer = strcasecmp(t, me) == 0 ? f
                         : strcasecmp(f, me) == 0 ? t : NULL;
        if (!peer || !peer[0]) continue;
        bool seen = false;
        for (int j = 0; j < s_peer_n; j++)
            if (strcasecmp(s_peer[j], peer) == 0) { seen = true; break; }
        if (!seen) snprintf(s_peer[s_peer_n++], 10, "%s", peer);
    }
}

/* Something arrived. Which room it landed in decides which rail row grows
 * a dot -- except the room being read, where arriving and being read are
 * the same event. */
static void chat_note_unread(const xprs_t *p)
{
    if (!s_board || !s_board->raw_key) return;   /* no rail to mark */
    char type[16];
    xprs_type(p, type, sizeof type);
    bool status = strcmp(type, "status") == 0;
    if (!status && strcmp(type, "message") != 0) return;

    xst_chat_t c;
    memset(&c, 0, sizeof c);
    if (!xprs_get_str(p, "f", c.from, sizeof c.from)) return;
    char dst[16], sc[12];
    bool direct = xprs_get_str(p, "d", dst, sizeof dst) && dst[0] != '#';
    if (direct) snprintf(c.to, sizeof c.to, "%.9s", dst);
    c.kind = status ? 3 : direct ? 2
           : (xprs_get_str(p, "scope", sc, sizeof sc) &&
              strcmp(sc, "local") == 0) ? 1 : 0;

    int r = room_of(&c);
    if (r < 0 || r >= (int)(sizeof s_room_unread / sizeof s_room_unread[0]))
        return;
    if (chat_active() && r == s_room) return;
    s_room_unread[r] = true;
}

/* Airs a wire on every bearer and spools it; defined with the HTTP API,
 * which was its first caller. The chat panel is its second. */
static bool api_send_wire(const char *wire, int len, const char *bearer,
                          char *took, size_t took_cap);

/* Hand what was typed to whoever has the stack to sign it. Returns false
 * when the slot is still full -- the previous saying has not left yet, and
 * dropping this one is better than overwriting that one. */
static bool chat_queue_send(void)
{
    if (s_outbox.full || !s_compose_n) return false;
    snprintf(s_outbox.text, sizeof s_outbox.text, "%s", s_compose);
    if (s_room >= RM_FIXED) {
        snprintf(s_outbox.to, sizeof s_outbox.to, "%s",
                 s_peer[s_room - RM_FIXED]);
        s_outbox.kind = RM_FIXED;
    } else {
        s_outbox.to[0] = 0;
        s_outbox.kind = (uint8_t)s_room;
    }
    s_outbox.full = true;         /* last: idx_task reads the rest first */
    s_compose[0] = 0;
    s_compose_n = 0;
    return true;
}

/* One keystroke while the chat panel is up. Returns true when it was
 * consumed, which is what keeps the console commands from seeing it. */
static bool chat_key(int ch)
{
    if (ch == 0x1b) {                        /* escape: drop the draft, go home */
        s_compose[0] = 0;
        s_compose_n = 0;
        s_panel = 0;
        return true;
    }
    if (ch == 0x0d || ch == '\n') {          /* enter: send */
        if (!s_compose_n) return true;
        if (!chat_queue_send())
            ESP_LOGW(TAG, "chat: the last message has not gone out yet");
        return true;
    }
    if (ch == 0x08 || ch == 0x7f) {          /* backspace */
        if (s_compose_n) s_compose[--s_compose_n] = 0;
        return true;
    }
    if (ch >= 0x20 && ch < 0x7f) {           /* printable ASCII */
        if (s_compose_n < (int)sizeof s_compose - 1) {
            s_compose[s_compose_n++] = (char)ch;
            s_compose[s_compose_n] = 0;
        }
        return true;
    }
    return false;
}


static void ui_render(void)
{
    char body[720];
    int list_n = 0;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    /* Bank transmitted-packet deltas into the hourly ring on every render
     * tick, whichever panel is up -- the bearers stay untouched and the
     * Stats panel never misses what happened while it was not looking. */
    {
        uint32_t issued = 0, done = 0, failed = 0;
        uint32_t lrx = 0, ltx = 0, lcancel = 0;
        xprsnow_tx_stats(&issued, &done, &failed);
        xprslan_stats(&lrx, &ltx, &lcancel);
        xst_tx_total(done + ltx);
    }

    body[0] = 0;
    xui_show_home(s_panel == 0);
    xui_show_table(s_panel != 0 && s_panel != 2 && !chat_active());
    xui_show_stats(s_panel == 2);
    xui_show_chat(chat_active());

    switch (s_panel) {
    case 0: {   /* Radar: the scope, and who is in reach on which link */
        /* One station, one count. The devices store is keyed by callsign,
         * so a station heard on three bearers is one row wearing the bearer
         * it was LAST heard on -- which is what "reachable via" honestly
         * means from here. */
        xst_dev_t devs[XST_SEEN_MAX];
        int nb = xst_devices(devs, XST_SEEN_MAX, UI_INRANGE_SEC);
        int n_now = 0, n_lan = 0, n_lora = 0, n_inet = 0;
        for (int i = 0; i < nb; i++) {
            if (strcmp(devs[i].bearer, "espnow") == 0) n_now++;
            else if (strcmp(devs[i].bearer, "lora") == 0) n_lora++;
            else if (strcmp(devs[i].bearer, "lan") == 0) {
                /* A LAN packet wearing via: was carried onto our network by
                 * a gateway -- the nearest thing this station has to "heard
                 * from the internet" until an internet bearer exists. */
                if (devs[i].hops > 0) n_inet++;
                else n_lan++;
            }
        }

        /* A count, and nothing else: the dot already says whether the link
         * is up, and the addresses and channel numbers that used to sit
         * under each name live on the This-device panel. A link the board
         * does not have takes no row at all -- an empty name hides it. */
        char d[8], note[24];
        int row = 0;
        snprintf(d, sizeof d, "%d", n_now);
        /* The channel is what decides whether two ESP-NOW stations can hear
         * each other at all, so it belongs where the link does. */
        snprintf(note, sizeof note, "channel %u", xprsnow_channel());
        xui_home_row(row++, "ESP-NOW", xprsnow_is_active(), d,
                     xprsnow_is_active() ? note : NULL);

        /* The address under the name: the one fact from this link somebody
         * actually needs off the screen -- to open the chat page, or to
         * curl the API -- and hunting it on the This-device panel while
         * standing at the board was the wrong trade. */
        snprintf(d, sizeof d, "%d", n_lan);
        xui_home_row(row++, "WiFi / LAN", s_ip_str[0] != 0,
                     s_ip_str[0] ? d : "",
                     s_ip_str[0] ? s_ip_str
                                 : (s_ssid[0] ? "joining..." : NULL));

        snprintf(d, sizeof d, "%d", n_inet);
        xui_home_row(row++, "Internet", s_inet_known && s_inet_up,
                     s_inet_known && s_inet_up ? d : "", NULL);

        /* Only a board with the radio says anything about it. */
        if (s_board->lora) {
            snprintf(d, sizeof d, "%d", n_lora);
            /* The band, for the same reason as the channel above: two
             * radios on different frequencies are simply deaf to each
             * other, and nothing else on the screen would say so. */
            uint32_t hz = s_board->lora->freq_hz ? s_board->lora->freq_hz
                                                 : 868000000u;
            snprintf(note, sizeof note, "%u.%u MHz",
                     (unsigned)(hz / 1000000u),
                     (unsigned)((hz % 1000000u) / 100000u));
            xui_home_row(row++, "LoRa", xprslora_is_active(), d,
                         xprslora_is_active() ? note : NULL);
        }
        for (; row < XUI_HOME_ROWS; row++)
            xui_home_row(row, "", false, "", NULL);

        xui_home_counts(nb, s_heard_count);

        /* The scope: everybody in reach, at their estimated distance. */
        xui_blip_t blips[XUI_BLIP_MAX];
        int nblip = nb > XUI_BLIP_MAX ? XUI_BLIP_MAX : nb;
        for (int i = 0; i < nblip; i++) {
            snprintf(blips[i].label, sizeof blips[i].label, "%s",
                     devs[i].call);
            blips[i].meters = xst_est_distance_m(devs[i].rssi);
        }
        xui_radar_blips(blips, nblip);
        xui_set_title("Radar 1/7");
        break;
    }
    case 4: {   /* Traffic: the packets going past, newest first */
        xui_flow_t rows[XUI_FLOW_ROWS];
        int nr = 0;
        for (int i = 0; i < FLOW_MAX && nr < XUI_FLOW_ROWS; i++) {
            int idx = (s_flow_n - 1 - i);
            if (idx < 0) break;
            flow_t *fl = &s_flow[idx % FLOW_MAX];
            if (!fl->call[0]) continue;
            xui_flow_t *r = &rows[nr++];
            snprintf(r->from, sizeof r->from, "%s", fl->call);
            snprintf(r->to, sizeof r->to, "%s", fl->to);
            snprintf(r->type, sizeof r->type, "%s", fl->type);
            snprintf(r->link, sizeof r->link, "%s", fl->link);
            r->dist_m = fl->rssi ? xst_est_distance_m(fl->rssi) : -1.0f;
            r->age_s = (now - fl->ms) / 1000;
            snprintf(r->text, sizeof r->text, "%s", fl->text);
        }
        xui_flow_rows(rows, nr);
        list_n = nr;
        xui_set_title("Traffic 5/7");
        break;
    }
    case 3: {   /* Devices: everyone in reach, detail on selection */
        static const char *const hdr[4] = { "Call", "Link", "Dist", "When" };
        static const int cw[4] = { 80, 74, 60, 106 };
        xui_table_setup(4, hdr, cw);

        static XPRS_PSRAM_BSS xui_row_t tr[XUI_TAB_ROWS];
        xst_dev_t devs[XUI_TAB_ROWS];
        int nr = xst_devices(devs, XUI_TAB_ROWS, UI_INRANGE_SEC);
        for (int i = 0; i < nr; i++) {
            uint32_t age = (now - devs[i].last_ms) / 1000;
            xui_row_t *r = &tr[i];
            snprintf(r->cell[0], sizeof r->cell[0], "%s", devs[i].call);
            snprintf(r->cell[1], sizeof r->cell[1], "%s", devs[i].bearer);
            if (devs[i].rssi) {
                int m = (int)(xst_est_distance_m(devs[i].rssi) + 0.5f);
                snprintf(r->cell[2], sizeof r->cell[2], "~%dm", m);
                snprintf(r->detail, sizeof r->detail,
                         "%s via %s: %d dBm, about %d m away. "
                         "Heard %lu s ago.",
                         devs[i].call, devs[i].bearer,
                         devs[i].rssi, m, (unsigned long)age);
            } else {
                snprintf(r->cell[2], sizeof r->cell[2], "-");
                snprintf(r->detail, sizeof r->detail,
                         "%s via %s: distance unknown on this link. "
                         "Heard %lu s ago.",
                         devs[i].call, devs[i].bearer,
                         (unsigned long)age);
            }
            snprintf(r->cell[3], sizeof r->cell[3], "%lus",
                     (unsigned long)age);
        }
        xui_table_rows(tr, nr);
        list_n = nr;
        xui_set_title("Reachable 4/7");
        break;
    }
    case 5: {   /* This device: the station's facts, full values on selection */
        static const char *const hdr[2] = { "Item", "Value" };
        static const int cw[2] = { 110, 210 };
        xui_table_setup(2, hdr, cw);

        static XPRS_PSRAM_BSS xui_row_t tr[XUI_TAB_ROWS];
        int nr = 0;
        #define NROW(c0, val, det) do { \
            snprintf(tr[nr].cell[0], sizeof tr[nr].cell[0], "%s", c0); \
            snprintf(tr[nr].cell[1], sizeof tr[nr].cell[1], "%s", val); \
            snprintf(tr[nr].detail, sizeof tr[nr].detail, "%s", det); \
            nr++; } while (0)
        char val[26], det[160];   /* val matches the table cell size */

        NROW("Callsign", s_call,
             "Station callsign, derived from the signing key so a receiver "
             "can re-derive and check it.");

        const char *npub = nostr_keys_get_npub();
        if (npub && npub[0]) {
            snprintf(val, sizeof val, "%.12s...", npub);
            NROW("Key", val, npub);
        } else {
            NROW("Key", "none", "No signing key: packets go out unsigned.");
        }

        uint32_t up = now / 1000;
        snprintf(val, sizeof val, "%02lu:%02lu:%02lu",
                 (unsigned long)(up / 3600), (unsigned long)((up / 60) % 60),
                 (unsigned long)(up % 60));
        NROW("Uptime", val, "Time since this station booted.");

        unsigned heap = (unsigned)esp_get_free_heap_size();
        snprintf(val, sizeof val, "%u KB", heap / 1024);
        snprintf(det, sizeof det, "%u bytes of internal RAM free.", heap);
        NROW("Heap", val, det);
        if (s_board->battery_mv) {
            if (s_bat_mv >= 0) snprintf(val, sizeof val, "%d mV", s_bat_mv);
            else snprintf(val, sizeof val, "--");
            snprintf(det, sizeof det, "%s%s. The screen blanks after %s s "
                     "idle on battery; any key or touch wakes it.",
                     bat_state_name()[0] == 'c' ? "Charging" :
                     bat_state_name()[0] == 'd' ? "Discharging" : "Trend unknown",
                     s_bat_n < BAT_RING ? " (first minute)" : "",
                     xcfg_get("screen_off_s", "120"));
            NROW("Battery", val, det);
        }

        NROW("Rendezvous", xprschan_busy() ? "Busy" : "Idle",
             xprschan_busy()
                 ? "Off the home channel for an agreed working-channel "
                   "meeting (XPRS 23.7)."
                 : "Listening on the home channel; no working-channel "
                   "meeting under way.");

        snprintf(val, sizeof val, "%d", s_peers_n);
        NROW("Peer keys", val,
             "Public keys learned from identity packets; they verify "
             "signatures and address private traffic.");

        NROW("IP", s_ip_str[0] ? s_ip_str : "none",
             s_ip_str[0] ? "Address on the local WiFi network."
                         : "No WiFi address yet.");
        #undef NROW
        xui_table_rows(tr, nr);
        list_n = nr;
        xui_set_title("This device 6/7");
        break;
    }
    case 6: {   /* Settings: OK (button A) toggles the selected row */
        static const char *const hdr[2] = { "Setting", "State" };
        static const int cw[2] = { 170, 150 };
        xui_table_setup(2, hdr, cw);

        static XPRS_PSRAM_BSS xui_row_t tr[XUI_TAB_ROWS];
        int nr = 0;
        #define SROW(c0, val, det) do { \
            snprintf(tr[nr].cell[0], sizeof tr[nr].cell[0], "%s", c0); \
            snprintf(tr[nr].cell[1], sizeof tr[nr].cell[1], "%s", val); \
            snprintf(tr[nr].detail, sizeof tr[nr].detail, "%s", det); \
            nr++; } while (0)
        /* Same, but the detail is a format -- the help has to name the key
         * that actually acts on THIS board, and that is only known at run
         * time (ok_key()). */
        #define SROWF(c0, val, ...) do { \
            snprintf(tr[nr].cell[0], sizeof tr[nr].cell[0], "%s", c0); \
            snprintf(tr[nr].cell[1], sizeof tr[nr].cell[1], "%s", val); \
            snprintf(tr[nr].detail, sizeof tr[nr].detail, __VA_ARGS__); \
            nr++; } while (0)

        SROWF("WiFi / LAN", xcfg_get_bool("wifi_on", true) ? "On" : "Off",
             "Join the WiFi network and speak XPRS over the LAN. "
             "%s toggles; applies after restart.", ok_key());
        SROWF("ESP-NOW", xcfg_get_bool("espnow_on", true) ? "On" : "Off",
             "The 2.4 GHz radio link between nearby stations. "
             "%s toggles; applies after restart.", ok_key());
        SROWF("Digipeater", xcfg_get_bool("digi_on", true) ? "On" : "Off",
             "Re-air packets heard on ESP-NOW back onto ESP-NOW, for "
             "stations past our reach. %s toggles; applies at once.", ok_key());
        SROWF("Bridge", xcfg_get_bool("bridge_on", true) ? "On" : "Off",
             "Carry LAN traffic onto the ESP-NOW radio. "
             "%s toggles; applies at once.", ok_key());
        SROWF("iGate", xcfg_get_bool("igate_on", true) ? "On" : "Off",
             "Carry ESP-NOW traffic onto the LAN, toward the internet "
             "side. %s toggles; applies at once.", ok_key());
        char idet[160];
        if (s_index) {
            xprsidx_stats_t ist;
            xprsindex_stats(s_index, &ist);
            snprintf(idet, sizeof idet,
                     "Keep every packet heard, answer cmd:history, hold "
                     "mail. Holding %lu packet%s. %s toggles.",
                     (unsigned long)ist.count, ist.count == 1 ? "" : "s",
                     ok_key());
        } else {
            snprintf(idet, sizeof idet,
                     "Keep every packet heard, answer cmd:history, hold "
                     "mail. Storage not mounted. %s toggles.", ok_key());
        }
        SROW("Indexer", xcfg_get_bool("index_on", true) ? "On" : "Off", idet);
        char det[160];
        if (xcfg_share_running())
            snprintf(det, sizeof det,
                     "Serving now: open http://%s/ in a browser to edit "
                     "config.ini (WiFi, name, nsec). %s turns it off.",
                     s_ip_str[0] ? s_ip_str : "<ip>", ok_key());
        else
            snprintf(det, sizeof det,
                     "Off. %s starts a browser editor for config.ini "
                     "(WiFi, name, nsec)%s.",
                     ok_key(), s_ip_str[0] ? "" : " -- needs WiFi first");
        SROW("Config share", xcfg_share_running() ? "On" : "Off", det);
        SROWF("Hotspot", xcfg_get_bool("ap_on", true) ? "On" : "Off",
             "The walk-up WiFi: an open network whose sign-in page is the "
             "chat. %s toggles; applies after restart.", ok_key());
        SROW("Name", xcfg_get("name", "--"),
             "The device's friendly name. Set it through the config "
             "share above.");
        {
            uint32_t tep = xst_epoch_now();
            char tval[26];
            if (tep) {
                time_t lt = (time_t)((int64_t)tep + s_tz_off);
                struct tm tmv;
                gmtime_r(&lt, &tmv);
                snprintf(tval, sizeof tval, "%02d:%02d UTC%+d",
                         tmv.tm_hour, tmv.tm_min, s_tz_off / 3600);
            } else {
                snprintf(tval, sizeof tval, "No sync");
            }
            snprintf(det, sizeof det,
                     "NTP: %s. Set the server and the timezone offset in "
                     "config.ini through the config share.",
                     xcfg_get("ntp", "pool.ntp.org"));
            SROW("Time", tval, det);
        }
        {
            char wdet[160];
            if (s_index) {
                xprsidx_stats_t wst;
                xprsindex_stats(s_index, &wst);
                snprintf(wdet, sizeof wdet,
                         "Delete every packet this station holds -- the "
                         "chats it hosts included. %lu held now. %s wipes "
                         "at once; there is no undo.",
                         (unsigned long)wst.count, ok_key());
            } else {
                snprintf(wdet, sizeof wdet,
                         "Delete every packet this station holds. "
                         "Storage not mounted.");
            }
            SROW("Wipe archive", "--", wdet);
        }
        SROWF("Restart", "--",
             "%s restarts the station so pending changes take effect.", ok_key());
        #undef SROW
        xui_table_rows(tr, nr);
        list_n = nr;
        xui_set_title("Settings 7/7");
        break;
    }
    case 1: if (chat_active()) {
        /* The interactive chat: rooms down the left, the conversation as
         * bubbles, and a composer. Only reached on a board that can type. */
        chat_refresh_peers();
        if (s_room >= RM_FIXED + s_peer_n) s_room = RM_GLOBAL;

        static xui_room_t rr[XUI_CHAT_ROOMS];
        int rn = 0, sel = 0;
        static const char *const fixed[RM_FIXED] = { "Local", "Global",
                                                     "Social" };
        for (int i = 0; i < XUI_CHAT_ROOMS; i++) s_rail_room[i] = -1;
        snprintf(rr[rn].name, sizeof rr[rn].name, "ROOMS");
        rr[rn].heading = true; rr[rn].unread = false; rn++;
        for (int i = 0; i < RM_FIXED && rn < XUI_CHAT_ROOMS; i++) {
            if (i == RM_SOCIAL) {
                snprintf(rr[rn].name, sizeof rr[rn].name, "FEED");
                rr[rn].heading = true; rr[rn].unread = false; rn++;
                if (rn >= XUI_CHAT_ROOMS) break;
            }
            snprintf(rr[rn].name, sizeof rr[rn].name, "%s", fixed[i]);
            rr[rn].heading = false;
            rr[rn].unread = s_room_unread[i];
            if (s_room == i) sel = rn;
            s_rail_room[rn] = i;
            rn++;
        }
        if (s_peer_n && rn < XUI_CHAT_ROOMS) {
            snprintf(rr[rn].name, sizeof rr[rn].name, "PEOPLE");
            rr[rn].heading = true; rr[rn].unread = false; rn++;
        }
        for (int i = 0; i < s_peer_n && rn < XUI_CHAT_ROOMS; i++) {
            snprintf(rr[rn].name, sizeof rr[rn].name, "%s", s_peer[i]);
            rr[rn].heading = false;
            rr[rn].unread = s_room_unread[RM_FIXED + i];
            if (s_room == RM_FIXED + i) sel = rn;
            s_rail_room[rn] = RM_FIXED + i;
            rn++;
        }
        xui_chat_rooms(rr, rn, sel);

        /* The conversation. xst_chat gives newest first; a thread reads
         * the other way, so it is walked backwards into the array. */
        static xui_msg_t mm[XUI_CHAT_MSGS];
        xst_chat_t *rows = s_chat_scratch;
        int cn = xst_chat(rows, XST_CHAT_MAX);
        uint32_t nowep = xst_epoch_now();
        char me[10];
        base_call(s_call, me, sizeof me);
        int mn = 0;
        for (int i = cn - 1; i >= 0 && mn < XUI_CHAT_MSGS; i--) {
            if (room_of(&rows[i]) != s_room) continue;
            xui_msg_t *m = &mm[mn++];
            char f[10];
            base_call(rows[i].from, f, sizeof f);
            m->outgoing = strcasecmp(f, me) == 0;
            snprintf(m->from, sizeof m->from, "%s", rows[i].from);
            snprintf(m->text, sizeof m->text, "%s", rows[i].text);
            if (rows[i].ep && nowep && nowep >= rows[i].ep) {
                uint32_t age = nowep - rows[i].ep;
                unsigned h = (unsigned)(age / 3600);
                /* "now", not "7s": a seconds counter changes every render,
                 * and the changing text is what forces the bubble rebuild
                 * the fragmentation guard below exists to avoid. */
                if (age < 60)
                    snprintf(m->when, sizeof m->when, "now");
                else if (age < 3600)
                    snprintf(m->when, sizeof m->when, "%um",
                             (unsigned)(age / 60));
                else if (h < 100)
                    snprintf(m->when, sizeof m->when, "%uh", h);
                else
                    snprintf(m->when, sizeof m->when, "%ud", h / 24);
            } else {
                m->when[0] = 0;
            }
        }
        s_room_unread[s_room] = false;   /* looking at it IS reading it */

        const char *head = s_room == RM_LOCAL  ? "Local"
                         : s_room == RM_GLOBAL ? "Global"
                         : s_room == RM_SOCIAL ? "Social"
                                               : s_peer[s_room - RM_FIXED];

        /* Rebuild the bubbles only when the conversation actually changed.
         * A rebuild frees and reallocates every bubble from LVGL's pool,
         * and doing that every two seconds for hours fragments the pool
         * until an allocation fails -- which in LVGL is not a failure but
         * an endless spin, i.e. the hang this line exists to prevent. */
        static uint32_t last_sig;
        uint32_t sig = 2166136261u;
        for (const unsigned char *b = (const unsigned char *)mm;
             b < (const unsigned char *)(mm + mn); b++)
            sig = (sig ^ *b) * 16777619u;
        for (const char *c = head; *c; c++)
            sig = (sig ^ (unsigned char)*c) * 16777619u;
        sig ^= (uint32_t)mn;
        if (sig != last_sig) {
            last_sig = sig;
            xui_chat_msgs(mm, mn, head);
        }

        xui_chat_input(s_compose, s_compose_focus);

        xui_set_title("Chat 2/7");
        break;
    } else {
        static const char *const hdr[3] = { "From", "Message", "When" };
        static const int cw[3] = { 68, 176, 76 };
        xui_table_setup(3, hdr, cw);

        static XPRS_PSRAM_BSS xui_row_t tr[XUI_TAB_ROWS];
        xst_chat_t rows[XUI_TAB_ROWS];
        int nr = xst_chat(rows, XUI_TAB_ROWS);
        uint32_t nowep = xst_epoch_now();
        for (int i = 0; i < nr; i++) {
            xst_chat_t *c = &rows[i];
            xui_row_t *r = &tr[i];
            snprintf(r->cell[0], sizeof r->cell[0], "%s", c->from);
            /* The room at a glance (13.11): a house for scope:local, a
             * pin for the global default, an envelope for a 1:1. */
            const char *ric = c->kind == 1 ? LV_SYMBOL_HOME
                              : c->kind == 2 ? LV_SYMBOL_ENVELOPE
                                             : LV_SYMBOL_GPS;
            if (c->r[0]) {
                /* A reply, compactly: @who-it-answers, then the text. The
                 * parent's callsign when this ring still holds it, its
                 * section 5 id otherwise -- same rule as the spec's "marked
                 * as answering a message it does not hold". */
                xst_chat_t parent;
                const char *pfrom =
                    xst_chat_find(c->r, &parent) ? parent.from : c->r;
                snprintf(r->cell[1], sizeof r->cell[1], "%s @%.7s %.9s",
                         ric, pfrom, c->text);
            } else {
                snprintf(r->cell[1], sizeof r->cell[1], "%s %.19s",
                         ric, c->text);
            }
            if (c->ep && nowep) {
                uint32_t age = nowep - c->ep;
                if (age < 60)
                    snprintf(r->cell[2], sizeof r->cell[2], "%lus",
                             (unsigned long)age);
                else if (age < 3600)
                    snprintf(r->cell[2], sizeof r->cell[2], "%lum",
                             (unsigned long)(age / 60));
                else
                    snprintf(r->cell[2], sizeof r->cell[2], "%luh",
                             (unsigned long)(age / 3600));
            } else {
                r->cell[2][0] = 0;
            }
            if (c->r[0]) {
                xst_chat_t parent;
                int have = xst_chat_find(c->r, &parent);
                const char *pfrom = have ? parent.from : c->r;
                const char *ptext = have ? parent.text : NULL;
                if (ptext)
                    snprintf(r->detail, sizeof r->detail,
                             "%s replying to %s (\"%.30s\"): %.90s",
                             c->from, pfrom, ptext, c->text);
                else
                    snprintf(r->detail, sizeof r->detail,
                             "%s replying to %s: %.120s",
                             c->from, pfrom, c->text);
            } else {
                snprintf(r->detail, sizeof r->detail, "%s: %s",
                         c->from, c->text);
            }
        }
        xui_table_rows(tr, nr);
        list_n = nr;
        xui_set_title("Chat 2/7");
        break;
    }
    /* falls out of the else: both chat forms end here */
    default: {  /* Stats: 10-minute, hourly or daily bars; arrows switch */
        uint16_t dev[XST_SBDAY_N], rxv[XST_SBDAY_N], txv[XST_SBDAY_N];
        int np = xst_stats_series(s_stats_view, dev, rxv, txv, XST_SBDAY_N);
        const char *suffix = !np ? " (waiting for time)"
                             : s_stats_view == 0 ? " / 10 min (4 h)"
                             : s_stats_view == 1 ? " / hour (24 h)"
                                                 : " / day (30 d)";
        char t0[48], t1[48], t2[48];
        snprintf(t0, sizeof t0, "Devices heard%s", suffix);
        snprintf(t1, sizeof t1, "Packets received%s", suffix);
        snprintf(t2, sizeof t2, "Packets sent%s", suffix);
        xui_stats_set(0, t0, dev, np);
        xui_stats_set(1, t1, rxv, np);
        xui_stats_set(2, t2, txv, np);
        list_n = 0;
        xui_set_title("Stats 3/7");
        break;
    }
    }
    xui_set_body(body);
    xui_set_device_count(xst_devices_in_range(UI_INRANGE_SEC));
    /* Last, and after every setter: a screen too small for seven panels
     * folds them into the views it has, and the panel is the only thing
     * that tells a chat table from a device table (xprs_ui.h). The
     * seven-panel implementation ignores this. */
    xui_set_panel(s_panel);

    /* Every list panel keeps its own selection: clamp it to what is on the
     * screen, and take the first row when rows appear after the panel was
     * visited empty. */
    /* Settings (6) was excluded here for a long time, which made the two
     * lines below that mention it unreachable: on that panel nothing ever
     * moved the highlight or scrolled the table, rows past the fifth could
     * be selected but never seen, and the selection grew without bound
     * until settings_ok() hit its default case and did nothing. Every
     * board had it. */
    s_list_n = list_n;
    if (s_panel != 0 && !chat_active()) {
        if (s_sel[s_panel] >= list_n) s_sel[s_panel] = list_n - 1;
        if (s_sel[s_panel] < 0 && list_n > 0) s_sel[s_panel] = 0;
        if (s_panel == 6 && !s_set_focus)
            xui_table_select(-1);   /* nothing armed until the arrows dive in */
        else
            xui_table_select(s_sel[s_panel]);
    }

    /* The bottom bar tells the user what the three buttons under it do:
     * A cycles the menus (long press goes home), B and C move the selection
     * on the panels that have one. */
    set_bar();
}

/* The bottom bar. On a board with buttons the three slots are legends for
 * the physical keys under them and nothing here changes -- those strings
 * are byte-for-byte what the M5Stack has always shown. On a board with a
 * touch panel there are no buttons to label, so each slot names what a TAP
 * does, and s_bar[].act is what touch_events() performs. */
#define BAR(i, t, a) do { s_bar[i].txt = (t); s_bar[i].act = (a); } while (0)
static void set_bar(void)
{
    if (!s_board->touch_read) {
        if (chat_active())
            /* The ball picks the room; the keyboard does everything else. */
            xui_set_keys("Menu", XUI_KEY_UP, XUI_KEY_DOWN);
        else if (s_panel == 6)
            xui_set_keys(s_set_focus ? "OK" : "Menu", XUI_KEY_UP, XUI_KEY_DOWN);
        else if (s_panel != 0)
            xui_set_keys("Menu", XUI_KEY_UP, XUI_KEY_DOWN);
        else
            xui_set_keys("Menu", "", s_rotate ? "Stop" : "Rotate");
        return;
    }
    BAR(0, "Home", BAR_KEY_HOME);
    if (s_panel == 0) {
        BAR(1, s_rotate ? "Stop" : "Rotate", BAR_KEY_DOWN);
        BAR(2, "Next", BAR_KEY_NEXT);
    } else if (s_panel == 6 && s_set_focus) {
        BAR(1, s_board->raw_key ? "Enter" : "OK", BAR_OK);
        BAR(2, "Next", BAR_NEXT_PANEL);
    } else {
        BAR(1, "Prev", BAR_KEY_PREV);
        BAR(2, "Next", BAR_KEY_NEXT);
    }
    xui_set_keys(s_bar[0].txt, s_bar[1].txt, s_bar[2].txt);
}

/* Drain the touch ring. Bar taps and swipes come back as the key they mean,
 * so they flow through the same state machine as the ball and the buttons;
 * row, room and composer taps act here, because they name a target the
 * keys never could. Returns the key to process this tick, if any. */
static xapp_key_t touch_events(xapp_key_t key, bool *force)
{
    xui_ev_t ev;
    while (xui_ev_pop(&ev)) {
        switch (ev.type) {
        case XUI_EV_PRESS:
            s_touch_pressed = true;     /* the wake signal; see ui_task */
            break;
        case XUI_EV_BAR: {
            int act = ev.arg >= 0 && ev.arg < 3 ? s_bar[ev.arg].act : BAR_NONE;
            ESP_LOGI(TAG, "touch: bar %d (%s)", ev.arg,
                     ev.arg >= 0 && ev.arg < 3 && s_bar[ev.arg].txt ? s_bar[ev.arg].txt : "?");
            if (key != XAPP_KEY_NONE) break;       /* a real key wins */
            switch (act) {
            case BAR_KEY_HOME: key = XAPP_KEY_HOME; break;
            case BAR_KEY_NEXT: key = XAPP_KEY_NEXT; break;
            case BAR_KEY_PREV: key = XAPP_KEY_PREV; break;
            case BAR_KEY_DOWN: key = XAPP_KEY_DOWN; break;
            case BAR_OK:       settings_ok(s_sel[6]); *force = true; break;
            case BAR_NEXT_PANEL: s_set_focus = false; key = XAPP_KEY_NEXT; break;
            default: break;
            }
            break;
        }
        case XUI_EV_SWIPE_LEFT:
            ESP_LOGI(TAG, "touch: swipe left");
            if (key == XAPP_KEY_NONE) { s_set_focus = false; key = XAPP_KEY_NEXT; }
            break;
        case XUI_EV_SWIPE_RIGHT:
            ESP_LOGI(TAG, "touch: swipe right");
            if (key == XAPP_KEY_NONE) { s_set_focus = false; key = XAPP_KEY_PREV; }
            break;
        case XUI_EV_ROW:
            ESP_LOGI(TAG, "touch: row %d", ev.arg);
            if (s_panel == 6) {
                /* First tap selects; a second tap on the selected row acts. */
                if (s_set_focus && s_sel[6] == ev.arg) settings_ok(ev.arg);
                else { s_set_focus = true; s_sel[6] = ev.arg; }
            } else if (s_panel != 0 && s_panel != 2 && !chat_active()) {
                s_sel[s_panel] = ev.arg;
            }
            *force = true;
            break;
        case XUI_EV_ROOM:
            if (ev.arg >= 0 && ev.arg < XUI_CHAT_ROOMS && s_rail_room[ev.arg] >= 0) {
                ESP_LOGI(TAG, "touch: room %d -> %d", ev.arg, s_rail_room[ev.arg]);
                s_room = s_rail_room[ev.arg];
                s_compose_focus = false;
                *force = true;
            }
            break;
        case XUI_EV_COMPOSER:
            ESP_LOGI(TAG, "touch: composer");
            s_compose_focus = true;
            *force = true;
            break;
        default:
            break;
        }
    }
    return key;
}

/* ── idx_task: the indexer's writer, announcer and replayer (core 1) ────── */

/* Sends one wire on the bearer an ask arrived on; announcements go on both. */
static void idx_air(const char *bearer, const char *wire, int len)
{
    /* One line per replayed packet: a replay that airs nothing is the
     * failure mode that costs a day, and a page is at most a handful. */
    ESP_LOGI(TAG, "replay on %s: %.*s", bearer, len > 70 ? 70 : len, wire);
    if (strcmp(bearer, "espnow") == 0)    xprsnow_send(wire, len);
    else if (strcmp(bearer, "lora") == 0) xprslora_send(wire, len);
    else if (strcmp(bearer, "ble") == 0)  xprsble_send(wire, len);
    else if (strcmp(bearer, "rns") == 0) {
        /* Answer the way we were asked, and prefer the lane that crosses.
         *
         * A reply carries `d:` -- it is for the one station that asked -- and
         * on Reticulum the broadcast lane is an ANNOUNCE, which the public
         * hubs do not cross-forward between their own clients. So a reply
         * aired that way reached whoever shared our uplink and nobody else,
         * which is the same as not answering. Addressed first; the broadcast
         * remains the fallback for a peer we have never heard announce, where
         * it is still better than silence. */
        xprs_t rp;
        char to[16] = "";
        if (xprs_parse(wire, len, &rp)) xprs_get_str(&rp, "d", to, sizeof to);
        if (!to[0] || !xprsrns_send_to(to, wire, len)) xprsrns_send(wire, len);
    }
    else                                  xprslan_send(wire, len);
}

/* The ESP-NOW counters the alive line prints, for cmd:zdiag. */
static void app_stats(uint32_t out[8])
{
    uint32_t rx = 0, tx = 0, cancel = 0, drop = 0, issued = 0, done = 0, fail = 0;
    xprsnow_stats(&rx, &tx, &cancel, &drop);
    xprsnow_tx_stats(&issued, &done, &fail);
    out[0] = rx; out[1] = tx; out[2] = cancel; out[3] = drop;
    out[4] = issued; out[5] = done; out[6] = fail;
    out[7] = (uint32_t)xprsnow_peer_count(600);
}

static void idx_result_m(const char *bearer, const char *to,
                         const char *cmdid, int code, const char *m);

static void idx_result(const char *bearer, const char *to, const char *cmdid,
                       int code)
{
    idx_result_m(bearer, to, cmdid, code, NULL);
}

/* As idx_result, with an optional m: tail -- the 404's `m:try <peers>`
 * redirect of 36.9: a miss is not a dead end when gossip knows who has
 * what was asked for. */
static void idx_result_m(const char *bearer, const char *to,
                         const char *cmdid, int code, const char *m)
{
    char w[XPRSIDX_WIRE_MAX + 1];
    time_t t = time(NULL);
    int n;
    if (t > 1700000000) {              /* clock synced: say when */
        struct tm tm;
        gmtime_r(&t, &tm);
        n = snprintf(w, sizeof w,
                     "t:result f:%s d:%s ts:%04d-%02d-%02d_%02d:%02d:%02d "
                     "r:%s code:%d",
                     s_call, to, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, cmdid, code);
    } else {
        n = snprintf(w, sizeof w, "t:result f:%s d:%s r:%s code:%d",
                     s_call, to, cmdid, code);
    }
    if (m && m[0] && n > 0 && n < (int)sizeof w - 4)
        n += snprintf(w + n, sizeof w - (size_t)n, " m:%s", m);
    n = sign_wire(w, n, sizeof w);
    idx_air(bearer, w, n);
}

/* Does this station get to write `super` (XPRS.md 36.9.4)?
 *
 * The word is a claim like any other `serve:` word and compels nothing -- but
 * 36.9.4 is explicit that a station which cannot back it should not write it,
 * "because every humble node that believes it will route its asks nowhere".
 * So the config key is permission to claim it, not the claim itself.
 *
 * What is checkable here is DEEP: "a spool measured in RECORDS, not in
 * whatever the board's flash happened to have spare. A store that holds a
 * busy neighbourhood's day is a pocket archiver with ambitions." This board's
 * archive is internal flash -- about 28k records -- so it declines, and says
 * so once rather than every ten minutes.
 *
 * ADDRESSABLE is not checked, because since the RNS bearer learned to send a
 * directed packet this firmware answers asks on every lane it hears them on;
 * the commitment names stations that can only announce, which this is no
 * longer one of. Reach is a different thing from addressability and gets a
 * warning of its own: a super whose only lanes are ESP-NOW and the LAN is a
 * super for the room it is standing in, which is worth being, and is not what
 * 36.12.2 leans on. The remaining commitments -- budgeted, concurrent,
 * complete, awake -- are properties of the code, and are what the rest of
 * this file has to earn.
 */
#define SUPER_MIN_BYTES  (64u * 1024u * 1024u)   /* ~200k records */
static uint64_t s_idx_budget;

static bool idx_is_super(void)
{
    if (!xcfg_get_bool("index_super", false)) return false;
    static int said;                 /* 0 unsaid, 1 said yes, 2 said no */
    if (s_idx_budget < SUPER_MIN_BYTES) {
        if (said != 2) {
            said = 2;
            ESP_LOGW(TAG, "index_super set, but this station declines `super`: "
                     "%llu MB of archive is not a deep spool (36.9.4)",
                     (unsigned long long)(s_idx_budget / (1024u * 1024u)));
        }
        return false;
    }
    if (said != 1) {
        said = 1;
        ESP_LOGI(TAG, "announcing serve:archive,super — %llu MB of spool",
                 (unsigned long long)(s_idx_budget / (1024u * 1024u)));
        if (!xprsrns_is_up())
            ESP_LOGW(TAG, "super without Reticulum: reachable by the stations "
                          "in this room, not by the ones 36.12.2 is about");
    }
    return true;
}

/* Budgets, section 31.2: replays per hour, per asker and global. */
#define HIST_PAGE         12
#define HIST_KNOWN_PH      6
#define HIST_STRANGER_PH   2
#define HIST_GLOBAL_PH    12
static struct { char call[16]; uint32_t when[HIST_KNOWN_PH]; } s_hist_asks[6];
static uint32_t s_hist_global[HIST_GLOBAL_PH];
static struct { char id[8]; uint32_t when; } s_hist_answered[8];

/*
 * A super's budgets (36.9.4: "orders of magnitude above the reference
 * numbers", and 31.2's six-an-hour is explicitly a pocket device's figure).
 * The Dart responder uses a thousandfold, so this does too.
 *
 * A SEPARATE implementation from the rings below, deliberately. The rings
 * hold one timestamp per permitted replay, which is what makes them an exact
 * sliding hour -- and which is also why they cannot be scaled: six thousand
 * timestamps per asker is not a table this board can hold. At super scale the
 * exact position of the hour boundary stops meaning anything (the budget is
 * there to stop a flood, not to ration a neighbour), so this counts within a
 * window instead and costs twelve bytes per asker. The ordinary path is left
 * exactly as it was: a pocket archiver's budget is the one that has to be
 * precise, because it is the one an asker actually runs into.
 */
#define HIST_SUPER_MULT   1000
#define HIST_SUPER_ASKERS   32
static struct { char call[16]; uint32_t win; uint32_t n; }
    s_hist_super[HIST_SUPER_ASKERS];
static struct { uint32_t win; uint32_t n; } s_hist_super_global;

static bool hist_budget_super(const char *from, uint32_t now_s)
{
    if (now_s - s_hist_super_global.win < 3600 &&
        s_hist_super_global.n >= (uint32_t)HIST_GLOBAL_PH * HIST_SUPER_MULT)
        return false;
    const uint32_t limit = (uint32_t)(peer_key(from) ? HIST_KNOWN_PH
                                                     : HIST_STRANGER_PH)
                           * HIST_SUPER_MULT;
    for (int i = 0; i < HIST_SUPER_ASKERS; i++) {
        if (strcasecmp(s_hist_super[i].call, from) != 0) continue;
        if (now_s - s_hist_super[i].win >= 3600) return true;
        return s_hist_super[i].n < limit;
    }
    return true;
}

static void hist_record_super(const char *from, uint32_t now_s)
{
    if (now_s - s_hist_super_global.win >= 3600) {
        s_hist_super_global.win = now_s;
        s_hist_super_global.n = 0;
    }
    s_hist_super_global.n++;

    int slot = -1, oldest = 0;
    for (int i = 0; i < HIST_SUPER_ASKERS; i++) {
        if (strcasecmp(s_hist_super[i].call, from) == 0) { slot = i; break; }
        if (!s_hist_super[i].call[0]) { slot = i; break; }
        if (s_hist_super[i].win < s_hist_super[oldest].win) oldest = i;
    }
    /* Full: the least recently active asker goes. Losing a row only forgets
     * what somebody already spent, and the global counter still holds. */
    if (slot < 0) slot = oldest;
    if (strcasecmp(s_hist_super[slot].call, from) != 0) {
        snprintf(s_hist_super[slot].call, sizeof s_hist_super[0].call, "%s", from);
        s_hist_super[slot].win = now_s;
        s_hist_super[slot].n = 0;
    } else if (now_s - s_hist_super[slot].win >= 3600) {
        s_hist_super[slot].win = now_s;
        s_hist_super[slot].n = 0;
    }
    s_hist_super[slot].n++;
}

static bool hist_budget(const char *from, uint32_t now_s)
{
    if (idx_is_super()) return hist_budget_super(from, now_s);
    int g = 0;
    for (int i = 0; i < HIST_GLOBAL_PH; i++)
        if (s_hist_global[i] && now_s - s_hist_global[i] < 3600) g++;
    if (g >= HIST_GLOBAL_PH) return false;
    int limit = peer_key(from) ? HIST_KNOWN_PH : HIST_STRANGER_PH;
    for (int a = 0; a < 6; a++) {
        if (strcasecmp(s_hist_asks[a].call, from) != 0) continue;
        int n = 0;
        for (int i = 0; i < HIST_KNOWN_PH; i++)
            if (s_hist_asks[a].when[i] && now_s - s_hist_asks[a].when[i] < 3600)
                n++;
        return n < limit;
    }
    return true;
}

static void hist_record(const char *from, uint32_t now_s)
{
    if (idx_is_super()) { hist_record_super(from, now_s); return; }
    for (int i = 0; i < HIST_GLOBAL_PH; i++)
        if (!s_hist_global[i] || now_s - s_hist_global[i] >= 3600) {
            s_hist_global[i] = now_s;
            break;
        }
    int slot = 0;
    for (int a = 0; a < 6; a++) {
        if (strcasecmp(s_hist_asks[a].call, from) == 0) { slot = a; goto have; }
        if (!s_hist_asks[a].call[0]) slot = a;
    }
    snprintf(s_hist_asks[slot].call, sizeof s_hist_asks[0].call, "%s", from);
    memset(s_hist_asks[slot].when, 0, sizeof s_hist_asks[0].when);
have:
    for (int i = 0; i < HIST_KNOWN_PH; i++)
        if (!s_hist_asks[slot].when[i] ||
            now_s - s_hist_asks[slot].when[i] >= 3600) {
            s_hist_asks[slot].when[i] = now_s;
            break;
        }
}

/* The replay page: wires verbatim -- the author's bytes, the author's sig. */
static struct {
    int n;
    bool more;
    char wire[HIST_PAGE][XPRSIDX_WIRE_MAX + 1];
    int16_t len[HIST_PAGE];
} s_page;

static bool hist_collect(const xprsidx_rec_t *rec, void *ctx)
{
    (void)ctx;
    if (s_page.n >= HIST_PAGE) { s_page.more = true; return false; }
    memcpy(s_page.wire[s_page.n], rec->wire, rec->len);
    s_page.wire[s_page.n][rec->len] = 0;
    s_page.len[s_page.n] = (int16_t)rec->len;
    s_page.n++;
    return true;
}

/*
 * A parked ask this station cannot serve -- no index mounted, or the operator
 * switched the indexer off. Say so and free the slot.
 *
 * `pending` used to be set on accept and cleared only inside
 * idx_answer_history(), which is not reached in either of those states. So it
 * latched: seen_note()'s "one at a time" guard then dropped every later ask,
 * and the station went silent for the rest of its uptime with no 404, no 429
 * and nothing in the log to say why. An archiver that cannot serve is over
 * budget as far as the asker is concerned, and 429 is the code that says
 * "not now" without claiming the window was empty.
 */
static void idx_refuse_history(const char *ask_wire, int ask_len,
                               const char *ask_bearer, const char *why)
{
    char wire[XPRSIDX_WIRE_MAX + 1];
    char bearer[7];
    int len = ask_len;
    memcpy(wire, ask_wire, len + 1);
    snprintf(bearer, sizeof bearer, "%s", ask_bearer);

    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return;
    char from[16], cmdid[8];
    if (!xprs_get_str(&p, "f", from, sizeof from)) return;
    xprs_id(&p, cmdid);
    ESP_LOGW(TAG, "history ask from %s refused: %s", from, why);
    idx_result(bearer, from, cmdid, 429);
}

/* One heard `cmd:history`, on idx_task: the check-everything-then-replay
 * shape of the Dart responder (xprs_history_server.dart) and the dongle,
 * paced a packet per 1500 ms so the replay never owns the channel. */
static void idx_answer_history(const char *ask_wire, int ask_len,
                               const char *ask_bearer)
{
    char wire[XPRSIDX_WIRE_MAX + 1];
    char bearer[7];
    int len = ask_len;
    memcpy(wire, ask_wire, len + 1);
    snprintf(bearer, sizeof bearer, "%s", ask_bearer);

    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return;
    char from[16], cmdid[8];
    if (!xprs_get_str(&p, "f", from, sizeof from)) return;
    xprs_id(&p, cmdid);

    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);

    /* One answer per command id, then quiet -- the ask's advert repeats. */
    for (int i = 0; i < 8; i++)
        if (strcmp(s_hist_answered[i].id, cmdid) == 0 &&
            now_s - s_hist_answered[i].when < 600) return;
    int slot = 0;
    for (int i = 0; i < 8; i++)
        if (!s_hist_answered[i].id[0] ||
            now_s - s_hist_answered[i].when >= 600) { slot = i; break; }
    snprintf(s_hist_answered[slot].id, sizeof s_hist_answered[0].id, "%s",
             cmdid);
    s_hist_answered[slot].when = now_s;

    /* A forged ask gets nothing at all. */
    const uint8_t *key = peer_key(from);
    char sigbuf[8];
    if (xprs_get_str(&p, "sig", sigbuf, sizeof sigbuf) && key &&
        !xprsid_verify(&p, key)) {
        ESP_LOGW(TAG, "history ask from %s is forged - ignored", from);
        return;
    }

    if (!hist_budget(from, now_s)) {
        ESP_LOGW(TAG, "history for %s refused - over budget (429)", from);
        idx_result(bearer, from, cmdid, 429);
        return;
    }

    /* kind: may be a comma-separated LIST of types (XPRS.md 25.2) --
     * `kind:info,warning,event,status,blog,message` is the neighbourhood
     * redundancy ask of 36.9.3 and is 37 characters. The old kind[16]
     * truncated it, xprsidx_type_code mapped the fragment to OTHER, and the
     * replay served packets of type "other": nothing, silently, for exactly
     * the asks the spec now standardises. */
    char since[24] = "", until[24] = "", only[16] = "", kind[64] = "";
    xprs_get_str(&p, "since", since, sizeof since);
    xprs_get_str(&p, "until", until, sizeof until);
    xprs_get_str(&p, "only", only, sizeof only);
    xprs_get_str(&p, "kind", kind, sizeof kind);

    /* `only:` is a CALLSIGN (36.6) and `kind:` is a TYPE (25.2). This used to
     * read only: as a type, which made only:message work by accident and the
     * spec's own only:X5A3F2 match nothing at all. With neither given, serve
     * the talking rather than the beacons -- see xi_is_talk. */
    xprsidx_query_t q = {
        .since_ts = since[0] ? xprsindex_ts_to_epoch(since, strlen(since)) : 0,
        .until_ts = until[0] ? xprsindex_ts_to_epoch(until, strlen(until)) : 0,
        .type = -1,
        .types = xprsidx_type_mask(kind),
        .only = only[0] ? only : NULL,
        .talk_only = !kind[0],
        .asker = from,
        .limit = HIST_PAGE + 1,
        .newest_first = true,
    };
    s_page.n = 0;
    s_page.more = false;
    xprsindex_query(s_index, &q, hist_collect, NULL);

    if (s_page.n == 0) {
        ESP_LOGI(TAG, "history for %s - nothing in that window (404)", from);
        char tries[40] = "";
        if (only[0]) {
            char list[32];
            if (goss_try(only, s_call, list, sizeof list) > 0)
                snprintf(tries, sizeof tries, "try %s", list);
        }
        idx_result_m(bearer, from, cmdid, 404, tries[0] ? tries : NULL);
        return;
    }
    hist_record(from, now_s);
    idx_result(bearer, from, cmdid, 202);
    ESP_LOGI(TAG, "history for %s - %d packet%s%s", from, s_page.n,
             s_page.n == 1 ? "" : "s", s_page.more ? ", more held" : "");
    for (int i = 0; i < s_page.n; i++) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_task_wdt_reset();
        idx_air(bearer, s_page.wire[i], s_page.len[i]);
    }
    vTaskDelay(pdMS_TO_TICKS(1500));
    idx_result(bearer, from, cmdid, s_page.more ? 206 : 200);
}


static void idx_task(void *arg)
{
    (void)arg;

    /* Mount the wear-levelled FAT filling the spare flash and open the
     * store there. Done on THIS task: it is the only one that touches it. */
    static wl_handle_t wl = WL_INVALID_HANDLE;
    const esp_vfs_fat_mount_config_t mc = {
        .max_files = CONFIG_SDCARD_MAX_FILES,  /* 4 KB of sector cache each */
        .format_if_mount_failed = true,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/idx", "storage",
                                                     &mc, &wl);
    if (err == ESP_OK) {
        s_index = xprsindex_open("/idx/xprs");
        xprsindex_set_own(s_index, s_call);
        /* The FAT partition is ~11 MB here; leave room for the log + stats.
         * A super sizes to the volume instead -- which on a board whose
         * archive is internal flash is barely more, and that IS the finding:
         * this board can serve a super's role, but not a super's depth. */
        s_idx_budget = xprsindex_budget("/idx", 10u * 1024u * 1024u,
                                        xcfg_get_bool("index_super", false));
        xprsindex_set_max_bytes(s_index, s_idx_budget);
        if (!s_goss) {
            /* Beside the archive, in a directory that already exists:
             * this volume will not make a new one. The bucket names cannot
             * collide -- the index reads seg_* and its own index files. */
            s_goss = xgossip_open("/idx/xprs");
            xgossip_set_super(s_goss, idx_is_super());
        }
        if (s_index) {
            s_api_cfg.index = s_index;
            xprsindex_set_verifier(s_index, index_verifier);
            xprsidx_stats_t st;
            xprsindex_stats(s_index, &st);
            ESP_LOGI(TAG, "indexer up: %lu packet%s held, epoch %c",
                     (unsigned long)st.count, st.count == 1 ? "" : "s",
                     st.epoch);
        }
    } else {
        ESP_LOGE(TAG, "indexer storage failed to mount: %s",
                 esp_err_to_name(err));
    }

    /* The first drain, the moment there is a file to drain into: everything
     * the station said while it was mounting is still in the ring. */
    mkdir("/idx/log", 0777);
    log_drain(true);

    xst_stats_load("/idx/stats.bin");
    /* The conversation, from whichever storage this board has: an SD card
     * where there is one, the internal flash where there is not. */
    xst_chat_load("/idx/chat.bin");
    esp_task_wdt_add(NULL);   /* a wedged storage task becomes a logged reboot */

    uint32_t last_announce_s = 0;
    uint32_t last_logflush_s = 0;
    uint32_t last_stats_save_s = 0;
    for (;;) {
        /* Drain what the radios heard. Every add is a flash write, and on
         * a bench where four stations digipeat one another the queue can
         * refill faster than it drains -- so the watchdog is fed per
         * packet, not per pass. Without this the T-Deck sat in this loop
         * for the whole 90 s budget and rebooted with "idx did not reset
         * the watchdog" while the UI task was the one caught running
         * (2026-08-30, the first evening the Heltec V3 was on the air). */
        while (s_idxq_r != s_idxq_w) {
            int r = s_idxq_r;
            if (s_index && xcfg_get_bool("index_on", true))
                xprsindex_add2(s_index, s_idxq[r].wire, s_idxq[r].len,
                               s_idxq[r].rssi, s_idxq[r].own,
                               s_idxq[r].own ? (uint32_t)time(NULL) : 0,
                               s_idxq[r].bearer);
            s_idxq_r = (r + 1) % IDXQ_N;
            esp_task_wdt_reset();
        }

        /* Say what this station serves, every ten minutes (section 36). */
        uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
        if (s_index && xcfg_get_bool("index_on", true) &&
            now_s - last_announce_s >= 600) {
            last_announce_s = now_s;
            xprsidx_stats_t st;
            xprsindex_stats(s_index, &st);
            char w[XPRSIDX_WIRE_MAX + 1];
            int n = snprintf(w, sizeof w,
                             "t:service f:%s serve:archive%s count:%lu fw:%s",
                             s_call, idx_is_super() ? ",super" : "",
                             (unsigned long)st.count, xota_version());
            /* uptime, health word, last crash: a few bytes on a packet that
             * goes out anyway, so a sick node is visible without an ask. */
            if (n > 0 && n < (int)sizeof w)
                n += xdiag_beacon_fields(w + n, (int)sizeof w - n);
            n = sign_wire(w, n, sizeof w);
            xprsnow_send(w, n);
            xprslan_send(w, n);
            /* And where the internet can hear it. Without this the service
             * beacon reaches ESP-NOW and the LAN only -- the two transports
             * whose listeners are, by definition, already in the room. A
             * station whose peers are all remote (36.12.2) could not learn
             * this archive existed, which for a super is the whole point. */
            if (xprsrns_is_up()) xprsrns_send(w, n);
        }

        if (s_qid_pending) {
            s_qid_pending = false;
            air_identity();
        }

        /* 13.7: a receipt we heard. The signature is what makes it mean
         * anything -- an unverifiable one changes nothing, because otherwise
         * anybody who could forge an s:ack could delete a message from every
         * carrier holding it. */
        if (s_rcpt_in.pending) {
            s_rcpt_in.pending = false;
            esp_task_wdt_reset();
            xprs_t rp;
            char r[XPRS_ID_LEN] = "", from[10] = "";
            if (xprs_parse(s_rcpt_in.wire, s_rcpt_in.len, &rp) &&
                xprs_get_str(&rp, "r", r, sizeof r) &&
                xprs_get_str(&rp, "f", from, sizeof from)) {
                if (verified(&rp)) {
                    acked_note(r);
                    ESP_LOGI(TAG, "receipt from %s: %s arrived, released "
                                  "(13.7)", from, r);
                } else {
                    /* Not an error: we may simply not hold their key yet.
                     * Says so rather than passing silently, because a
                     * station whose receipts never land looks exactly like
                     * one that never sends them. */
                    ESP_LOGW(TAG, "receipt for %s from %s unverifiable -- "
                                  "still holding", r, from);
                }
            }
        }

        /* 13.7.1: acknowledge a direct message from a station we correspond
         * with. Signed, because an unsigned ack is worth less than none. */
        if (s_rcpt_out.pending) {
            s_rcpt_out.pending = false;
            esp_task_wdt_reset();
            xprs_t mp;
            char from[10] = "", id[XPRS_ID_LEN] = "";
            if (xprs_parse(s_rcpt_out.wire, s_rcpt_out.len, &mp) &&
                xprs_get_str(&mp, "f", from, sizeof from) &&
                xprs_id_of(s_rcpt_out.wire, s_rcpt_out.len, id)) {
                char w[XPRS_MAX_WIRE + 1], ts[24];
                time_field(ts, sizeof ts);
                int n = snprintf(w, sizeof w,
                                 "t:receipt f:%s d:%s %s r:%s s:ack",
                                 s_call, from, ts, id);
                if (n > 0 && n <= XPRS_MAX_WIRE) {
                    n = sign_wire(w, n, (int)sizeof w);
                    idx_air(s_rcpt_out.bearer, w, n);
                    ESP_LOGI(TAG, "acked %s to %s on %s (13.7.1)",
                             id, from, s_rcpt_out.bearer);
                }
            }
        }

        /* 36.8.1: a recipient was heard; re-air what the index holds FOR
         * them, paced, on the bearer they were heard on. The stored wires are
         * the authors' originals, re-signed by nobody -- we only add via:.
         *
         * What ends the retries here is REL_THROTTLE_SEC, a timer, and not
         * the 13.7 receipt it should be: this station composes no
         * t:receipt s:ack for a t:message, only xprs_chan does, and only for
         * a 23.7 channel move. It hears the receipts its Flutter peers send
         * and does nothing with them. Until that is built, held mail stops
         * being re-aired because time passed, not because it arrived. */
        if (s_rel.pending && s_index && xcfg_get_bool("index_on", true)) {
            char rcall[10], rbearer[8];
            snprintf(rcall, sizeof rcall, "%s", (const char *)s_rel.call);
            snprintf(rbearer, sizeof rbearer, "%s", (const char *)s_rel.bearer);
            s_rel.pending = false;
            xprsidx_query_t q = {
                .type = -1,
                .types = xprsidx_type_mask("message"),
                .only = rcall,
                .asker = rcall,          /* xi_may_serve: their own mail */
                .limit = 4,
                .newest_first = true,
            };
            s_page.n = 0;
            s_page.more = false;
            xprsindex_query(s_index, &q, hist_collect, NULL);
            int aired = 0;
            for (int i = 0; i < s_page.n; i++) {
                /* only: matched from OR to; mail is what carries d:THEM.
                 * Their own sayings are not deliveries. */
                xprs_t mp;
                char mto[16] = "";
                if (!xprs_parse(s_page.wire[i], s_page.len[i], &mp)) continue;
                if (!xprs_get_str(&mp, "d", mto, sizeof mto)) continue;
                if (strncasecmp(mto, rcall, strlen(rcall)) != 0) continue;
                /* 13.3: "the recipient's s:ack releases carriers still
                 * holding a copy". Without this the release is ended by a
                 * clock, and the same packet goes out every REL_THROTTLE_SEC
                 * for as long as the board holds it. */
                char relid[XPRS_ID_LEN];
                if (xprs_id_of(s_page.wire[i], s_page.len[i], relid) &&
                    acked_has(relid)) continue;
                vTaskDelay(pdMS_TO_TICKS(1500));
                esp_task_wdt_reset();
                /* 36.8.1: "via: gains the holder's callsign, the 13.1 budget
                 * and the 13.2 loop check apply". We are handing on someone
                 * else's mail, so the recipient must be able to tell this
                 * copy came through us rather than from the author directly
                 * -- and a copy that comes back to us is then recognisably
                 * ours and stops. Appended here and not inside idx_air(),
                 * whose other callers air packets we WROTE (a cmd result,
                 * an archive reply): via: on those would claim we relayed
                 * our own saying. 250 bytes is XPRS_MAX_WIRE; when our
                 * callsign no longer fits, the row stays held rather than
                 * going out mislabelled as direct. */
                char rel[XPRS_MAX_WIRE + 1];
                int rn = xprs_append_via(s_page.wire[i], s_page.len[i],
                                         s_call, rel, (int)sizeof rel);
                if (rn <= 0) {
                    /* Refused: either we are already in this copy's via:
                     * (13.2 -- it came back to us after we relayed it) or the
                     * type's hop budget is spent (13.1). Either way it stays
                     * held, and a row that can never leave is worth a line:
                     * silent non-delivery is the failure that costs a day. */
                    ESP_LOGW(TAG, "held for %s: via: refused, staying put",
                             rcall);
                    continue;
                }
                /* The tail, because via: and m: live at the END of a wire
                 * and idx_air's line stops at 70 chars -- which is why a
                 * release that went out mislabelled as direct was invisible
                 * for as long as it was. */
                ESP_LOGI(TAG, "  released via:%s ...%s", s_call,
                         rel + (rn > 80 ? rn - 80 : 0));
                idx_air(rbearer, rel, rn);
                aired++;
            }
            if (aired)
                ESP_LOGI(TAG, "released %d held for %s on %s (36.8.1)",
                         aired, rcall, rbearer);
        } else if (s_rel.pending) {
            s_rel.pending = false;   /* index down: the slot must not latch */
        }

        /*
         * Every ask leaves the queue on EVERY path out, not only the one
         * that answers. This used to be a single slot set on accept and
         * cleared only inside idx_answer_history(), which is not reached
         * with the index unmounted or the indexer switched off -- so it
         * latched, the "one at a time" guard dropped every later ask, and
         * the station went silent for good with no 404, no 429 and nothing
         * in the log. Say so instead: an archiver that cannot serve is over
         * budget as far as the asker is concerned.
         */
        while (s_askq_n) {
            const uint8_t r = s_askq_r;
            if (s_index && xcfg_get_bool("index_on", true)) {
                idx_answer_history(s_askq[r].wire, s_askq[r].len,
                                   s_askq[r].bearer);
            } else {
                idx_refuse_history(s_askq[r].wire, s_askq[r].len,
                                   s_askq[r].bearer, "indexer unavailable");
            }
            s_askq_r = (uint8_t)((r + 1) % ASKQ_MAX);
            s_askq_n--;
        }

        /* Gossip's card work, on the task that owns the volume. Queued on
         * whichever radio task heard the packet; written here (esp32.md:
         * never write from the task that heard it). */
        xgossip_pump(s_goss);

        /* And the one that did not fit while all of that was airing. */
        if (s_ask_over_pending) {
            s_ask_over_pending = false;
            idx_refuse_history(s_ask_over.wire, s_ask_over.len,
                               s_ask_over.bearer, "already replaying");
        }

        /* The verify, the decision and the answer for a parked update --
         * on this task because it can afford the curve work. */
        if (s_upd.pending) {
            s_upd.pending = false;
            xprs_t up;
            ESP_LOGW(TAG, "cmd:update taken up on idx_task");
            if (xprs_parse(s_upd.wire, s_upd.len, &up)) {
                char id[8] = "", from[16] = "";
                int prev = 0;
                xauth_verdict_t v = xauth_check(&up, s_call, id, from, &prev);
                ESP_LOGW(TAG, "cmd:update from %s -> verdict %d", from, (int)v);
                if (v == XAUTH_REPEAT)
                    ota_answer(from, s_upd.bearer, id, prev, NULL);
                else if (v == XAUTH_403)
                    ota_answer(from, s_upd.bearer, id, 403,
                               "not on the allow list");
                else if (v == XAUTH_408)
                    ota_answer(from, s_upd.bearer, id, 408, NULL);
                else if (v == XAUTH_OK) {
                    char ver[24] = "", url[160] = "";
                    xprs_get_str(&up, "ver", ver, sizeof ver);
                    xprs_get_str(&up, "url", url, sizeof url);
                    xota_code_t code = xota_request(ver[0] ? ver : NULL,
                                                    url[0] ? url : NULL,
                                                    from, s_upd.bearer, id);
                    xauth_remember(id, (int)code);
                    ota_answer(from, s_upd.bearer, id, (int)code,
                               code == XOTA_BUSY ? "updating already" : NULL);
                }
                /* XAUTH_SILENT: nothing at all, deliberately. */
            }
        }

        /* The diagnostics asks: verdict and, when a page is open, its next
         * frame. Same task, same reason -- it can afford the curve work. */
        xdiag_pump((uint32_t)(esp_timer_get_time() / 1000));

        /* An install, if one was asked for: minutes of flash work on the
         * task that already owns the card. */
        xota_poll();

        /*
         * Whose turn is it? The beacon that first introduces an archiver is
         * what puts it in the table, but it cannot be what paces the asking:
         * a peer beacons every ten minutes, so a cadence driven by beacons
         * alone could never be faster than ten minutes however busy the room
         * got -- and the whole point of 36.10.2 is that a busy room is
         * followed in seconds. The ladder is checked here instead, on the
         * task that airs the ask.
         */
        {
            /*
             * An ask that is never answered must not freeze its peer's chain.
             * A page is paced at a packet every 1500 ms and a responder may
             * be working through somebody else's first, so this is generous
             * -- but it is finite, because `await` left set forever is how a
             * station stops catching up with no error anywhere.
             */
            const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
            for (int i = 0; i < CU_PEERS; i++) {
                cu_peer_t *r = &s_cu_peers[i];
                if (!r->used || !r->await) continue;
                if (now_s - r->ask_at_s < 120) continue;
                ESP_LOGW(TAG, "catch-up: %s never answered - backing off",
                         r->call);
                r->resume = 0;
                cu_answered(r, XC_QUIET, now_s);
            }
        }

        /*
         * Super-archivers named in the config, seeded into the table so they
         * are pulled whether or not this station ever hears them beacon. A
         * board on a LAN hears its neighbours; the super that matters for
         * the public rooms may be on the far side of a hub (36.12.2), and a
         * peer nobody has heard from is exactly the one worth asking.
         */
        static uint32_t s_supers_at;
        {
            const uint32_t nows = (uint32_t)(esp_timer_get_time() / 1000000);
            if (s_supers_at && nows - s_supers_at < 60) goto supers_done;
            s_supers_at = nows ? nows : 1;
            const char *list = xcfg_get("supers", "");
            char one[10];
            int w = 0;
            for (const char *c = list; ; c++) {
                if (*c && *c != ',' && w < (int)sizeof one - 1) {
                    one[w++] = *c;
                    continue;
                }
                one[w] = 0;
                if (w) {
                    cu_peer_t *r = cu_find(one, true);
                    if (r) {
                        r->super = true;
                        if (!r->bearer[0])
                            snprintf(r->bearer, sizeof r->bearer, "lan");
                        if (!r->next_ask_s)
                            r->next_ask_s =
                                (uint32_t)(esp_timer_get_time() / 1000000);
                    }
                }
                w = 0;
                if (!*c) break;
            }
        }
supers_done:

        if (!s_cu.pending && s_index && xst_epoch_now()) {
            const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
            for (int i = 0; i < CU_PEERS; i++) {
                cu_peer_t *r = &s_cu_peers[i];
                if (!r->used || r->await || !r->next_ask_s) continue;
                if (now_s < r->next_ask_s) continue;
                if (!r->since) {
                    r->since = cu_since_floor();
                    if (!r->since) continue;
                }
                snprintf(s_cu.call, sizeof s_cu.call, "%s", r->call);
                snprintf(s_cu.bearer, sizeof s_cu.bearer, "%s", r->bearer);
                s_cu.since = r->since;
                s_cu.until = r->resume;
                s_cu.pending = true;
                r->await = true;
                r->ask_at_s = now_s;
                r->rows = 0;
                r->oldest_seen = 0;
                break;                       /* one ask in flight at a time */
            }
        }

        /* The parked 36.10 catch-up ask, signed on THIS task's stack. */
        if (s_cu.pending) {
            char since[24], nowts[24];
            struct tm tmv;
            time_t t = (time_t)s_cu.since;
            gmtime_r(&t, &tmv);
            strftime(since, sizeof since, "%Y-%m-%d_%H:%M:%S", &tmv);
            time_t t2 = time(NULL);
            gmtime_r(&t2, &tmv);
            strftime(nowts, sizeof nowts, "%Y-%m-%d_%H:%M:%S", &tmv);
            char until[32] = "";
            if (s_cu.until) {
                /* Continuing a 206: serve what is OLDER than this. */
                char u[24];
                time_t tu = (time_t)s_cu.until;
                gmtime_r(&tu, &tmv);
                strftime(u, sizeof u, "%Y-%m-%d_%H:%M:%S", &tmv);
                snprintf(until, sizeof until, " until:%s", u);
            }
            char ask[XPRSIDX_WIRE_MAX + 1];
            int an = snprintf(ask, sizeof ask,
                              "t:command f:%s d:%s ts:%s cmd:history since:%s%s",
                              s_call, s_cu.call, nowts, since, until);
            if (an > 0 && an < (int)sizeof ask) {
                an = sign_wire(ask, an, sizeof ask);
                /*
                 * Addressed Reticulum FIRST when the peer can be reached that
                 * way (36.12.1). The lane used to be chosen from where the
                 * peer was last heard, and the choice was between ESP-NOW and
                 * the LAN -- so an archiver reachable only through a hub could
                 * not be asked at all, which is precisely the peer worth
                 * asking. `d:` is on the wire, so this goes out as one
                 * DATA/SINGLE packet to that station rather than as a
                 * broadcast everybody pays for.
                 */
                const char *lane;
                if (xprsrns_can_address(s_cu.call) &&
                    xprsrns_send_to(s_cu.call, ask, an)) {
                    lane = "rns";
                } else if (strcmp(s_cu.bearer, "espnow") == 0) {
                    xprsnow_send(ask, an);
                    lane = "espnow";
                } else {
                    xprslan_send(ask, an);
                    lane = "lan";
                }
                ESP_LOGI(TAG, "catch-up: asked %s for history since %s on %s",
                         s_cu.call, since, lane);
            }
            s_cu.pending = false;
        }

        /* What somebody typed on the keyboard, signed on THIS task's stack
         * for the same reason the ask above is: ui_task has 6 KB and
         * secp256k1 wants several of them.
         *
         * The field order is the one the station's own web page fixed, and
         * it matters: sig: is spliced in before m:, and m: runs to the end
         * of the packet, so anything after it would be swallowed. */
        if (s_outbox.full) {
            char ts[24];
            time_t t2 = time(NULL);
            if (t2 > 1700000000) {
                struct tm tmv;
                gmtime_r(&t2, &tmv);
                strftime(ts, sizeof ts, "ts:%Y-%m-%d_%H:%M:%S", &tmv);
            } else {
                snprintf(ts, sizeof ts, "epoch:0.%u",
                         (unsigned)(esp_timer_get_time() / 1000000));
            }
            char where[16] = "";
            if (s_outbox.kind == RM_LOCAL)
                snprintf(where, sizeof where, " scope:local");
            else if (s_outbox.kind == RM_FIXED)
                snprintf(where, sizeof where, " d:%s", s_outbox.to);

            char wire[XPRS_MAX_WIRE + 1];
            int wn = snprintf(wire, sizeof wire, "%s f:%s %s%s m:%s",
                              s_outbox.kind == RM_SOCIAL ? "t:status"
                                                         : "t:message",
                              s_call, ts, where, s_outbox.text);
            if (wn > 0 && wn <= XPRS_MAX_WIRE) {
                wn = sign_wire(wire, wn, sizeof wire);
                if (wn <= XPRS_MAX_WIRE && api_send_wire(wire, wn, NULL, NULL, 0))
                    ESP_LOGI(TAG, "chat: sent %d bytes", wn);
                else
                    ESP_LOGW(TAG, "chat: no bearer took it");
            } else {
                ESP_LOGW(TAG, "chat: too long for one packet (%d)", wn);
            }
            s_outbox.full = false;
        }

        /* A saying is worth a write of its own: they are rare, and losing
         * the last one to a power pull is exactly the complaint this store
         * exists to answer. The flag makes it one write per conversation
         * turn rather than one per tick. */
        if (xst_chat_dirty()) xst_chat_save("/idx/chat.bin");

        /* The stats rings hit the flash every ten minutes -- losing at most
         * ten minutes of bars to a power pull. */
        if (xst_epoch_now() && now_s - last_stats_save_s >= 600) {
            last_stats_save_s = now_s;
            xst_stats_save("/idx/stats.bin");
        }

        /* The heartbeat esp32.md prescribes: a writer that stops is visible
         * here without a boot log. */
        static uint32_t dbg_last;
        if (s_index && now_s - dbg_last >= 30) {
            dbg_last = now_s;
            uint32_t waiting = 0, dropped = 0;
            xprsindex_queue_stats(s_index, &waiting, &dropped);
            xprsidx_stats_t st2;
            xprsindex_stats(s_index, &st2);
            ESP_LOGI(TAG, "index queue: waiting=%lu dropped=%lu held=%lu "
                     "verified=%lu forged=%lu", (unsigned long)waiting,
                     (unsigned long)dropped, (unsigned long)st2.count,
                     (unsigned long)st2.verified, (unsigned long)st2.forged);
        }

        /* Settings asked for the archive to be deleted: close the store,
         * remove its files, reopen empty -- all here, the one task allowed
         * near the storage. The chats this station hosts are gone; the log
         * and the statistics stay. */
        if (s_wipe_req) {
            s_wipe_req = false;
            if (s_index) {
                xprsindex_close(s_index);
                s_index = NULL;
                s_api_cfg.index = NULL;
            }
            static const char *const dirs[] = { "/idx/xprs/t", "/idx/xprs" };
            for (int di = 0; di < 2; di++) {
                DIR *d = opendir(dirs[di]);
                if (!d) continue;
                struct dirent *e;
                char path[320];   /* dir + d_name, however long FatFs makes it */
                while ((e = readdir(d)) != NULL) {
                    if (e->d_name[0] == '.') continue;
                    snprintf(path, sizeof path, "%s/%s", dirs[di], e->d_name);
                    remove(path);
                }
                closedir(d);
            }
            s_index = xprsindex_open("/idx/xprs");
        xprsindex_set_own(s_index, s_call);
        /* The FAT partition is ~11 MB here; leave room for the log + stats.
         * A super sizes to the volume instead -- which on a board whose
         * archive is internal flash is barely more, and that IS the finding:
         * this board can serve a super's role, but not a super's depth. */
        s_idx_budget = xprsindex_budget("/idx", 10u * 1024u * 1024u,
                                        xcfg_get_bool("index_super", false));
        xprsindex_set_max_bytes(s_index, s_idx_budget);
        if (!s_goss) {
            /* Beside the archive, in a directory that already exists:
             * this volume will not make a new one. The bucket names cannot
             * collide -- the index reads seg_* and its own index files. */
            s_goss = xgossip_open("/idx/xprs");
            xgossip_set_super(s_goss, idx_is_super());
        }
            if (s_index) {
                xprsindex_set_verifier(s_index, index_verifier);
                s_api_cfg.index = s_index;
            }
            ESP_LOGW(TAG, "archive wiped from the Settings panel");
        }

        /* The log ring hits the flash every 10 s -- a freeze leaves the
         * last moments readable at /log.txt on the config share.
         *
         * And whenever it is a quarter full, which is what boot needs: the ring
         * holds six kilobytes, a booting station says far more than that in
         * its first ten seconds, and on the old cadence the middle of every
         * boot -- the part worth reading -- was refused and never written.
         * This task wakes four times a second, so the check is free. */
        bool due = now_s - last_logflush_s >= 10;
        if (due || log_fill() > LOGRING_N / 4) {
            if (due) last_logflush_s = now_s;
            log_drain(due);
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* OK on the Settings panel: toggle or act on the selected row. Radio
 * toggles persist and apply on restart; the config share flips live. */
static void settings_ok(int row)
{
    ESP_LOGI(TAG, "ok: settings row %d", row);
    switch (row) {
    case 0:
        xcfg_set_bool("wifi_on", !xcfg_get_bool("wifi_on", true));
        break;
    case 1:
        xcfg_set_bool("espnow_on", !xcfg_get_bool("espnow_on", true));
        break;
    case 2:
        xcfg_set_bool("digi_on", !xcfg_get_bool("digi_on", true));
        break;
    case 3:
        xcfg_set_bool("bridge_on", !xcfg_get_bool("bridge_on", true));
        break;
    case 4:
        xcfg_set_bool("igate_on", !xcfg_get_bool("igate_on", true));
        break;
    case 5:
        xcfg_set_bool("index_on", !xcfg_get_bool("index_on", true));
        break;
    case 6:
        if (xcfg_share_running()) {
            xcfg_share_stop();
            xcfg_set_bool("share_on", false);
        } else if (xcfg_share_start() == ESP_OK) {
            xcfg_set_bool("share_on", true);
        }
        break;
    case 7:
        xcfg_set_bool("ap_on", !xcfg_get_bool("ap_on", true));
        break;
    case 10:
        s_wipe_req = true;   /* idx_task owns the storage; it does the deed */
        break;
    case 11:
        ESP_LOGI(TAG, "restart from the Settings panel");
        esp_restart();
        break;
    default:
        break;
    }
}

/* ── The HTTP API (spec/API-HTTP.md) ─────────────────────────────────────── */

/* Air one already-validated wire on both bearers, and spool it as our own
 * outgoing traffic (section 36.5: the author must be able to replay the
 * author). Cheap: a UDP send and an ESP-NOW enqueue; no flash here. */
static bool api_send_wire(const char *wire, int len, const char *bearer,
                          char *took, size_t took_cap)
{
    /* An identity submitted through the API teaches this station its key
     * exactly as a heard one would -- the hotspot chat's users introduce
     * themselves this way, and without it their signatures would sit
     * unverified on the very station they are talking through. */
    xprs_t p;
    if (xprs_parse(wire, len, &p)) {
        char type[16];
        xprs_type(&p, type, sizeof type);
        if (strcmp(type, "identity") == 0) identity_heard(&p);
        /* 13.7.1's condition is "in EITHER direction", so a direct message we
         * send establishes the pair just as one we receive does -- otherwise
         * the station we wrote to first would have its reply go unacked. */
        if (strcmp(type, "message") == 0) {
            char dst[16];
            if (xprs_get_str(&p, "d", dst, sizeof dst) && dst[0]) {
                char *dash = strchr(dst, '-');
                if (dash) *dash = 0;
                exchanged_note(dst);
            }
        }
        xst_chat_note(&p);   /* our hotspot users' messages show on the LCD too */
    }

    /* Every bearer, or exactly the one that was named (xapi_send.h): the
     * bench needs to put a packet on LoRa and NOWHERE ELSE to prove that
     * what reaches the LAN got there through two other stations. */
    #define WANT(name) (!bearer || strcmp(bearer, name) == 0)
    bool lan = WANT("lan")    && xprslan_send(wire, len);
    bool now = WANT("espnow") && xcfg_get_bool("espnow_on", true) && xprsnow_send(wire, len);
    bool lra = WANT("lora")   && xprslora_is_active() && xprslora_send(wire, len);
    bool ble = WANT("ble")    && xprsble_is_active() && xprsble_send(wire, len);
    bool rns = WANT("rns")    && xprsrns_is_up() && xprsrns_send(wire, len);
    #undef WANT
    if (took && took_cap) {
        int n = 0;
        #define TOOK(flag, name) if (flag) n += snprintf(took + n, took_cap - n, "%s%s", n ? "," : "", name)
        TOOK(lan, "lan"); TOOK(now, "espnow"); TOOK(lra, "lora"); TOOK(ble, "ble"); TOOK(rns, "rns");
        #undef TOOK
        if (!n) took[0] = 0;
    }
    if (lan || now || lra || ble)
        idx_enqueue_own(wire, len,
                        lan ? XI_B_LAN : now ? XI_B_ESPNOW : lra ? XI_B_LORA : XI_B_BLE);
    return lan || now || lra || ble || rns;
}

static int api_serve_json(char *buf, size_t cap)
{
    if (!s_index || !xcfg_get_bool("index_on", true)) {
        buf[0] = 0;
        return 0;
    }
    return snprintf(buf, cap, "\"index\",\"history\",\"mailbox\"");
}

static int api_features_json(char *buf, size_t cap)
{
    return snprintf(buf, cap,
        "\"digipeater\":%s,\"bridge\":%s,\"igate\":%s,\"indexer\":%s,"
        "\"share\":%s,\"hotspot\":%s",
        xcfg_get_bool("digi_on", true) ? "true" : "false",
        xcfg_get_bool("bridge_on", true) ? "true" : "false",
        xcfg_get_bool("igate_on", true) ? "true" : "false",
        xcfg_get_bool("index_on", true) ? "true" : "false",
        xcfg_share_running() ? "true" : "false",
        xcfg_get_bool("ap_on", true) ? "true" : "false");
}

/*
 * /api/xprs/peers -- the archiver's own working state, over the network.
 *
 * The phone has published this for a while (XprsCatchup.statusJson: the
 * interval each archiver has earned, how long since it was asked, which ones
 * are mid-continuation) and the boards published nothing, so every question
 * about a board was answered by opening a serial cable -- which reboots it.
 * A cadence that settles over hours and a paging chain that walks a resume
 * mark backwards over minutes are both unwatchable that way.
 *
 * Runs on the HTTP task and must not block: everything here is a RAM read.
 */
static int api_peers_json(char *buf, size_t cap)
{
    const uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000);
    int n = snprintf(buf, cap, ",\"catchup\":[");
    bool first = true;
    for (int i = 0; i < CU_PEERS && n < (int)cap; i++) {
        const cu_peer_t *r = &s_cu_peers[i];
        if (!r->used) continue;
        n += snprintf(buf + n, cap - n,
            "%s{\"call\":\"%s\",\"super\":%s,\"bearer\":\"%s\","
            "\"interval_s\":%u,\"due_in_s\":%d,\"asked_ago_s\":%d,"
            "\"await\":%s,\"rows\":%u,\"resume\":%u}",
            first ? "" : ",", r->call, r->super ? "true" : "false", r->bearer,
            (unsigned)r->interval_s,
            r->next_ask_s ? (int)((int32_t)r->next_ask_s - (int32_t)now_s) : -1,
            r->ask_at_s ? (int)(now_s - r->ask_at_s) : -1,
            r->await ? "true" : "false", (unsigned)r->rows,
            (unsigned)r->resume);
        first = false;
    }
    if (n < (int)cap) n += snprintf(buf + n, cap - n, "]");

    xgossip_stats_t g = { 0 };
    xgossip_stats(s_goss, &g);
    if (n < (int)cap)
        n += snprintf(buf + n, cap - n,
            ",\"gossip\":{\"rows\":%u,\"accepted\":%u,"
            "\"refused_unsigned\":%u,\"refused_quota\":%u,"
            "\"refused_need\":%u,\"dropped\":%u}",
            (unsigned)g.rows, (unsigned)g.accepted,
            (unsigned)g.refused_unsigned, (unsigned)g.refused_quota,
            (unsigned)g.refused_need, (unsigned)g.dropped);

    /* The counters that say an exchange was ADDRESSED (a DATA/SINGLE packet
     * to one station) rather than an announce everybody paid for. */
    uint32_t atx = 0, arx = 0, anp = 0;
    int apeers = 0;
    xprsrns_addressed_stats(&atx, &arx, &anp, &apeers);
    uint32_t hrx = 0, htx = 0, hconn = 0, hdrop = 0;
    rns_tcp_stats(&hrx, &htx, &hconn, &hdrop);
    /* What the BEARER made of those frames, which is a different question
     * from how many arrived: `other` counts frames that reached the handler
     * and were rejected -- somebody else's app, not our tag, a signature
     * that did not verify -- and `rx` counts XPRS wires actually taken. A
     * socket delivering hundreds of frames while both of these stay zero is
     * a callback that is not being called, which is a different fault from
     * a callback that is refusing everything. */
    uint32_t brx = 0, btx = 0, bpaced = 0, bother = 0;
    xprsrns_stats(&brx, &btx, &bpaced, &bother);
    char peerlist[96];
    int pl = 0;
    peerlist[0] = 0;
    for (int i = 0, shown = 0; i < 16 && pl < (int)sizeof peerlist - 12; i++) {
        char pc[16];
        if (!xprsrns_peer_at(i, pc, sizeof pc)) continue;
        pl += snprintf(peerlist + pl, sizeof peerlist - pl, "%s\"%s\"",
                       shown ? "," : "", pc);
        shown++;
    }
    if (n < (int)cap)
        n += snprintf(buf + n, cap - n,
            /* `up` is the BEARER; `hub` is the socket underneath it. They
             * are not the same answer, and confusing them cost a bench hour:
             * a station reporting up:true with peers:0 looks like a network
             * with nothing on it, when what it has is a bearer initialised
             * over a connection that never came up. */
            ",\"rns\":{\"up\":%s,\"hub\":%s,\"peers\":%d,\"addressed\":"
            "{\"tx\":%u,\"rx\":%u,\"no_peer\":%u},"
            /* The socket's own traffic. `hub:true` with hub_rx:0 is a
             * connection that was accepted and then said nothing, which is a
             * different fault from a connection that never opened. */
            "\"hub_rx\":%u,\"hub_tx\":%u,\"hub_conns\":%u,"
            "\"wires_rx\":%u,\"wires_tx\":%u,\"paced\":%u,\"other\":%u,"
            "\"peer_list\":[%s]}",
            xprsrns_is_up() ? "true" : "false",
            rns_tcp_is_up() ? "true" : "false", apeers,
            (unsigned)atx, (unsigned)arx, (unsigned)anp,
            (unsigned)hrx, (unsigned)htx, (unsigned)hconn,
            (unsigned)brx, (unsigned)btx, (unsigned)bpaced, (unsigned)bother,
            peerlist);

    if (s_index && n < (int)cap) {
        xprsidx_stats_t xs;
        xprsindex_stats(s_index, &xs);
        n += snprintf(buf + n, cap - n,
            ",\"index\":{\"count\":%u,\"segments\":%u}",
            (unsigned)xs.count, (unsigned)xs.segments);
    }
    return n;
}

static xprs_api_cfg_t s_api_cfg = {
    .app = "xprs-esp32",
    .board = "?",           /* the board names itself in xapp_run() */
    .send_wire = api_send_wire,
    .serve_json = api_serve_json,
    .features_json = api_features_json,
    .status_json = api_status_json,
    .peers_json = api_peers_json,
    .log_cur = "/idx/log/cur.txt",
    .log_prev = "/idx/log/prev.txt",
    .tz = "+00:00",
};

/* Bridge the generic UI's flush callback onto whatever panel the board
 * brought up. */
static void lcd_flush_adapter(int x1, int y1, int x2, int y2,
                              const uint16_t *px, void *ctx)
{
    s_board->flush(x1, y1, x2, y2, px, ctx);
}

/*
 * The screenshot door's board half (xprs_api_cfg_t.capture_screen).
 *
 * Two tasks and one row between them. LVGL belongs to the UI task, so it is
 * the one that repaints; but an httpd response may only be written by the
 * task that owns the request, so the HTTP task is the one that sends. The
 * UI task therefore copies each row into a single 640-byte slot, hands it
 * over, and waits for it back -- a ping-pong that costs one row of memory
 * on a board whose whole screen would be 24 KB as pixels, and that keeps
 * each half doing what only it is allowed to do.
 */
#define CAP_ROW_MAX 320

static struct {
    volatile bool     want;       /* a capture has been asked for   */
    volatile bool     done;       /* the UI task has finished it    */
    esp_err_t         err;
    SemaphoreHandle_t ready;      /* UI  -> HTTP: a row is waiting  */
    SemaphoreHandle_t taken;      /* HTTP -> UI: send it and go on  */
    uint16_t          row[CAP_ROW_MAX];
    int               row_y, row_x1, row_x2;
} s_cap;

/* Runs on the UI task, inside the repaint. */
static void cap_slice(int x1, int y1, int x2, int y2,
                      const uint16_t *px, void *ctx)
{
    (void)ctx;
    int w = x2 - x1 + 1;
    if (w > CAP_ROW_MAX) return;
    for (int y = y1; y <= y2; y++, px += w) {
        memcpy(s_cap.row, px, (size_t)w * sizeof *px);
        s_cap.row_y = y;
        s_cap.row_x1 = x1;
        s_cap.row_x2 = x2;
        xSemaphoreGive(s_cap.ready);
        /* If the HTTP side has gone away, do not wedge the UI task. */
        if (xSemaphoreTake(s_cap.taken, pdMS_TO_TICKS(2000)) != pdTRUE) return;
    }
}

static esp_err_t app_capture_screen(xapi_slice_fn cb, void *ctx,
                                    int *w, int *h)
{
    if (!s_display_up) return ESP_ERR_INVALID_STATE;
    if (!cb) return xui_capture(NULL, NULL, w, h);   /* size only */
    if (s_cap.want) return ESP_ERR_INVALID_STATE;    /* one at a time */

    if (!s_cap.ready) {
        s_cap.ready = xSemaphoreCreateBinary();
        s_cap.taken = xSemaphoreCreateBinary();
        if (!s_cap.ready || !s_cap.taken) return ESP_ERR_NO_MEM;
    }
    while (xSemaphoreTake(s_cap.ready, 0) == pdTRUE) { }   /* drain stale */
    while (xSemaphoreTake(s_cap.taken, 0) == pdTRUE) { }

    s_cap.err = ESP_FAIL;
    s_cap.done = false;
    s_cap.want = true;

    /* Pump the rows on THIS task, which is the one httpd will accept. */
    for (;;) {
        if (xSemaphoreTake(s_cap.ready, pdMS_TO_TICKS(3000)) == pdTRUE) {
            cb(s_cap.row_x1, s_cap.row_y, s_cap.row_x2, s_cap.row_y,
               s_cap.row, ctx);
            xSemaphoreGive(s_cap.taken);
            continue;
        }
        break;                          /* nothing came: done, or wedged */
    }
    esp_err_t err = s_cap.done ? s_cap.err : ESP_ERR_TIMEOUT;
    s_cap.want = false;
    if (w || h) xui_capture(NULL, NULL, w, h);
    return err;
}

static void ui_task(void *arg)
{
    (void)arg;

    if (s_board->input_init) s_board->input_init();

    esp_task_wdt_add(NULL);   /* a frozen UI becomes a logged reboot */

    /* A board with no reachable control asks for the tour to be running
     * already (xprs_app.h). The first press stops it, exactly as if it had
     * been started from the home panel. */
    if (s_board->rotate) {
        s_rotate = true;
        s_rotate_next_us = esp_timer_get_time() + 30000000ULL;
        ESP_LOGI(TAG, "rotate: on (this board asked for it at boot)");
    }

    uint64_t next_render_us = 0;
    for (;;) {
        bool force = false;
        xapp_key_t key = s_board->input_poll ? s_board->input_poll()
                                             : XAPP_KEY_NONE;
        s_touch_pressed = false;
        key = touch_events(key, &force);
        if (s_screen_off && (key != XAPP_KEY_NONE || s_touch_pressed)) {
            screen_wake(key != XAPP_KEY_NONE ? "key" : "touch");
            key = XAPP_KEY_NONE;        /* it woke the screen; that is all it does */
            force = true;
        }
        if (key != XAPP_KEY_NONE) xui_activity();

        /* NEXT steps to the next panel, or answers OK on a focused
         * Settings row; HOME is ESC, back to the dashboard. Which gesture
         * produced either is the board's business (xprs_app.h). */
        if (key == XAPP_KEY_HOME) {
            s_panel = 0;
            s_set_focus = false;
            force = true;
            ESP_LOGI(TAG, "key: home");
        } else if (key == XAPP_KEY_NEXT || key == XAPP_KEY_PREV) {
            if (s_rotate) { s_rotate = false; ESP_LOGI(TAG, "rotate: off"); }
            if (key == XAPP_KEY_NEXT && s_panel == 6 && s_set_focus &&
                !s_board->raw_key) {
                /* Inside the Settings list, NEXT is OK -- but only on a board
                 * whose only controls are buttons. Where there is a keyboard,
                 * a trackball click is far too easy to make by accident while
                 * rolling to a row, and it was silently flipping radios; there
                 * ENTER is the deliberate act that changes a value. */
                settings_ok(s_sel[6]);
            } else {
                s_panel = key == XAPP_KEY_NEXT
                        ? (s_panel + 1) % UI_PANEL_COUNT
                        : (s_panel + UI_PANEL_COUNT - 1) % UI_PANEL_COUNT;
                s_set_focus = false;
                ESP_LOGI(TAG, "key: panel %d", s_panel);
            }
            force = true;
        }

        /* UP and DOWN walk the rows on every list panel, and the strip
         * below shows the selected row's detail. */
        /* In a chat room the ball walks the rail instead of a table, and
         * the headings are stepped over -- they are labels, not rooms. */
        if (chat_active() && (key == XAPP_KEY_UP || key == XAPP_KEY_DOWN)) {
            int last = RM_FIXED + s_peer_n - 1;
            s_room += key == XAPP_KEY_UP ? -1 : 1;
            if (s_room < 0) s_room = last;
            if (s_room > last) s_room = 0;
            force = true;
            ESP_LOGI(TAG, "chat: room %d", s_room);
            key = XAPP_KEY_NONE;
        }

        if (key == XAPP_KEY_UP) {
            if (s_rotate) { s_rotate = false; ESP_LOGI(TAG, "rotate: off"); }
            if (s_panel == 6) {
                if (s_set_focus && s_sel[6] > 0) s_sel[6]--;
                else s_set_focus = false;   /* above the top row: back out */
            } else if (s_panel == 2) {
                s_stats_view = (s_stats_view + 2) % 3;
            } else if (s_panel != 0 && s_sel[s_panel] > 0) {
                s_sel[s_panel]--;
            }
            force = true;
            ESP_LOGI(TAG, "key: up (sel %d)", s_sel[s_panel]);
        }
        if (key == XAPP_KEY_DOWN && s_panel == 0 && !s_rotate) {
            /* Down on the home screen starts the tour. */
            s_rotate = true;
            s_rotate_next_us = esp_timer_get_time() + 30000000ULL;
            s_panel = 2;                 /* Stats first, Chat, back home */
            force = true;
            ESP_LOGI(TAG, "rotate: on");
        } else if (key == XAPP_KEY_DOWN) {
            if (s_rotate) { s_rotate = false; ESP_LOGI(TAG, "rotate: off"); }
            else if (s_panel == 6) {
                if (!s_set_focus) { s_set_focus = true; s_sel[6] = 0; }
                else if (s_sel[6] < s_list_n - 1) s_sel[6]++;
            } else if (s_panel == 2) {
                s_stats_view = (s_stats_view + 1) % 3;
            } else if (s_panel != 0) {
                if (s_sel[s_panel] < s_list_n - 1) s_sel[s_panel]++;
            }
            force = true;
            ESP_LOGI(TAG, "key: down (sel %d)", s_sel[s_panel]);
        }

        /* A new packet flashes the RX dot the moment it lands; the panels
         * themselves keep their cadence -- an instantly-updating list is
         * distracting to read. */
        static uint32_t last_heard;
        if (s_heard_count != last_heard) {
            last_heard = s_heard_count;
            xui_pulse();
        }

        /* Console debug: 'S' = screenshot over the UART, '1'..'8' = jump
         * to a panel (the buttons, but reachable from a script). A board
         * with a real keyboard feeds the same handler, so every one of
         * these works from the device as well as from a script. */
        int ch = getchar();

        /* The cable, on this console too. The single-key commands below
         * are the console's whole vocabulary, so a LINE is gathered only
         * when it starts with "cfg" -- "cfg set fwkey <hex>" is how a pinned
         * key is rotated with a USB lead and nothing else, on every board,
         * by the one implementation in xprs_config (see xcfg_console). */
        {
            static char cfgline[160];
            static int  cfgn = -1;           /* -1: not inside a cfg line */
            if (cfgn < 0 && ch == 'c') { cfgn = 0; }
            /* Inside a cfg line, drain everything that has arrived. One
             * character per 10 ms tick is slower than a pasted 76-char
             * "cfg set own1 npub..." and the USB-JTAG ring then drops the
             * tail, which once saved a mangled owner key. */
            while (cfgn >= 0 && ch > 0) {
                if (ch == '\n' || ch == '\r') {
                    cfgline[cfgn] = 0;
                    if (cfgn >= 3 && strncmp(cfgline, "cfg", 3) == 0 &&
                        !xdiag_console(cfgline)) xcfg_console(cfgline);
                    cfgn = -1;
                    ch = 0;
                } else if (cfgn < (int)sizeof cfgline - 1) {
                    cfgline[cfgn++] = (char)ch;
                    /* Not a cfg line after all: hand the key back. */
                    if (cfgn <= 3 && strncmp(cfgline, "cfg", cfgn) != 0) { cfgn = -1; break; }
                    ch = getchar();            /* consumed; take the next now */
                } else { cfgn = -1; ch = 0; }
            }
            if (cfgn >= 0 && ch < 0) ch = 0;   /* mid-line, ring empty: wait */
        }

        /* The serial console can type too, and it has to: without it there
         * is no way to exercise the composer from a script, and "it works,
         * I pressed the keys myself" is not a test. While a room is open,
         * lower case and space and enter go into the message; the commands
         * are all upper case (S, U, D, K, W) and stay reachable. */
        if (ch > 0 && chat_active() &&
            (ch == 0x0d || ch == '\n' || ch == 0x08 || ch == 0x7f ||
             ch == 0x1b ||
             (ch >= 0x20 && ch < 0x7f && !(ch >= 'A' && ch <= 'Z')))) {
            if (chat_key(ch)) { force = true; ch = 0; }
        }
        if (ch >= 'a' && ch <= 'z') ch -= 32;  /* one key, either case */

        /* The two key sources part company here, and they have to.
         *
         * While a chat room is open the KEYBOARD belongs to the person
         * writing -- '1' is a character in a message long before it is a
         * panel number, and there is no shift that means "not this time".
         * The SERIAL console keeps its commands throughout, so a script can
         * still take a screenshot of the very panel being typed into, which
         * is the only way to see what it looks like mid-sentence. */
        int kb = s_board->raw_key ? s_board->raw_key() : 0;
        if (kb > 0 && s_screen_off) { screen_wake("keyboard"); kb = 0; force = true; }
        kb_light_tick(kb);
        /* Enter is what changes a setting on a keyboard board -- see the note
         * at the NEXT handler above. Taken before the console mapping, or the
         * 0x0d would be folded into something else. */
        if (kb == 0x0d && s_panel == 6 && s_set_focus && !chat_active()) {
            settings_ok(s_sel[6]);
            force = true;
            kb = 0;
        }
        if (kb > 0) {
            xui_activity();
            s_compose_focus = true;
            if (chat_active()) {
                if (chat_key(kb)) force = true;
            } else {
                if (kb >= 'a' && kb <= 'z') kb -= 32;
                if (ch <= 0) ch = kb;
            }
        }

        if (ch == 'S') xui_framedump();
        if (ch >= '1' && ch <= '0' + UI_PANEL_COUNT) {
            s_panel = ch - '1';
            force = true;
        }
        /* 'U'/'D' move the selection, 'K' is OK -- the buttons, scripted. */
        if (ch == 'U' && s_panel != 0) {
            if (s_panel == 6 && (!s_set_focus || s_sel[6] == 0))
                s_set_focus = false;
            else if (s_panel == 2) s_stats_view = (s_stats_view + 2) % 3;
            else if (s_sel[s_panel] > 0) s_sel[s_panel]--;
            force = true;
        }
        if (ch == 'D' && s_panel != 0) {
            if (s_panel == 6 && !s_set_focus) { s_set_focus = true; s_sel[6] = 0; }
            else if (s_panel == 2) s_stats_view = (s_stats_view + 1) % 3;
            else if (s_sel[s_panel] < s_list_n - 1) s_sel[s_panel]++;
            force = true;
        }
        if (ch == 'K' && s_panel == 6 && s_set_focus) {
            ESP_LOGI(TAG, "serial OK on settings row %d", s_sel[6]);
            settings_ok(s_sel[6]);
            force = true;
        }
        if (ch == 'W') s_wipe_req = true;   /* scripted archive wipe */

        uint64_t now_us = esp_timer_get_time();
        if (s_rotate && now_us >= s_rotate_next_us) {
            s_rotate_next_us = now_us + 30000000ULL;
            s_panel = s_panel == 0 ? 2 : s_panel == 2 ? 1 : 0;
            force = true;
        }
        battery_tick();
        screen_tick();

        /* The strip's top bar. On a LoRa board it carries the last packet's
         * signal, and says so plainly when nothing has been heard for five
         * minutes; on a board without a radio it stays empty and the UI
         * keeps showing the uptime. The big UI ignores it. */
        if (s_board->lora) {
            static uint32_t note_ms;
            uint32_t t = now_ms();
            if (t - note_ms > 2000) {
                note_ms = t;
                char note[16];
                if (s_lora_rssi_ms && t - s_lora_rssi_ms < 300000)
                    snprintf(note, sizeof note, "%ddBm", s_lora_rssi);
                else
                    snprintf(note, sizeof note, "LoRa quiet");
                xui_set_note(note);
            }
        }
        static bool s_rendered_once, s_splash_gone;
        if (!s_screen_off && (force || now_us >= next_render_us)) {
            ui_render();
            s_rendered_once = true;
            /* Scope and flow settle every 10 s; the counter panels at 2 s. */
            next_render_us = now_us +
                ((s_panel == 0 || s_panel == 4 || s_panel == 2)
                     ? 10000000ULL : 2000000ULL);
        }

        /* The splash goes once the dashboard behind it has been filled in
         * at least once, so nothing flashes an empty screen -- and BEFORE
         * xui_update(), so the deletion's invalidation is served by the same
         * lv_timer_handler pass: one repaint, not two. Latched, or this
         * would ask a hundred times a second forever. */
        if (s_rendered_once && !s_splash_gone)
            s_splash_gone = xui_splash_dismiss();

        /* A screenshot asked for over HTTP: repaint here, where LVGL
         * lives, and let the waiting HTTP task have the slices. */
        if (s_cap.want && !s_cap.done) {
            s_cap.err = xui_capture(cap_slice, NULL, NULL, NULL);
            s_cap.done = true;
        }

        xui_update();
        esp_task_wdt_reset();

        /* At 100 Hz tick a small delay can round to zero and starve IDLE0 —
         * same guard the T-Dongle carries. */
        TickType_t d = pdMS_TO_TICKS(10);
        vTaskDelay(d ? d : 1);
    }
}

void xapp_run(const xapp_board_t *board)
{
    /*
     * STDIN MUST NOT BLOCK, and on a board whose console is a plain UART it
     * does. The UI task reads one character a tick -- that is how 'S' takes
     * a screenshot and how `cfg set ...` reaches a board with nothing but a
     * cable -- and with the primitive UART VFS (no driver installed) that
     * read waits for a byte that never comes. The task then never loops:
     * the screen freezes on whatever frame it had, the buttons stop, and
     * GET /api/screen times out with nobody to repaint for it. Found on the
     * Heltec V3, whose console is a CP2102; the boards with native USB
     * install a driver and were non-blocking by luck.
     */
    setvbuf(stdin, NULL, _IONBF, 0);
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl >= 0) fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);

    s_board = board;
    s_api_cfg.capture_screen = app_capture_screen;
    s_api_cfg.board = board->board_id;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    xcfg_init();
    /* Before anything airs: a packet sent with the ordinal still zero is a
     * packet no receiver can order against the last boot (10.7). */
    boot_epoch_init();
    snprintf(s_ssid, sizeof s_ssid, "%s", xcfg_get("ssid", board->wifi_ssid));
    snprintf(s_pass, sizeof s_pass, "%s", xcfg_get("pass", board->wifi_pass));

    /* The rotating log first, so everything after this lands in it, and
     * the reset reason first of all -- it is the one fact a freeze leaves
     * behind. */
    s_log_orig = esp_log_set_vprintf(log_hook);
    {
        static const char *const why[] = {
            [ESP_RST_UNKNOWN] = "unknown", [ESP_RST_POWERON] = "power-on",
            [ESP_RST_EXT] = "external pin", [ESP_RST_SW] = "software",
            [ESP_RST_PANIC] = "PANIC", [ESP_RST_INT_WDT] = "INTERRUPT WDT",
            [ESP_RST_TASK_WDT] = "TASK WDT", [ESP_RST_WDT] = "other WDT",
            [ESP_RST_DEEPSLEEP] = "deep-sleep wake",
            [ESP_RST_BROWNOUT] = "BROWNOUT", [ESP_RST_SDIO] = "SDIO",
        };
        esp_reset_reason_t r = esp_reset_reason();
        ESP_LOGW(TAG, "reset reason: %s (%d)",
                 r < sizeof why / sizeof why[0] && why[r] ? why[r] : "?", r);
        if (r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT ||
            r == ESP_RST_INT_WDT) {
            /* The panic's UART print dies with the reboot; the core dump
             * does not. One line here names the task that hung. */
            esp_core_dump_summary_t *cd =
                malloc(sizeof(esp_core_dump_summary_t));
            if (cd) {
                if (esp_core_dump_get_summary(cd) == ESP_OK)
                    ESP_LOGW(TAG, "crash was in task \"%s\" at PC 0x%08lx",
                             cd->exc_task, (unsigned long)cd->exc_pc);
                free(cd);
            }
        }
    }

    /* A task that stops feeding this reboots the board with TASK WDT in the
     * log -- a stuck station becomes a diagnosable one. 90 s: longer than
     * any honest pause (a full history replay feeds it mid-way). */
    {
        esp_task_wdt_config_t wdt = {
            .timeout_ms = 90000,
            .idle_core_mask = 0,
            .trigger_panic = true,
        };
        /* IDF starts the TWDT itself, so reshape it -- calling init first
         * works too but prints an error into the very log this exists to
         * keep clean. */
        if (esp_task_wdt_reconfigure(&wdt) != ESP_OK)
            esp_task_wdt_init(&wdt);
    }

    if (nostr_keys_init() != ESP_OK || !nostr_keys_available()) {
        ESP_LOGE(TAG, "no signing key — this station cannot take part in §23.7, "
                      "which follows only invitations it can check");
    }
    /* An nsec written into config.ini is consumed exactly once: import it,
     * wipe it from the store, and carry the new identity from here on. */
    const char *pending_nsec = xcfg_get("nsec", NULL);
    if (pending_nsec) {
        if (nostr_keys_import_nsec(pending_nsec) == ESP_OK)
            ESP_LOGI(TAG, "identity imported from config");
        else
            ESP_LOGE(TAG, "config nsec was invalid; keeping the old identity");
        xcfg_set("nsec", "");
    }

    /* The indexer's writer/replayer, pinned to core 1 (flash work off the
     * radio cores) and created HERE, before WiFi and the bearers carve up
     * the heap -- esp32.md: claim big stacks first, check the result, or a
     * silent pdFAIL becomes a station that answers 404 to everything. The
     * task mounts its own storage; its radio sends no-op until the bearers
     * are up. */
    if (xTaskCreatePinnedToCore(idx_task, "idx", 8192, NULL, 3, NULL, 1)
            != pdPASS)
        ESP_LOGE(TAG, "indexer task failed to start -- nothing will be kept");

    /* The updater claims no stack of its own: it runs on idx_task, which
     * already has one, already sits on core 1, and already owns the flash
     * an install pauses. Its own 8 KB task cost this board the last of its
     * heap the first time -- ENOMEM in the LAN bearer and in the index. */
    /* A board with nothing pinned can install nothing and obey nobody.
     * Seed the compiled-in defaults ONCE; after that they are the
     * operator's, changeable with a cable or through the config share. */
    if (!xcfg_get("fwkey", "")[0] && s_board->fw_key && s_board->fw_key[0])
        xcfg_set("fwkey", s_board->fw_key);
    if (!xcfg_get("own1", "")[0] && s_board->fw_owner && s_board->fw_owner[0])
        xcfg_set("own1", s_board->fw_owner);
    if (!xcfg_get("scriptkey", "")[0] && s_board->script_key && s_board->script_key[0])
        xcfg_set("scriptkey", s_board->script_key);
    /* Same idiom, for the one setting whose right answer is a property of
     * the board rather than of the operator (xprs_app.h). Seeded once so
     * that this file and the status page read the same stored value, and
     * changeable with a cable ever after. */
    if (!xcfg_get("ap_on", "")[0])
        xcfg_set("ap_on", s_board->hotspot ? "1" : "0");

    /* Only NOW may the script host load anything: it needs xcfg_init() to
     * have run and the publisher key above to be seeded, and both happen
     * here, well after app_main claimed the script task's stack.
     *
     * Same shape as relay_task on the T-Dongle (docs/esp32.md): claim the
     * big stack at the top of app_main where the heap is still one block,
     * then block on a flag until the rest of the station is ready. Weak, so
     * a board that does not link the script host gets the no-op. */
    xs_app_ready();
    {
        static xota_cfg_t oc;
        oc.board = s_board->board_id;
        oc.callsign = s_call;
        oc.air = idx_air;
        oc.quiesce = ota_quiesce;
        xota_start(&oc);
    }
    {
        /* Diagnostics over the air, behind the same gate as the updater. */
        static xdiag_cfg_t dc;
        dc.callsign = s_call;
        dc.sign = sign_wire;
        dc.air = idx_air;
        dc.stats = app_stats;
        dc.log_cur = "/idx/log/cur.txt";
        dc.log_prev = "/idx/log/prev.txt";
        dc.epoch_now = xst_epoch_now;
        dc.budget = hist_budget;
        dc.budget_record = hist_record;
        xdiag_init(&dc);
    }
    /* Did the bootloader put us back? Then the update that asked for this
     * failed, and the station says so itself. */
    xota_report_rollback();

    derive_callsign();
    /* §3: an X3 callsign derives from the signing key, so a receiver can
     * re-derive it. This board keeps its MAC-derived X5 name only when there is
     * no key to derive from. */
    if (nostr_keys_get_callsign() && nostr_keys_get_callsign()[0]) {
        snprintf(s_call, sizeof s_call, "%s", nostr_keys_get_callsign());
    }
    ESP_LOGI(TAG, "%s XPRS station %s — %s",
             board->board_id, s_call, board->banner);
    xui_set_call(s_call);   /* the top bar names its owner, not the format */

    if (!xcfg_get_bool("wifi_on", true)) {
        /* ESP-NOW still needs the WiFi driver started, just not a network:
         * wifi_up() with no SSID does exactly that, so blank the name. */
        s_ssid[0] = 0;
        ESP_LOGI(TAG, "WiFi/LAN disabled by config (radio up for ESP-NOW only)");
    }
    /* Declare the roster before starting anything: a part registered only
     * on success can never be reported missing, and "never started" is
     * the failure this catches (xprs_health.h). */
    /* A part the config switched OFF is not a part that failed. This used to
     * expect every bearer unconditionally, and the OTA self-test consumes the
     * same verdict -- so a station with ESP-NOW (or WiFi) disabled by its
     * owner condemned every new image at 120 s and rolled back, for a
     * setting. The expectation follows the config; the heartbeat below
     * re-reads it the same way. */
    xh_expect(XH_HTTP,  true);
    xh_expect(XH_LAN,   xcfg_get_bool("wifi_on", true));
    xh_expect(XH_NOW,   xcfg_get_bool("espnow_on", true));
    xh_expect(XH_ADDR,  true);
    xh_expect(XH_INDEX, true);

    /*
     * BLE5 goes up BEFORE WiFi, and the order is the whole difference between
     * a working radio and a boot loop.
     *
     * The BT controller wants a contiguous block of INTERNAL DRAM -- PSRAM does
     * not satisfy it however the host is configured. Started after WiFi and the
     * hotspot have taken theirs, it gets "BLE_INIT: Malloc failed", trips
     * `assert emi.c 164` inside the controller, and the interrupt watchdog
     * reboots the board about three and a half seconds in, forever. Started
     * here there is ~140 KB internal free and it fits with room to spare.
     *
     * docs/esp32.md says it in one line: "the boot order is the allocator --
     * whoever starts last gets the fragments." The radio cannot live on
     * fragments; the HTTP server can.
     */
    if (board->ble) {
        heap_mark("before ble");
        if (xprsble_start(s_call) == ESP_OK) {
            xprsble_set_rx_cb(on_ble);
        } else {
            ESP_LOGE(TAG, "BLE5 failed to start -- carrying on without");
        }
        heap_mark("after ble");
    }

    /*
     * The screen, before WiFi rather than after everything.
     *
     * It used to be last, on the reasoning that everything it reads already
     * exists by then. True, but it meant the glass stayed dark through the
     * whole of the slow part -- association, SNTP, the bearers, a secp256k1
     * signature -- and st7789_init raises the backlight over GRAM it never
     * clears, so what the user saw was several seconds of nothing and then a
     * dashboard. A splash that arrives after the boot it is meant to cover
     * is decoration; here it is the thing that makes those seconds legible.
     *
     * It is also cheaper here. The draw buffer wants one contiguous DMA
     * block and takes an eighth of the screen when it can get it: measured
     * on X3R8XX, 15 rows at the old site against the full 30 here, so the
     * panel flushes a frame in half as many slices for the rest of the run.
     *
     * NOT before BLE, though: the controller wants contiguous internal DRAM
     * and this firmware has already learned what happens when it does not
     * get it -- see the note on BLE_INIT above.
     */
    int lcd_w = 0, lcd_h = 0;
    void *lcd = NULL;
    s_display_up = board->display_init(&lcd_w, &lcd_h, &lcd) == ESP_OK &&
                   xui_init(lcd_w, lcd_h, lcd_flush_adapter, lcd) == ESP_OK;
    if (!s_display_up)
        ESP_LOGE(TAG, "display init failed -- running headless");

    splash_step("network");
    heap_mark("before wifi");
    wifi_up();
    heap_mark("after wifi");

    /* The HTTP server starts HERE, and the position is a memory decision,
     * not a matter of taste. docs/esp32.md: "the boot order is the
     * allocator -- whoever starts last gets the fragments."
     *
     * httpd creates its own task with a 6,144-byte stack (xprs_api.c), and
     * a task stack must be INTERNAL memory. Measured on the T-Deck with
     * PSRAM on, this is what the internal heap looks like across boot:
     *
     *     before wifi   173,975 free, largest 63,488
     *     after wifi     50,171 free, largest 47,104   <- we are here
     *     at the old call site, after the bearers, the radio and the index:
     *                     6,459 free, largest  6,144
     *
     * httpd asked for 6,144 against a largest block of 6,144 and lost, every
     * boot, and the station served nothing on the LAN while gossiping
     * happily over ESP-NOW -- the exact presentation docs/esp32.md records
     * for this bug. Asked here it sees a 47 KB block.
     *
     * Starting before the network has an address is safe and deliberate:
     * httpd binds a socket, it does not need a route, and nothing can
     * arrive in the gap. xprs_api_start() keeps a POINTER to the config, so
     * the index handle that idx_task fills in later is picked up live; a
     * request that beats it gets an honest 404 rather than a stale answer. */
    s_api_cfg.callsign = s_call;
    if (xprs_api_start(&s_api_cfg) == ESP_OK)
        xcfg_share_attach(xprs_api_httpd());
    heap_mark("after api");
    splash_step("services");

    /* A wall clock makes since:/until: windows mean something; without one
     * the index still works, ordered by its own monotonic index. AFTER
     * wifi_up(): SNTP posts to the lwip thread, which exists only once the
     * network stack does. */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, xcfg_get("ntp", "pool.ntp.org"));
    esp_sntp_init();

    /* The timezone ("time fuse"): an offset like +01:00 or -05:30 in the
     * config. It places the day boundary for the daily statistics. */
    {
        const char *tz = xcfg_get("tz", "+00:00");
        int th = 0, tm = 0;
        if (sscanf(tz, "%d:%d", &th, &tm) >= 1)
            s_tz_off = th * 3600 + (th < 0 ? -tm : tm) * 60;
        ESP_LOGI(TAG, "NTP %s, timezone %+03d:%02d",
                 xcfg_get("ntp", "pool.ntp.org"),
                 s_tz_off / 3600, abs(s_tz_off / 60) % 60);
    }
    xst_init(s_call, s_tz_off);

    /* The LAN bearer first, deliberately: its task is what pumps every bearer's
     * re-air queue and beacon, ESP-NOW included. Starting ESP-NOW without it
     * would leave nothing driving either — which xprsnow_start() says out loud
     * rather than letting it be discovered in the field. */
    splash_step("mesh");
    if (xprslan_start(s_call) == ESP_OK) {
        xprslan_set_rx_cb(on_lan);
        xprslan_set_beacon(lan_beacon, 60, 10);
    } else {
        ESP_LOGE(TAG, "LAN bearer failed — nothing will pump ESP-NOW either");
    }

    if (!xcfg_get_bool("espnow_on", true)) {
        ESP_LOGI(TAG, "ESP-NOW disabled by config");
    } else if (xprsnow_start(s_call) == ESP_OK) {
        xprsnow_set_rx_cb(on_espnow);
        xprsnow_set_heard_cb(heard_espnow);
        /* Faster than the dongle's 300 s: this board exists to be measured, and
         * a minute between beacons is a long time to watch a serial console. */
        xprsnow_set_beacon(espnow_beacon, 60, 5);
        xprschan_init(s_call, &k_chan_ops);
    splash_step("identity");
        air_identity();
    }

    /* The LoRa radio, on the boards that have one. After the LAN bearer,
     * whose task is what pumps this one's queue too. */
    if (board->lora) {
        splash_step("radio");
        if (xprslora_start(s_call, board->lora) == ESP_OK) {
            xprslora_set_rx_cb(on_lora);
            /* 31.1 is a legal figure, and the law differs by band and region,
             * so the operator gets the last word. The default is already set
             * inside the bearer; this only lets a station be stricter (or,
             * knowingly, looser) without a rebuild. */
            const char *pace = xcfg_get("lora_pace_ms", NULL);
            if (pace && pace[0]) xprslora_set_pace((uint32_t)atoi(pace));
        }
        else
            ESP_LOGE(TAG, "LoRa radio failed to start -- carrying on without");
    }

    /* The Reticulum uplink (compiled out on boards without the memory;
     * idle unless config.ini names an rns_hub). Wires it carries land in
     * seen_note like every radio's, and idx_air answers on it. */
    xprsrns_init(on_rns);


    /* 6 KB, not 3: once a minute this task calls air_identity(), and signing
     * is several KB of secp256k1 stack -- the same trap that once PANIC'd
     * xprslan (see sign_wire's neighbours above). On the original ESP32 it
     * fitted by luck; on the S3 the frames are bigger and the task died at
     * the first identity, exactly 60 s after every boot. Stack overflow, and
     * it named itself: "A stack overflow in task status has been detected." */
    xTaskCreate(status_task, "status", 6144, NULL, 1, NULL);
    xTaskCreate(inet_probe_task, "inet", 3072, NULL, 1, NULL);


    /*
     * The panel itself came up far earlier (see the splash, above); what is
     * left here is the two things that genuinely need a finished station.
     *
     * ui_task renders from the bearers, the station store and the channel
     * table, none of which existed before xst_init; and xui_touch_enable
     * registers an LVGL input device whose read callback drives the I2C bus
     * the keyboard shares, which must not start polling while the board is
     * still bringing that bus up.
     */
    if (s_display_up) {
        if (s_board->touch_read) xui_touch_enable(s_board->touch_read);
        splash_step("ready");
        /*
         * 8 KB where there is room, 6 KB where there is not.
         *
         * The T-Deck's UI task overflowed 6,144 on 2026-08-30 ("A stack
         * overflow in task ui has been detected", in the core dump) under
         * the traffic of a fourth station: what lives on this stack is
         * LVGL's draw path and it is deeper than it looks. But a stack is
         * one contiguous block, and the Heltec V3 reaches this line with a
         * largest block of about 7.5 KB -- there 8,192 simply fails, the
         * task never starts, and a station with a screen and no UI task is
         * a board that answers nothing and repaints nothing. It cost an
         * evening to find, so it logs which one it got.
         */
        /* PINNED TO CORE 1, and docs/esp32.md is why: "anything that blocks
         * for milliseconds at a time must be pinned to core 1". A panel
         * flush is exactly that -- the Heltec V3's is 8,192 pixel writes
         * and a 1 KB I2C transfer per frame -- and a bare xTaskCreate
         * leaves the task with NO affinity, which is not the same as core
         * 1: it lands beside the BLE controller and the WiFi task on core
         * 0. On the Heltec that showed up as a station that answered HTTP
         * at 94 bytes a second and then failed to answer ping at all,
         * which the same page warns reads as a wedged server and is not
         * one. The T-Dongle and the M5Stack were lucky, not right. */
        if (xTaskCreatePinnedToCore(ui_task, "ui", 8192, NULL, 4, NULL, 1)
            != pdPASS) {
            if (xTaskCreatePinnedToCore(ui_task, "ui", 6144, NULL, 4, NULL, 1)
                != pdPASS)
                ESP_LOGE(TAG, "UI task failed to start");
            else
                ESP_LOGW(TAG, "UI task on a 6 KB stack: no room for 8 "
                              "(largest block %u)",
                         (unsigned)heap_caps_get_largest_free_block(
                             MALLOC_CAP_INTERNAL));
        }
    }

    /* The station's LAN face: AFTER the screen, but no longer INSIDE it.
     *
     * The order is not a preference, it is the heap. The draw buffer is
     * 19 KB in ONE piece and the HTTP server's needs are smaller and
     * divisible, so the screen asks first -- docs/esp32.md's rule that the
     * big claim goes early, while the heap is still whole. Starting the
     * API first was tried and the panel went dark with 13 KB left.
     *
     * What DID have to change is the nesting: this used to sit inside the
     * display's `if`, so a board whose panel failed answered nothing on the
     * network either -- no API, no chat page, and no way to ask it what was
     * wrong except the serial port. A station with no screen is still a
     * station. */
    {
        /* The API (spec/API-HTTP.md): always on when the network is. The
         * config share joins the same server so its toggle opens and closes
         * doors, not servers. */
        /* The server itself was started right after wifi_up() -- see the
         * note there. What is left here is everything that only needs its
         * handle, and which is cheap. */
        /* The walk-up hotspot rides the same server. With a STA up the AP
         * shares its channel (one radio), so ESP-NOW is untouched; alone it
         * sits on the ESP-NOW fallback channel. */
        /* The chat page answers on the LAN side whether or not the AP is
         * up -- easier to test, and a station without the hotspot is still
         * a chat server for its network. */
        if (xprs_api_httpd()) xprs_hotspot_serve_page(xprs_api_httpd());
        if (xcfg_get_bool("ap_on", true) && xprs_api_httpd()) {
            char ssid[33];
            const char *want = xcfg_get("ap_ssid", NULL);
            if (want && want[0]) snprintf(ssid, sizeof ssid, "%s", want);
            else snprintf(ssid, sizeof ssid, "XPRS-%s", s_call);
            xprs_hotspot_start(ssid, xprs_api_httpd());
            heap_mark("after hotspot");
        }
        xcfg_share_set_log("/idx/log/cur.txt", "/idx/log/prev.txt");
        if (xcfg_get_bool("share_on", false) &&
            xcfg_get_bool("wifi_on", true))
            xcfg_share_start();
    }

    /* Everything has had its chance now. Name whatever did not take it,
     * and complain if this board came up with less room than the tables
     * in docs/esp32.md record for it. WiFi may still be associating, so
     * the address is not judged here -- the heartbeat picks it up. */
    xh_set(XH_HTTP, xprs_api_httpd() != NULL);
    xh_set(XH_LAN, xprslan_is_active());
    xh_set(XH_NOW, xprsnow_is_active() || !xcfg_get_bool("espnow_on", true));
    xh_set(XH_INDEX, s_index != NULL || !xcfg_get_bool("index_on", true));
    xh_heap_floor(heap_floor());
}
