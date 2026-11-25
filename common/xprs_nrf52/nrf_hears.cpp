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

/* What a shared LoRa channel may be offered at all: worth LoRa by type
 * (lr_worth) and not kept to the room by its author (9.11.1). Everything
 * else that used to be decided here -- reached on Bluetooth already, a
 * transaction for nobody on LoRa, an identity again too soon, old news,
 * twice in half an hour -- is QoS.md's, decided by the bearer's gate
 * (xb_set_qos, fed by nhr_qos_hooks), the same rules on every station. */
bool nhr_to_lora(const char *wire, int len)
{
    return lr_worth(wire, len, false) &&
           xprs_reach_admits(xprs_scope_reach_wire(wire, len), XPRS_REACH_REGIONAL);
}

static bool q_here(const char *call)    { return nhr_on(call, false); }
static bool q_cheaper(const char *call) { return nhr_on(call, true); }
static uint32_t q_epoch(void)           { return nst_now(); }

void nhr_qos_hooks(xq_hooks_t *h)
{
    if (!h) return;
    memset(h, 0, sizeof *h);
    h->heard_here = q_here;
    h->reached_cheaper = q_cheaper;
    h->epoch_s = q_epoch;
}
