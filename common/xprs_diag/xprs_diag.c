/* xprs_diag.c -- see the header. Everything here runs on the task that
 * calls xdiag_pump(), except xdiag_park() (receive task: parse and memcpy)
 * and xdiag_log_line() (whoever logs: a bounded copy under a spinlock). */
#include "xprs_diag.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "sdkconfig.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include "esp_core_dump.h"
#define XD_HAVE_COREDUMP 1
#else
#define XD_HAVE_COREDUMP 0
#endif

#include "xprs.h"
#include "xprs_auth.h"
#include "xprs_health.h"

static const char *TAG = "xdiag";

/* A result frame must fit BLE's 248 bytes on every bearer: one limit, the
 * strictest, so a page built for ESP-NOW is not refused by the advert. */
#define XD_WIRE_CAP   248
#define XD_SIG_BYTES  66          /* " sig:" + 61, measured */

static xdiag_cfg_t s_cfg;
static bool s_ready;        /* lines logged before init belong to the previous boot's evidence */

/* ── Crash facts, read once per boot ───────────────────────────────────── */

static esp_reset_reason_t s_reset;
static bool     s_crash_boot;      /* this boot followed a panic/watchdog */
static bool     s_core_valid;      /* a coredump summary was readable */
static char     s_core_task[16];
static uint32_t s_core_pc;
static uint32_t s_core_bt[16];
static int      s_core_depth;

static const char *reset_word(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_EXT:       return "ext";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "intwdt";
    case ESP_RST_TASK_WDT:  return "taskwdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "sleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "sdio";
    default:                return "unknown";
    }
}

static void read_core_summary(void)
{
#if XD_HAVE_COREDUMP
    /* ~220 bytes: on this stack once, never static. */
    esp_core_dump_summary_t s;
    if (esp_core_dump_get_summary(&s) != ESP_OK) return;
    snprintf(s_core_task, sizeof s_core_task, "%s", s.exc_task);
    s_core_pc = s.exc_pc;
    s_core_depth = (int)s.exc_bt_info.depth;
    if (s_core_depth > 16) s_core_depth = 16;
    memcpy(s_core_bt, s.exc_bt_info.bt, (size_t)s_core_depth * sizeof s_core_bt[0]);
    s_core_valid = true;
#endif
}

/* ── Last words: the log's tail, kept where a reboot cannot reach ───────── */

/* RTC slow memory is not cleared by a panic, a watchdog or esp_restart --
 * only by power. Ten lines of a hundred bytes is one kilobyte of an eight
 * kilobyte segment nothing else on these boards uses. The check word is
 * what tells a cold boot's garbage from a warm boot's evidence. */
#define XD_RTC_SLOTS 10
#define XD_RTC_LINE  100
#define XD_RTC_MAGIC 0x58444c57u   /* "XDLW" */

typedef struct {
    uint32_t magic;
    uint32_t w;                     /* lines ever written */
    char     line[XD_RTC_SLOTS][XD_RTC_LINE];
    uint32_t check;
} rtc_words_t;

/* Two generations, both in RTC: this boot writes into one while the words
 * of the boot that crashed stay readable in the other. The obvious version
 * -- freeze the ring into a static copy at startup -- spent a kilobyte of
 * internal DRAM on a duplicate of memory the chip was already holding for
 * free, on the boards docs/esp32.md says have the least of it. RTC slow
 * memory is 8 KB here and nothing else in this firmware uses any of it. */
static RTC_NOINIT_ATTR rtc_words_t s_rtc[2];
static portMUX_TYPE s_rtc_mux = portMUX_INITIALIZER_UNLOCKED;
static int s_gen;                  /* the half this boot writes */
static int s_last_gen = -1;        /* the half the last words are in, if any */
static int s_last_n;
static bool s_last_valid;

static uint32_t rtc_check(const rtc_words_t *r)
{
    return r->magic ^ (r->w * 2654435761u) ^ 0xa5a5a5a5u;
}

/* Boards without a log on flash keep a short tail in RAM for cmd:zlog. */
#define XD_TAIL_SLOTS 16
#define XD_TAIL_LINE  112
static char     (*s_tail)[XD_TAIL_LINE];
static uint32_t s_tail_w;

static void rtc_begin(int gen)
{
    portENTER_CRITICAL(&s_rtc_mux);
    memset(&s_rtc[gen], 0, sizeof s_rtc[gen]);
    s_rtc[gen].magic = XD_RTC_MAGIC;
    s_rtc[gen].check = rtc_check(&s_rtc[gen]);
    s_gen = gen;
    portEXIT_CRITICAL(&s_rtc_mux);
}

static bool rtc_valid(const rtc_words_t *r)
{
    return r->magic == XD_RTC_MAGIC && r->check == rtc_check(r) && r->w > 0;
}

/* The i-th line of [gen], newest first. NULL past the end. */
static const char *rtc_line(int gen, int i)
{
    const rtc_words_t *r = &s_rtc[gen];
    int have = r->w < XD_RTC_SLOTS ? (int)r->w : XD_RTC_SLOTS;
    if (gen < 0 || i < 0 || i >= have) return NULL;
    return r->line[(r->w - 1 - (uint32_t)i) % XD_RTC_SLOTS];
}

static void last_words_recover(void)
{
    /* Whichever half the last boot was writing: keep it, write the other. */
    int prev = -1;
    for (int g = 0; g < 2; g++)
        if (rtc_valid(&s_rtc[g]) &&
            (prev < 0 || s_rtc[g].w > s_rtc[prev].w)) prev = g;

    if (prev >= 0 && s_crash_boot) {
        s_last_gen = prev;
        s_last_n = s_rtc[prev].w < XD_RTC_SLOTS ? (int)s_rtc[prev].w : XD_RTC_SLOTS;
        s_last_valid = true;
        /* Into this boot's log too, so the flash copy has them as well. */
        ESP_LOGW(TAG, "last words before the %s, newest first:", reset_word(s_reset));
        for (int i = 0; i < s_last_n; i++)
            ESP_LOGW(TAG, "  %s", rtc_line(prev, i));
    }
    rtc_begin(prev == 0 ? 1 : 0);
}

void xdiag_log_line(const char *line, int n)
{
    if (!s_ready || !line || n <= 0) return;
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    if (n <= 0) return;

    /* Strip the colour here rather than trusting the caller to. The app
     * strips on the way into ITS ring and hands us the raw line, so the
     * escapes were reaching the air -- and hiding the level letter from
     * the filter below, which is why the first captured crash came back
     * as ten BLE sightings. */
    char buf[XD_TAIL_LINE];
    int o = 0;
    bool esc = false;
    for (int i = 0; i < n && o < (int)sizeof buf - 1; i++) {
        char c = line[i];
        if (esc) { if (isalpha((unsigned char)c)) esc = false; continue; }
        if (c == 0x1b) { esc = true; continue; }
        if (c == '\r') continue;
        buf[o++] = c;
    }
    buf[o] = 0;
    if (o <= 0) return;
    line = buf;
    n = o;

    /* Ten slots fill in a second with INFO on a busy board. The RTC ring
     * keeps warnings and errors -- what a post-mortem is made of -- and
     * the RAM tail keeps everything. */
    bool keep = true;
    {
        const char *sp = memchr(line, ' ', (size_t)n);
        if (sp && sp + 2 < line + n && sp[2] == ' ' &&
            (sp[1] == 'I' || sp[1] == 'D' || sp[1] == 'V')) keep = false;
    }
    int k = n < XD_RTC_LINE - 1 ? n : XD_RTC_LINE - 1;
    portENTER_CRITICAL(&s_rtc_mux);
    if (keep && s_rtc[s_gen].magic == XD_RTC_MAGIC) {
        rtc_words_t *r = &s_rtc[s_gen];
        char *dst = r->line[r->w % XD_RTC_SLOTS];
        memcpy(dst, line, (size_t)k);
        dst[k] = 0;
        r->w++;
        r->check = rtc_check(r);
    }
    if (s_tail) {
        int t = n < XD_TAIL_LINE - 1 ? n : XD_TAIL_LINE - 1;
        char *dst = s_tail[s_tail_w % XD_TAIL_SLOTS];
        memcpy(dst, line, (size_t)t);
        dst[t] = 0;
        s_tail_w++;
    }
    portEXIT_CRITICAL(&s_rtc_mux);
}

bool xdiag_last_words_valid(void) { return s_last_valid; }

/* The hook for boards that have none of their own (the dongle). Same
 * shape as xprs_app's: format small, stamp, strip colour, hand on. */
static vprintf_like_t s_log_orig;

static int log_hook(const char *fmt, va_list ap)
{
    va_list ap2;
    va_copy(ap2, ap);
    int out = s_log_orig ? s_log_orig(fmt, ap) : vprintf(fmt, ap);
    char line[160];
    uint32_t ep = s_cfg.epoch_now ? s_cfg.epoch_now() : 0;
    int n = ep ? snprintf(line, sizeof line, "%lu ", (unsigned long)ep)
               : snprintf(line, sizeof line, "+%lu ",
                          (unsigned long)(esp_timer_get_time() / 1000));
    int m = vsnprintf(line + n, sizeof line - n, fmt, ap2);
    va_end(ap2);
    if (m <= 0) return out;
    n += m;
    if (n >= (int)sizeof line) n = sizeof line - 1;
    /* strip ANSI colour and \r in place */
    int o = 0;
    bool esc = false;
    for (int i = 0; i < n; i++) {
        char c = line[i];
        if (esc) { if (isalpha((unsigned char)c)) esc = false; continue; }
        if (c == 0x1b) { esc = true; continue; }
        if (c == '\r') continue;
        line[o++] = c;
    }
    xdiag_log_line(line, o);
    return out;
}

/* ── Init ───────────────────────────────────────────────────────────────── */

void xdiag_init(const xdiag_cfg_t *cfg)
{
    if (!cfg) return;
    s_cfg = *cfg;
    s_reset = esp_reset_reason();
    s_crash_boot = s_reset == ESP_RST_PANIC || s_reset == ESP_RST_INT_WDT ||
                   s_reset == ESP_RST_TASK_WDT || s_reset == ESP_RST_WDT;
    read_core_summary();
    last_words_recover();
    if (!s_cfg.log_cur) {
        /* 1.8 KB, internal: the only log this board will ever serve. */
        s_tail = calloc(XD_TAIL_SLOTS, XD_TAIL_LINE);
        if (!s_tail) ESP_LOGW(TAG, "no RAM for the log tail; zlog serves last words only");
    }
    if (s_cfg.hook_log) s_log_orig = esp_log_set_vprintf(log_hook);
    s_ready = true;
    ESP_LOGI(TAG, "over-the-air diagnostics: reset %s%s%s",
             reset_word(s_reset), s_core_valid ? ", crash in " : "",
             s_core_valid ? s_core_task : "");
}

/* ── Helpers shared by the frames ───────────────────────────────────────── */

static void uptime_word(char *out, int cap)
{
    uint32_t s = (uint32_t)(esp_timer_get_time() / 1000000);
    if (s < 3600)              snprintf(out, cap, "%lum", (unsigned long)(s / 60));
    else if (s < 48 * 3600)    snprintf(out, cap, "%luh", (unsigned long)(s / 3600));
    else                       snprintf(out, cap, "%lud", (unsigned long)(s / 86400));
}

static int ts_now(char *out, int cap)
{
    time_t t = time(NULL);
    if (t < 1700000000) { out[0] = 0; return 0; }
    struct tm tmv;
    gmtime_r(&t, &tmv);
    return (int)strftime(out, cap, "%Y-%m-%d_%H:%M:%S", &tmv);
}

/* "YYYY-MM-DD_hh:mm:ss" -> epoch, 0 when it is not one. UTC, no libc
 * timegm on this target. */
static uint32_t ts_epoch(const char *s)
{
    int Y, M, D, h, m, sec;
    if (!s || sscanf(s, "%4d-%2d-%2d_%2d:%2d:%2d", &Y, &M, &D, &h, &m, &sec) != 6)
        return 0;
    if (M < 1 || M > 12 || D < 1 || D > 31) return 0;
    /* days from civil, Howard Hinnant's arithmetic */
    int y = Y - (M <= 2);
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    long days = (long)era * 146097 + doe - 719468;
    return (uint32_t)(days * 86400L + h * 3600 + m * 60 + sec);
}

/* One signed result on the asker's bearer. [fields] are extra key:value
 * pairs (no leading space); [m] goes last because the signer splices sig:
 * before it. Whatever does not fit 248 bytes is cut from the tail of m:. */
static void air_result(const char *to, const char *bearer, const char *id,
                       int code, const char *fields, const char *m)
{
    if (!s_cfg.sign || !s_cfg.air || !to || !to[0] || !id || !id[0]) return;
    char w[XPRS_MAX_WIRE + 1], ts[24];
    ts_now(ts, sizeof ts);
    int n = snprintf(w, sizeof w, "t:result f:%s d:%s%s%s r:%s code:%d",
                     s_cfg.callsign, to, ts[0] ? " ts:" : "", ts[0] ? ts : "",
                     id, code);
    if (n <= 0 || n >= (int)sizeof w) return;
    if (fields && fields[0]) {
        /* Room for the signature is reserved first. What does not fit is
         * cut at a field boundary and said out loud -- a diagnostic that
         * silently drops its own contents is worse than none. */
        int room = XD_WIRE_CAP - XD_SIG_BYTES - n - 1;
        int fl = (int)strlen(fields);
        if (fl > room) {
            while (room > 0 && fields[room] != ' ') room--;
            ESP_LOGW(TAG, "result %s trimmed to %d of %d bytes of fields",
                     id, room, fl);
            fl = room;
        }
        if (fl > 0) n += snprintf(w + n, sizeof w - n, " %.*s", fl, fields);
    }
    if (m && m[0]) {
        int room = XD_WIRE_CAP - XD_SIG_BYTES - n - 3;
        if (room > 0) {
            int ml = (int)strlen(m);
            if (ml > room) ml = room;
            n += snprintf(w + n, sizeof w - n, " m:%.*s", ml, m);
        }
    }
    n = s_cfg.sign(w, n, sizeof w);
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    s_cfg.air(bearer, w, n);
}

/* ── Metering (31.2) ────────────────────────────────────────────────────
 *
 * Every asker here is an allow-listed owner: xauth_check said so before
 * this is reached. The history meter would file them as strangers -- it
 * classifies by whether a signing key was learned from the air, which an
 * owner known only by npub never is -- and hand them two pages an hour.
 * So the pages are counted here at the KNOWN rate, and also recorded in
 * the history meter, so diagnostics and replays together cannot cost a
 * station more than replays alone were already allowed to.
 */
#define XD_PAGES_PH   6
#define XD_GLOBAL_PH 12
static struct { char call[16]; uint32_t when[XD_PAGES_PH]; } s_asks[4];
static uint32_t s_global[XD_GLOBAL_PH];

static bool page_budget(const char *from, uint32_t now_s)
{
    int g = 0;
    for (int i = 0; i < XD_GLOBAL_PH; i++)
        if (s_global[i] && now_s - s_global[i] < 3600) g++;
    if (g >= XD_GLOBAL_PH) return false;
    for (int a = 0; a < 4; a++) {
        if (strcasecmp(s_asks[a].call, from) != 0) continue;
        int n = 0;
        for (int i = 0; i < XD_PAGES_PH; i++)
            if (s_asks[a].when[i] && now_s - s_asks[a].when[i] < 3600) n++;
        return n < XD_PAGES_PH;
    }
    return true;
}

static void page_record(const char *from, uint32_t now_s)
{
    for (int i = 0; i < XD_GLOBAL_PH; i++)
        if (!s_global[i] || now_s - s_global[i] >= 3600) { s_global[i] = now_s; break; }
    int slot = 0;
    for (int a = 0; a < 4; a++) {
        if (strcasecmp(s_asks[a].call, from) == 0) { slot = a; goto have; }
        if (!s_asks[a].call[0]) slot = a;
    }
    snprintf(s_asks[slot].call, sizeof s_asks[0].call, "%s", from);
    memset(s_asks[slot].when, 0, sizeof s_asks[0].when);
have:
    for (int i = 0; i < XD_PAGES_PH; i++)
        if (!s_asks[slot].when[i] || now_s - s_asks[slot].when[i] >= 3600) {
            s_asks[slot].when[i] = now_s;
            break;
        }
    if (s_cfg.budget_record) s_cfg.budget_record(from, now_s);
}

/* ── The parked ask ─────────────────────────────────────────────────────── */

enum { XD_NONE = 0, XD_DIAG, XD_CORE, XD_LOG };

static struct {
    volatile bool pending;
    char wire[XPRS_MAX_WIRE + 1];
    int  len;
    char bearer[8];
    uint32_t heard_ms;      /* when the first copy landed */
} s_ask;

/* How long to let the other copies of one ask arrive before answering.
 * A gateway airs on every bearer it has and they do not land together:
 * LoRa is seconds behind the LAN. Answering the first copy heard means
 * answering on whichever bearer happened to win, and a page that would
 * take eighteen seconds on ESP-NOW takes two minutes on LoRa. Waiting a
 * second and a half costs nothing next to that, and it also collapses the
 * duplicates into one answer instead of one answer each. */
#define XD_SETTLE_MS 1500

static int cmd_code(const char *cmd)
{
    if (strcmp(cmd, "zdiag") == 0) return XD_DIAG;
    if (strcmp(cmd, "zcore") == 0) return XD_CORE;
    if (strcmp(cmd, "zlog") == 0)  return XD_LOG;
    return XD_NONE;
}

/* Cheapest first: the LAN costs nothing, LoRa costs seconds of a shared band. */
static int bearer_rank(const char *b)
{
    if (!b) return 9;
    if (strcmp(b, "lan") == 0)    return 0;
    if (strcmp(b, "espnow") == 0) return 1;
    if (strcmp(b, "ble") == 0)    return 2;
    return 3;                                             /* lora */
}

static bool addressed_to_us(const xprs_t *p)
{
    char dst[16] = "";
    if (!xprs_get_str(p, "d", dst, sizeof dst) || !dst[0]) return false;
    char *dash = strchr(dst, '-');
    if (dash) *dash = 0;
    char base[16];
    snprintf(base, sizeof base, "%s", s_cfg.callsign ? s_cfg.callsign : "");
    dash = strchr(base, '-');
    if (dash) *dash = 0;
    return strcasecmp(dst, base) == 0;
}

bool xdiag_park(const char *wire, int len, const char *bearer)
{
    if (!wire || len <= 0 || len > XPRS_MAX_WIRE) return false;
    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return false;
    return xdiag_park_parsed(&p, wire, len, bearer);
}

bool xdiag_park_parsed(const xprs_t *p, const char *wire, int len,
                       const char *bearer)
{
    if (!p || !wire || len <= 0 || len > XPRS_MAX_WIRE || !s_cfg.callsign) return false;
    char type[16], cmd[16];
    xprs_type(p, type, sizeof type);
    if (strcmp(type, "command") != 0) return false;
    if (!xprs_get_str(p, "cmd", cmd, sizeof cmd) || cmd[0] != 'z') return false;
    if (cmd_code(cmd) == XD_NONE) return false;
    if (!addressed_to_us(p)) return false;
    if (s_ask.pending) {
        /* The same ask on a second bearer: a gateway airs on everything it
         * has, so one command arrives two or three times. Answer on the
         * quickest of them -- a page that could have taken eighteen seconds
         * on ESP-NOW should not take two minutes on LoRa because the LoRa
         * copy happened to land first. */
        if (s_ask.len == len && memcmp(s_ask.wire, wire, (size_t)len) == 0 &&
            bearer_rank(bearer) < bearer_rank(s_ask.bearer))
            snprintf(s_ask.bearer, sizeof s_ask.bearer, "%s", bearer);
        return false;                         /* one in flight, like history */
    }
    memcpy(s_ask.wire, wire, (size_t)len);
    s_ask.wire[len] = 0;
    s_ask.len = len;
    snprintf(s_ask.bearer, sizeof s_ask.bearer, "%s", bearer ? bearer : "lan");
    s_ask.heard_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_ask.pending = true;                     /* published last */
    return true;
}

/* ── Paging ─────────────────────────────────────────────────────────────── */

enum { SRC_FILE = 0, SRC_LAST, SRC_TAIL };

static struct {
    bool     active;
    int      cmd;
    char     from[16];
    char     id[8];
    char     bearer[8];
    int      sent;            /* frames of payload aired so far */
    int      page_max;
    uint32_t spacing_ms;
    uint32_t next_due;
    /* zlog */
    int      src;
    uint32_t since, until;
    char     zq[24];
} s_pg;

/* Pacing per bearer (31.4): the history replay's 1.5 s on the fast ones;
 * on LoRa one 250-byte frame is ~400 ms of a 1 % band, so 30 s apart and
 * four to a page keeps a full page near the duty cycle, not through it. */
static void pace_for(const char *bearer, int *page, uint32_t *spacing)
{
    if (strcmp(bearer, "lora") == 0)     { *page = 4;  *spacing = 30000; }
    else if (strcmp(bearer, "ble") == 0) { *page = 6;  *spacing = 1500; }
    else                                 { *page = 12; *spacing = 1500; }
}

/* A log line passes when its stamp is inside the window and it carries
 * the filter. Lines stamped +ms (no clock yet) pass only an unbounded ask. */
static bool line_passes(const char *line, int n)
{
    if (s_pg.zq[0]) {
        char tmp[XD_TAIL_LINE + 1];
        int k = n < (int)sizeof tmp - 1 ? n : (int)sizeof tmp - 1;
        memcpy(tmp, line, (size_t)k);
        tmp[k] = 0;
        if (!strstr(tmp, s_pg.zq)) return false;
    }
    if (!s_pg.since && !s_pg.until) return true;
    if (n <= 0 || !isdigit((unsigned char)line[0])) return false;
    uint32_t ts = (uint32_t)strtoul(line, NULL, 10);
    if (s_pg.since && ts < s_pg.since) return false;
    if (s_pg.until && ts > s_pg.until) return false;
    return true;
}

/* The [skip]-th passing line from the end of one file, newest first. The
 * same backwards block walk /api/log does, minus the HTTP. Returns the
 * line length, 0 when the file ran out; [skip] is decremented per passing
 * line so a second file continues the count. */
static int file_nth_line(const char *path, int *skip, char *out, int cap)
{
    FILE *f = path ? fopen(path, "rb") : NULL;
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    long pos = ftell(f);
    /* 256 + 128 rather than 512 + 200: this walks backwards looking for
     * newlines, and the block only has to be comfortably longer than one
     * log line. docs/esp32.md counts every static on these boards. */
    static char carry[128];
    static char blk[256 + sizeof carry];
    int carry_n = 0, found = 0;
    while (pos > 0 && !found) {
        int n = pos > 256 ? 256 : (int)pos;
        pos -= n;
        if (fseek(f, pos, SEEK_SET) != 0) break;
        if (fread(blk, 1, (size_t)n, f) != (size_t)n) break;
        memcpy(blk + n, carry, (size_t)carry_n);
        int total = n + carry_n;
        int end = total, i = total;
        while (i > 0 && !found) {
            i--;
            if (blk[i] != '\n') continue;
            if (end > i + 1) {
                const char *l = blk + i + 1;
                int ln = end - i - 1;
                if (line_passes(l, ln)) {
                    if (*skip == 0) {
                        found = ln < cap - 1 ? ln : cap - 1;
                        memcpy(out, l, (size_t)found);
                        out[found] = 0;
                    } else (*skip)--;
                }
            }
            end = i;
        }
        if (!found) {
            if (pos == 0 && end > 0 && line_passes(blk, end)) {
                if (*skip == 0) {
                    found = end < cap - 1 ? end : cap - 1;
                    memcpy(out, blk, (size_t)found);
                    out[found] = 0;
                } else (*skip)--;
            } else {
                carry_n = end > (int)sizeof carry ? (int)sizeof carry : end;
                memcpy(carry, blk + end - carry_n, (size_t)carry_n);
            }
        }
    }
    fclose(f);
    return found;
}

/* The next line of the current page, by source. 0 when exhausted. */
static int next_line(char *out, int cap)
{
    int skip = s_pg.sent;
    if (s_pg.src == SRC_LAST) {
        for (int i = 0; i < s_last_n; i++) {
            const char *l = rtc_line(s_last_gen, i);
            if (!l) break;
            int ln = (int)strlen(l);
            if (!line_passes(l, ln)) continue;
            if (skip-- == 0) { snprintf(out, cap, "%s", l); return ln; }
        }
        return 0;
    }
    if (s_pg.src == SRC_TAIL) {
        if (!s_tail) return 0;
        uint32_t have = s_tail_w < XD_TAIL_SLOTS ? s_tail_w : XD_TAIL_SLOTS;
        for (uint32_t i = 0; i < have; i++) {
            const char *l = s_tail[(s_tail_w - 1 - i) % XD_TAIL_SLOTS];
            int ln = (int)strlen(l);
            if (!line_passes(l, ln)) continue;
            if (skip-- == 0) { snprintf(out, cap, "%s", l); return ln; }
        }
        return 0;
    }
    int n = file_nth_line(s_cfg.log_cur, &skip, out, cap);
    if (!n) n = file_nth_line(s_cfg.log_prev, &skip, out, cap);
    return n;
}

static void page_done(int code)
{
    air_result(s_pg.from, s_pg.bearer, s_pg.id, code, NULL, NULL);
    s_pg.active = false;
}

static void page_step(uint32_t now_ms)
{
    if (!s_pg.active || (int32_t)(now_ms - s_pg.next_due) < 0) return;
    s_pg.next_due = now_ms + s_pg.spacing_ms;

    if (s_pg.cmd == XD_CORE) {
        /* Frame 2: the rest of the backtrace, or just the close. */
        char m[160] = "";
        int o = 0;
        for (int i = 8; i < s_core_depth && o < (int)sizeof m - 10; i++)
            o += snprintf(m + o, sizeof m - o, "%s%08lx", o ? " " : "",
                          (unsigned long)s_core_bt[i]);
        air_result(s_pg.from, s_pg.bearer, s_pg.id, 200, NULL, m);
        s_pg.active = false;
        return;
    }

    /* zlog: one line per tick, then the close. A page that fills says
     * 206 -- more on request, with until: moved to the oldest stamp
     * heard (25.2.1); one that runs dry says 200. */
    if (s_pg.sent >= s_pg.page_max) { page_done(206); return; }
    char line[XD_TAIL_LINE + 40];
    int n = next_line(line, sizeof line);
    if (!n) { page_done(200); return; }
    air_result(s_pg.from, s_pg.bearer, s_pg.id, 206, NULL, line);
    s_pg.sent++;
}

/* ── The three answers ──────────────────────────────────────────────────── */

static void answer_zdiag(const char *from, const char *bearer, const char *id)
{
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);
    uint32_t s[8] = {0};
    if (s_cfg.stats) s_cfg.stats(s);
    uint16_t up = 0, req = 0;
    xh_masks(&up, &req);
    char ut[12];
    uptime_word(ut, sizeof ut);
    char f[200];
    int n = snprintf(f, sizeof f,
        "fw:%s uptime:%s peers:%lu zr:%s zm:%u/%u/%u zh:%x/%x "
        "zn:%lu/%lu/%lu/%lu zs:%lu/%lu/%lu zp:%s/%d",
        d ? d->version : "?", ut, (unsigned long)s[7], reset_word(s_reset),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
        up, req,
        (unsigned long)s[0], (unsigned long)s[1], (unsigned long)s[2], (unsigned long)s[3],
        (unsigned long)s[5], (unsigned long)s[4], (unsigned long)s[6],
        run ? run->label : "?", (int)st);
    if (s_core_valid && n > 0 && n < (int)sizeof f)
        snprintf(f + n, sizeof f - n, " zc:%s", s_core_task);
    air_result(from, bearer, id, 200, f, NULL);
    xauth_remember(id, 200);
}

static void answer_zcore(const char *from, const char *bearer, const char *id,
                         uint32_t now_ms)
{
    if (!s_core_valid) {
        air_result(from, bearer, id, 404, NULL, "no crash recorded");
        xauth_remember(id, 404);
        return;
    }
    char f[64], m[100];
    snprintf(f, sizeof f, "zc:%s,%s,%08lx", reset_word(s_reset), s_core_task,
             (unsigned long)s_core_pc);
    int o = 0;
    for (int i = 0; i < s_core_depth && i < 8; i++)
        o += snprintf(m + o, sizeof m - o, "%s%08lx", o ? " " : "",
                      (unsigned long)s_core_bt[i]);
    bool more = s_core_depth > 8;
    air_result(from, bearer, id, more ? 206 : 200, f, m);
    xauth_remember(id, more ? 206 : 200);
    if (!more) return;
    s_pg.active = true;
    s_pg.cmd = XD_CORE;
    snprintf(s_pg.from, sizeof s_pg.from, "%s", from);
    snprintf(s_pg.id, sizeof s_pg.id, "%s", id);
    snprintf(s_pg.bearer, sizeof s_pg.bearer, "%s", bearer);
    pace_for(bearer, &s_pg.page_max, &s_pg.spacing_ms);
    s_pg.next_due = now_ms + s_pg.spacing_ms;
}

static void answer_zlog(const xprs_t *p, const char *from, const char *bearer,
                        const char *id, uint32_t now_ms)
{
    char zl[12] = "", ts[24];
    xprs_get_str(p, "zl", zl, sizeof zl);
    s_pg.src = strcmp(zl, "last") == 0 ? SRC_LAST : s_cfg.log_cur ? SRC_FILE : SRC_TAIL;
    s_pg.since = xprs_get_str(p, "since", ts, sizeof ts) ? ts_epoch(ts) : 0;
    s_pg.until = xprs_get_str(p, "until", ts, sizeof ts) ? ts_epoch(ts) : 0;
    if (!xprs_get_str(p, "zq", s_pg.zq, sizeof s_pg.zq)) s_pg.zq[0] = 0;
    if (s_pg.src == SRC_LAST && !s_last_valid) {
        air_result(from, bearer, id, 404, NULL, "no last words: the previous boot ended cleanly");
        xauth_remember(id, 404);
        return;
    }
    s_pg.active = true;
    s_pg.cmd = XD_LOG;
    s_pg.sent = 0;
    snprintf(s_pg.from, sizeof s_pg.from, "%s", from);
    snprintf(s_pg.id, sizeof s_pg.id, "%s", id);
    snprintf(s_pg.bearer, sizeof s_pg.bearer, "%s", bearer);
    pace_for(bearer, &s_pg.page_max, &s_pg.spacing_ms);
    s_pg.next_due = now_ms + s_pg.spacing_ms;
    char f[24];
    snprintf(f, sizeof f, "zl:%s", s_pg.src == SRC_LAST ? "last" :
                                  s_pg.src == SRC_TAIL ? "tail" : "file");
    air_result(from, bearer, id, 202, f, NULL);
    xauth_remember(id, 202);
}

/* A repeat is not a mistake: a gateway airs one ask on every bearer it
 * has, so the same command arrives two or three times and 25.4 says to
 * answer again without doing the work twice. Answering it with a bare
 * code, though, hands the asker a worse copy of what it already has --
 * so a repeated zdiag is simply answered again (the data is live and it
 * is one frame), while a repeated zlog or zcore, whose page is already
 * on its way, gets the code and no second page. */
static void answer_repeat(const xprs_t *p, const char *cmd, const char *from,
                          const char *bearer, const char *id, int prev,
                          uint32_t now_ms)
{
    (void)p; (void)now_ms;
    if (cmd_code(cmd) == XD_DIAG) answer_zdiag(from, bearer, id);
    else                          air_result(from, bearer, id, prev, NULL, NULL);
}

/* ── The pump ───────────────────────────────────────────────────────────── */

void xdiag_pump(uint32_t now_ms)
{
    if (s_ask.pending && (int32_t)(now_ms - s_ask.heard_ms) >= XD_SETTLE_MS) {
        s_ask.pending = false;
        xprs_t p;
        if (xprs_parse(s_ask.wire, s_ask.len, &p)) {
            char id[8] = "", from[16] = "", cmd[16] = "";
            int prev = 0;
            xprs_get_str(&p, "cmd", cmd, sizeof cmd);
            xauth_verdict_t v = xauth_check(&p, s_cfg.callsign, id, from, &prev);
            ESP_LOGW(TAG, "cmd:%s from %s on %s -> verdict %d", cmd, from,
                     s_ask.bearer, (int)v);
            if (v == XAUTH_REPEAT) {
                answer_repeat(&p, cmd, from, s_ask.bearer, id, prev, now_ms);
            } else if (v == XAUTH_403) {
                air_result(from, s_ask.bearer, id, 403, NULL, "not on the allow list");
            } else if (v == XAUTH_408) {
                air_result(from, s_ask.bearer, id, 408, NULL, NULL);
            } else if (v == XAUTH_OK) {
                int c = cmd_code(cmd);
                if (s_pg.active) {
                    /* One page at a time on the channel, the history rule. */
                    air_result(from, s_ask.bearer, id, 429, NULL, "busy, ask again later");
                    xauth_remember(id, 429);
                } else if (c == XD_DIAG) {
                    answer_zdiag(from, s_ask.bearer, id);
                } else {
                    uint32_t now_s = s_cfg.epoch_now ? s_cfg.epoch_now() : 0;
                    if (!now_s) now_s = (uint32_t)(esp_timer_get_time() / 1000000);
                    if (!page_budget(from, now_s)) {
                        /* Refuse out loud (31.2): silence means "did not
                         * arrive", and this did. */
                        air_result(from, s_ask.bearer, id, 429, NULL, "over budget, ask later");
                        xauth_remember(id, 429);
                    } else {
                        page_record(from, now_s);
                        if (c == XD_CORE) answer_zcore(from, s_ask.bearer, id, now_ms);
                        else              answer_zlog(&p, from, s_ask.bearer, id, now_ms);
                    }
                }
            }
            /* XAUTH_SILENT: nothing at all, deliberately (25.4). */
        }
    }
    page_step(now_ms);
}

/* ── Beacon fields ──────────────────────────────────────────────────────── */

int xdiag_beacon_fields(char *buf, int cap)
{
    if (!buf || cap <= 0) return 0;
    uint16_t up = 0, req = 0;
    xh_masks(&up, &req);
    char ut[12];
    uptime_word(ut, sizeof ut);
    int n = snprintf(buf, cap, " uptime:%s zh:%x/%x", ut, up, req);
    if (n <= 0 || n >= cap) { buf[0] = 0; return 0; }
    if (s_crash_boot && s_core_valid) {
        int k = snprintf(buf + n, cap - n, " zc:%s,%s,%08lx", reset_word(s_reset),
                         s_core_task, (unsigned long)s_core_pc);
        if (k > 0 && n + k < cap) n += k;
        else buf[n] = 0;
    }
    return n;
}

/* ── Test hooks ─────────────────────────────────────────────────────────── */

bool xdiag_console(const char *line)
{
#ifdef XDIAG_TEST_HOOKS
    if (!line) return false;
    if (strcmp(line, "cfg zpanic") == 0) {
        ESP_LOGE(TAG, "XDIAG_TEST_HOOKS: aborting on request");
        vTaskDelay(pdMS_TO_TICKS(50));
        abort();
    }
    if (strcmp(line, "cfg zhang") == 0) {
        ESP_LOGE(TAG, "XDIAG_TEST_HOOKS: hanging with interrupts off on request");
        vTaskDelay(pdMS_TO_TICKS(50));
        portDISABLE_INTERRUPTS();
        for (;;) { }
    }
#else
    (void)line;
#endif
    return false;
}
