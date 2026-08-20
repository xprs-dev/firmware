/* xprs_station.c -- see the header. The logic is the m5stack firmware's,
 * moved verbatim where possible; the one addition is the spinlock, which
 * the single-board original never needed. */
#include "xprs_station.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "xst";

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOCK()   portENTER_CRITICAL(&s_mux)
#define UNLOCK() portEXIT_CRITICAL(&s_mux)

static char s_call[10];
static int  s_tz_off;

/* ── Who we have heard, by name ─────────────────────────────────────── */
static xst_dev_t s_seen[XST_SEEN_MAX];

/* ── The chat ring ──────────────────────────────────────────────────── */
static xst_chat_t s_chat[XST_CHAT_MAX];
static int s_chat_n;              /* total ever; ring pos = n % MAX */

/* ── Statistics on the wall clock ───────────────────────────────────── */
#define STAT_DEVS 12
typedef struct {
    uint32_t rx, tx;
    uint16_t dev, pad;
} sbucket_t;
static sbucket_t s_sb10[XST_SB10_N];
static uint32_t  s_sb10_id[XST_SB10_N];
static sbucket_t s_sbday[XST_SBDAY_N];
static uint32_t  s_sbday_id[XST_SBDAY_N];

/* Distinct devices in the LIVE 10-minute bucket only. */
static uint32_t s_cur_devh[STAT_DEVS];
static int      s_cur_devn;
static uint32_t s_cur_slot;

/* TX primed delta (was static locals in the m5 ui_render). */
static uint32_t s_tx_prev;
static bool     s_tx_primed;

uint32_t xst_epoch_now(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (uint32_t)t : 0;   /* 0 until NTP has spoken */
}

static sbucket_t *stat10(uint32_t ep)
{
    uint32_t slot = ep / 600;
    int i = (int)(slot % XST_SB10_N);
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
    int i = (int)(slot % XST_SBDAY_N);
    if (s_sbday_id[i] != slot) {
        memset(&s_sbday[i], 0, sizeof s_sbday[i]);
        s_sbday_id[i] = slot;
    }
    return &s_sbday[i];
}

void xst_init(const char *own_call, int tz_off_sec)
{
    xst_set_call(own_call ? own_call : "");
    s_tz_off = tz_off_sec;
}

void xst_set_call(const char *own_call)
{
    snprintf(s_call, sizeof s_call, "%s", own_call ? own_call : "");
}

/* Devices row upsert. Caller holds no lock. */
static void dev_upsert(const char *call, const char *bearer, int rssi)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    LOCK();
    int slot = -1, oldest = 0;
    for (int i = 0; i < XST_SEEN_MAX; i++) {
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
    UNLOCK();
}

void xst_dev_note(const char *call, const char *bearer, int rssi)
{
    if (!call || !call[0]) return;
    if (s_call[0] && strcasecmp(call, s_call) == 0) return;
    dev_upsert(call, bearer ? bearer : "", rssi);
}

void xst_chat_note(const xprs_t *p)
{
    char from[10], text[120];
    char type[16];
    xprs_type(p, type, sizeof type);
    if (strcmp(type, "message") != 0) return;
    if (!xprs_get_str(p, "f", from, sizeof from)) return;
    if (!xprs_get_str(p, "m", text, sizeof text) || !text[0]) return;

    xst_chat_t row;
    memset(&row, 0, sizeof row);
    snprintf(row.from, sizeof row.from, "%s", from);
    snprintf(row.text, sizeof row.text, "%s", text);
    xprs_id(p, row.id);
    if (!xprs_get_str(p, "r", row.r, sizeof row.r)) row.r[0] = 0;
    char sc[12], dst[16];
    if (xprs_get_str(p, "d", dst, sizeof dst) && dst[0] != '#')
        row.kind = 2;                                  /* a 1:1 */
    else if (xprs_get_str(p, "scope", sc, sizeof sc) &&
             strcmp(sc, "local") == 0)
        row.kind = 1;                                  /* the local room */
    else
        row.kind = 0;                                  /* global, default */
    row.ep = xst_epoch_now();

    LOCK();
    /* The same message arrives more than once -- our own send plus the
     * relay's re-air, or both bearers. One line per saying. */
    for (int i = 0; i < XST_CHAT_MAX; i++) {
        if (s_chat[i].from[0] && strcmp(s_chat[i].from, row.from) == 0 &&
            strcmp(s_chat[i].text, row.text) == 0) {
            UNLOCK();
            return;
        }
    }
    s_chat[s_chat_n % XST_CHAT_MAX] = row;
    s_chat_n++;
    UNLOCK();
}

bool xst_ingest_parsed(const xprs_t *p, const char *bearer, int rssi)
{
    char call[10];
    if (!xprs_get_str(p, "f", call, sizeof call)) return false;
    if (s_call[0] && strcasecmp(call, s_call) == 0) return false;

    xst_chat_note(p);

    uint32_t ep = xst_epoch_now();
    if (ep) {
        LOCK();
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
        UNLOCK();
    }

    dev_upsert(call, bearer ? bearer : "", rssi);
    return true;
}

bool xst_ingest(const char *wire, int len, const char *bearer, int rssi)
{
    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return false;
    return xst_ingest_parsed(&p, bearer, rssi);
}

void xst_tx_total(uint32_t tx_total_now)
{
    uint32_t ep = xst_epoch_now();
    LOCK();
    if (s_tx_primed && tx_total_now > s_tx_prev && ep) {
        stat10(ep)->tx += tx_total_now - s_tx_prev;
        statday(ep)->tx += tx_total_now - s_tx_prev;
    }
    if (ep || !s_tx_primed) s_tx_prev = tx_total_now;
    s_tx_primed = true;
    UNLOCK();
}

int xst_devices(xst_dev_t *out, int max, int in_range_sec)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    xst_dev_t rows[XST_SEEN_MAX];
    int n = 0;
    LOCK();
    for (int i = 0; i < XST_SEEN_MAX; i++) {
        if (!s_seen[i].call[0]) continue;
        if ((now - s_seen[i].last_ms) / 1000 >= (uint32_t)in_range_sec)
            continue;
        rows[n++] = s_seen[i];
    }
    UNLOCK();
    /* Freshest first (insertion sort; n <= 16). */
    for (int i = 1; i < n; i++) {
        xst_dev_t v = rows[i];
        int j = i - 1;
        while (j >= 0 && rows[j].last_ms < v.last_ms) {
            rows[j + 1] = rows[j];
            j--;
        }
        rows[j + 1] = v;
    }
    if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = rows[i];
    return n;
}

int xst_devices_in_range(int in_range_sec)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    int n = 0;
    LOCK();
    for (int i = 0; i < XST_SEEN_MAX; i++) {
        if (s_seen[i].call[0] &&
            (now - s_seen[i].last_ms) / 1000 < (uint32_t)in_range_sec)
            n++;
    }
    UNLOCK();
    return n;
}

int xst_chat(xst_chat_t *out, int max)
{
    int n = 0;
    LOCK();
    int total = s_chat_n < XST_CHAT_MAX ? s_chat_n : XST_CHAT_MAX;
    for (int k = 0; k < total && n < max; k++) {
        /* newest first: walk backwards from the last written slot */
        int i = (s_chat_n - 1 - k) % XST_CHAT_MAX;
        if (i < 0) i += XST_CHAT_MAX;
        if (!s_chat[i].from[0]) continue;
        out[n++] = s_chat[i];
    }
    UNLOCK();
    return n;
}

int xst_chat_find(const char *id, xst_chat_t *out)
{
    if (!id || !id[0]) return 0;
    int found = 0;
    LOCK();
    for (int i = 0; i < XST_CHAT_MAX; i++) {
        if (s_chat[i].from[0] && strcmp(s_chat[i].id, id) == 0) {
            *out = s_chat[i];
            found = 1;
            break;
        }
    }
    UNLOCK();
    return found;
}

int xst_stats_series(int view, uint16_t *dev, uint16_t *rx, uint16_t *tx,
                     int max)
{
    uint32_t ep = xst_epoch_now();
    if (!ep) return 0;
    int np = 0;
    LOCK();
    if (view == 0) {
        /* Last 24 ten-minute buckets: four hours. */
        uint32_t slot = ep / 600;
        np = 24 > max ? max : 24;
        for (int k = 0; k < np; k++) {
            uint32_t sl = slot - (np - 1 - k);
            int i = (int)(sl % XST_SB10_N);
            bool live = s_sb10_id[i] == sl;
            dev[k] = live ? s_sb10[i].dev : 0;
            rx[k] = live ? (uint16_t)s_sb10[i].rx : 0;
            tx[k] = live ? (uint16_t)s_sb10[i].tx : 0;
        }
    } else if (view == 1) {
        /* Last 24 hours, each the sum of its six buckets. */
        uint32_t hour = ep / 3600;
        np = 24 > max ? max : 24;
        for (int k = 0; k < np; k++) {
            uint32_t hk = hour - (np - 1 - k);
            uint32_t rxs = 0, txs = 0;
            uint16_t dv = 0;
            for (int j = 0; j < 6; j++) {
                uint32_t sl = hk * 6 + j;
                int i = (int)(sl % XST_SB10_N);
                if (s_sb10_id[i] != sl) continue;
                rxs += s_sb10[i].rx;
                txs += s_sb10[i].tx;
                if (s_sb10[i].dev > dv) dv = s_sb10[i].dev;
            }
            dev[k] = dv;
            rx[k] = (uint16_t)(rxs > 65535 ? 65535 : rxs);
            tx[k] = (uint16_t)(txs > 65535 ? 65535 : txs);
        }
    } else {
        /* Last 30 days. */
        uint32_t dslot = (uint32_t)((int64_t)ep + s_tz_off) / 86400;
        np = 30 > max ? max : 30;
        for (int k = 0; k < np; k++) {
            uint32_t sl = dslot - (np - 1 - k);
            int i = (int)(sl % XST_SBDAY_N);
            bool live = s_sbday_id[i] == sl;
            dev[k] = live ? s_sbday[i].dev : 0;
            rx[k] = live ? (uint16_t)(s_sbday[i].rx > 65535 ? 65535
                                      : s_sbday[i].rx) : 0;
            tx[k] = live ? (uint16_t)(s_sbday[i].tx > 65535 ? 65535
                                      : s_sbday[i].tx) : 0;
        }
    }
    UNLOCK();
    return np;
}

/* 36.10: one ask per peer per absence. The last-heard side lives in the
 * devices ring; this adds only "when did we last ask". */
#define XST_ASK_MAX 8
static struct { char call[10]; uint32_t asked_ms; } s_ask[XST_ASK_MAX];

bool xst_catchup_due(const char *call, int absent_sec)
{
    if (!call || !call[0]) return false;
    if (s_call[0] && strcasecmp(call, s_call) == 0) return false;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    /* "Back in radio range, or just powered on" (36.10): during the first
     * minutes after our own boot everything looks freshly heard, yet the
     * hole in the archive is OURS -- so a young station treats every
     * archiver as worth one ask. The cooldown below still bounds it. */
    bool young = now < 15u * 60u * 1000u;

    bool absent = true;
    LOCK();
    for (int i = 0; i < XST_SEEN_MAX; i++) {
        if (s_seen[i].call[0] && strcasecmp(s_seen[i].call, call) == 0) {
            absent = (now - s_seen[i].last_ms) / 1000 >=
                     (uint32_t)absent_sec;
            break;
        }
    }
    UNLOCK();
    if (!absent && !young) return false;

    /* Asked recently? The cooldown is the same absence window. */
    int slot = -1, oldest = 0;
    for (int i = 0; i < XST_ASK_MAX; i++) {
        if (s_ask[i].call[0] && strcasecmp(s_ask[i].call, call) == 0) {
            if ((now - s_ask[i].asked_ms) / 1000 < (uint32_t)absent_sec)
                return false;
            slot = i;
            break;
        }
        if (!s_ask[i].call[0]) { if (slot < 0) slot = i; }
        else if (s_ask[i].asked_ms < s_ask[oldest].asked_ms) oldest = i;
    }
    if (slot < 0) slot = oldest;
    snprintf(s_ask[slot].call, sizeof s_ask[slot].call, "%s", call);
    s_ask[slot].asked_ms = now;
    return true;
}

float xst_est_distance_m(int rssi)
{
    if (!rssi) return -1.0f;
    return powf(10.0f, ((-40.0f - (float)rssi) / 27.0f));
}

/* ── Persistence: the whole rings, serialised (magic XST1) ──────────── */
#define STATS_MAGIC 0x31545358u   /* "XST1" */
typedef struct {
    uint32_t  magic;
    sbucket_t sb10[XST_SB10_N];
    uint32_t  sb10_id[XST_SB10_N];
    sbucket_t sbday[XST_SBDAY_N];
    uint32_t  sbday_id[XST_SBDAY_N];
} stats_blob_t;

void xst_stats_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    static stats_blob_t bl;      /* static: too big for a task stack */
    bool ok = fread(&bl, sizeof bl, 1, f) == 1 && bl.magic == STATS_MAGIC;
    fclose(f);
    if (!ok) return;
    LOCK();
    memcpy(s_sb10, bl.sb10, sizeof s_sb10);
    memcpy(s_sb10_id, bl.sb10_id, sizeof s_sb10_id);
    memcpy(s_sbday, bl.sbday, sizeof s_sbday);
    memcpy(s_sbday_id, bl.sbday_id, sizeof s_sbday_id);
    UNLOCK();
    ESP_LOGI(TAG, "stats reloaded from %s", path);
}

void xst_stats_save(const char *path)
{
    static stats_blob_t bl;
    bl.magic = STATS_MAGIC;
    LOCK();
    memcpy(bl.sb10, s_sb10, sizeof bl.sb10);
    memcpy(bl.sb10_id, s_sb10_id, sizeof bl.sb10_id);
    memcpy(bl.sbday, s_sbday, sizeof bl.sbday);
    memcpy(bl.sbday_id, s_sbday_id, sizeof bl.sbday_id);
    UNLOCK();
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(&bl, sizeof bl, 1, f);
    fclose(f);
}
