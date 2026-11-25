/* Who an nRF52 station hears, and what Bluetooth hands to LoRa
 * (nrf_hears.h). Moved from models/sensecap-t1000e/firmware/src/main.cpp,
 * behaviour unchanged but for the table's size (12 -> 16 rows: the P1-Pro
 * on a pole hears sixteen stations on Bluetooth alone). */
#include <Arduino.h>
#include <string.h>

#include "nrf_hears.h"
#include "nrf_station.h"

extern "C" {
#include "xprs.h"
#include "xprsbearer.h"
#include "lr_worth.h"
}

static struct { char call[10]; uint32_t t_ms, ble_ms, lora_ms; } s_hears[NHR_MAX];

uint64_t nhr_peer_of(const char *wire, int len, bool *direct)
{
    xprs_t p;
    char from[10] = "";
    if (direct) *direct = false;
    if (!xprs_parse(wire, len, &p) || !xprs_get_str(&p, "f", from, sizeof from) || !from[0]) return 0;
    if (direct) *direct = xprs_via_count(&p) == 0;
    uint64_t h = 1469598103934665603ULL;
    for (const char *c = from; *c; c++) { h ^= (uint8_t)*c; h *= 1099511628211ULL; }
    return h ? h : 1;
}

void nhr_touch(const char *wire, int len, bool ble)
{
    xprs_t p;
    char from[10] = "";
    if (!xprs_parse(wire, len, &p) || xprs_via_count(&p) != 0) return;
    if (!xprs_get_str(&p, "f", from, sizeof from) || !from[0] || strcmp(from, nst_call()) == 0) return;
    uint32_t now = millis();
    int slot = -1, oldest = 0;
    for (int i = 0; i < NHR_MAX; i++) {
        if (strcmp(s_hears[i].call, from) == 0 || !s_hears[i].call[0]) { slot = i; break; }
        if ((int32_t)(s_hears[i].t_ms - s_hears[oldest].t_ms) < 0) oldest = i;
    }
    if (slot < 0) slot = oldest;
    if (strcmp(s_hears[slot].call, from) != 0) s_hears[slot].ble_ms = s_hears[slot].lora_ms = 0;
    snprintf(s_hears[slot].call, sizeof s_hears[slot].call, "%s", from);
    s_hears[slot].t_ms = now;
    if (ble) s_hears[slot].ble_ms = now ? now : 1;
    else     s_hears[slot].lora_ms = now ? now : 1;
}

bool nhr_heard_directly(const char *call, uint32_t ms)
{
    uint32_t now = millis();
    for (int i = 0; i < NHR_MAX; i++)
        if (s_hears[i].call[0] && !strcmp(s_hears[i].call, call) && now - s_hears[i].t_ms <= ms)
            return true;
    return false;
}

bool nhr_on(const char *call, bool ble)
{
    uint32_t now = millis();
    for (int i = 0; i < NHR_MAX; i++) {
        uint32_t t = ble ? s_hears[i].ble_ms : s_hears[i].lora_ms;
        if (t && strcmp(s_hears[i].call, call) == 0 && now - t <= NHR_FRESH_MS) return true;
    }
    return false;
}

int nhr_count(void)
{
    uint32_t now = millis();
    int n = 0;
    for (int i = 0; i < NHR_MAX; i++)
        if (s_hears[i].call[0] && now - s_hears[i].t_ms <= NHR_FRESH_MS) n++;
    return n;
}

const char *nhr_find_ble(uint32_t ms, bool (*want)(const char *call))
{
    uint32_t now = millis();
    for (int i = 0; i < NHR_MAX; i++)
        if (s_hears[i].ble_ms && now - s_hears[i].ble_ms < ms && (!want || want(s_hears[i].call)))
            return s_hears[i].call;
    return NULL;
}

int nhr_render(char *out, int cap)
{
    uint32_t now = millis();
    bool used[NHR_MAX] = {false};
    int n = 0;
    if (cap > 0) out[0] = 0;
    for (;;) {
        int best = -1;
        for (int i = 0; i < NHR_MAX; i++) {
            if (used[i] || !s_hears[i].call[0] || now - s_hears[i].t_ms > NHR_FRESH_MS) continue;
            if (best < 0 || (int32_t)(s_hears[i].t_ms - s_hears[best].t_ms) > 0) best = i;
        }
        if (best < 0) break;
        used[best] = true;
        int w = snprintf(out + n, (size_t)(cap - n), "%s%s", n ? "," : " hears:", s_hears[best].call);
        if (w <= 0 || n + w >= cap) { out[n] = 0; break; }
        n += w;
    }
    return n;
}

#define LORA_FRESH_S      1800u
#define LORA_SENT_RING    24
#define LORA_SENT_MS      1800000UL
#define LORA_IDENT_MS     (3 * 3600000UL)
/* 32 authors: with 8, a bench of sixteen callsigns turned the ring over in
 * minutes and the P1-Pro re-aired the same identity 3-4 times in 13 minutes
 * (2026-11-25) -- 24 of its 27 LoRa frames. */
#define LORA_IDENT_N      32
static struct { char call[10]; uint32_t t_ms; } s_lora_ident[LORA_IDENT_N];
static struct { char id[XB_ID_LEN]; uint32_t t_ms; } s_lora_sent[LORA_SENT_RING];
static int      s_lora_sent_pos;
static uint32_t s_newest_ts, s_newest_ts_ms;

bool nhr_to_lora(const char *wire, int len)
{
    if (!lr_worth(wire, len, false) ||
        !xprs_reach_admits(xprs_scope_reach_wire(wire, len), XPRS_REACH_REGIONAL))
        return false;
    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return false;
    char d[16] = "", ts[24] = "", t[16] = "", f[10] = "";
    xprs_get_str(&p, "t", t, sizeof t);
    xprs_get_str(&p, "d", d, sizeof d);
    if (d[0] && nhr_on(d, true)) return false;
    if ((!strcmp(t, "command") || !strcmp(t, "result") || !strcmp(t, "request")) &&
        !(d[0] && nhr_on(d, false)))
        return false;
    if (!strcmp(t, "identity") && xprs_get_str(&p, "f", f, sizeof f)) {
        uint32_t ms = millis();
        int slot = 0;
        for (int i = 0; i < LORA_IDENT_N; i++) {
            if (!strcmp(s_lora_ident[i].call, f)) {
                if (ms - s_lora_ident[i].t_ms < LORA_IDENT_MS) return false;
                slot = i;
                break;
            }
            if ((int32_t)(s_lora_ident[i].t_ms - s_lora_ident[slot].t_ms) < 0) slot = i;
        }
        snprintf(s_lora_ident[slot].call, sizeof s_lora_ident[slot].call, "%s", f);
        s_lora_ident[slot].t_ms = ms ? ms : 1;
    }
    if (xprs_get_str(&p, "ts", ts, sizeof ts)) {
        uint32_t tv = nst_ts_to_epoch(ts), ms = millis();
        if (tv && (!s_newest_ts || (int32_t)(tv - s_newest_ts) > 0 ||
                   ms - s_newest_ts_ms > NHR_FRESH_MS)) {
            s_newest_ts = tv;
            s_newest_ts_ms = ms;
        }
        uint32_t now = nst_now();
        if (!now && s_newest_ts) now = s_newest_ts + (ms - s_newest_ts_ms) / 1000u;
        if (tv && now && tv + LORA_FRESH_S < now) return false;
    }
    char id[XB_ID_LEN];
    if (!xprs_id_of(wire, len, id)) return false;
    uint32_t ms = millis();
    for (int i = 0; i < LORA_SENT_RING; i++)
        if (s_lora_sent[i].t_ms && strcmp(s_lora_sent[i].id, id) == 0 &&
            ms - s_lora_sent[i].t_ms < LORA_SENT_MS)
            return false;
    snprintf(s_lora_sent[s_lora_sent_pos].id, XB_ID_LEN, "%s", id);
    s_lora_sent[s_lora_sent_pos].t_ms = ms ? ms : 1;
    s_lora_sent_pos = (s_lora_sent_pos + 1) % LORA_SENT_RING;
    return true;
}
