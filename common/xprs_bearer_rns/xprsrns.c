/* The Reticulum bearer -- see xprsrns.h for the contract. */

#include <string.h>
#include <strings.h>   /* strcasecmp */
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
#include "xprs.h"      /* xprs_parse/xprs_get_str: the f: a peer answers to */
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

/* ── Who we can talk back to ────────────────────────────────────────────── */

/* An announce introduces an identity; that is its whole job (rns.h). What it
 * leaves behind is exactly what is needed to answer one: the destination to
 * address, and the public half to encrypt to. Keeping them is what turns this
 * bearer from a loudspeaker into something a station can hold a conversation
 * with.
 *
 * Keyed by CALLSIGN, because that is what an XPRS wire addresses with `d:`.
 * The binding comes from the announce's own payload -- a station's wires carry
 * `f:<callsign>` -- so it is learned from traffic we already verified, never
 * asserted by anyone.
 *
 * Small and in RAM on purpose at this stage: a board that needs to address
 * hundreds of stations needs the table on its card, and that belongs with the
 * storage work rather than here. Oldest-heard is evicted first. */
#define XRNS_PEERS 16

typedef struct {
    char     call[16];
    uint8_t  dest[RNS_HASH_LEN];
    uint8_t  xpub[RNS_KEY_HALF];
    uint32_t heard_s;
} xrns_peer_t;

static xrns_peer_t s_peers[XRNS_PEERS];
static uint32_t s_addressed_tx, s_addressed_rx, s_no_peer;

static void peer_learn(const char *call, const uint8_t dest[RNS_HASH_LEN],
                       const uint8_t pub[RNS_PUB_LEN])
{
    if (!call || !call[0]) return;
    uint32_t now = (uint32_t)time(NULL);
    int slot = -1;
    uint32_t oldest = 0xFFFFFFFFu;
    for (int i = 0; i < XRNS_PEERS; i++) {
        if (strcasecmp(s_peers[i].call, call) == 0) { slot = i; break; }
        if (!s_peers[i].call[0]) { slot = i; oldest = 0; continue; }
        if (oldest && s_peers[i].heard_s < oldest) { oldest = s_peers[i].heard_s; slot = i; }
    }
    if (slot < 0) return;
    snprintf(s_peers[slot].call, sizeof s_peers[slot].call, "%s", call);
    memcpy(s_peers[slot].dest, dest, RNS_HASH_LEN);
    /* pub is x25519_pub || ed25519_pub; only the first half encrypts. */
    memcpy(s_peers[slot].xpub, pub, RNS_KEY_HALF);
    s_peers[slot].heard_s = now;
}

static const xrns_peer_t *peer_find(const char *call)
{
    if (!call || !call[0]) return NULL;
    for (int i = 0; i < XRNS_PEERS; i++)
        if (strcasecmp(s_peers[i].call, call) == 0) return &s_peers[i];
    return NULL;
}

bool xprsrns_can_address(const char *call) { return peer_find(call) != NULL; }

int xprsrns_peer_count(void)
{
    int n = 0;
    for (int i = 0; i < XRNS_PEERS; i++) if (s_peers[i].call[0]) n++;
    return n;
}

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

    /* Addressed to US: somebody answered rather than announced.
     *
     * Cheap first, as everywhere on this path: the destination is a 16-byte
     * compare, and only a packet that is actually ours is worth a curve
     * multiplication. There is no signature to check here and none is needed
     * -- the wire inside carries its own XPRS signature (section 9.1), which
     * is what the station verifies before believing any of it. What the
     * decryption proves is narrower and still worth having: it was encrypted
     * to this destination's key, so it was not readable by the hubs that
     * carried it. */
    if (p.packet_type == RNS_PACKET_DATA &&
        p.dest_type == RNS_DEST_SINGLE &&
        memcmp(p.dest, s_wapp_dest, RNS_HASH_LEN) == 0) {
        static uint8_t plain[RNS_MTU];
        int pn = rns_decrypt_from(s_id.prv, s_wapp_dest, p.data, p.data_len,
                                  plain, sizeof plain - 1);
        if (pn <= 0) { s_other++; return; }   /* not for us, or forged */
        plain[pn] = 0;
        s_addressed_rx++;
        s_rx++;
        if (s_cb) s_cb((const char *)plain, pn);
        return;
    }

    if (p.packet_type != RNS_PACKET_ANNOUNCE) { s_other++; return; }

    /* Read it, but do not believe it yet.
     *
     * An Ed25519 verify is tweetnacl in portable C on a chip with no
     * acceleration for it -- the most expensive thing this station does per
     * packet by a wide margin. Bridged to the public Reticulum network
     * nearly every announce belongs to some other application, and verifying
     * the whole flood before asking whether any of it was ours is what took
     * two T-Decks into a reboot loop: this task outranked the one the
     * watchdog watches, so ninety seconds of announces became a panic in
     * tweetnacl with a healthy heap and no other symptom.
     *
     * So the cheap questions come first -- is it for our app, is it our own
     * echo, is it even carrying an XPRS packet -- and only what survives all
     * three is worth a curve multiplication. Everything below the verify is
     * still UNTRUSTED: nothing is acted on, counted as heard, or handed to
     * the station until the signature has been checked. */
    rns_announce_t a;
    if (!rns_announce_open(&p, &a)) { s_other++; return; }
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

    /* Now it is worth the maths. */
    if (!rns_announce_verify(&p, &a)) { s_other++; return; }

    s_rx++;

    /* Verified: now the announce is worth remembering. A station's own wires
     * carry `f:<callsign>`, so this is the callsign-to-destination binding
     * learned from traffic rather than asserted -- and it is what lets us
     * answer this station later on the addressed lane instead of shouting. */
    xprs_t xp;
    char from[16];
    if (xprs_parse((const char *)wire, wl, &xp) &&
        xprs_get_str(&xp, "f", from, sizeof from) && from[0]) {
        peer_learn(from, a.dest, a.pub);
    }

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

bool xprsrns_send_to(const char *callsign, const char *wire, int len)
{
    if (!s_ready || !rns_tcp_is_up() || len <= 0) return false;

    const xrns_peer_t *pr = peer_find(callsign);
    if (!pr) { s_no_peer++; return false; }

    /* The ciphertext carries an ephemeral public key, an IV and a MAC on top
     * of the wire, and the packet carries a header on top of that. */
    if (len > RNS_MTU - RNS_ENC_OVERHEAD - 32) return false;

    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2500)) != pdTRUE) {
        s_paced++;
        return false;
    }

    static uint8_t cipher[RNS_MTU];
    /* The salt is the DESTINATION's hash, not the identity's -- theirs, not
     * ours, because this is being encrypted to them. */
    int cn = rns_encrypt_to(pr->xpub, pr->dest, (const uint8_t *)wire,
                            (size_t)len, NULL, NULL, cipher, sizeof cipher);
    bool ok = false;
    if (cn > 0) {
        rns_packet_t p;
        memset(&p, 0, sizeof p);
        p.header_type    = RNS_HEADER_1;
        p.transport_type = RNS_TRANSPORT_BROADCAST;  /* how it is FORWARDED */
        p.dest_type      = RNS_DEST_SINGLE;          /* who it is FOR */
        p.packet_type    = RNS_PACKET_DATA;
        memcpy(p.dest, pr->dest, RNS_HASH_LEN);
        p.data     = cipher;
        p.data_len = (size_t)cn;

        static uint8_t pkt[RNS_MTU + 64];
        int n = rns_packet_build(&p, pkt, sizeof pkt);
        ok = n > 0 && rns_tcp_send(pkt, (size_t)n);
        if (!ok) ESP_LOGW(TAG, "addressed send failed: build=%d up=%d",
                          n, (int)rns_tcp_is_up());
    }
    if (ok) s_addressed_tx++;
    /* Deliberately NOT paced like an announce. Pacing exists because hubs
     * police announce RATES per destination; an addressed packet is ordinary
     * traffic to one station and is bounded by that station's own budget
     * (XPRS 31.2) instead. */
    xSemaphoreGive(s_tx_lock);
    return ok;
}

bool xprsrns_is_up(void) { return s_ready && rns_tcp_is_up(); }

void xprsrns_addressed_stats(uint32_t *tx, uint32_t *rx, uint32_t *no_peer,
                             int *peers)
{
    if (tx) *tx = s_addressed_tx;
    if (rx) *rx = s_addressed_rx;
    if (no_peer) *no_peer = s_no_peer;
    if (peers) *peers = xprsrns_peer_count();
}

void xprsrns_stats(uint32_t *rx, uint32_t *tx, uint32_t *paced, uint32_t *other)
{
    if (rx) *rx = s_rx;
    if (tx) *tx = s_tx;
    if (paced) *paced = s_paced;
    if (other) *other = s_other;
}

/* ── Bring-up ───────────────────────────────────────────────────────────── */

/* Everything that is about THIS station rather than about the transport:
 * the identity, the destination it listens on, the pacing. Shared by both
 * entry points below. */
static bool xprsrns_setup(xprsrns_wire_cb_t cb)
{
    s_pace_ms = atoi(xcfg_get("rns_pace_ms", "1100"));
    if (s_pace_ms < 100) s_pace_ms = 100;

    if (!identity_load()) {
        ESP_LOGE(TAG, "no identity -- bearer stays down");
        return false;
    }
    const char *aspects[] = { "wapp" };
    rns_name_hash("xprs", aspects, 1, s_wapp_name);
    rns_destination_hash(s_wapp_name, s_id.hash, s_wapp_dest);

    s_cb = cb;
    if (!s_tx_lock) s_tx_lock = xSemaphoreCreateMutex();
    return true;
}

void xprsrns_attach(xprsrns_wire_cb_t cb)
{
    /*
     * For a board whose Reticulum transport is already running and whose own
     * code owns the hub's receive callback -- there is exactly one of those
     * slots, so a bearer that grabbed it would silently unhook whatever was
     * there. That board feeds frames in with xprsrns_feed() instead.
     *
     * The identity is this component's own, under its own NVS namespace, and
     * the destination is `xprs.wapp` -- the one the rest of the fleet
     * listens on. A station that speaks XPRS on a destination nobody else
     * subscribes to is not on the network, however connected it looks.
     */
    if (s_ready) return;
    if (!xprsrns_setup(cb)) return;
    s_ready = true;
    ESP_LOGI(TAG, "attached to a running uplink, dest %02x%02x%02x%02x..",
             s_wapp_dest[0], s_wapp_dest[1], s_wapp_dest[2], s_wapp_dest[3]);
}

void xprsrns_feed(const uint8_t *frame, size_t len)
{
    if (s_ready && frame && len) on_frame(frame, len, NULL);
}

void xprsrns_init(xprsrns_wire_cb_t cb)
{
    const char *hub = xcfg_get("rns_hub", "");
    if (!hub || !hub[0]) {
        ESP_LOGI(TAG, "idle: no rns_hub configured");
        return;
    }
    if (!xprsrns_setup(cb)) return;

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
