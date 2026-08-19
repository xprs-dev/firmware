/*
 * An XPRS station on an M5Stack Core, whose whole job is to be somebody else.
 *
 * Testing a bearer with one device proves nothing: a station that hears its own
 * broadcast has learned that the loopback works. This is the second voice — it
 * beacons on ESP-NOW, repeats what it hears, and prints every packet with the
 * signal it arrived at, so the two ends can be compared.
 *
 * WHAT THIS BOARD CANNOT DO. It is an original ESP32 (D0WDQ6): no BLE5 extended
 * advertising, so it can never join the mesh plane the T-Dongle runs on
 * (docs/esp32.md, "Radio capability per chip"). It has ESP-NOW and WiFi, which
 * is the entire point of it being here.
 *
 * THE CHANNEL IS THE WHOLE TRICK. ESP-NOW rides whatever channel the WiFi
 * station is on, and two devices on different channels hear nothing from each
 * other with no error anywhere. So this node associates to the SAME access
 * point as the dongle, which lands it on the same channel without anybody
 * having to guess one. With no credentials it stays unassociated and pins the
 * channel from the build config instead — fine between two idle boards, useless
 * against a dongle that is associated somewhere else.
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
#include "xprsid.h"
#include "xprschan.h"
#include "nostr_keys.h"
#include "bech32.h"

#include "wifi_secrets.h"   /* gitignored; see wifi_secrets.h.example */

/* The credentials actually used: config.ini overrides the compiled-in
 * secrets, so a device can be repointed without a rebuild. */
static char s_ssid[32];   /* sized to wifi_config_t */
static char s_pass[64];

#include <math.h>
#include <sys/stat.h>
#include <dirent.h>
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "ili9342.h"
#include "ili9342.h"
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

static const char *TAG = "m5xprs";

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

static uint32_t epoch_now(void);

static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int out = s_log_orig ? s_log_orig(fmt, ap) : vprintf(fmt, ap);

    /* Stamp the line: epoch seconds under a synced clock, +ms-since-boot
     * before that -- what makes /api/log machine readable (API-HTTP.md). */
    char line[176];
    int n;
    uint32_t ep = epoch_now();
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

/* ── Who we have heard, by name — the display's device list ─────────────── */

/* The bearer's own peer table is MAC-keyed and nameless; the UI wants
 * callsigns. Harvested from the f: field of everything heard, one row per
 * callsign, freshest wins. Metadata only, like everything the screen shows. */
#define SEEN_MAX 16
typedef struct {
    char     call[10];
    char     bearer[7];        /* "espnow" or "lan" */
    int      rssi;             /* 0 for LAN */
    uint32_t last_ms;
} seen_t;
static seen_t s_seen[SEEN_MAX];
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

/* The chat ring: t:message packets with a human m: body, for the Chat
 * panel. Fed from both directions -- what the radios heard and what the
 * API sent. */
#define CHAT_MAX 8
typedef struct {
    char     from[10];
    char     text[120];
    char     id[XPRS_ID_LEN];   /* section 5, so replies can find us */
    char     r[XPRS_ID_LEN];    /* parent id when this is a reply (6.4) */
    uint8_t  kind;        /* 0 global, 1 scope:local, 2 direct (d:) */
    uint32_t ep;          /* epoch when heard, 0 when the clock was not up */
} chat_t;
static chat_t s_chat[CHAT_MAX];
static int s_chat_n;      /* total ever; ring position = n % CHAT_MAX */

/* LVGL's FontAwesome glyph bytes, without dragging lvgl.h into main.c:
 * HOME f015, GPS f124, ENVELOPE f0e0 -- the same values xprs_ui renders. */
#define LV_SYMBOL_HOME     "\xEF\x80\x95"
#define LV_SYMBOL_GPS      "\xEF\x84\xA4"
#define LV_SYMBOL_ENVELOPE "\xEF\x83\xA0"

static void chat_note(const xprs_t *p)
{
    char from[10], text[120];
    char type[16];
    xprs_type(p, type, sizeof type);
    if (strcmp(type, "message") != 0) return;
    if (!xprs_get_str(p, "f", from, sizeof from)) return;
    if (!xprs_get_str(p, "m", text, sizeof text) || !text[0]) return;
    /* The same message arrives more than once -- our own send plus the
     * relay's re-air, or both bearers. One line per saying. */
    for (int i = 0; i < CHAT_MAX; i++) {
        if (s_chat[i].from[0] && strcmp(s_chat[i].from, from) == 0 &&
            strcmp(s_chat[i].text, text) == 0)
            return;
    }
    chat_t *c = &s_chat[s_chat_n % CHAT_MAX];
    snprintf(c->from, sizeof c->from, "%s", from);
    snprintf(c->text, sizeof c->text, "%s", text);
    xprs_id(p, c->id);
    if (!xprs_get_str(p, "r", c->r, sizeof c->r)) c->r[0] = 0;
    char sc[12], dst[16];
    if (xprs_get_str(p, "d", dst, sizeof dst) && dst[0] != '#')
        c->kind = 2;                                   /* a 1:1 */
    else if (xprs_get_str(p, "scope", sc, sizeof sc) &&
             strcmp(sc, "local") == 0)
        c->kind = 1;                                   /* the local room */
    else
        c->kind = 0;                                   /* global, the default */
    c->ep = epoch_now();
    s_chat_n++;
}

/* Statistics for the Stats panel, on the WALL CLOCK so they survive a
 * reboot: 10-minute buckets covering a day and daily buckets covering a
 * month, both rings persisted to /idx/stats.bin by idx_task and reloaded
 * at boot. Before NTP syncs nothing is banked -- a bucket keyed by a wrong
 * clock would poison the ring it lands in. Aggregation upward: packets
 * add, distinct-device counts take the busiest sub-bucket (a device seen
 * in two buckets is one device, not two, so summing would lie). */
#define SB10_N   144            /* 10-minute buckets: one day */
#define SBDAY_N  30             /* daily buckets: one month */
#define STAT_DEVS 12
typedef struct {
    uint32_t rx, tx;
    uint16_t dev, pad;
} sbucket_t;
static sbucket_t s_sb10[SB10_N];
static uint32_t  s_sb10_id[SB10_N];     /* absolute period each slot holds */
static sbucket_t s_sbday[SBDAY_N];
static uint32_t  s_sbday_id[SBDAY_N];
static int s_tz_off;                    /* seconds east of UTC, from config */

/* Distinct devices in the LIVE 10-minute bucket only. */
static uint32_t s_cur_devh[STAT_DEVS];
static int      s_cur_devn;
static uint32_t s_cur_slot;

static uint32_t epoch_now(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;   /* 0 until NTP has spoken */
}

static sbucket_t *stat10(uint32_t ep)
{
    uint32_t slot = ep / 600;
    int i = (int)(slot % SB10_N);
    if (s_sb10_id[i] != slot) {
        memset(&s_sb10[i], 0, sizeof s_sb10[i]);
        s_sb10_id[i] = slot;
    }
    if (s_cur_slot != slot) { s_cur_slot = slot; s_cur_devn = 0; }
    return &s_sb10[i];
}

static sbucket_t *statday(uint32_t ep)
{
    uint32_t slot = (uint32_t)((int64_t)ep + s_tz_off) / 86400;
    int i = (int)(slot % SBDAY_N);
    if (s_sbday_id[i] != slot) {
        memset(&s_sbday[i], 0, sizeof s_sbday[i]);
        s_sbday_id[i] = slot;
    }
    return &s_sbday[i];
}
static int s_sel[8];      /* per-panel selected row (arrows move it) */
/* Settings is modal: A stays the Menu key until the arrows dive into the
 * list, then it becomes OK; climbing back out above the top row hands the
 * Menu key back. */
static bool s_set_focus;
static int s_stats_view;  /* Stats panel: 0 = 10 min, 1 = hour, 2 = day */
/* Rotate: C on the home panel starts a 30 s tour of Home, Stats, Chat;
 * any button ends it and hands the wheel back. */
static bool s_rotate;
static uint64_t s_rotate_next_us;      /* total ever, ring position = s_flow_n % FLOW_MAX */

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

    chat_note(&sp);

    uint32_t ep = epoch_now();
    if (ep) {
        sbucket_t *b = stat10(ep);
        sbucket_t *d = statday(ep);
        b->rx++;
        d->rx++;
        uint32_t chh = 5381;
        for (const char *c = call; *c; c++) chh = chh * 33 + (uint8_t)*c;
        bool known = false;
        for (int i = 0; i < s_cur_devn; i++)
            if (s_cur_devh[i] == chh) { known = true; break; }
        if (!known && s_cur_devn < STAT_DEVS) {
            s_cur_devh[s_cur_devn++] = chh;
            b->dev = (uint16_t)s_cur_devn;
            if (b->dev > d->dev) d->dev = b->dev;
        }
    }

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

    int slot = -1, oldest = 0;
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i].call[0] && strcasecmp(s_seen[i].call, call) == 0) {
            slot = i;
            break;
        }
        if (!s_seen[i].call[0]) { if (slot < 0) slot = i; }
        else if (s_seen[i].last_ms < s_seen[oldest].last_ms) oldest = i;
    }
    if (slot < 0) slot = oldest;
    snprintf(s_seen[slot].call, sizeof s_seen[slot].call, "%s", call);
    snprintf(s_seen[slot].bearer, sizeof s_seen[slot].bearer, "%s", bearer);
    s_seen[slot].rssi = rssi;
    s_seen[slot].last_ms = now;
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
    /* Bridge: carry LAN traffic onto the radio. */
    if (xcfg_get_bool("bridge_on", true)) xprsnow_offer(wire, len);
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
                      "dongle's channel", s_ssid);
    } else {
        ESP_LOGW(TAG, "no WiFi credentials: staying unassociated on channel %d. "
                      "That only meets another unassociated board — an "
                      "associated dongle is on its access point's channel.",
                 ESPNOW_FALLBACK_CHANNEL);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!s_ssid[0]) {
        ESP_ERROR_CHECK(esp_wifi_set_channel(ESPNOW_FALLBACK_CHANNEL,
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

/* The T-Dongle's dashboard, on the bigger panel: same bands, same rule —
 * metadata only, never message content. Buttons: A = previous panel,
 * C = next, B = refresh now. */
#define BTN_A GPIO_NUM_39
#define BTN_B GPIO_NUM_38
#define BTN_C GPIO_NUM_37

#define UI_PANEL_COUNT 8
#define UI_INRANGE_SEC 300

static int s_panel;

/* Distance from signal strength, the way the phones estimate BLE range:
 * log-distance path loss, d = 10^((A - rssi) / (10 n)). For ESP-NOW at
 * 2.4 GHz between these boards, A (RSSI at 1 m) is about -40 dBm and the
 * indoor exponent about 2.7. An estimate, honestly rough -- walls, bodies
 * and antennas bend it -- but the same packet at -46 and at -85 are a room
 * and a street apart, and that is what the scope shows. */
static float est_distance_m(int rssi)
{
    if (!rssi) return -1.0f;
    return powf(10.0f, ((-40.0f - (float)rssi) / 27.0f));
}

static int seen_in_range(void)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int n = 0;
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i].call[0] && (now - s_seen[i].last_ms) / 1000 < UI_INRANGE_SEC) n++;
    }
    return n;
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
        static uint32_t tx_prev;
        static bool tx_primed;
        uint32_t tx_total = done + ltx;
        uint32_t ep = epoch_now();
        if (tx_primed && tx_total > tx_prev && ep) {
            stat10(ep)->tx += tx_total - tx_prev;
            statday(ep)->tx += tx_total - tx_prev;
        }
        if (ep || !tx_primed) tx_prev = tx_total;
        tx_primed = true;
    }

    body[0] = 0;
    xui_show_home(s_panel == 0);
    xui_show_table(s_panel != 0 && s_panel != 6);
    xui_show_stats(s_panel == 6);

    switch (s_panel) {
    case 0: {   /* Links: the graphic home panel */
        char d[48];
        snprintf(d, sizeof d, "Ch %u, %d peers",
                 xprsnow_channel(), xprsnow_peer_count(600));
        xui_home_row(0, "ESP-NOW", xprsnow_is_active(), d);

        if (s_ip_str[0])
            snprintf(d, sizeof d, "%s", s_ip_str);
        else
            snprintf(d, sizeof d, "%s", s_ssid[0] ? "Joining..." : "Down");
        xui_home_row(1, "WiFi / LAN", s_ip_str[0] != 0, d);

        if (!s_ip_str[0])       snprintf(d, sizeof d, "No address");
        else if (!s_inet_known) snprintf(d, sizeof d, "Probing...");
        else                    snprintf(d, sizeof d, "%s",
                                          s_inet_up ? "Reachable" : "Down");
        xui_home_row(2, "Internet", s_inet_known && s_inet_up, d);

        xui_home_heard(s_heard_count);

        /* The scope: everybody in reach, at their estimated distance. */
        xui_blip_t blips[XUI_BLIP_MAX];
        int nb = 0;
        for (int i = 0; i < SEEN_MAX && nb < XUI_BLIP_MAX; i++) {
            if (!s_seen[i].call[0]) continue;
            if ((now - s_seen[i].last_ms) / 1000 >= UI_INRANGE_SEC) continue;
            snprintf(blips[nb].label, sizeof blips[nb].label, "%s",
                     s_seen[i].call);
            blips[nb].meters = est_distance_m(s_seen[i].rssi);
            nb++;
        }
        xui_radar_blips(blips, nb);
        xui_set_title("Links 1/8");
        break;
    }
    case 1: {   /* Flow: the packets going past, newest first */
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
            r->dist_m = fl->rssi ? est_distance_m(fl->rssi) : -1.0f;
            r->age_s = (now - fl->ms) / 1000;
            snprintf(r->text, sizeof r->text, "%s", fl->text);
        }
        xui_flow_rows(rows, nr);
        list_n = nr;
        xui_set_title("Flow 2/8");
        break;
    }
    case 2: {   /* Traffic: counters, one per row, explained when selected */
        uint32_t rx = 0, tx = 0, cancelled = 0, dropped = 0;
        uint32_t issued = 0, done = 0, failed = 0;
        uint32_t lrx = 0, ltx = 0, lcancel = 0;
        xprsnow_stats(&rx, &tx, &cancelled, &dropped);
        xprsnow_tx_stats(&issued, &done, &failed);
        xprslan_stats(&lrx, &ltx, &lcancel);

        static const char *const hdr[2] = { "Counter", "Value" };
        static const int cw[2] = { 170, 150 };
        xui_table_setup(2, hdr, cw);

        static xui_row_t tr[XUI_TAB_ROWS];
        int nr = 0;
        #define TROW(c0, det, fmt, ...) do { \
            snprintf(tr[nr].cell[0], sizeof tr[nr].cell[0], "%s", c0); \
            snprintf(tr[nr].cell[1], sizeof tr[nr].cell[1], fmt, __VA_ARGS__); \
            snprintf(tr[nr].detail, sizeof tr[nr].detail, "%s", det); \
            nr++; } while (0)
        TROW("ESP-NOW rx",
             "Packets received over the ESP-NOW radio since boot.",
             "%lu", (unsigned long)rx);
        char det[160];
        snprintf(det, sizeof det,
                 "Completed / queued transmissions over ESP-NOW. "
                 "Failed: %lu.", (unsigned long)failed);
        TROW("ESP-NOW tx", det, "%lu/%lu",
             (unsigned long)done, (unsigned long)issued);
        snprintf(det, sizeof det,
                 "Frames dropped: %lu. Radio-busy cancels: %lu.",
                 (unsigned long)dropped, (unsigned long)cancelled);
        TROW("ESP-NOW drop", det, "%lu",
             (unsigned long)(dropped + cancelled));
        TROW("LAN rx",
             "Packets received over WiFi / LAN (UDP broadcast).",
             "%lu", (unsigned long)lrx);
        snprintf(det, sizeof det,
                 "Packets sent over WiFi / LAN. Cancelled: %lu.",
                 (unsigned long)lcancel);
        TROW("LAN tx", det, "%lu", (unsigned long)ltx);
        TROW("Heard",
             "All XPRS packets heard on every link since boot.",
             "%lu", (unsigned long)s_heard_count);
        if (s_last_call[0]) {
            snprintf(det, sizeof det, "%s was the last station heard, "
                     "%lu s ago.", s_last_call,
                     (unsigned long)((now - s_last_rx_ms) / 1000));
            TROW("Last station", det, "%s", s_last_call);
        } else {
            TROW("Last station", "Nobody heard yet.", "%s", "--");
        }
        #undef TROW
        xui_table_rows(tr, nr);
        list_n = nr;
        xui_set_title("Traffic 3/8");
        break;
    }
    case 3: {   /* Devices: everyone in reach, detail on selection */
        static const char *const hdr[4] = { "Call", "Link", "Dist", "When" };
        static const int cw[4] = { 80, 74, 60, 106 };
        xui_table_setup(4, hdr, cw);

        static xui_row_t tr[XUI_TAB_ROWS];
        int nr = 0;
        for (int i = 0; i < SEEN_MAX && nr < XUI_TAB_ROWS; i++) {
            if (!s_seen[i].call[0]) continue;
            uint32_t age = (now - s_seen[i].last_ms) / 1000;
            if (age >= UI_INRANGE_SEC) continue;
            xui_row_t *r = &tr[nr++];
            snprintf(r->cell[0], sizeof r->cell[0], "%s", s_seen[i].call);
            snprintf(r->cell[1], sizeof r->cell[1], "%s", s_seen[i].bearer);
            if (s_seen[i].rssi) {
                int m = (int)(est_distance_m(s_seen[i].rssi) + 0.5f);
                snprintf(r->cell[2], sizeof r->cell[2], "~%dm", m);
                snprintf(r->detail, sizeof r->detail,
                         "%s via %s: %d dBm, about %d m away. "
                         "Heard %lu s ago.",
                         s_seen[i].call, s_seen[i].bearer,
                         s_seen[i].rssi, m, (unsigned long)age);
            } else {
                snprintf(r->cell[2], sizeof r->cell[2], "-");
                snprintf(r->detail, sizeof r->detail,
                         "%s via %s: distance unknown on this link. "
                         "Heard %lu s ago.",
                         s_seen[i].call, s_seen[i].bearer,
                         (unsigned long)age);
            }
            snprintf(r->cell[3], sizeof r->cell[3], "%lus",
                     (unsigned long)age);
        }
        xui_table_rows(tr, nr);
        list_n = nr;
        xui_set_title("Devices 4/8");
        break;
    }
    case 4: {   /* Node: this station's facts, full values on selection */
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
        xui_set_title("Node 5/8");
        break;
    }
    case 5: {   /* Settings: OK (button A) toggles the selected row */
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
            uint32_t tep = epoch_now();
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
        xui_set_title("Settings 6/8");
        break;
    }
    case 7: {   /* Chat: the human messages passing through this station */
        static const char *const hdr[3] = { "From", "Message", "When" };
        static const int cw[3] = { 68, 176, 76 };
        xui_table_setup(3, hdr, cw);

        static xui_row_t tr[XUI_TAB_ROWS];
        int nr = 0;
        uint32_t nowep = epoch_now();
        for (int i = 0; i < CHAT_MAX && nr < XUI_TAB_ROWS; i++) {
            int idx = s_chat_n - 1 - i;
            if (idx < 0) break;
            chat_t *c = &s_chat[idx % CHAT_MAX];
            if (!c->from[0]) continue;
            xui_row_t *r = &tr[nr++];
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
                const char *pfrom = c->r;
                for (int j = 0; j < CHAT_MAX; j++)
                    if (s_chat[j].from[0] &&
                        strcmp(s_chat[j].id, c->r) == 0) {
                        pfrom = s_chat[j].from;
                        break;
                    }
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
                const char *pfrom = c->r;
                const char *ptext = NULL;
                for (int j = 0; j < CHAT_MAX; j++)
                    if (s_chat[j].from[0] &&
                        strcmp(s_chat[j].id, c->r) == 0) {
                        pfrom = s_chat[j].from;
                        ptext = s_chat[j].text;
                        break;
                    }
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
        xui_set_title("Chat 8/8");
        break;
    }
    default: {  /* Stats: 10-minute, hourly or daily bars; arrows switch */
        uint16_t dev[SBDAY_N], rxv[SBDAY_N], txv[SBDAY_N];
        int np = 0;
        uint32_t ep = epoch_now();
        const char *suffix = "";
        if (!ep) {
            suffix = " (waiting for time)";
        } else if (s_stats_view == 0) {
            /* Last 24 ten-minute buckets: four hours. */
            suffix = " / 10 min (4 h)";
            uint32_t slot = ep / 600;
            np = 24;
            for (int k = 0; k < np; k++) {
                uint32_t sl = slot - (np - 1 - k);
                int i = (int)(sl % SB10_N);
                bool live = s_sb10_id[i] == sl;
                dev[k] = live ? s_sb10[i].dev : 0;
                rxv[k] = live ? (uint16_t)s_sb10[i].rx : 0;
                txv[k] = live ? (uint16_t)s_sb10[i].tx : 0;
            }
        } else if (s_stats_view == 1) {
            /* Last 24 hours, each the sum of its six buckets. */
            suffix = " / hour (24 h)";
            uint32_t hour = ep / 3600;
            np = 24;
            for (int k = 0; k < np; k++) {
                uint32_t hk = hour - (np - 1 - k);
                uint32_t rx = 0, tx = 0;
                uint16_t dv = 0;
                for (int j = 0; j < 6; j++) {
                    uint32_t sl = hk * 6 + j;
                    int i = (int)(sl % SB10_N);
                    if (s_sb10_id[i] != sl) continue;
                    rx += s_sb10[i].rx;
                    tx += s_sb10[i].tx;
                    if (s_sb10[i].dev > dv) dv = s_sb10[i].dev;
                }
                dev[k] = dv;
                rxv[k] = (uint16_t)(rx > 65535 ? 65535 : rx);
                txv[k] = (uint16_t)(tx > 65535 ? 65535 : tx);
            }
        } else {
            /* Last 30 days. */
            suffix = " / day (30 d)";
            uint32_t dslot = (uint32_t)((int64_t)ep + s_tz_off) / 86400;
            np = 30;
            for (int k = 0; k < np; k++) {
                uint32_t sl = dslot - (np - 1 - k);
                int i = (int)(sl % SBDAY_N);
                bool live = s_sbday_id[i] == sl;
                dev[k] = live ? s_sbday[i].dev : 0;
                rxv[k] = live ? (uint16_t)(s_sbday[i].rx > 65535 ? 65535
                                           : s_sbday[i].rx) : 0;
                txv[k] = live ? (uint16_t)(s_sbday[i].tx > 65535 ? 65535
                                           : s_sbday[i].tx) : 0;
            }
        }
        char t0[48], t1[48], t2[48];
        snprintf(t0, sizeof t0, "Devices heard%s", suffix);
        snprintf(t1, sizeof t1, "Packets received%s", suffix);
        snprintf(t2, sizeof t2, "Packets sent%s", suffix);
        xui_stats_set(0, t0, dev, np);
        xui_stats_set(1, t1, rxv, np);
        xui_stats_set(2, t2, txv, np);
        list_n = 0;
        xui_set_title("Stats 7/8");
        break;
    }
    }
    xui_set_body(body);
    xui_set_device_count(seen_in_range());

    /* Every list panel keeps its own selection: clamp it to what is on the
     * screen, and take the first row when rows appear after the panel was
     * visited empty. */
    if (s_panel != 0 && s_panel != 6) {
        if (s_sel[s_panel] >= list_n) s_sel[s_panel] = list_n - 1;
        if (s_sel[s_panel] < 0 && list_n > 0) s_sel[s_panel] = 0;
        if (s_panel == 5 && !s_set_focus)
            xui_table_select(-1);   /* nothing armed until the arrows dive in */
        else
            xui_table_select(s_sel[s_panel]);
    }

    /* The bottom bar tells the user what the three buttons under it do:
     * A cycles the menus (long press goes home), B and C move the selection
     * on the panels that have one. */
    if (s_panel == 5)
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

/* The stats rings, serialised whole: under 3 KB, written every ten minutes
 * by idx_task (the only task allowed near the storage) and read back at
 * boot -- a reboot no longer forgets the day. */
#define STATS_MAGIC 0x31545358u   /* "XST1" */
typedef struct {
    uint32_t  magic;
    sbucket_t sb10[SB10_N];
    uint32_t  sb10_id[SB10_N];
    sbucket_t sbday[SBDAY_N];
    uint32_t  sbday_id[SBDAY_N];
} stats_blob_t;

static void stats_load(void)
{
    FILE *f = fopen("/idx/stats.bin", "rb");
    if (!f) return;
    static stats_blob_t bl;
    bool ok = fread(&bl, sizeof bl, 1, f) == 1 && bl.magic == STATS_MAGIC;
    fclose(f);
    if (!ok) return;
    memcpy(s_sb10, bl.sb10, sizeof s_sb10);
    memcpy(s_sb10_id, bl.sb10_id, sizeof s_sb10_id);
    memcpy(s_sbday, bl.sbday, sizeof s_sbday);
    memcpy(s_sbday_id, bl.sbday_id, sizeof s_sbday_id);
    ESP_LOGI(TAG, "stats reloaded from flash");
}

static void stats_save(void)
{
    FILE *f = fopen("/idx/stats.bin", "wb");
    if (!f) return;
    static stats_blob_t bl;
    bl.magic = STATS_MAGIC;
    memcpy(bl.sb10, s_sb10, sizeof s_sb10);
    memcpy(bl.sb10_id, s_sb10_id, sizeof s_sb10_id);
    memcpy(bl.sbday, s_sbday, sizeof s_sbday);
    memcpy(bl.sbday_id, s_sbday_id, sizeof s_sbday_id);
    fwrite(&bl, sizeof bl, 1, f);
    fclose(f);
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

    stats_load();
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
                             "t:service f:%s serve:index,history,mailbox "
                             "count:%lu",
                             s_call, (unsigned long)st.count);
            n = sign_wire(w, n, sizeof w);
            xprsnow_send(w, n);
            xprslan_send(w, n);
        }

        if (s_ask.pending && s_index && xcfg_get_bool("index_on", true))
            idx_answer_history();

        /* The stats rings hit the flash every ten minutes -- losing at most
         * ten minutes of bars to a power pull. */
        if (epoch_now() && now_s - last_stats_save_s >= 600) {
            last_stats_save_s = now_s;
            stats_save();
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
        chat_note(&p);   /* our hotspot users' messages show on the LCD too */
    }

    bool lan = xprslan_send(wire, len);
    bool now = xcfg_get_bool("espnow_on", true) && xprsnow_send(wire, len);
    if ((lan || now) && s_index && xcfg_get_bool("index_on", true))
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
    .board = "m5stack-core",
    .send_wire = api_send_wire,
    .serve_json = api_serve_json,
    .features_json = api_features_json,
    .log_cur = "/idx/log/cur.txt",
    .log_prev = "/idx/log/prev.txt",
    .tz = "+00:00",
};

/* Bridge the generic UI's flush callback onto this board's panel driver. */
static void lcd_flush_adapter(int x1, int y1, int x2, int y2,
                              const uint16_t *px, void *ctx)
{
    ili9342_flush((ili9342_handle_t)ctx, x1, y1, x2, y2, px);
}

/* Poll one active-low button; 3 agreeing samples = state, fire on the edge.
 * GPIO 37-39 are input-only with the board's own pull-ups. */
typedef struct { gpio_num_t pin; int low_count; bool armed; } btn_t;

static bool btn_pressed(btn_t *b)
{
    if (gpio_get_level(b->pin) == 0) {
        if (b->low_count < 3) b->low_count++;
        if (b->low_count == 3 && b->armed) {
            b->armed = false;
            return true;
        }
    } else {
        b->low_count = 0;
        b->armed = true;
    }
    return false;
}

static void ui_task(void *arg)
{
    (void)arg;

    static btn_t bb = { BTN_B, 0, true };
    static btn_t bc = { BTN_C, 0, true };
    gpio_config_t io = {
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     /* 37-39 have no internal pulls */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
        .pin_bit_mask = (1ULL << BTN_A) | (1ULL << BTN_B) | (1ULL << BTN_C),
    };
    gpio_config(&io);

    esp_task_wdt_add(NULL);   /* a frozen UI becomes a logged reboot */

    uint64_t next_render_us = 0;
    int a_held_ms = 0;
    bool a_long_fired = false;
    for (;;) {
        bool force = false;

        /* Button A: a short press steps to the next menu, holding it is ESC
         * -- back to the home panel. */
        if (gpio_get_level(BTN_A) == 0) {
            a_held_ms += 10;
            if (a_held_ms >= 700 && !a_long_fired) {
                a_long_fired = true;
                s_panel = 0;
                s_set_focus = false;
                force = true;
                ESP_LOGI(TAG, "button A long: home");
            }
        } else {
            if (a_held_ms >= 30 && !a_long_fired) {
                if (s_rotate) { s_rotate = false; ESP_LOGI(TAG, "rotate: off"); }
                if (s_panel == 5 && s_set_focus) {
                    /* Inside the Settings list, A is OK. */
                    settings_ok(s_sel[5]);
                } else {
                    s_panel = (s_panel + 1) % UI_PANEL_COUNT;
                    s_set_focus = false;
                    ESP_LOGI(TAG, "button A: panel %d", s_panel);
                }
                force = true;
            }
            a_held_ms = 0;
            a_long_fired = false;
        }

        /* B and C: up / down. On every list panel they walk the rows and
         * the strip below shows the selected row's detail. */
        if (btn_pressed(&bb)) {
            if (s_rotate) { s_rotate = false; ESP_LOGI(TAG, "rotate: off"); }
            if (s_panel == 5) {
                if (s_set_focus && s_sel[5] > 0) s_sel[5]--;
                else s_set_focus = false;   /* above the top row: back out */
            } else if (s_panel == 6) {
                s_stats_view = (s_stats_view + 2) % 3;
            } else if (s_panel != 0 && s_sel[s_panel] > 0) {
                s_sel[s_panel]--;
            }
            force = true;
            ESP_LOGI(TAG, "button B: up (sel %d)", s_sel[s_panel]);
        }
        if (btn_pressed(&bc) && s_panel == 0 && !s_rotate) {
            /* The right button on the home screen starts the tour. */
            s_rotate = true;
            s_rotate_next_us = esp_timer_get_time() + 30000000ULL;
            s_panel = 6;                 /* Stats first, Chat, back home */
            force = true;
            ESP_LOGI(TAG, "rotate: on");
        } else if (btn_pressed(&bc)) {
            if (s_rotate) { s_rotate = false; ESP_LOGI(TAG, "rotate: off"); }
            else if (s_panel == 5) {
                if (!s_set_focus) { s_set_focus = true; s_sel[5] = 0; }
                else s_sel[5]++;             /* render clamps to the list */
            } else if (s_panel == 6) {
                s_stats_view = (s_stats_view + 1) % 3;
            } else if (s_panel != 0) {
                s_sel[s_panel]++;
            }
            force = true;
            ESP_LOGI(TAG, "button C: down (sel %d)", s_sel[s_panel]);
        }

        /* A new packet flashes the RX dot the moment it lands; the panels
         * themselves keep their cadence -- an instantly-updating list is
         * distracting to read. */
        static uint32_t last_heard;
        if (s_heard_count != last_heard) {
            last_heard = s_heard_count;
            xui_pulse();
        }

        /* Console debug: 'S' = screenshot over the UART, '1'..'5' = jump
         * to a panel (the buttons, but reachable from a script). */
        int ch = getchar();
        if (ch == 'S') xui_framedump();
        if (ch >= '1' && ch <= '0' + UI_PANEL_COUNT) {
            s_panel = ch - '1';
            force = true;
        }
        /* 'U'/'D' move the selection, 'K' is OK -- the buttons, scripted. */
        if (ch == 'U' && s_panel != 0) {
            if (s_panel == 5 && (!s_set_focus || s_sel[5] == 0))
                s_set_focus = false;
            else if (s_panel == 6) s_stats_view = (s_stats_view + 2) % 3;
            else if (s_sel[s_panel] > 0) s_sel[s_panel]--;
            force = true;
        }
        if (ch == 'D' && s_panel != 0) {
            if (s_panel == 5 && !s_set_focus) { s_set_focus = true; s_sel[5] = 0; }
            else if (s_panel == 6) s_stats_view = (s_stats_view + 1) % 3;
            else s_sel[s_panel]++;
            force = true;
        }
        if (ch == 'K' && s_panel == 5 && s_set_focus) {
            ESP_LOGI(TAG, "serial OK on settings row %d", s_sel[5]);
            settings_ok(s_sel[5]);
            force = true;
        }
        if (ch == 'W') s_wipe_req = true;   /* scripted archive wipe */

        uint64_t now_us = esp_timer_get_time();
        if (s_rotate && now_us >= s_rotate_next_us) {
            s_rotate_next_us = now_us + 30000000ULL;
            s_panel = s_panel == 0 ? 6 : s_panel == 6 ? 7 : 0;
            force = true;
        }
        if (force || now_us >= next_render_us) {
            ui_render();
            /* Scope and flow settle every 10 s; the counter panels at 2 s. */
            next_render_us = now_us +
                ((s_panel == 0 || s_panel == 1 || s_panel == 6)
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

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    xcfg_init();
    snprintf(s_ssid, sizeof s_ssid, "%s", xcfg_get("ssid", WIFI_SSID));
    snprintf(s_pass, sizeof s_pass, "%s", xcfg_get("pass", WIFI_PASS));

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
    ESP_LOGI(TAG, "M5Stack XPRS station %s (ESP-NOW + LAN, no BLE5 on this chip)",
             s_call);

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

    xTaskCreate(status_task, "status", 3072, NULL, 1, NULL);
    xTaskCreate(inet_probe_task, "inet", 3072, NULL, 1, NULL);

    /* The screen last: everything it reads already exists by now. M5Stack
     * Core LCD on VSPI-ish pins, backlight active-high. */
    ili9342_config_t lcd_cfg = {
        .mosi_pin = 23, .miso_pin = 19, .sclk_pin = 18,
        .cs_pin = 14, .dc_pin = 27, .rst_pin = 33, .bl_pin = 32,
    };
    ili9342_handle_t lcd = NULL;
    if (ili9342_init(&lcd_cfg, &lcd) == ESP_OK &&
        xui_init(ILI9342_WIDTH, ILI9342_HEIGHT, lcd_flush_adapter,
                 lcd) == ESP_OK) {
        if (xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL) != pdPASS)
            ESP_LOGE(TAG, "UI task failed to start");
        /* The API is the station's LAN face (spec/API-HTTP.md): always on
         * when the network is. The config share joins the same server so
         * its toggle opens and closes doors, not servers. */
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
    } else {
        ESP_LOGE(TAG, "display init failed — running headless");
    }
}
