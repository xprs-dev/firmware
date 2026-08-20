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

#include "xprs.h"
#include "xprsnow.h"
#include "xprslan.h"
#include "xprslora.h"
#include "xprsid.h"
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
#include "xprs_hotspot.h"
#include "xprsindex.h"
#include "esp_vfs_fat.h"
#include "wear_levelling.h"
#include "esp_sntp.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "esp_core_dump.h"
#include "esp_system.h"

static const char *TAG = "xprs";

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
 * wins. Small: this exists so §23.7 has somebody it can believe, not to be a
 * directory. */
#define PEERKEYS_MAX 4
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
        if (nw == s_logring_r) break;          /* full: drop the tail */
        s_logring[s_logring_w] = c;
        s_logring_w = nw;
    }
    portEXIT_CRITICAL(&s_logring_mux);
    return out;
}

#define LOG_ROTATE_BYTES (64 * 1024)
static FILE *s_logfile;
static long  s_logfile_len;

/* idx_task only. Append what the ring holds, rotate at the cap. */
static void log_drain(void)
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
    if (s_logfile) {
        fflush(s_logfile);
        fsync(fileno(s_logfile));
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
} s_idxq[IDXQ_N];
static volatile int s_idxq_w, s_idxq_r;   /* single writer set, single reader */
static uint32_t s_idxq_dropped;

static xprsidx_t *s_index;
static xprs_api_cfg_t s_api_cfg;    /* filled below; idx_task publishes into it */

/* One pending history ask; a second one arriving while busy is dropped --
 * one replay in flight protects the channel (31.4). */
static struct {
    volatile bool pending;
    char wire[XPRSIDX_WIRE_MAX + 1];
    int  len;
    char bearer[7];
} s_ask;
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
    s_idxq_w = nw;
}

static uint8_t bearer_code(const char *name)
{
    if (strcmp(name, "espnow") == 0) return XI_B_ESPNOW;
    if (strcmp(name, "lan") == 0)    return XI_B_LAN;
    if (strcmp(name, "lora") == 0)   return XI_B_LORA;
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
/* Settings is modal: A stays the Menu key until the arrows dive into the
 * list, then it becomes OK; climbing back out above the top row hands the
 * Menu key back. */
static bool s_set_focus;
static int s_stats_view;  /* Stats panel: 0 = 10 min, 1 = hour, 2 = day */
/* Rotate: C on the home panel starts a 30 s tour of Home, Stats, Chat;
 * any button ends it and hands the wheel back. */
static bool s_rotate;
static uint64_t s_rotate_next_us;      /* total ever, ring position = s_flow_n % FLOW_MAX */

/* A parked 36.10 catch-up ask (built + signed on idx_task's stack). */
static struct {
    char call[10];
    char bearer[7];
    uint32_t since;
    volatile bool pending;
} s_cu;

/* Marks the room a heard saying belongs to as having something new in it.
 * Defined with the rest of the chat panel, below. */
static void chat_note_unread(const xprs_t *p);

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
        if (s_ask.pending) break;
        memcpy(s_ask.wire, wire, len);
        s_ask.wire[len] = 0;
        s_ask.len = len;
        snprintf(s_ask.bearer, sizeof s_ask.bearer, "%s", bearer);
        s_ask.pending = true;                 /* published last */
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
        uint32_t newest = s_index ? xprsindex_boot_newest_ts(s_index) : 0;
        if (!newest) break;                 /* empty store: nothing missed */
        if (!xst_catchup_due(call, 600)) break;
        /* Park it. Signing is several KB of secp256k1 stack and this runs
         * on the 5 KB bearer task -- the first build here PANIC'd xprslan.
         * idx_task (8 KB) builds and airs the ask. */
        if (s_cu.pending) break;
        snprintf(s_cu.call, sizeof s_cu.call, "%s", call);
        snprintf(s_cu.bearer, sizeof s_cu.bearer, "%s", bearer);
        s_cu.since = newest;
        s_cu.pending = true;
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
    /* iGate: what arrived on the radio goes toward the LAN (and whatever
     * internet sits behind it), under the ordinary relay rules -- the bearer
     * refuses when we are already in via: or the hop budget is spent. */
    if (xcfg_get_bool("igate_on", true)) xprslan_offer(wire, len);
    /* Digipeater: re-air on the radio itself, for stations past our reach. */
    if (xcfg_get_bool("digi_on", false)) xprsnow_offer(wire, len);
    /* And toward the long-range radio, which reaches who WiFi cannot. */
    if (xcfg_get_bool("bridge_on", true)) xprslora_offer(wire, len);
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
    ESP_LOGI(TAG, "lan    %u.%u.%u.%u %3dB  %s",
             (unsigned)(ip & 0xFF), (unsigned)((ip >> 8) & 0xFF),
             (unsigned)((ip >> 16) & 0xFF), (unsigned)((ip >> 24) & 0xFF),
             len, wire);
    /* Bridge: carry LAN traffic onto the radios. */
    if (xcfg_get_bool("bridge_on", true)) {
        xprsnow_offer(wire, len);
        xprslora_offer(wire, len);
    }
}

static void on_lora(const char *wire, int len, int rssi)
{
    s_heard_count++;
    seen_note(wire, len, "lora", rssi);
    xprs_t p;
    if (xprs_parse(wire, len, &p)) {
        char type[16];
        xprs_type(&p, type, sizeof type);
        if (strcmp(type, "identity") == 0) identity_heard(&p);
    }
    ESP_LOGI(TAG, "lora   %19d dBm %3dB  %s", rssi, len, wire);
    /* What arrived over kilometres goes toward the LAN and the local radio
     * under the same iGate rule ESP-NOW uses -- and never back onto LoRa
     * here: the bearer's own digipeat path owns that decision, with its
     * jitter and its cancel window. */
    if (xcfg_get_bool("igate_on", true)) {
        xprslan_offer(wire, len);
        xprsnow_offer(wire, len);
    }
    if (xcfg_get_bool("digi_on", false)) xprslora_offer(wire, len);
}

/* ── What we say ────────────────────────────────────────────────────────── */

/* t:observation f:<call> link:espnow peers:<n> — §10.6, and §10.6.1's rule that
 * a reading belongs to the bearer it names, which is why this says espnow and
 * the LAN beacon below says lan. */
static int espnow_beacon(char *out, int cap)
{
    if (!s_call[0]) return 0;
    return snprintf(out, (size_t)cap, "t:observation f:%s link:espnow peers:%d",
                    s_call, xprsnow_peer_count(600));
}

static int lan_beacon(char *out, int cap)
{
    if (!s_call[0]) return 0;
    return snprintf(out, (size_t)cap, "t:observation f:%s link:lan peers:%d",
                    s_call, xprslan_peer_count(600));
}

/* ── Meeting on a working channel (§23.7) ───────────────────────────────── */

/* No SNTP on this board, so most of the time there is no wall clock and
 * `epoch:<boot>.<uptime>` is the honest thing to send (§4.3). The deadline that
 * brings us home is local milliseconds either way. */
static void time_field(char *out, int cap)
{
    snprintf(out, cap, "epoch:0.%u",
             (unsigned)(esp_timer_get_time() / 1000000ULL));
}

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t no_clock(void) { return 0; }

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
    .epoch = no_clock,
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
        if (++n % 120 == 0) air_identity();       /* every 60 s */
        if (n % 30) continue;                     /* the rest every 15 s */

        uint32_t rx = 0, tx = 0, cancelled = 0, dropped = 0;
        uint32_t issued = 0, done = 0, failed = 0;
        xprsnow_stats(&rx, &tx, &cancelled, &dropped);
        /* `tx` counts what this station decided to say; `done`/`fail` count what
         * the radio actually did with it. They were the same number until the
         * send callback existed, and the difference is where a rendezvous that
         * "sent" its acceptance and was not heard shows up. */
        xprsnow_tx_stats(&issued, &done, &failed);
        ESP_LOGW(TAG, "alive %us heap=%u call=%s ch=%u espnow rx=%u tx=%u "
                      "cancel=%u drop=%u sent=%u/%u fail=%u peers=%d heard=%u",
                 (unsigned)(esp_timer_get_time() / 1000000ULL),
                 (unsigned)esp_get_free_heap_size(), s_call,
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
static xst_chat_t s_chat_scratch[XST_CHAT_MAX];
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
static bool api_send_wire(const char *wire, int len);

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
        char d[8];
        int row = 0;
        snprintf(d, sizeof d, "%d", n_now);
        xui_home_row(row++, "ESP-NOW", xprsnow_is_active(), d, NULL);

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
            xui_home_row(row++, "LoRa", xprslora_is_active(), d, NULL);
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

        static xui_row_t tr[XUI_TAB_ROWS];
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

        static xui_row_t tr[XUI_TAB_ROWS];
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

        static xui_row_t tr[XUI_TAB_ROWS];
        int nr = 0;
        #define SROW(c0, val, det) do { \
            snprintf(tr[nr].cell[0], sizeof tr[nr].cell[0], "%s", c0); \
            snprintf(tr[nr].cell[1], sizeof tr[nr].cell[1], "%s", val); \
            snprintf(tr[nr].detail, sizeof tr[nr].detail, "%s", det); \
            nr++; } while (0)

        SROW("WiFi / LAN", xcfg_get_bool("wifi_on", true) ? "On" : "Off",
             "Join the WiFi network and speak XPRS over the LAN. "
             "OK toggles; applies after restart.");
        SROW("ESP-NOW", xcfg_get_bool("espnow_on", true) ? "On" : "Off",
             "The 2.4 GHz radio link between nearby stations. "
             "OK toggles; applies after restart.");
        SROW("Digipeater", xcfg_get_bool("digi_on", false) ? "On" : "Off",
             "Re-air packets heard on ESP-NOW back onto ESP-NOW, for "
             "stations past our reach. OK toggles; applies at once.");
        SROW("Bridge", xcfg_get_bool("bridge_on", true) ? "On" : "Off",
             "Carry LAN traffic onto the ESP-NOW radio. "
             "OK toggles; applies at once.");
        SROW("iGate", xcfg_get_bool("igate_on", true) ? "On" : "Off",
             "Carry ESP-NOW traffic onto the LAN, toward the internet "
             "side. OK toggles; applies at once.");
        char idet[160];
        if (s_index) {
            xprsidx_stats_t ist;
            xprsindex_stats(s_index, &ist);
            snprintf(idet, sizeof idet,
                     "Keep every packet heard, answer cmd:history, hold "
                     "mail. Holding %lu packet%s. OK toggles.",
                     (unsigned long)ist.count, ist.count == 1 ? "" : "s");
        } else {
            snprintf(idet, sizeof idet,
                     "Keep every packet heard, answer cmd:history, hold "
                     "mail. Storage not mounted. OK toggles.");
        }
        SROW("Indexer", xcfg_get_bool("index_on", true) ? "On" : "Off", idet);
        char det[160];
        if (xcfg_share_running())
            snprintf(det, sizeof det,
                     "Serving now: open http://%s/ in a browser to edit "
                     "config.ini (WiFi, name, nsec). OK turns it off.",
                     s_ip_str[0] ? s_ip_str : "<ip>");
        else
            snprintf(det, sizeof det,
                     "Off. OK starts a browser editor for config.ini "
                     "(WiFi, name, nsec)%s.",
                     s_ip_str[0] ? "" : " -- needs WiFi first");
        SROW("Config share", xcfg_share_running() ? "On" : "Off", det);
        SROW("Hotspot", xcfg_get_bool("ap_on", true) ? "On" : "Off",
             "The walk-up WiFi: an open network whose sign-in page is the "
             "chat. OK toggles; applies after restart.");
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
                         "chats it hosts included. %lu held now. OK wipes "
                         "at once; there is no undo.",
                         (unsigned long)wst.count);
            } else {
                snprintf(wdet, sizeof wdet,
                         "Delete every packet this station holds. "
                         "Storage not mounted.");
            }
            SROW("Wipe archive", "--", wdet);
        }
        SROW("Restart", "--",
             "OK restarts the station so pending changes take effect.");
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

        xui_chat_input(s_compose, true);

        xui_set_title("Chat 2/7");
        break;
    } else {
        static const char *const hdr[3] = { "From", "Message", "When" };
        static const int cw[3] = { 68, 176, 76 };
        xui_table_setup(3, hdr, cw);

        static xui_row_t tr[XUI_TAB_ROWS];
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

    /* Every list panel keeps its own selection: clamp it to what is on the
     * screen, and take the first row when rows appear after the panel was
     * visited empty. */
    if (s_panel != 0 && s_panel != 6 && !chat_active()) {
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
    if (chat_active())
        /* The ball picks the room; the keyboard does everything else. */
        xui_set_keys("Menu", XUI_KEY_UP, XUI_KEY_DOWN);
    else if (s_panel == 6)
        xui_set_keys(s_set_focus ? "OK" : "Menu", XUI_KEY_UP, XUI_KEY_DOWN);
    else if (s_panel != 0)
        xui_set_keys("Menu", XUI_KEY_UP, XUI_KEY_DOWN);
    else
        xui_set_keys("Menu", "", s_rotate ? "Stop" : "Rotate");
}

/* ── idx_task: the indexer's writer, announcer and replayer (core 1) ────── */

/* Sends one wire on the bearer an ask arrived on; announcements go on both. */
static void idx_air(const char *bearer, const char *wire, int len)
{
    /* One line per replayed packet: a replay that airs nothing is the
     * failure mode that costs a day, and a page is at most a handful. */
    ESP_LOGI(TAG, "replay on %s: %.*s", bearer, len > 70 ? 70 : len, wire);
    if (strcmp(bearer, "espnow") == 0) xprsnow_send(wire, len);
    else                               xprslan_send(wire, len);
}

static void idx_result(const char *bearer, const char *to, const char *cmdid,
                       int code)
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
    n = sign_wire(w, n, sizeof w);
    idx_air(bearer, w, n);
}

/* Budgets, section 31.2: replays per hour, per asker and global. */
#define HIST_PAGE         12
#define HIST_KNOWN_PH      6
#define HIST_STRANGER_PH   2
#define HIST_GLOBAL_PH    12
static struct { char call[16]; uint32_t when[HIST_KNOWN_PH]; } s_hist_asks[6];
static uint32_t s_hist_global[HIST_GLOBAL_PH];
static struct { char id[8]; uint32_t when; } s_hist_answered[8];

static bool hist_budget(const char *from, uint32_t now_s)
{
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

/* One heard `cmd:history`, on idx_task: the check-everything-then-replay
 * shape of the Dart responder (xprs_history_server.dart) and the dongle,
 * paced a packet per 1500 ms so the replay never owns the channel. */
static void idx_answer_history(void)
{
    char wire[XPRSIDX_WIRE_MAX + 1];
    char bearer[7];
    int len = s_ask.len;
    memcpy(wire, s_ask.wire, len + 1);
    snprintf(bearer, sizeof bearer, "%s", s_ask.bearer);
    s_ask.pending = false;

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

    char since[24] = "", until[24] = "", only[16] = "";
    xprs_get_str(&p, "since", since, sizeof since);
    xprs_get_str(&p, "until", until, sizeof until);
    xprs_get_str(&p, "only", only, sizeof only);

    xprsidx_query_t q = {
        .since_ts = since[0] ? xprsindex_ts_to_epoch(since, strlen(since)) : 0,
        .until_ts = until[0] ? xprsindex_ts_to_epoch(until, strlen(until)) : 0,
        .type = only[0] ? xprsidx_type_code(only) : -1,
        .asker = from,
        .limit = HIST_PAGE + 1,
        .newest_first = true,
    };
    s_page.n = 0;
    s_page.more = false;
    xprsindex_query(s_index, &q, hist_collect, NULL);

    if (s_page.n == 0) {
        ESP_LOGI(TAG, "history for %s - nothing in that window (404)", from);
        idx_result(bearer, from, cmdid, 404);
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
        .max_files = 5,    /* each open FILE holds a 4 KB sector cache */
        .format_if_mount_failed = true,
        .allocation_unit_size = 4096,
    };
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl("/idx", "storage",
                                                     &mc, &wl);
    if (err == ESP_OK) {
        s_index = xprsindex_open("/idx/xprs");
        xprsindex_set_own(s_index, s_call);
        /* The FAT partition is ~14 MB; leave room for the log + stats. */
        xprsindex_set_max_bytes(s_index, 10u * 1024u * 1024u);
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

    xst_stats_load("/idx/stats.bin");
    /* The conversation, from whichever storage this board has: an SD card
     * where there is one, the internal flash where there is not. */
    xst_chat_load("/idx/chat.bin");
    mkdir("/idx/log", 0777);
    esp_task_wdt_add(NULL);   /* a wedged storage task becomes a logged reboot */

    uint32_t last_announce_s = 0;
    uint32_t last_logflush_s = 0;
    uint32_t last_stats_save_s = 0;
    for (;;) {
        /* Drain what the radios heard. */
        while (s_idxq_r != s_idxq_w) {
            int r = s_idxq_r;
            if (s_index && xcfg_get_bool("index_on", true))
                xprsindex_add2(s_index, s_idxq[r].wire, s_idxq[r].len,
                               s_idxq[r].rssi, false, 0, s_idxq[r].bearer);
            s_idxq_r = (r + 1) % IDXQ_N;
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
                             "t:service f:%s serve:archive count:%lu",
                             s_call, (unsigned long)st.count);
            n = sign_wire(w, n, sizeof w);
            xprsnow_send(w, n);
            xprslan_send(w, n);
        }

        if (s_ask.pending && s_index && xcfg_get_bool("index_on", true))
            idx_answer_history();

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
            char ask[XPRSIDX_WIRE_MAX + 1];
            int an = snprintf(ask, sizeof ask,
                              "t:command f:%s d:%s ts:%s cmd:history since:%s",
                              s_call, s_cu.call, nowts, since);
            if (an > 0 && an < (int)sizeof ask) {
                an = sign_wire(ask, an, sizeof ask);
                if (strcmp(s_cu.bearer, "espnow") == 0)
                    xprsnow_send(ask, an);
                else
                    xprslan_send(ask, an);
                ESP_LOGI(TAG, "catch-up: asked %s for history since %s",
                         s_cu.call, since);
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
                if (wn <= XPRS_MAX_WIRE && api_send_wire(wire, wn))
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
        /* The FAT partition is ~14 MB; leave room for the log + stats. */
        xprsindex_set_max_bytes(s_index, 10u * 1024u * 1024u);
            if (s_index) {
                xprsindex_set_verifier(s_index, index_verifier);
                s_api_cfg.index = s_index;
            }
            ESP_LOGW(TAG, "archive wiped from the Settings panel");
        }

        /* The log ring hits the flash every 10 s -- a freeze leaves the
         * last moments readable at /log.txt on the config share. */
        if (now_s - last_logflush_s >= 10) {
            last_logflush_s = now_s;
            log_drain();
        }

        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}

/* OK on the Settings panel: toggle or act on the selected row. Radio
 * toggles persist and apply on restart; the config share flips live. */
static void settings_ok(int row)
{
    switch (row) {
    case 0:
        xcfg_set_bool("wifi_on", !xcfg_get_bool("wifi_on", true));
        break;
    case 1:
        xcfg_set_bool("espnow_on", !xcfg_get_bool("espnow_on", true));
        break;
    case 2:
        xcfg_set_bool("digi_on", !xcfg_get_bool("digi_on", false));
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
static bool api_send_wire(const char *wire, int len)
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
        xst_chat_note(&p);   /* our hotspot users' messages show on the LCD too */
    }

    bool lan = xprslan_send(wire, len);
    bool now = xcfg_get_bool("espnow_on", true) && xprsnow_send(wire, len);
    bool lra = xprslora_is_active() && xprslora_send(wire, len);
    (void)lra;
    if ((lan || now || lra) && s_index && xcfg_get_bool("index_on", true))
        xprsindex_add2(s_index, wire, len, 0, true, (uint32_t)time(NULL),
                       lan ? XI_B_LAN : XI_B_ESPNOW);
    return lan || now;
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
        xcfg_get_bool("digi_on", false) ? "true" : "false",
        xcfg_get_bool("bridge_on", true) ? "true" : "false",
        xcfg_get_bool("igate_on", true) ? "true" : "false",
        xcfg_get_bool("index_on", true) ? "true" : "false",
        xcfg_share_running() ? "true" : "false",
        xcfg_get_bool("ap_on", true) ? "true" : "false");
}

static xprs_api_cfg_t s_api_cfg = {
    .app = "xprs-esp32",
    .board = "?",           /* the board names itself in xapp_run() */
    .send_wire = api_send_wire,
    .serve_json = api_serve_json,
    .features_json = api_features_json,
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

static void ui_task(void *arg)
{
    (void)arg;

    if (s_board->input_init) s_board->input_init();

    esp_task_wdt_add(NULL);   /* a frozen UI becomes a logged reboot */

    uint64_t next_render_us = 0;
    for (;;) {
        bool force = false;
        xapp_key_t key = s_board->input_poll ? s_board->input_poll()
                                             : XAPP_KEY_NONE;

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
            if (key == XAPP_KEY_NEXT && s_panel == 6 && s_set_focus) {
                /* Inside the Settings list, NEXT is OK. */
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
                else s_sel[6]++;             /* render clamps to the list */
            } else if (s_panel == 2) {
                s_stats_view = (s_stats_view + 1) % 3;
            } else if (s_panel != 0) {
                s_sel[s_panel]++;
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
        if (kb > 0) {
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
            else s_sel[s_panel]++;
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
        if (force || now_us >= next_render_us) {
            ui_render();
            /* Scope and flow settle every 10 s; the counter panels at 2 s. */
            next_render_us = now_us +
                ((s_panel == 0 || s_panel == 4 || s_panel == 2)
                     ? 10000000ULL : 2000000ULL);
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
    s_board = board;
    s_api_cfg.board = board->board_id;

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    xcfg_init();
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
    ESP_LOGI(TAG, "heap before wifi: %u (largest %u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
    wifi_up();
    ESP_LOGI(TAG, "heap after wifi: %u (largest %u)",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

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
        air_identity();
    }

    /* The LoRa radio, on the boards that have one. After the LAN bearer,
     * whose task is what pumps this one's queue too. */
    if (board->lora) {
        if (xprslora_start(s_call, board->lora) == ESP_OK)
            xprslora_set_rx_cb(on_lora);
        else
            ESP_LOGE(TAG, "LoRa radio failed to start -- carrying on without");
    }

    /* 6 KB, not 3: once a minute this task calls air_identity(), and signing
     * is several KB of secp256k1 stack -- the same trap that once PANIC'd
     * xprslan (see sign_wire's neighbours above). On the original ESP32 it
     * fitted by luck; on the S3 the frames are bigger and the task died at
     * the first identity, exactly 60 s after every boot. Stack overflow, and
     * it named itself: "A stack overflow in task status has been detected." */
    xTaskCreate(status_task, "status", 6144, NULL, 1, NULL);
    xTaskCreate(inet_probe_task, "inet", 3072, NULL, 1, NULL);


    /* The screen last: everything it reads already exists by now. The
     * board brings its own panel up and hands back its size. */
    int lcd_w = 0, lcd_h = 0;
    void *lcd = NULL;
    if (board->display_init(&lcd_w, &lcd_h, &lcd) == ESP_OK &&
        xui_init(lcd_w, lcd_h, lcd_flush_adapter, lcd) == ESP_OK) {
        if (xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL) != pdPASS)
            ESP_LOGE(TAG, "UI task failed to start");
    } else {
        ESP_LOGE(TAG, "display init failed — running headless");
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
        s_api_cfg.callsign = s_call;
        if (xprs_api_start(&s_api_cfg) == ESP_OK)
            xcfg_share_attach(xprs_api_httpd());
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
            ESP_LOGI(TAG, "heap after hotspot: %u (largest %u)",
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(
                         MALLOC_CAP_8BIT));
        }
        xcfg_share_set_log("/idx/log/cur.txt", "/idx/log/prev.txt");
        if (xcfg_get_bool("share_on", false) &&
            xcfg_get_bool("wifi_on", true))
            xcfg_share_start();
    }
}
