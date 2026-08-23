/* The Reticulum bearer -- see xprsrns.h for the contract. */

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "rns.h"
#include "rns_tcp.h"
#include "xprsrns.h"
#include "xprs_config.h"

static const char *TAG = "xprsrns";

/* TweetNaCl, by its real names (the macro header is not included here). */
extern int crypto_sign_ed25519_tweet_keypair(unsigned char *, unsigned char *);
extern void randombytes(unsigned char *, unsigned long long);

static rns_identity_t     s_id;
static uint8_t            s_wapp_name[RNS_NAME_HASH_LEN];
static uint8_t            s_wapp_dest[RNS_HASH_LEN];
static xprsrns_wire_cb_t  s_cb;
static bool               s_ready;
static SemaphoreHandle_t  s_tx_lock;
static int64_t            s_last_tx_us;
static int                s_pace_ms = 1100;

static uint32_t s_rx, s_tx, s_paced, s_other;

/* ── Identity: minted once, kept in NVS ─────────────────────────────────── */

static bool identity_load(void)
{
    nvs_handle_t h;
    if (nvs_open("xprsrns", NVS_READWRITE, &h) != ESP_OK) return false;
    uint8_t prv[RNS_PRV_LEN], pub[RNS_PUB_LEN];
    size_t n = sizeof prv, m = sizeof pub;
    esp_err_t a = nvs_get_blob(h, "prv", prv, &n);
    esp_err_t b = nvs_get_blob(h, "pub", pub, &m);
    if (a != ESP_OK || b != ESP_OK || n != sizeof prv || m != sizeof pub) {
        /* First boot: mint. TweetNaCl cannot derive the Ed25519 public half
         * from a stored seed, so the public key is persisted whole beside
         * the private -- the same shape rns_identity_init() documents. */
        uint8_t ed_pk[32], ed_sk[64];
        crypto_sign_ed25519_tweet_keypair(ed_pk, ed_sk);
        randombytes(prv, RNS_KEY_HALF);          /* x25519 scalar */
        memcpy(prv + RNS_KEY_HALF, ed_sk, 32);   /* ed25519 seed  */
        rns_x25519_base(prv, pub);
        memcpy(pub + RNS_KEY_HALF, ed_pk, 32);
        if (nvs_set_blob(h, "prv", prv, sizeof prv) != ESP_OK ||
            nvs_set_blob(h, "pub", pub, sizeof pub) != ESP_OK) {
            nvs_close(h);
            return false;
        }
        nvs_commit(h);
        ESP_LOGI(TAG, "minted a new RNS identity");
    }
    nvs_close(h);
    rns_identity_init(prv, pub, &s_id);
    return true;
}

/* ── Inbound: hub frames -> XPRS wires ──────────────────────────────────── */

static void on_frame(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    if (!frame && len == 0) {
        /* Freshly connected. Say we exist: the peer cannot address a station
         * it has never heard, and XPRS's ingest learns the wapp lane from
         * exactly this announce shape. */
        char hello[8];
        int hn = snprintf(hello, sizeof hello, "%cxprs", 4);
        (void)hn;
        static uint8_t pkt[RNS_MTU + 64];
        /* The build's buffers are shared statics: every builder holds the
         * tx lock, this connect-time hello included. */
        if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2500)) != pdTRUE) return;
        int n = rns_announce_build(&s_id, s_wapp_name,
                                   (const uint8_t *)hello, 5,
                                   (uint64_t)time(NULL), pkt, sizeof pkt);
        bool ok = n > 0 && rns_tcp_send(pkt, (size_t)n);
        xSemaphoreGive(s_tx_lock);
        if (n > 0) ESP_LOGI(TAG, "hello announce: %dB, sent=%d", n, (int)ok);
        else       ESP_LOGE(TAG, "hello announce did not build (%d)", n);
        return;
    }

    rns_packet_t p;
    if (!rns_packet_parse(frame, len, &p)) return;
    if (p.packet_type != RNS_PACKET_ANNOUNCE) { s_other++; return; }

    rns_announce_t a;
    if (!rns_announce_parse(&p, &a)) { s_other++; return; }
    if (memcmp(a.name_hash, s_wapp_name, RNS_NAME_HASH_LEN) != 0) {
        s_other++;                       /* someone else's app -- not ours */
        return;
    }
    if (memcmp(a.dest, s_wapp_dest, RNS_HASH_LEN) == 0) return; /* own echo */

    /* app_data = [tagLen][tag][payload]; only tag "xprs" is ours. */
    if (a.app_len < 2) return;
    uint8_t tl = a.app_data[0];
    if (tl + 1u > a.app_len) return;
    if (tl != 4 || memcmp(a.app_data + 1, "xprs", 4) != 0) { s_other++; return; }
    const uint8_t *wire = a.app_data + 1 + tl;
    int wl = (int)(a.app_len - 1 - tl);
    if (wl <= 0) return;

    s_rx++;
    if (s_cb) s_cb((const char *)wire, wl);
}

/* ── Outbound ───────────────────────────────────────────────────────────── */

bool xprsrns_send(const char *wire, int len)
{
    if (!s_ready || !rns_tcp_is_up() || len <= 0) return false;
    if (len > RNS_MTU - 5 - 100) return false;   /* announce overhead */

    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2500)) != pdTRUE) {
        s_paced++;
        return false;
    }
    /* Pace inside the lock, so a burst serialises instead of interleaving.
     * The replay path already spaces packets 1.5 s apart; this floor only
     * matters when two tasks air at once. */
    int64_t now = esp_timer_get_time();
    int64_t gap_us = (int64_t)s_pace_ms * 1000;
    if (now - s_last_tx_us < gap_us) {
        vTaskDelay(pdMS_TO_TICKS((uint32_t)((gap_us - (now - s_last_tx_us)) / 1000) + 1));
    }

    static uint8_t app[RNS_MTU];
    int an = 0;
    app[an++] = 4;
    memcpy(app + an, "xprs", 4); an += 4;
    memcpy(app + an, wire, (size_t)len); an += len;

    static uint8_t pkt[RNS_MTU + 64];
    int n = rns_announce_build(&s_id, s_wapp_name, app, (size_t)an,
                               (uint64_t)time(NULL), pkt, sizeof pkt);
    bool ok = n > 0 && rns_tcp_send(pkt, (size_t)n);
    if (!ok) ESP_LOGW(TAG, "send failed: build=%d up=%d", n, (int)rns_tcp_is_up());
    if (ok) s_tx++;
    s_last_tx_us = esp_timer_get_time();
    xSemaphoreGive(s_tx_lock);
    return ok;
}

bool xprsrns_is_up(void) { return s_ready && rns_tcp_is_up(); }

void xprsrns_stats(uint32_t *rx, uint32_t *tx, uint32_t *paced, uint32_t *other)
{
    if (rx) *rx = s_rx;
    if (tx) *tx = s_tx;
    if (paced) *paced = s_paced;
    if (other) *other = s_other;
}

/* ── Bring-up ───────────────────────────────────────────────────────────── */

void xprsrns_init(xprsrns_wire_cb_t cb)
{
    const char *hub = xcfg_get("rns_hub", "");
    if (!hub || !hub[0]) {
        ESP_LOGI(TAG, "idle: no rns_hub configured");
        return;
    }
    s_pace_ms = atoi(xcfg_get("rns_pace_ms", "1100"));
    if (s_pace_ms < 100) s_pace_ms = 100;

    if (!identity_load()) {
        ESP_LOGE(TAG, "no identity -- bearer stays down");
        return;
    }
    const char *aspects[] = { "wapp" };
    rns_name_hash("xprs", aspects, 1, s_wapp_name);
    rns_destination_hash(s_wapp_name, s_id.hash, s_wapp_dest);

    s_cb = cb;
    s_tx_lock = xSemaphoreCreateMutex();

    /* "host" or "host:port". */
    char host[96];
    snprintf(host, sizeof host, "%s", hub);
    uint16_t port = RNS_TCP_DEFAULT_PORT;
    char *colon = strrchr(host, ':');
    if (colon && colon[1]) { *colon = 0; port = (uint16_t)atoi(colon + 1); }

    rns_tcp_set_rx_cb(on_frame, NULL);
    rns_tcp_add_hub(host, port);
    if (rns_tcp_start(NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "uplink did not start");
        return;
    }
    s_ready = true;
    ESP_LOGI(TAG, "up: uplink %s:%u, dest %02x%02x%02x%02x..",
             host, (unsigned)port,
             s_wapp_dest[0], s_wapp_dest[1], s_wapp_dest[2], s_wapp_dest[3]);
}
