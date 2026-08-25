/*
 * Full Reticulum BLE5 node for the LilyGO T-Dongle-S3.
 *
 * RECEIVE: NimBLE extended scan; decode RNS announces (manufacturer 0xFFFF,
 *   marker 0x3E, subtype 0x55) and print the chat text.
 * TRANSMIT: a real RNS Identity (X25519 + Ed25519, persisted in NVS) with its
 *   own "aurora.chat" destination. It builds and Ed25519-SIGNS valid announces
 *   and airs them as BLE5 extended advertisements, so the phones accept and
 *   display them exactly like another phone.
 *
 * Crypto: TweetNaCl (Ed25519 sign + X25519 base-point + SHA-512) for the
 * identity/signature, mbedTLS SHA-256 for the RNS hashes. No app-layer secrets
 * leave the device; the BLE transport itself is unauthenticated (RNS provides
 * its own crypto), so no pairing is needed.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mbedtls/sha256.h"

#include "driver/gpio.h"

#include "esp_mac.h"
#include "tinynimble.h"

#include "model_init.h"
#include "st7735.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "fw_secrets.h"     /* gitignored; see fw_secrets.h.example */
#include "xprs_auth.h"
#include "xprs_config.h"
#include "xprs_health.h"
#include "xprs_diag.h"
#include "xapi_send.h"

/* What this board is supposed to have running. Names are literals and are
 * borrowed, not copied (xprs_health.h), and they are what an ESP_LOGE will
 * print at 3am, so they read as things rather than as symbols. */
#define XH_HTTP   "http api"
#define XH_BLE    "ble host"
#define XH_LAN    "lan bearer"
#define XH_NOW    "esp-now"
#define XH_RELAY  "relay task"
#define XH_CARD   "sd card"

/* What this board is documented to boot with, from the table in
 * docs/esp32.md. Measured 2026-08-21 with the hub link off: end of
 * app_main sits around 5 KB with the bearers just started and settles
 * near 14 KB once association finishes, so the floor is set below the
 * transient and above the failure. It exists to catch a step change --
 * a setting that stopped being applied -- not to police a few hundred
 * bytes of drift. Raise it when the board genuinely gets roomier. */
#define TDONGLE_HEAP_FLOOR 4000
#include "xprs_ota.h"
#include "xprs_station.h"
#include "xprs_ui_mini.h"
#include "tweetnacl.h"

/* APRS-IS iGate: WiFi STA + APRS-IS client (reused generic components). */
#include "wifi_bsp.h"
#include "esp_wifi.h"
#include "aprsis.h"

/* LAN presence: passive listener on the XPRS app UDP discovery broadcast. */
#include "lanwatch.h"

/* BLE street mesh (aurora docs/mesh.md): route beacon + DV table + SCF. */
#include <sys/stat.h>
#include <time.h>
#include "blemesh.h"
#include "sdcard.h"

/* XPRS (aurora docs/XPRS.md): the text wire format the whole device fleet
 * speaks now. This station reads it, answers pings, parks 1:1 mail and
 * relays with a via: path. */
#include "xprs.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "xprsindex.h"
#include "xprslan.h"
#include "xprsnow.h"
#include "xprsid.h"
#include "xprschan.h"
#include "rns_tcp.h"
#include "xgossip.h"
#include "xprssig.h"
#include "bech32.h"
#include <ctype.h>
#include "nostr_keys.h"
#include "esp_netif_sntp.h"

/* Provisioning defaults (WiFi creds + callsign). The real file is gitignored;
 * values are written to NVS on first boot and NVS is the source of truth after.
 * Builds fine without the file (creds then come only from NVS). */
#if __has_include("igate_secrets.h")
#include "igate_secrets.h"
#endif
#ifndef IGATE_WIFI_SSID
#define IGATE_WIFI_SSID ""
#endif
#ifndef IGATE_WIFI_PASSWORD
#define IGATE_WIFI_PASSWORD ""
#endif
#ifndef IGATE_CALLSIGN
#define IGATE_CALLSIGN ""
#endif

static const char *TAG = "rns_ble5";
static uint8_t s_own_addr_type;

#define COMPANY_LO 0xFF
#define COMPANY_HI 0xFF
#define MARKER     0x3E
#define SUBTYPE      0x55   /* Reticulum packet */
#define SUBTYPE_APRS 0x41   /* APRS broadcast parcel ('A') — plaintext */
#define SUBTYPE_MESH BLEMESH_SUBTYPE /* 0x4D street-mesh route beacon ('M') */
/* Hubs this station dials, in order, until one answers. Measured from a
 * domestic line rather than copied: wisco 113 ms, birdsnet 225 ms, inertia
 * 285 ms, sydney 287 ms — and rns.beleth.net, which was the default here and in
 * the Flutter app, does not answer at all. A station that knew only that one
 * was never on the network.
 *
 * -DRNS_HUB_HOST=\"host\" puts one in front of these, which is how a private
 * hub or a laptop running one is pointed at for a test. */
#ifndef RNS_HUB_ONLY
static const struct { const char *host; uint16_t port; } k_rns_hubs[] = {
    { "rns.wisco.network",   4242 },
    { "rns.birdsnet.com.br", 4242 },
    { "use.inertia.chat",    4242 },
    { "sydney.reticulum.au", 4242 },
};
#endif

/* How often this station says it is an indexer, and how many callsigns its
 * directory may list. 64 is a dongle's worth: an indexer archiving more than
 * that has outgrown this hardware. */
#define XPRS_SERVICE_EVERY_SEC 600
#define XPRS_SERVICE_FIRST_SEC  30
#define XPRS_DIR_MAX           32

/* One directory buffer for the whole firmware. Both users run on tasks that
 * take the index lock, and a second copy is 640 bytes this board does not have
 * — heap here is measured in single-digit kilobytes. */
static xprsidx_dir_entry_t s_dir[XPRS_DIR_MAX];

#define SUBTYPE_XPRS 0x58   /* XPRS text packet ('X') — docs/ble5.md §2 */

#define RNS_PKT_ANNOUNCE 0x01
#define DST_HASH_LEN     16
#define CALLSIGN_MAX     12   /* max callsign chars shown on the dashboard */
#define KEYSIZE          64
#define NAME_HASH_LEN    10
#define RANDOM_HASH_LEN  10
#define RATCHET_LEN      32
#define SIG_LEN          64

/* Bumped when a stored identity must be thrown away. 1 = the first generation
 * that announces with a real timestamp (see the rotation below). */
#define RNS_IDENTITY_VERSION 1

/* The Reticulum destination namespace. This was "aurora" while the phone and
 * desktop app announced "xprs", which meant the dongle was in a namespace of
 * its own: its announces were structurally fine, routed by the hubs, and then
 * discarded by the app because they matched none of its service tuples. A
 * dongle has therefore never appeared as one of our devices in the mesh graph.
 * Both sides say "xprs" now, which is what finally puts them on the same
 * overlay. A flashed dongle has no override for this -- it is compile-time, so
 * every unit has to be reflashed. */
#define APP_NAME "xprs"
#define ASPECT   "chat"
#define FULL_NAME "xprs.chat"        /* expand_name(None, app, aspect) */

/* ---- our identity (RNS) ------------------------------------------------- */
static uint8_t s_ed_sk[64];   /* Ed25519 secret: seed(32) || pub(32) */
static uint8_t s_ed_pk[32];
static uint8_t s_x_sk[32];     /* X25519 scalar */
static uint8_t s_x_pk[32];
static uint8_t s_pubkey[KEYSIZE];     /* x25519_pub(32) || ed25519_pub(32) */
static uint8_t s_id_hash[16];
static uint8_t s_name_hash[NAME_HASH_LEN];

/* The XPRS signing key is the station's NOSTR key, held by xprs_nostr:
 * secp256k1, x-only public half, bech32 npub, persisted in NVS, and the
 * callsign already derived from it the way §3 says. This firmware had none of
 * that only because it never linked the component — the key, the npub and the
 * derivation were all sitting there. */
static bool s_xprs_can_sign;
static const nostr_keys_t *xprs_keys(void) { return nostr_keys_get(); }
static uint8_t s_dest_hash[DST_HASH_LEN];

/* Repeater: re-air a received RNS packet so out-of-range nodes still get it. */
static void maybe_relay(const uint8_t *pkt, int len, int rssi);
/* UI hook (metadata only; defined in the UI section, no-op until UI is wired). */
static void ui_log_packet(const uint8_t *dest_hash, int hops, int rssi,
                          const char *name);
static uint32_t now_sec(void);
/* APRS (subtype 0x41) is plaintext broadcast chat — relay it (not shown; the
 * display is a reach dashboard now, never message content). */
static void handle_aprs(const uint8_t *payload, int len, int rssi);
/* iGate: remember a callsign heard over BLE5 (for the APRS-IS filter). */
#define XPRS_BEARER_BLE 1
#define XPRS_BEARER_LAN 2
#define XPRS_BEARER_NOW 3
static void igate_heard_add(const char *call, uint8_t bearer);
static int  xprs_hears_render(uint8_t bearer, char *out, int cap, int *total);
static bool xprs_verify_sig(const xprs_t *p, const uint8_t pub[32]);
static const uint8_t *xprs_peer_key(const char *call);
/* Gossip, defined with the serving path below but fed from the receive one. */
static xgossip_t *s_goss;
static void xprs_gossip_heard(const xprs_t *p, uint8_t bearer);
static int  xprs_peer_key_count(void);
static void xprs_identity_heard(const xprs_t *p);
static void xprs_hist_accept(const char *wire, int len, const xprs_t *p,
                             uint8_t bearer);
static void xprs_hist_pump(void);
static void start_scan(void);
/* Street mesh: beacon TX + ingest + store-and-forward delivery. */
static void handle_mesh(const uint8_t *payload, int len, int rssi);
static void mesh_beacon_air(void);
static void mesh_deliver_pending(const char *target);
static volatile bool s_mesh_dirty;      /* topology changed -> beacon early */
static bool s_mesh_up;
static char s_aprs_call[10];            /* tentative; defined with iGate below */
/* XPRS station: ingest (both 0x58 and text-form 0x41), pong, presence beacon. */
static void handle_xprs(const uint8_t *payload, int len, int rssi, uint8_t subtype);
static void xprs_air(const char *wire, int len, uint8_t subtype);

/* The card-backed index (XPRS.md §36) and the LAN bearer (docs/lan.md). Both
 * are the components the legacy T-Dongle firmware proved; this firmware is the
 * one that can also put a packet on the BLE5 air. */
static xprsidx_t *s_xprs_index;
static void xprs_beacon_air(void);
static int  xprs_sign_wire(char *wire, int len, int cap);
static uint32_t s_boot_epoch;           /* NVS boot counter (XPRS.md §10.7) */
/* lifetime: (XPRS.md §10.5) — cumulative service seconds across every restart,
 * accumulated in NVS. s_life_base is the total saved by PREVIOUS runs; the
 * current figure is s_life_base + now_sec(). Saved every 15 min from
 * relay_task, so a power pull costs at most that much history. */
static uint32_t s_life_base;
#define LIFE_SAVE_SEC 900

/* TweetNaCl entropy hook: provided by xprs_rns (rns_entropy.c), which is
 * also where tweetnacl itself now lives -- one copy, every consumer. */
extern void randombytes(unsigned char *p, unsigned long long n);

static void sha256(const uint8_t *in, size_t n, uint8_t *out32)
{
    mbedtls_sha256(in, n, out32, 0);
}

static void hexn(const uint8_t *b, int n, char *out)
{
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < n; i++) {
        out[i * 2] = h[b[i] >> 4];
        out[i * 2 + 1] = h[b[i] & 0xf];
    }
    out[n * 2] = 0;
}

/* ---- receive path ------------------------------------------------------- */
static char s_last[160];

static void handle_rns_packet(const uint8_t *pkt, int len, int rssi)
{
    if (len < 2 + DST_HASH_LEN + 1) return;
    uint8_t flags = pkt[0];
    if ((flags & 0x03) != RNS_PKT_ANNOUNCE) return;
    uint8_t htype = (flags >> 6) & 0x01;
    uint8_t ctxflag = (flags >> 5) & 0x01;

    int dataoff = htype ? (2 + DST_HASH_LEN + DST_HASH_LEN + 1)
                        : (2 + DST_HASH_LEN + 1);
    const uint8_t *dhash = htype ? pkt + 2 + DST_HASH_LEN : pkt + 2;
    if (len <= dataoff) return;

    const uint8_t *ad = pkt + dataoff;
    int adlen = len - dataoff;
    int appoff = KEYSIZE + NAME_HASH_LEN + RANDOM_HASH_LEN +
                 (ctxflag ? RATCHET_LEN : 0) + SIG_LEN;
    if (adlen <= appoff) return;

    const uint8_t *app = ad + appoff;
    int applen = adlen - appoff;
    if (applen > 120) applen = 120;

    char text[121];
    memcpy(text, app, applen);
    text[applen] = 0;
    for (int i = 0; i < applen; i++)
        if (text[i] < 32 || text[i] > 126) text[i] = '.';

    /* Ignore our own announces (we hear our own broadcasts). */
    if (memcmp(dhash, s_dest_hash, DST_HASH_LEN) == 0) return;

    /* An announce's dest IS the announcing node, and its plaintext app_data is
     * the device callsign — the right signal for the "in range" dashboard. Feed
     * it every time so the peer stays fresh (the serial line below is deduped). */
    ui_log_packet(dhash, pkt[1], rssi, text);

    char dh[2 * 4 + 1];
    hexn(dhash, 4, dh);
    char line[160];
    snprintf(line, sizeof(line), "%s|%s", dh, text);
    if (strcmp(line, s_last) == 0) return;
    strncpy(s_last, line, sizeof(s_last) - 1);
    ESP_LOGI(TAG, "RX announce  dest=%s..  rssi=%d  app=\"%s\"", dh, rssi, text);
}

/* Scan liveness: vendor controllers can silently stop delivering results
 * (the phones needed the same watchdog). Stamped on EVERY disc event. */
static volatile uint32_t s_last_disc;
static volatile uint32_t s_disc_count;

static volatile int s_rssi_min = 0, s_rssi_max = -127;
static volatile uint32_t s_rssi_sum, s_rssi_n;

/*
 * THE RADIO CALLBACK RUNS ON THE CONTROLLER'S OWN TASK, SO IT ONLY COPIES.
 *
 * tinynimble delivers reports straight out of the VHCI callback, which the BT
 * controller calls on its task, not ours. Everything reached from here --
 * Reticulum decode, APRS, mesh routing, XPRS signature verification -- is far
 * too heavy for that stack. The T-Deck learned this the expensive way: doing
 * the work inline overflowed btController and reboot-looped the station nine
 * times in twenty-six seconds. Copy, post, return.
 */
typedef struct {
    uint8_t data[254];
    uint8_t len;
    int8_t  rssi;
} ad_item_t;

#define AD_Q_DEPTH 6
static QueueHandle_t s_ad_q;
static uint32_t      s_ad_dropped;

static void handle_ad(const uint8_t *p, int n, int rssi);

/* Controller context: nothing here may block or allocate. */
static void tn_report(const tn_adv_report_t *r, void *ctx)
{
    (void)ctx;
    if (!r || r->data_len == 0) return;
    s_last_disc = now_sec();
    s_disc_count++;
    int rssi = r->rssi;
    if (rssi < s_rssi_min) s_rssi_min = rssi;
    if (rssi > s_rssi_max) s_rssi_max = rssi;
    s_rssi_sum += (uint32_t)(-rssi);
    s_rssi_n++;

    if (!s_ad_q) return;
    ad_item_t it;
    int n = r->data_len > (uint8_t)sizeof it.data
          ? (int)sizeof it.data : (int)r->data_len;
    memcpy(it.data, r->data, (size_t)n);
    it.len  = (uint8_t)n;
    it.rssi = (int8_t)rssi;
    if (xQueueSend(s_ad_q, &it, 0) != pdTRUE) {
        if ((++s_ad_dropped % 200) == 1)
            ESP_LOGW(TAG, "BLE receive queue full, %u dropped",
                     (unsigned)s_ad_dropped);
    }
}

/* Our task, our stack -- this is where the AD is actually understood. */
static void ble_rx_task(void *arg)
{
    (void)arg;
    ad_item_t it;
    for (;;)
        if (xQueueReceive(s_ad_q, &it, portMAX_DELAY) == pdTRUE)
            handle_ad(it.data, it.len, it.rssi);
}

static void handle_ad(const uint8_t *p, int n, int rssi)
{
    for (int i = 0; i + 2 <= n;) {
        int adlen = p[i];
        if (adlen == 0 || i + 1 + adlen > n) break;
        if (p[i + 1] == 0xFF && adlen >= 1 + 2) {
            const uint8_t *m = &p[i + 2];
            int mlen = adlen - 1;
            if (mlen >= 4 && m[0] == COMPANY_LO && m[1] == COMPANY_HI &&
                m[2] == MARKER) {
                if (m[3] == SUBTYPE) {            /* Reticulum (encrypted) */
                    handle_rns_packet(&m[4], mlen - 4, rssi);
                    maybe_relay(&m[4], mlen - 4, rssi);
                } else if (m[3] == SUBTYPE_APRS) { /* APRS (plaintext) */
                    handle_aprs(&m[4], mlen - 4, rssi);
                } else if (m[3] == SUBTYPE_MESH) { /* street-mesh route beacon */
                    handle_mesh(&m[4], mlen - 4, rssi);
                } else if (m[3] == SUBTYPE_XPRS) { /* XPRS text */
                    handle_xprs(&m[4], mlen - 4, rssi, SUBTYPE_XPRS);
                }
            }
        }
        i += 1 + adlen;
    }
}

/* ---- identity ----------------------------------------------------------- */
static void identity_init(void)
{
    nvs_handle_t h;
    bool have = false;
    if (nvs_open("rns", NVS_READWRITE, &h) == ESP_OK) {
        size_t a = sizeof(s_ed_sk), b = sizeof(s_x_sk);
        /* Identities made before the random_hash fix are unusable on the public
         * network and cannot be repaired. Those announces carried five RANDOM
         * bytes where RNS reads a timestamp, so transports recorded this
         * destination with an announce time that was usually far in the future,
         * and every honest announce since looks older than what they hold — so
         * they drop it, for as long as they keep the entry. Proven on the
         * hardware: the stored identity is never relayed, and a fresh one
         * generated by the same firmware, on the same hub, is relayed at once.
         *
         * So a station carrying a pre-fix identity rotates it, once. The marker
         * is what makes it once rather than every boot. */
        uint32_t idver = 0;
        (void)nvs_get_u32(h, "idver", &idver);
#ifndef RNS_NEW_ID          /* -DRNS_NEW_ID: a fresh identity every boot, which
                             * is how the above was isolated */
        if (idver >= RNS_IDENTITY_VERSION &&
            nvs_get_blob(h, "ed_sk", s_ed_sk, &a) == ESP_OK && a == sizeof(s_ed_sk) &&
            nvs_get_blob(h, "x_sk", s_x_sk, &b) == ESP_OK && b == sizeof(s_x_sk)) {
            have = true;
        }
#else
        (void)a; (void)b;
#endif
        if (!have && idver < RNS_IDENTITY_VERSION && idver != 0) {
            ESP_LOGW(TAG, "rotating the Reticulum identity: the stored one was "
                          "announced with malformed timestamps and the network "
                          "will not carry it");
        }
        if (!have) {
            crypto_sign_keypair(s_ed_pk, s_ed_sk);   /* random Ed25519 */
            randombytes(s_x_sk, sizeof(s_x_sk));     /* X25519 scalar */
            nvs_set_blob(h, "ed_sk", s_ed_sk, sizeof(s_ed_sk));
            nvs_set_blob(h, "x_sk", s_x_sk, sizeof(s_x_sk));
            nvs_set_u32(h, "idver", RNS_IDENTITY_VERSION);
            nvs_commit(h);
            ESP_LOGI(TAG, "generated new identity");
        }
        nvs_close(h);
    } else {
        crypto_sign_keypair(s_ed_pk, s_ed_sk);
        randombytes(s_x_sk, sizeof(s_x_sk));
    }
    /* The station's NOSTR key: loaded from NVS or generated by the component
     * that owns it. Its callsign, npub and x-only public half all come from
     * here, so signing, §3's derivation and §9.3's identity packet agree by
     * construction rather than by three separate copies happening to match. */
    if (nostr_keys_init() == ESP_OK && nostr_keys_available()) {
        s_xprs_can_sign = true;
        ESP_LOGI(TAG, "XPRS identity %s (%s)", nostr_keys_get_callsign(),
                 nostr_keys_get_npub());
    } else {
        ESP_LOGW(TAG, "no NOSTR key — packets go out unsigned, and a receiver "
                      "may not treat f: as established");
    }

    memcpy(s_ed_pk, s_ed_sk + 32, 32);               /* pub = sk[32:64] */
    crypto_scalarmult_base(s_x_pk, s_x_sk);          /* X25519 pubkey */

    memcpy(s_pubkey, s_x_pk, 32);
    memcpy(s_pubkey + 32, s_ed_pk, 32);
    uint8_t h32[32];
    sha256(s_pubkey, KEYSIZE, h32);
    memcpy(s_id_hash, h32, DST_HASH_LEN);
    sha256((const uint8_t *)FULL_NAME, strlen(FULL_NAME), h32);
    memcpy(s_name_hash, h32, NAME_HASH_LEN);
    uint8_t hm[NAME_HASH_LEN + DST_HASH_LEN];
    memcpy(hm, s_name_hash, NAME_HASH_LEN);
    memcpy(hm + NAME_HASH_LEN, s_id_hash, DST_HASH_LEN);
    sha256(hm, sizeof(hm), h32);
    memcpy(s_dest_hash, h32, DST_HASH_LEN);

    char dh[2 * DST_HASH_LEN + 1], ih[2 * DST_HASH_LEN + 1];
    hexn(s_dest_hash, DST_HASH_LEN, dh);
    hexn(s_id_hash, DST_HASH_LEN, ih);
    ESP_LOGI(TAG, "identity=%s dest(%s)=%s", ih, FULL_NAME, dh);
}

/* ---- transmit (signed announce as a BLE5 extended advertisement) -------- */
static bool s_adv_configured = false;

/* Is the NimBLE host running?
 *
 * Section 23.7 takes it down for the length of a working-channel exchange, and
 * the reason is measured rather than defensive: with the BLE controller up, a
 * WiFi station that is not associated receives NOTHING (esp32/espnow_probe, and
 * the table in docs/espnow.md). Moving to a working channel means leaving the
 * access point, so the two cannot both happen.
 *
 * While it is false, every path that would touch NimBLE must return instead of
 * calling into a host that is not there. */
static volatile bool s_ble_up;
/* Section 23.7 asks for these through the ops in k_chan_ops, which is composed
 * long before the NimBLE glue is defined. */
static void ble_stack_down(void);
static void ble_stack_up(void);
static void xc_bluetooth(bool on) { if (on) ble_stack_up(); else ble_stack_down(); }

/* Big buffers kept static (off-stack); announce() is only ever called from the
 * single announce task, so this is safe. */
static uint8_t s_signed[DST_HASH_LEN + KEYSIZE + NAME_HASH_LEN + RANDOM_HASH_LEN + 128];
static uint8_t s_sm[64 + sizeof(s_signed)];
static uint8_t s_ad[256];

/* Air a complete BLE AD buffer on ext-adv instance 0 (configure on first use,
 * then stop+set_data+start). Used by both our own announce and the repeater. */
static void air_raw_ad(const uint8_t *ad, int n)
{
    if (!s_ble_up) return;      /* away on a working channel; see s_ble_up */
    if (!s_adv_configured) {
        /* Configure ONCE and keep the set: re-creating it makes the controller
         * rotate its random address, which fragments every peer's address
         * book. Same rule xprs_bearer_ble follows. */
        tn_adv_cfg_t cfg = {
            .handle        = 0,
            .props         = 0,      /* non-connectable, non-scannable */
            .itvl_min      = 0x100,  /* 160 ms */
            .itvl_max      = 0x100,
            .chan_map      = 0x07,
            .own_addr_type = s_own_addr_type,
            .tx_power      = 127,
            .primary_phy   = TN_PHY_1M,
            .secondary_phy = TN_PHY_1M,
            .sid           = 0,
        };
        esp_err_t err = tn_adv_configure(&cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "tn_adv_configure: %s", esp_err_to_name(err));
            return;
        }
        s_adv_configured = true;
    }
    /* tn_adv_set_data does the stop -> set -> start itself. */
    esp_err_t err = tn_adv_set_data(ad, (size_t)n);
    if (err != ESP_OK)
        ESP_LOGE(TAG, "tn_adv_set_data: %s", esp_err_to_name(err));
}

/* A fresh connection announces immediately; after that, at most this often.
 *
 * One hour, because that is RNS's announce_rate_target. A destination that
 * announces faster is penalised by the transport and its announces are dropped
 * — not rejected visibly, just not propagated, which is indistinguishable from
 * a broken signature until you replay the identical bytes from another machine
 * and watch them vanish too. This station spent hours announcing every 26
 * seconds and burned its reputation on the hub it was using; a fresh identity
 * carrying its exact name_hash and app data was relayed immediately. */
#define HUB_ANNOUNCE_MIN_SEC 3600
static uint32_t s_last_hub_announce;
static volatile bool s_hub_announce_force;

static void announce(const char *app, int applen)
{
    /* random_hash is NOT ten random bytes: RNS reads it as 5 random bytes plus
     * 5 big-endian seconds since the epoch, and hubs judge an announce's
     * freshness by that timestamp. Filling all ten with random gave every
     * announce a nonsense time, and the public hubs accepted our connection,
     * took the bytes and propagated none of them — which is exactly what a
     * replay looks like from their side. */
    uint8_t random_hash[RANDOM_HASH_LEN];
    randombytes(random_hash, 5);
    uint64_t now = (uint64_t)time(NULL);
    for (int i = 0; i < 5; i++) {
        random_hash[5 + i] = (uint8_t)((now >> ((4 - i) * 8)) & 0xFF);
    }

    /* signed_data = dest + pubkey + name_hash + random_hash + app  (no ratchet) */
    uint8_t *signed_data = s_signed;
    int sp = 0;
    memcpy(signed_data + sp, s_dest_hash, DST_HASH_LEN); sp += DST_HASH_LEN;
    memcpy(signed_data + sp, s_pubkey, KEYSIZE); sp += KEYSIZE;
    memcpy(signed_data + sp, s_name_hash, NAME_HASH_LEN); sp += NAME_HASH_LEN;
    memcpy(signed_data + sp, random_hash, RANDOM_HASH_LEN); sp += RANDOM_HASH_LEN;
    memcpy(signed_data + sp, app, applen); sp += applen;

    /* Ed25519 detached signature = crypto_sign output[0:64]. */
    unsigned long long smlen = 0;
    crypto_sign(s_sm, &smlen, signed_data, sp, s_ed_sk);
    const uint8_t *sig = s_sm;  /* first 64 bytes */

    /* announce_data = pubkey + name_hash + random_hash + signature + app */
    /* rns_packet   = flags(0x01) hops(0x00) dest_hash(16) context(0x00) data */
    /* ad           = len 0xFF FF FF 3E 55 <rns_packet> */
    uint8_t *ad = s_ad;
    int n = 0;
    ad[n++] = 0;            /* AD length placeholder */
    ad[n++] = 0xFF;         /* manufacturer specific data */
    ad[n++] = COMPANY_LO;
    ad[n++] = COMPANY_HI;
    ad[n++] = MARKER;
    ad[n++] = SUBTYPE;
    ad[n++] = 0x01;         /* flags: HEADER_1, broadcast, SINGLE, ANNOUNCE */
    ad[n++] = 0x00;         /* hops */
    memcpy(ad + n, s_dest_hash, DST_HASH_LEN); n += DST_HASH_LEN;
    ad[n++] = 0x00;         /* context NONE */
    memcpy(ad + n, s_pubkey, KEYSIZE); n += KEYSIZE;
    memcpy(ad + n, s_name_hash, NAME_HASH_LEN); n += NAME_HASH_LEN;
    memcpy(ad + n, random_hash, RANDOM_HASH_LEN); n += RANDOM_HASH_LEN;
    memcpy(ad + n, sig, SIG_LEN); n += SIG_LEN;
    memcpy(ad + n, app, applen); n += applen;
    ad[0] = n - 1;          /* AD length = everything after the length byte */

    air_raw_ad(ad, n);

    /* The same announce, unwrapped, to the hub — but far more rarely than on
     * the air. The BLE advert carries the RNS packet after a 6-byte
     * manufacturer-data header, so everything from ad+6 IS the packet and the
     * two bearers share one signature.
     *
     * The cadence is the point. Bluetooth neighbours come and go in seconds, so
     * announcing every half-minute there is right; a Reticulum hub keeps a path
     * for as long as it is used and rate-limits destinations that announce
     * faster than they need to — the reference target is an hour. Announcing to
     * the hub at the BLE cadence got every one of ours dropped: the hub took
     * the bytes and relayed none of them, while a one-off announce from this
     * laptop was relayed immediately. */
    bool hub_due = s_hub_announce_force ||
                   (now_sec() - s_last_hub_announce) >= HUB_ANNOUNCE_MIN_SEC;
    if (rns_tcp_is_up() && hub_due) {
        s_hub_announce_force = false;
        s_last_hub_announce = now_sec();
        rns_tcp_send(ad + 6, (size_t)(n - 6));
        ESP_LOGI(TAG, "announced to the hub");
    }

    ESP_LOGI(TAG, "TX announce app=\"%.*s\" (%dB adv%s)", applen, app, n,
             hub_due && rns_tcp_is_up() ? ", and to the hub" : "");
}

/* ---- repeater (BLE5 RNS transport node) --------------------------------- */
static uint32_t now_sec(void) { return (uint32_t)(esp_timer_get_time() / 1000000ULL); }

static uint32_t fnv1a(const uint8_t *d, int n)
{
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= d[i]; h *= 16777619u; }
    return h;
}

/* content dedup: don't re-air the same packet within 10 minutes */
#define RDEDUP_MAX      32
#define RELAY_DEDUP_SEC 600
typedef struct { uint32_t hash; uint32_t t; } dedup_t;
static dedup_t s_rdedup[RDEDUP_MAX];
static int     s_rdedup_cnt;

static bool relay_seen(uint32_t hash)
{
    uint32_t t = now_sec();
    for (int i = 0; i < RDEDUP_MAX; i++)
        if (s_rdedup[i].hash == hash && (t - s_rdedup[i].t) < RELAY_DEDUP_SEC)
            return true;
    return false;
}
static void relay_remember(uint32_t hash)
{
    s_rdedup[s_rdedup_cnt % RDEDUP_MAX].hash = hash;
    s_rdedup[s_rdedup_cnt % RDEDUP_MAX].t = now_sec();
    s_rdedup_cnt++;
}

static volatile bool s_relay_may_run;   /* set by ble_bring_up() */
/* Loops the relay task has completed. Printed in the heartbeat: a task that
 * dies, never starts, or blocks forever is otherwise invisible from outside,
 * and this one carries the beacons, the announcements, the history replay and
 * the section 23.7 clock. A number that stops climbing is the whole symptom. */
static volatile uint32_t s_relay_ticks;

/* re-air queue: full BLE AD buffers with a TTL, round-robin aired by relay_task */
#define RELAY_MAX     8
#define RELAY_TTL_SEC 30
/* `id_hash` and `not_before` are what make this a §13.2.1 queue rather than a
 * plain rotation: a copy waits a random moment before it is eligible, and is
 * thrown away if the same packet is heard from somebody else meanwhile. Three
 * dongles in a room therefore air one copy between them, not three. */
typedef struct {
    uint8_t  ad[256];
    uint8_t  len;
    uint32_t expire;
    uint32_t id_hash;      /* §5 identifier of the packet inside, 0 = unknown */
    int64_t  not_before;   /* esp_timer µs; before this it is not aired */
} relay_slot_t;

#define RELAY_JITTER_MIN_MS 200
#define RELAY_JITTER_MAX_MS 1200
static relay_slot_t      s_relay[RELAY_MAX];
static int               s_relay_rr;
static SemaphoreHandle_t s_relay_mtx;
static volatile uint32_t s_relayed_count;

static void relay_enqueue_id(const uint8_t *ad, int len, uint32_t id_hash)
{
    if (len <= 0 || len > 256) return;
    xSemaphoreTake(s_relay_mtx, portMAX_DELAY);
    uint32_t t = now_sec();
    int slot = -1; uint32_t soonest = 0xffffffff;
    for (int i = 0; i < RELAY_MAX; i++) {
        if (s_relay[i].len == 0 || s_relay[i].expire <= t) { slot = i; break; }
        if (s_relay[i].expire < soonest) { soonest = s_relay[i].expire; slot = i; }
    }
    uint32_t span = RELAY_JITTER_MAX_MS - RELAY_JITTER_MIN_MS;
    memcpy(s_relay[slot].ad, ad, len);
    s_relay[slot].len = len;
    s_relay[slot].expire = t + RELAY_TTL_SEC;
    s_relay[slot].id_hash = id_hash;
    s_relay[slot].not_before = esp_timer_get_time() +
        (int64_t)(RELAY_JITTER_MIN_MS + (esp_random() % (span + 1))) * 1000;
    xSemaphoreGive(s_relay_mtx);
}

static void relay_enqueue(const uint8_t *ad, int len)
{
    relay_enqueue_id(ad, len, 0);
}

/* Somebody else aired this packet. Ours is now pointless — this is the whole
 * reason the copy waits before going out (§13.2.1). */
static void relay_cancel(uint32_t id_hash)
{
    if (!id_hash) return;
    xSemaphoreTake(s_relay_mtx, portMAX_DELAY);
    for (int i = 0; i < RELAY_MAX; i++) {
        if (s_relay[i].len && s_relay[i].id_hash == id_hash) {
            s_relay[i].len = 0;
            s_relay[i].id_hash = 0;
            ESP_LOGI(TAG, "%08x already aired by somebody else — dropping ours",
                     (unsigned)id_hash);
        }
    }
    xSemaphoreGive(s_relay_mtx);
}

/* Copy the next live queued AD into [out] (round-robin). Returns its length or 0. */
static int relay_pick(uint8_t *out)
{
    int got = 0;
    xSemaphoreTake(s_relay_mtx, portMAX_DELAY);
    uint32_t t = now_sec();
    for (int k = 0; k < RELAY_MAX; k++) {
        int i = (s_relay_rr + k) % RELAY_MAX;
        if (s_relay[i].len > 0 && s_relay[i].expire <= t) { s_relay[i].len = 0; continue; }
        if (s_relay[i].len > 0 && esp_timer_get_time() < s_relay[i].not_before) {
            continue;                      /* still inside its random wait */
        }
        if (s_relay[i].len > 0) {
            memcpy(out, s_relay[i].ad, s_relay[i].len);
            got = s_relay[i].len;
            s_relay_rr = (i + 1) % RELAY_MAX;
            break;
        }
    }
    xSemaphoreGive(s_relay_mtx);
    return got;
}

/* Rewrite a received RNS packet into transport form (HEADER_2, hops+1,
 * transport_id = our identity hash) and frame it as a BLE AD into [out].
 * Returns AD length, or 0 if not relayable. The origin's signature is NOT
 * affected (it covers dest+pubkey+name_hash+random_hash+app, not hops/tid). */
static int build_relay_ad(const uint8_t *in, int in_len, uint8_t *out)
{
    if (in_len < 2 + DST_HASH_LEN + 1) return 0;
    uint8_t flags = in[0];
    uint8_t hops = in[1];
    if (hops >= 128) return 0;
    bool h2 = (flags >> 6) & 0x01;
    int tail_start = h2 ? (2 + DST_HASH_LEN) : 2;   /* dest_hash + context + data */
    int tail_len = in_len - tail_start;
    if (tail_len <= 0) return 0;
    uint8_t nflags = flags | (1 << 6) | (1 << 4);   /* HEADER_2 + TRANSPORT */
    int n = 0;
    out[n++] = 0;            /* AD length placeholder */
    out[n++] = 0xFF;
    out[n++] = COMPANY_LO;
    out[n++] = COMPANY_HI;
    out[n++] = MARKER;
    out[n++] = SUBTYPE;
    out[n++] = nflags;
    out[n++] = hops + 1;
    memcpy(out + n, s_id_hash, DST_HASH_LEN); n += DST_HASH_LEN;
    if (n + tail_len > 254) return 0;               /* one AD max 254 bytes */
    memcpy(out + n, in + tail_start, tail_len); n += tail_len;
    out[0] = n - 1;
    return n;
}

static void maybe_relay(const uint8_t *pkt, int len, int rssi)
{
    if (len < 2 + DST_HASH_LEN + 1) return;
    uint8_t flags = pkt[0];
    bool h2 = (flags >> 6) & 0x01;
    const uint8_t *dhash = h2 ? pkt + 2 + DST_HASH_LEN : pkt + 2;
    if (memcmp(dhash, s_dest_hash, DST_HASH_LEN) == 0) return;  /* our own */

    uint32_t ch = fnv1a(pkt, len);
    if (relay_seen(ch)) return;                                  /* already handled */
    relay_remember(ch);

    uint8_t ad[256];
    int n = build_relay_ad(pkt, len, ad);
    if (n <= 0) return;
    relay_enqueue(ad, n);
    s_relayed_count++;
    ESP_LOGI(TAG, "relayed dest=%02x%02x%02x%02x hops=%d->%d rssi=%d (#%u)",
             dhash[0], dhash[1], dhash[2], dhash[3], pkt[1], pkt[1] + 1, rssi,
             (unsigned)s_relayed_count);
}

/* Split an XPRS APRS parcel — from <0x1F> to <0x1F> text — into NUL-terminated
 * fields (the caller zeroes them). Returns false if there is no 0x1F separator
 * (a non-XPRS frame we still show/relay but do not gate). */
static bool split_aprs_fields(const uint8_t *p, int len,
                              char *from, int fcap, char *to, int tcap,
                              char *text, int xcap)
{
    char *f[3] = { from, to, text };
    int cap[3] = { fcap - 1, tcap - 1, xcap - 1 };
    int fi = 0, fp = 0;
    bool sep = false;
    for (int i = 0; i < len; i++) {
        uint8_t b = p[i];
        if (b == 0x1F) { sep = true; if (fi < 2) { fi++; fp = 0; } continue; }
        if (fp < cap[fi]) f[fi][fp++] = (char)b;
    }
    return sep;
}

/* Frame a raw payload as a BLE AD: [len][FF FF][3E][subtype][payload]. */
static int build_ad(uint8_t subtype, const uint8_t *payload, int len,
                    uint8_t *out)
{
    int n = 0;
    out[n++] = 0;            /* AD length placeholder */
    out[n++] = 0xFF;
    out[n++] = COMPANY_LO;
    out[n++] = COMPANY_HI;
    out[n++] = MARKER;
    out[n++] = subtype;
    if (n + len > 254) return 0;             /* one AD max 254 bytes */
    memcpy(out + n, payload, len); n += len;
    out[0] = n - 1;
    return n;
}

static int build_aprs_ad(const uint8_t *payload, int len, uint8_t *out)
{
    return build_ad(SUBTYPE_APRS, payload, len, out);
}

/* An APRS group message heard over BLE5. Unlike Reticulum, APRS is PLAINTEXT
 * (a public, radio-compatible bulletin), so the dongle may show it. It is also
 * relayed (re-aired once) to extend reach — one-to-many, deduped by content. */
static void handle_aprs(const uint8_t *payload, int len, int rssi)
{
    if (len <= 0) return;
    /* The format seam (aurora mesh_frame.dart): an XPRS packet starts `t:`
     * and holds no 0x1F byte; a compact XPRS parcel holds two. Chat and
     * carried mail both ride this subtype as XPRS now — the compact path
     * below stays for the leftovers (?ACK, ?MAIL, ?IGATE) only. */
    if (xprs_looks_like(payload, len)) {
        handle_xprs(payload, len, rssi, SUBTYPE_APRS);
        return;
    }
    uint32_t ch = fnv1a(payload, len);
    if (relay_seen(ch)) return;              /* already handled (dedup) */
    relay_remember(ch);

    /* Split the XPRS app parcel: from <0x1F> to <0x1F> text. */
    char from[CALLSIGN_MAX] = {0}, to[12] = {0}, text[160] = {0};
    bool aurora = split_aprs_fields(payload, len, from, sizeof from,
                                    to, sizeof to, text, sizeof text);
    if (!aurora) {
        /* Non-XPRS frame: printable dump for the dashboard, not gated. */
        int t = 0;
        for (int i = 0; i < len && t < (int)sizeof(text) - 1; i++) {
            uint8_t c = payload[i];
            text[t++] = (c >= 32 && c <= 126) ? (char)c : '.';
        }
        text[t] = 0;
        snprintf(from, sizeof from, "APRS");
    }
    ESP_LOGI(TAG, "RX APRS  rssi=%d  %s>%s: \"%s\"", rssi, from, to, text);

    /* Receipt id: 1:1 messages carry a PREPENDED "am:<6hex> " token; receipts
     * come back as "?ACK <6hex> d|r" control frames (aurora receipts design). */
    char am[8] = "";
    const char *body = text;
    if (strncmp(text, "am:", 3) == 0 && strlen(text) >= 9) {
        memcpy(am, text + 3, 6); am[6] = 0;
        body = text + 9;
        while (*body == ' ') body++;
    }
    if (aurora && strncmp(text, "?ACK ", 5) == 0 && strlen(text) >= 11) {
        char ack_am[8]; memcpy(ack_am, text + 5, 6); ack_am[6] = 0;
        int purged = blemesh_scf_ack(ack_am);
        if (purged) ESP_LOGI(TAG, "SCF: ack %s purged %d", ack_am, purged);
    }

    /* iGate uplink (RF -> Internet): remember the sender and gate it to APRS-IS.
     * No-op if WiFi/APRS-IS is down. Skip control frames (text starting '?',
     * e.g. ?ACK/?PING/?MAIL) and ENCRYPTED payloads — the phones deliberately
     * keep ENC1 ciphertext OFF APRS-IS (7-bit air mangles it into
     * "cannot decrypt" garbage on every receiver). */
    if (aurora) {
        igate_heard_add(from, XPRS_BEARER_BLE);
        if (to[0] && to[0] != '?' && text[0] != '?' &&
            strncmp(body, "ENC1:", 5) != 0)
            aprsis_uplink(from, to, text);
    }

    /* Store-and-forward custody (docs/mesh.md §6): park heard 1:1 messages so a
     * receiver that is out of range / asleep gets them when it reappears. The
     * sender was just heard transmitting — deliver anything parked for IT too. */
    if (aurora && s_mesh_up) {
        bool one2one = to[0] && to[0] != '#' && to[0] != '?' && to[0] != '!' &&
                       text[0] != '?' && strcmp(to, s_aprs_call) != 0;
        if (one2one && blemesh_scf_offer(to, am, payload, len, now_sec(),
                                         BLEMESH_URG_NORMAL))
            ESP_LOGI(TAG, "SCF: parked %dB for %s (am=%s, %d held)",
                     len, to, am[0] ? am : "-", blemesh_scf_count());
        mesh_deliver_pending(from);
    }

    /* Relay (extend reach). Re-air the same plaintext frame, deduped above. */
    uint8_t ad[256];
    int n = build_aprs_ad(payload, len, ad);
    if (n > 0) {
        relay_enqueue(ad, n);
        s_relayed_count++;
        ESP_LOGI(TAG, "relayed APRS %dB rssi=%d (#%u)", len, rssi,
                 (unsigned)s_relayed_count);
    }
}

/* ---- XPRS station (aurora docs/XPRS.md) ----------------------------------- */

/* ts: when the clock is plausibly synced, epoch:<boot>.<uptime> otherwise
 * (§10.7 — a clockless station with NVS keeps a boot counter, and a receiver
 * holding a clock anchors the epoch when it first hears it). This firmware
 * has no SNTP, so epoch: is the everyday form. */
static void xprs_time_field(char *out, int cap)
{
    time_t t = time(NULL);
    if (t > 1750000000) {                       /* mid-2025: a real wall clock */
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(out, cap, "ts:%04d-%02d-%02d_%02d:%02d:%02d",
                 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                 tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
        snprintf(out, cap, "epoch:%u.%u", (unsigned)s_boot_epoch,
                 (unsigned)now_sec());
    }
}

/* A duration as an XPRS qty (§10.9: s, min, h, day) — coarse on purpose. The
 * reading changes by the second while its meaning changes by the hour, so the
 * spec asks for `uptime:26h`, not `uptime:94340s`. */
static void xprs_fmt_duration(uint32_t sec, char *out, int cap)
{
    if (sec < 120)              snprintf(out, cap, "%us", (unsigned)sec);
    else if (sec < 120 * 60)    snprintf(out, cap, "%umin", (unsigned)(sec / 60));
    else if (sec < 48 * 3600)   snprintf(out, cap, "%uh", (unsigned)(sec / 3600));
    else                        snprintf(out, cap, "%uday", (unsigned)(sec / 86400));
}

/* Is [word] one of the comma-separated words in [list]? (`s:ack,read`) */
static bool xprs_words_has(const char *list, const char *word)
{
    int wl = (int)strlen(word);
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        int n = e ? (int)(e - p) : (int)strlen(p);
        if (n == wl && strncasecmp(p, word, wl) == 0) return true;
        p = e ? e + 1 : p + n;
    }
    return false;
}

/* Air one XPRS wire on [subtype], remembering its identifier first so the
 * echo (and any phone's relay of it) reads as already-handled. */
/* XPRS identifiers get their OWN dedup ring. The shared relay ring is 32
 * slots across three planes (RNS, compact, XPRS) and a busy street evicts an
 * id within a couple of minutes — measured live: an echo then re-relayed as
 * "new" 104 s after the original, which is exactly the spam the digipeater
 * policy exists to stop. XPRS traffic is low-rate (one unique id per packet,
 * not per airing), so 64 slots hold the full dedup window comfortably. */
#define XPRS_SEEN_MAX 64
static struct { uint32_t idh; uint32_t t; } s_xseen[XPRS_SEEN_MAX];
static int s_xseen_rr;

static bool xprs_seen(uint32_t idh)
{
    uint32_t t = now_sec();
    for (int i = 0; i < XPRS_SEEN_MAX; i++)
        if (s_xseen[i].idh == idh && s_xseen[i].t &&
            (t - s_xseen[i].t) < RELAY_DEDUP_SEC) return true;
    return false;
}

static void xprs_seen_remember(uint32_t idh)
{
    s_xseen[s_xseen_rr % XPRS_SEEN_MAX].idh = idh;
    s_xseen[s_xseen_rr % XPRS_SEEN_MAX].t = now_sec();
    s_xseen_rr++;
}

/* ── Updating without a ladder (XPRS.md 25.8) ─────────────────────────── */

/* The diagnostics' answers: already signed, on the bearer named. */
static void xdiag_air(const char *bearer, const char *wire, int len)
{
    if (!bearer || strcmp(bearer, "ble") == 0) xprs_air(wire, len, SUBTYPE_XPRS);
    else if (strcmp(bearer, "espnow") == 0)    xprsnow_send(wire, len);
    else                                       xprslan_send(wire, len);
}

static void xdiag_stats(uint32_t out[8])
{
    uint32_t rx = 0, tx = 0, cancel = 0, drop = 0, issued = 0, done = 0, fail = 0;
    xprsnow_stats(&rx, &tx, &cancel, &drop);
    xprsnow_tx_stats(&issued, &done, &fail);
    out[0] = rx; out[1] = tx; out[2] = cancel; out[3] = drop;
    out[4] = issued; out[5] = done; out[6] = fail;
    out[7] = (uint32_t)xprsnow_peer_count(600);
}

/* The bench gateway: a validated wire from /api/xprs/send goes on every
 * bearer this station has, which is how a signed command reaches a roof. */
static bool xdiag_gateway_send(const char *wire, int len)
{
    bool any = false;
    if (xprslan_send(wire, len)) any = true;
    if (xprsnow_send(wire, len)) any = true;
    xprs_air(wire, len, SUBTYPE_XPRS);
    return any || true;
}

/* Air one wire on the bearer named, for the updater's answers. */
static void ota_air(const char *bearer, const char *wire, int len)
{
    char w[XPRS_MAX_WIRE + 1];
    if (len > XPRS_MAX_WIRE) return;
    memcpy(w, wire, len);
    w[len] = 0;
    len = xprs_sign_wire(w, len, (int)sizeof w);
    if (!bearer || strcmp(bearer, "ble") == 0) xprs_air(w, len, SUBTYPE_XPRS);
    else if (strcmp(bearer, "espnow") == 0)    xprsnow_send(w, len);
    else                                       xprslan_send(w, len);
}

/* The card is the one thing an install must not fight. */
/* Stand the station down for the length of an install.
 *
 * Pausing the index writer was not nearly enough. A push is 1.4 MB of TCP
 * arriving at a board whose free heap is about 11 KB, and the first three
 * attempts died the same way: the worker received 44-130 KB, lwip ran out
 * of buffers, the window shut and never reopened, and the socket timed out
 * with recv=-3 while the flash writes themselves reported ESP_OK.
 *
 * So the radios that are competing for those buffers go quiet: the hub
 * link hands back its socket and both its windows, ESP-NOW hands back its
 * queues. Neither is load-bearing for the next two minutes, and the
 * station is about to reboot into new firmware anyway. Both come back on
 * their own when this is called with false -- including on the failure
 * path, because a refused image must not leave the station deaf. */
static void ota_quiesce(bool quiet)
{
    if (s_xprs_index) xprsindex_pause_writes(s_xprs_index, quiet);
    rns_tcp_pause(quiet);
    if (quiet) {
        xprsnow_stop();
        ESP_LOGW(TAG, "station stood down: installing firmware");
    } else {
        xprsnow_start(s_aprs_call[0] ? s_aprs_call : "TDONGLE");
        ESP_LOGI(TAG, "station back up");
    }
}

/* One answer to a command, signed, on the bearer it arrived on. */
static void ota_answer(const char *to, const char *bearer, const char *id,
                       int code, const char *msg)
{
    if (!to || !to[0] || !id || !id[0]) return;
    char ts[32];
    xprs_time_field(ts, sizeof ts);
    char w[XPRS_MAX_WIRE + 1];
    int n = snprintf(w, sizeof w, "t:result f:%s d:%s %s r:%s code:%d",
                     s_aprs_call[0] ? s_aprs_call : "TDONGLE", to, ts, id, code);
    if (msg && msg[0] && n > 0 && n < (int)sizeof w)
        n += snprintf(w + n, sizeof w - n, " m:%s", msg);
    if (n <= 0 || n >= (int)sizeof w) return;
    ota_air(bearer, w, n);
}

/* A cmd:update heard on any bearer. The gate first: unsigned or
 * unverifiable dies here unanswered, a stranger we can identify gets 403,
 * a stale one 408, a repeat re-airs its first answer (25.4). */
/* A cmd:update, parked by whoever heard it. NOT verified here: a
 * signature check is a secp256k1 operation, several kilobytes of stack and
 * milliseconds of work, and the receive path is the NimBLE host task or a
 * bearer task. esp32.md spent a whole section on this exact mistake --
 * "the receive task decides WHETHER to answer (parse, dedupe, budget: all
 * RAM), and a core-1 task does the query, the signature and every reply."
 * The first cut of this file verified inline and the station stopped
 * answering HTTP at all. */
static struct {
    char wire[XPRS_MAX_WIRE + 1];
    int  len;
    char bearer[10];
    volatile bool pending;
} s_upd;

/* Cheap and RAM-only: is this an update command addressed to us? */
static void xprs_update_maybe(const xprs_t *p, const char *bearer)
{
    char type[16], cmd[16], dst[16];
    xprs_type(p, type, sizeof type);
    if (strcmp(type, "command") != 0) return;
    if (!xprs_get_str(p, "cmd", cmd, sizeof cmd) ||
        strcmp(cmd, "update") != 0) return;
    if (!xprs_get_str(p, "d", dst, sizeof dst) || !dst[0]) return;
    if (strncasecmp(dst, s_aprs_call, strlen(s_aprs_call)) != 0) return;
    if (s_upd.pending) return;                   /* one at a time */
    int n = xprs_encode(p, s_upd.wire, sizeof s_upd.wire);
    if (n <= 0) return;
    s_upd.len = n;
    snprintf(s_upd.bearer, sizeof s_upd.bearer, "%s", bearer ? bearer : "ble");
    s_upd.pending = true;
}

/* The half that costs: verify, decide, answer. relay_task only (core 1). */
static void xprs_update_answer(void)
{
    if (!s_upd.pending) return;
    s_upd.pending = false;
    xprs_t p;
    if (!xprs_parse(s_upd.wire, s_upd.len, &p)) return;
    const char *bearer = s_upd.bearer;

    char id[8] = "", from[16] = "";
    int prev = 0;
    xauth_verdict_t v = xauth_check(&p, s_aprs_call, id, from, &prev);
    if (v == XAUTH_SILENT) return;
    if (v == XAUTH_REPEAT) { ota_answer(from, bearer, id, prev, NULL); return; }
    if (v == XAUTH_403) { ota_answer(from, bearer, id, 403,
                                     "not on the allow list"); return; }
    if (v == XAUTH_408) { ota_answer(from, bearer, id, 408, NULL); return; }

    char ver[24] = "", url[160] = "";
    xprs_get_str(&p, "ver", ver, sizeof ver);
    xprs_get_str(&p, "url", url, sizeof url);
    xota_code_t code = xota_request(ver[0] ? ver : NULL, url[0] ? url : NULL,
                                    from, bearer, id);
    xauth_remember(id, (int)code);
    ota_answer(from, bearer, id, (int)code,
               code == XOTA_BUSY ? "updating already" : NULL);
}

/* XPRS.md 36.10: a serve:archive announcement from a station that was
 * away -- ask it for the window we missed, on the bearer it spoke on
 * (0 = BLE5, else xprslan/xprsnow). The reply is ordinary heard traffic. */
static struct {
    char call[16];
    uint32_t since;
    int bearer;
    volatile bool pending;
} s_cu;

static void xprs_catchup_maybe(const xprs_t *p, int bearer)
{
    char type[16], sv[40], from[16];
    xprs_type(p, type, sizeof type);
    if (strcmp(type, "service") != 0) return;
    if (!xprs_get_str(p, "serve", sv, sizeof sv)) return;
    if (!strstr(sv, "archive")) return;
    /* A meeting is DIRECT (36.10): a relayed announcement is not a peer in
     * range, and the asker's reply would go to the relay's neighbourhood. */
    if (xprs_get(p, "via", NULL) != NULL) return;
    if (!xprs_get_str(p, "f", from, sizeof from)) return;
    if (strcasecmp(from, s_aprs_call) == 0) return;
    if (!s_xprs_index || !s_aprs_call[0]) return;
    time_t nowt = time(NULL);
    if (nowt < 1700000000) return;                /* no clock, no since: */
    uint32_t newest = xprsindex_boot_newest_ts(s_xprs_index);
    if (!newest) return;                          /* empty store */
    if (!xst_catchup_due(from, 600)) return;
    /* Park it. Signing is several KB of secp256k1 stack, and this runs on
     * a 5 KB bearer task -- relay_task (8 KB) builds and airs the ask. */
    if (s_cu.pending) return;
    snprintf(s_cu.call, sizeof s_cu.call, "%s", from);
    s_cu.since = newest;
    s_cu.bearer = bearer;
    s_cu.pending = true;
}

/* Build, sign and air the parked catch-up ask. relay_task only. */
static void xprs_catchup_air(void)
{
    if (!s_cu.pending) return;
    char since[24], nowts[32];
    struct tm tmv;
    time_t t = (time_t)s_cu.since;
    gmtime_r(&t, &tmv);
    strftime(since, sizeof since, "%Y-%m-%d_%H:%M:%S", &tmv);
    time_t nowt = time(NULL);
    gmtime_r(&nowt, &tmv);
    strftime(nowts, sizeof nowts, "%Y-%m-%d_%H:%M:%S", &tmv);
    char ask[XPRS_MAX_WIRE + 1];
    int an = snprintf(ask, sizeof ask,
                      "t:command f:%s d:%s ts:%s cmd:history since:%s",
                      s_aprs_call, s_cu.call, nowts, since);
    if (an > 0 && an < (int)sizeof ask) {
        an = xprs_sign_wire(ask, an, (int)sizeof ask);
        if (s_cu.bearer == 0)                    xprs_air(ask, an, SUBTYPE_XPRS);
        else if (s_cu.bearer == XPRS_BEARER_LAN) xprslan_send(ask, an);
        else                                     xprsnow_send(ask, an);
        ESP_LOGI(TAG, "catch-up: asked %s for history since %s",
                 s_cu.call, since);
    }
    s_cu.pending = false;
}

static void xprs_air(const char *wire, int len, uint8_t subtype)
{
    char id[XPRS_ID_LEN];
    if (xprs_id_of(wire, len, id))
        xprs_seen_remember((uint32_t)strtoul(id, NULL, 16));
    uint8_t ad[256];
    int n = build_ad(subtype, (const uint8_t *)wire, len, ad);
    if (n > 0) relay_enqueue(ad, n);
}

/* Answer a ping (§11.6): the reply reports the signal the test ARRIVED with —
 * the receiver's measurement, not the sender's. Bounded per §31.2: serving a
 * stranger is metered, so at most one pong per caller per minute and one
 * globally per 5 s; over budget is silence, not code:429 — a pong is a
 * measurement, not a command answer. */
#define XPRS_PONG_SLOTS      8
#define XPRS_PONG_PER_CALL   60
#define XPRS_PONG_GLOBAL     5
static struct { char call[10]; uint32_t last; } s_pong[XPRS_PONG_SLOTS];
static uint32_t s_pong_last;

static void xprs_pong(const char *to, int rssi)
{
    uint32_t t = now_sec();
    if (s_pong_last && t - s_pong_last < XPRS_PONG_GLOBAL) return;
    int slot = -1;
    for (int i = 0; i < XPRS_PONG_SLOTS; i++)
        if (strcasecmp(s_pong[i].call, to) == 0) { slot = i; break; }
    if (slot >= 0 && s_pong[slot].last &&
        t - s_pong[slot].last < XPRS_PONG_PER_CALL) return;
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < XPRS_PONG_SLOTS; i++)
            if (s_pong[i].last < s_pong[slot].last) slot = i;
    }
    snprintf(s_pong[slot].call, sizeof s_pong[slot].call, "%s", to);
    s_pong[slot].last = t ? t : 1;
    s_pong_last = t ? t : 1;

    char tf[32];
    xprs_time_field(tf, sizeof tf);
    char wire[XPRS_MAX_WIRE + 1];
    int n = snprintf(wire, sizeof wire, "t:pong f:%s d:%s %s rssi:%ddBm",
                     s_aprs_call[0] ? s_aprs_call : "TDONGLE", to, tf, rssi);
    if (n <= 0 || n >= (int)sizeof wire) return;
    n = xprs_sign_wire(wire, n, (int)sizeof wire);
    xprs_air(wire, n, SUBTYPE_XPRS);
    ESP_LOGI(TAG, "TX pong -> %s (their signal here: %ddBm)", to, rssi);
}

/* ── Digipeater discipline (docs/XPRS.md §13 + the anti-spam rule) ────── *
 *
 * A message is digipeated ONCE. Hearing the same identifier again re-airs it
 * only when the copy comes from the ORIGIN — no via:, meaning the sender
 * itself is still transmitting (a courier retry, a long advert) — and then
 * at most once per XPRS_DIGI_REPEAT_SEC and XPRS_DIGI_TIMES_MAX times in
 * total, so a stuck beacon cannot ride us forever. Copies wearing a via: are
 * the mesh echoing (our own repeat included) and never re-trigger anything.
 * The repeat mirrors the sender's own persistence and nothing else: when the
 * origin goes quiet, so do we. */
#define XPRS_DIGI_MAX        24
#define XPRS_DIGI_REPEAT_SEC 90
#define XPRS_DIGI_TIMES_MAX  5

typedef struct {
    uint32_t idh;        /* the derived identifier, as u32 */
    uint32_t last_digi;  /* when we last aired our repeat of it */
    uint8_t  times;      /* how many times we have aired it in total */
} xprs_digi_t;
static xprs_digi_t s_digi[XPRS_DIGI_MAX];
static int s_digi_rr;                    /* ring insert position */
static volatile uint32_t s_digi_repeats; /* origin-follow repeats (status) */

static xprs_digi_t *digi_find(uint32_t idh)
{
    for (int i = 0; i < XPRS_DIGI_MAX; i++)
        if (s_digi[i].idh == idh && s_digi[i].times) return &s_digi[i];
    return 0;
}

static void digi_record(uint32_t idh, uint32_t now)
{
    xprs_digi_t *e = digi_find(idh);
    if (!e) {
        e = &s_digi[s_digi_rr % XPRS_DIGI_MAX];
        s_digi_rr++;
        e->idh = idh;
        e->times = 0;
    }
    e->last_digi = now;
    if (e->times < 255) e->times++;
}

/* One XPRS packet heard on the air — from its own subtype 0x58 or as the
 * text form of 0x41 (the handle_aprs seam). This is the station's front
 * door: dedup, sighting, ping/pong, receipt release, custody, relay. */
/* Heard on the LAN: keep it, and put it on the BLE5 air for the stations that
 * have no network. That is the whole point of a dongle sitting on both. */
/*
 * One line every 15 s. This board logs only new callsigns and its WiFi
 * reconnect goes quiet after ten attempts, so a healthy idle dongle and a
 * wedged one look identical on the console. `min` is the heap low-water mark:
 * a dip that has already recovered is invisible any other way, and on this
 * hardware the dips are what take the station off the air.
 */
/* ---- the query surface (XPRS.md §36.6) ---------------------------------- */

/* The one response buffer this server ever uses.
 *
 * Every handler here used to malloc its own two kilobytes when a request
 * arrived. That is the wrong end of the boot to ask from: by the time WiFi,
 * NimBLE, the SD card and the bearers have taken theirs, this board's
 * largest free block is a few hundred bytes, so the malloc failed and the
 * server accepted the connection and then said nothing at all -- which is
 * the exact row in the docs/esp32.md symptom table, and it reads from the
 * outside like a dead station rather than a full one.
 *
 * So it is claimed once, here, while the heap is still one 31 KB block, and
 * shared: esp_http_server runs its handlers on a single task, so there is
 * never a second one in flight. If this allocation fails the server is not
 * started at all, because a server that cannot answer is worse than an
 * honest missing one. */
#define API_BUF_SIZE 2048
static char *s_api_buf;

/* The shared door, lent this server's one response buffer (see the note
 * above s_api_buf, and xapi_send.h for why it is a separate object). */
static esp_err_t api_xprs_send(httpd_req_t *req)
{
    return xprs_api_send_handler(req, s_api_buf, API_BUF_SIZE,
                                 xdiag_gateway_send);
}


/*
 * GET /api/xprs?type=&recent=&since=&until=&days=&from=&asker=&limit=
 *
 * Deliberately NOT the xprs_http component: that one pulls in the station
 * API, websockets, mesh and nostr, and this firmware wants a socket and one
 * handler. Everything a reader can ask is a field the packet already carries.
 */
typedef struct { char *buf; size_t size, len; bool first, full; } xq_ctx_t;

static bool xq_emit(const xprsidx_rec_t *r, void *vctx)
{
    xq_ctx_t *c = (xq_ctx_t *)vctx;
    size_t room = (c->len + 128 < c->size) ? c->size - c->len - 128 : 0;
    if (!room) { c->full = true; return false; }
    /* `sig` is the reader's whole basis for believing `from` (§9.1): a name is
     * only worth as much as the signature under it, and "unverified" is a
     * different statement from "unsigned". */
    static const char *sig_name[] = { "unsigned", "unverified", "verified" };
    int n = snprintf(c->buf + c->len, room,
        "%s{\"i\":%u,\"ts\":%u,\"rssi\":%d,\"type\":\"%s\",\"from\":\"%s\","
        "\"sig\":\"%s\",\"mail\":%s,\"wire\":\"",
        c->first ? "" : ",", (unsigned)r->index, (unsigned)r->ts, (int)r->rssi,
        xprsidx_type_name(r->type), r->from,
        sig_name[xprsidx_sig_of(r->flags)],
        (r->flags & XI_F_MAIL) ? "true" : "false");
    if (n < 0 || (size_t)n >= room) { c->full = true; return false; }
    size_t len = c->len + (size_t)n;
    for (const char *w = r->wire; *w; w++) {          /* escape, never overrun */
        if (len + 4 >= c->size) { c->full = true; return false; }
        if (*w == '"' || *w == '\\') c->buf[len++] = '\\';
        c->buf[len++] = *w;
    }
    if (len + 3 >= c->size) { c->full = true; return false; }
    c->buf[len++] = '"'; c->buf[len++] = '}'; c->buf[len] = 0;
    c->len = len; c->first = false;
    return true;
}

static esp_err_t api_xprs_get(httpd_req_t *req)
{
    char query[224] = {0}, param[48];
    /* Trusted: this is the operator's own API, and without it every record
     * carrying d: -- every t:result -- is hidden from the bench (the same
     * reasoning as common/xprs_api). */
    xprsidx_query_t q = { .type = -1, .trusted = true };
    char from[XPRSIDX_CALL_LEN] = {0}, asker[XPRSIDX_CALL_LEN] = {0};
    uint32_t days = 0;

    if (httpd_req_get_url_query_str(req, query, sizeof query) == ESP_OK) {
        if (httpd_query_key_value(query, "type", param, sizeof param) == ESP_OK)
            q.type = xprsidx_type_code(param);
        if (httpd_query_key_value(query, "since", param, sizeof param) == ESP_OK)
            q.since_ts = (uint32_t)strtoul(param, NULL, 10);
        if (httpd_query_key_value(query, "until", param, sizeof param) == ESP_OK)
            q.until_ts = (uint32_t)strtoul(param, NULL, 10);
        if (httpd_query_key_value(query, "days", param, sizeof param) == ESP_OK)
            days = (uint32_t)strtoul(param, NULL, 10);
        if (httpd_query_key_value(query, "limit", param, sizeof param) == ESP_OK)
            q.limit = (uint32_t)strtoul(param, NULL, 10);
        if (httpd_query_key_value(query, "recent", param, sizeof param) == ESP_OK)
            q.newest_first = (param[0] == '1' || param[0] == 't');
        if (httpd_query_key_value(query, "from", param, sizeof param) == ESP_OK)
            strlcpy(from, param, sizeof from);
        if (httpd_query_key_value(query, "asker", param, sizeof param) == ESP_OK)
            strlcpy(asker, param, sizeof asker);
    }
    if (days) {
        time_t nowt = time(NULL);
        if (nowt > 1600000000) q.since_ts = (uint32_t)nowt - days * 86400u;
    }
    q.from  = from[0]  ? from  : NULL;
    q.asker = asker[0] ? asker : NULL;
    if (q.limit == 0 || q.limit > 200) q.limit = 30;

    char *buf = s_api_buf;
    xprsidx_stats_t st;
    xprsindex_stats(s_xprs_index, &st);
    xq_ctx_t ctx = { .buf = buf, .size = 2048, .len = 0, .first = true };
    ctx.len = (size_t)snprintf(buf, 2048,
        "{\"epoch\":\"%c\",\"count\":%u,\"segments\":%u,\"recs\":[",
        st.epoch, (unsigned)st.count, (unsigned)st.segments);

    /* Take the card for the read and hand it straight back: the writer keeps
     * accepting records into RAM meanwhile, and this server has one worker. */
    xprsindex_pause_writes(s_xprs_index, true);
    int64_t t0 = esp_timer_get_time();
    size_t n = s_xprs_index ? xprsindex_query(s_xprs_index, &q, xq_emit, &ctx) : 0;
    int64_t us = esp_timer_get_time() - t0;
    xprsindex_pause_writes(s_xprs_index, false);

    int m = snprintf(buf + ctx.len, 2048 - ctx.len,
                     "],\"n\":%u,\"truncated\":%s,\"us\":%u}",
                     (unsigned)n, ctx.full ? "true" : "false", (unsigned)us);
    if (m > 0) ctx.len += (size_t)m;

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, ctx.len);
    return ESP_OK;
}

/* GET /api/xprs/dir — the XDIR1 listing of §36.9, as text.
 *
 * §36.9 has it fetched as a content-addressed file named in the service
 * announcement; this station has no file transfer yet, so it serves the same
 * bytes over HTTP. The format is the one a peer expects, which is the part
 * that has to be right. */
static esp_err_t api_xprs_dir_get(httpd_req_t *req)
{
    int n = xprsindex_directory(s_xprs_index, s_dir, XPRS_DIR_MAX);

    char *buf = s_api_buf;
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    int len = xprsindex_dir_render(s_dir, n, buf, 1536);
    if (len < 0) len = (int)strlen(buf);
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* GET /api/xprs/key — this station's identity, as the network sees it.
 *
 * The npub is the published form (§9.3); the hex is a convenience for anyone
 * verifying a signature by hand. */
static esp_err_t api_xprs_key_get(httpd_req_t *req)
{
    char hex[2 * XPRSSIG_KEY_LEN + 160];
    int n = 0;
    const nostr_keys_t *keys = xprs_keys();
    if (!s_xprs_can_sign || !keys) {
        n = snprintf(hex, sizeof hex, "{\"signing\":false}");
    } else {
        n = snprintf(hex, sizeof hex,
                     "{\"signing\":true,\"callsign\":\"%s\",\"npub\":\"%s\",\"pubx\":\"",
                     keys->callsign, keys->npub);
        for (int i = 0; i < NOSTR_PUBLIC_KEY_LEN && n < (int)sizeof hex - 4; i++) {
            n += snprintf(hex + n, sizeof hex - n, "%02x", keys->public_key[i]);
        }
        n += snprintf(hex + n, sizeof hex - n, "\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_send(req, hex, n);
    return ESP_OK;
}

static httpd_handle_t s_api;

/* Everything a person would have climbed a ladder to read (25.8). */
static esp_err_t api_diag_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    char *out = s_api_buf;
    const esp_app_desc_t *d = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (run) esp_ota_get_state_partition(run, &st);
    int n = snprintf(out, 760,
        "{\"ok\":true,\"board\":\"tdongle-s3\",\"callsign\":\"%s\","
        "\"fw\":{\"version\":\"%s\",\"project\":\"%s\",\"idf\":\"%s\","
        "\"built\":\"%s %s\"},"
        "\"boot\":{\"reason\":%d,\"uptime_s\":%u},"
        "\"heap\":{\"free\":%u,\"largest\":%u,\"min_ever\":%u},"
        "\"part\":{\"running\":\"%s\",\"state\":%d,\"rollback\":%s},"
        "\"ota\":{\"busy\":%s,\"pct\":%d},"
        "\"sd\":%s}",
        s_aprs_call[0] ? s_aprs_call : "TDONGLE",
        d ? d->version : "?", d ? d->project_name : "?", d ? d->idf_ver : "?",
        d ? d->date : "?", d ? d->time : "?",
        (int)esp_reset_reason(), (unsigned)now_sec(),
        (unsigned)esp_get_free_heap_size(),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                   MALLOC_CAP_8BIT),
        (unsigned)esp_get_minimum_free_heap_size(),
        run ? run->label : "?", (int)st,
        esp_ota_check_rollback_is_possible() ? "true" : "false",
        xota_busy() ? "true" : "false", xota_progress(),
        sdcard_is_mounted() ? "true" : "false");
    esp_err_t rc = httpd_resp_send(req, out, n);
    return rc;
}

/* POST /api/update is the shared door in common/xprs_ota/xota_http.c;
 * this board registers it on its own server in api_start(). The copy that
 * lived here had already drifted from xprs_api's. */


static void api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 5120;
    /* Core 1. The push door writes flash, and a flash erase stops the cache
     * for both cores -- doing that on an unaffined worker put it next to the
     * BLE controller and the WiFi task (esp32.md, "the two processors"). */
    cfg.core_id = 1;
    /* A firmware push is 1.4 MB arriving while this same task erases and
     * writes flash, and an erase stops the cache for both cores. The
     * default five-second socket wait is sized for a JSON request: two
     * pushes died at 128 KB and at 403 KB with recv=-3, the window having
     * closed while the worker was inside a write and the retransmits
     * needing longer than five seconds to recover on a 2,880-byte window.
     * Thirty seconds is still short enough to reap a genuinely dead
     * client, and long enough that the transfer survives the flash. */
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 30;
    cfg.max_uri_handlers = 12;
    cfg.max_open_sockets = 4;
    cfg.lru_purge_enable = true;
    httpd_handle_t srv = NULL;
    if (!s_api_buf) s_api_buf = malloc(API_BUF_SIZE);
    if (!s_api_buf) {
        ESP_LOGE(TAG, "no room for the API response buffer; not starting");
        return;
    }
    if (httpd_start(&srv, &cfg) != ESP_OK) {
        ESP_LOGW(TAG, "HTTP API failed to start");
        return;
    }
    static const httpd_uri_t u = { .uri = "/api/xprs", .method = HTTP_GET,
                                   .handler = api_xprs_get, .user_ctx = NULL };
    httpd_register_uri_handler(srv, &u);
    static const httpd_uri_t ud = { .uri = "/api/xprs/dir", .method = HTTP_GET,
                                    .handler = api_xprs_dir_get, .user_ctx = NULL };
    httpd_register_uri_handler(srv, &ud);
    static const httpd_uri_t uk = { .uri = "/api/xprs/key", .method = HTTP_GET,
                                    .handler = api_xprs_key_get, .user_ctx = NULL };
    httpd_register_uri_handler(srv, &uk);
    static const httpd_uri_t udg = { .uri = "/api/diag", .method = HTTP_GET,
                                     .handler = api_diag_get, .user_ctx = NULL };
    httpd_register_uri_handler(srv, &udg);
    static const httpd_uri_t usp = { .uri = "/api/xprs/send", .method = HTTP_POST,
                                     .handler = api_xprs_send, .user_ctx = NULL };
    httpd_register_uri_handler(srv, &usp);
    static const httpd_uri_t usg = { .uri = "/api/xprs/send", .method = HTTP_GET,
                                     .handler = api_xprs_send, .user_ctx = NULL };
    httpd_register_uri_handler(srv, &usg);
    xota_http_register(srv, s_aprs_call);
    s_api = srv;
    xh_set(XH_HTTP, true);
    ESP_LOGI(TAG, "HTTP API up: /api/xprs, /api/xprs/dir, /api/diag, "
                  "POST /api/update");
}


static void heartbeat_task(void *arg)
{
    (void)arg;
    int stats_tick = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        /* The stats rings to the card every ~10 min, so a reboot does not
         * forget the day. This task runs on core 1 and is the only writer
         * of this file (the storage discipline in docs/esp32.md). */
        if (++stats_tick >= 40) {
            stats_tick = 0;
            if (sdcard_is_mounted() && xst_epoch_now())
                xst_stats_save("/sdcard/xprs/stats.bin");
        }
        uint32_t qwait = 0, qdrop = 0;
        xprsindex_queue_stats(s_xprs_index, &qwait, &qdrop);
        xprsidx_stats_t xs;
        xprsindex_stats(s_xprs_index, &xs);
        uint32_t hrx = 0, htx = 0, hconn = 0, hdrop = 0;
        rns_tcp_stats(&hrx, &htx, &hconn, &hdrop);
        ESP_LOGW(TAG, "hub %s%s rx=%u tx=%u conns=%u dropped=%u",
                 rns_tcp_is_up() ? "" : "(down) ",
                 rns_tcp_current_hub(), (unsigned)hrx, (unsigned)htx,
                 (unsigned)hconn, (unsigned)hdrop);
        /* Authorship, since boot: what this indexer could stand behind, what it
         * merely kept, and what it refused (§9.1). A `forged` that climbs is
         * either somebody lying or a key binding that went wrong — both worth
         * seeing without asking the device anything. */
        ESP_LOGW(TAG, "sig ok=%u unverified=%u forged=%u keys=%d",
                 (unsigned)xs.verified, (unsigned)xs.unverified,
                 (unsigned)xs.forged, xprs_peer_key_count());
        /* The bearer had no voice at all on this board: rx, tx, the §13.2.1
         * cancels and — the one that mattered — frames the receive callback had
         * nowhere to put. A dropped frame was invisible here while the M5Stack
         * printed the same counter all along. */
        uint32_t nrx = 0, ntx = 0, ncan = 0, ndrop = 0;
        uint32_t nissued = 0, ndone = 0, nfail = 0;
        xprsnow_stats(&nrx, &ntx, &ncan, &ndrop);
        /* `tx` is what this station decided to say; `sent` is what the radio
         * finished with. A gap between them is a driver still holding frames,
         * and it is the difference between an acceptance that was sent and one
         * that was merely handed over before the channel changed under it. */
        xprsnow_tx_stats(&nissued, &ndone, &nfail);
        ESP_LOGW(TAG, "espnow ch=%u rx=%u tx=%u cancel=%u drop=%u sent=%u/%u "
                      "fail=%u peers=%d",
                 xprsnow_channel(), (unsigned)nrx, (unsigned)ntx,
                 (unsigned)ncan, (unsigned)ndrop, (unsigned)ndone,
                 (unsigned)nissued, (unsigned)nfail, xprsnow_peer_count(600));
        ESP_LOGW(TAG, "alive %us heap=%u min=%u big=%u recs=%u q=%u/%u lan=%d "
                      "relay=%u",
                 (unsigned)(esp_timer_get_time() / 1000000ULL),
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)esp_get_minimum_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                            MALLOC_CAP_8BIT),
                 (unsigned)xs.count, (unsigned)qwait, (unsigned)qdrop,
                 xprslan_is_active() ? xprslan_peer_count(600) : -1,
                 (unsigned)s_relay_ticks);

        /* The parts that can die after boot, re-read every beat. relay is
         * the one that matters most and the one that went unnoticed for
         * months: the count was printed all along, at zero, and a number
         * in a log line is not an alarm. Ticking is the test, not
         * existing -- a parked task still has a counter. */
        static uint32_t last_relay;
        xh_set(XH_RELAY, s_relay_ticks != last_relay);
        last_relay = s_relay_ticks;
        xh_set(XH_LAN, xprslan_is_active());
        xh_set(XH_NOW, xprsnow_channel() != 0);
        xh_set(XH_CARD, sdcard_is_mounted());
        xh_set(XH_HTTP, s_api != NULL);
        xh_set(XH_BLE, s_ble_up);
        xh_report(false);
        xh_heap_floor(TDONGLE_HEAP_FLOOR);
    }
}

/*
 * A packet heard on a NON-BLUETOOTH bearer — the LAN or ESP-NOW.
 *
 * One function for both, because none of this is about the medium: who spoke,
 * whether it was direct, whether it is an ask for us, storing it, and putting
 * it on the Bluetooth air. Only the bearer id and what to print differ, and a
 * second copy would be a second place to fix the next rule.
 */
static void xprs_from_bearer(const char *wire, int len, int rssi,
                             uint8_t bearer, const char *where)
{
    /* Who just spoke. §10.6.3 admits only DIRECTLY heard stations to a `hears:`
     * list, and a relayed copy carries the originator in `f:` exactly like a
     * direct one — so a packet wearing `via:` reached us through somebody else
     * and says nothing about what this station can hear.
     *
     * Without this the iGate was deaf to the LAN by construction: a desktop
     * could broadcast all day and never appear in the gateway's heard list,
     * because only the Bluetooth path ever fed it. */
    {
        static xprs_t hp;              /* the bearer task's own, not a stack */
        char from[16], type[16];
        if (xprs_parse(wire, len, &hp)) {
            bool direct = xprs_get(&hp, "via", NULL) == NULL;
            if (direct && xprs_get_str(&hp, "f", from, sizeof from) &&
                strcasecmp(from, s_aprs_call) != 0) {
                igate_heard_add(from, bearer);
                /* Our own radio is its own witness (36.9.4): direct, so no
                 * signature is owed, and it is what makes a callsign this
                 * station's business at all. Debounced in the component. */
                xgossip_note_direct(s_goss, from, s_aprs_call,
                                    bearer == XPRS_BEARER_LAN ? "lan"
                                                              : "espnow",
                                    now_sec());
            }
            xprs_gossip_heard(&hp, bearer);
            /* An ask arriving on a bearer is answered on it — a reply aired
             * somewhere else is a reply the requester never hears. */
            xprs_type(&hp, type, sizeof type);
            if (strcmp(type, "command") == 0) {
                xprs_hist_accept(wire, len, &hp, bearer);
            } else if (strcmp(type, "identity") == 0) {
                xprs_identity_heard(&hp);
            }
            xprs_update_maybe(&hp, bearer == XPRS_BEARER_LAN ? "lan"
                                                             : "espnow");
            xdiag_park_parsed(&hp, wire, len,
                              bearer == XPRS_BEARER_LAN ? "lan" : "espnow");
            xprs_catchup_maybe(&hp, bearer);
            /* The screen's stores (xprs_station): every hearing counts,
             * relayed copies included, exactly as the m5stack banks them. */
            xst_ingest_parsed(&hp, bearer == XPRS_BEARER_LAN ? "lan"
                                                             : "espnow",
                              rssi);
            /* §23.7 is NOT dispatched here. It lives on the heard callback,
             * which sees the duplicate airing step 4 depends on — see
             * xprs_heard_on_now. Dispatching from both would hand the state
             * machine the first airing twice. */
        }
    }

    if (s_xprs_index) {
        /* A network reports no signal and passes 0, which is the store's
         * "unknown"; ESP-NOW measured one and passes it. */
        xprsindex_add(s_xprs_index, wire, len, rssi, false, (uint32_t)time(NULL));
    }

    /* Onto the BLE5 air under the SAME rules as anything heard on the radio:
     * append ourselves to via:, which also refuses when we are already in the
     * path or the type's hop budget is spent (§13.1, §13.2). It used to go out
     * verbatim, so a packet could cross a bearer and the air forever without
     * either copy ever admitting it had been relayed. */
    char rewired[XPRS_MAX_WIRE + 1];
    int rl = xprs_append_via(wire, len, s_aprs_call, rewired, XPRS_MAX_WIRE - 1);
    if (rl <= 0) {
        ESP_LOGI(TAG, "not re-airing from %s: already in the path, or the hop "
                      "budget is spent", where);
        return;
    }
    char id[XPRS_ID_LEN];
    uint32_t idh = xprs_id_of(wire, len, id)
                       ? (uint32_t)strtoul(id, NULL, 16) : 0;
    if (idh && xprs_seen(idh)) return;      /* we already handled this one */
    if (idh) xprs_seen_remember(idh);

    uint8_t ad[256];
    int an = build_ad(SUBTYPE_XPRS, (const uint8_t *)rewired, rl, ad);
    if (an > 0) relay_enqueue_id(ad, an, idh);
    if (rssi) ESP_LOGI(TAG, "XPRS from %s: %d B, %d dBm", where, len, rssi);
    else      ESP_LOGI(TAG, "XPRS from %s: %d B", where, len);
}

static void xprs_from_lan(const char *wire, int len, uint32_t ip)
{
    (void)ip;
    xprs_from_bearer(wire, len, 0, XPRS_BEARER_LAN, "the LAN");
    /* And on to the other short-range bearer. The bearer refuses when we are
     * already in `via:` or the budget is spent, so this is safe to offer
     * unconditionally. */
    xprsnow_offer(wire, len);
}

static void xprs_from_now(const char *wire, int len, const uint8_t mac[6],
                          int rssi)
{
    (void)mac;
    xprs_from_bearer(wire, len, rssi, XPRS_BEARER_NOW, "ESP-NOW");
    xprslan_offer(wire, len);
}

/* A packet from the hub, or (frame == NULL) the moment the socket comes up.
 *
 * A hub knows nothing about a station that has not spoken since it connected,
 * so the first thing we do on a fresh connection is announce; after that, what
 * arrives is fed to the same decoder the BLE5 air uses, because an announce is
 * an announce whichever interface carried it. */
/* Set from the socket task, acted on by relay_task — see rns_from_hub(). */
static volatile bool s_hub_announce_pending;

static void rns_from_hub(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    if (!frame || len == 0) {
        /* Ask the relay task to announce rather than doing it here. announce()
         * signs into static buffers that relay_task also uses, and an Ed25519
         * signature is not something to put on a socket task's stack — the
         * same mistake the LAN beacon made on an esp_timer. */
        s_hub_announce_pending = true;
        s_hub_announce_force = true;    /* a new hub knows nothing about us */
        ESP_LOGI(TAG, "hub connected — announce queued");
        return;
    }
    handle_rns_packet(frame, (int)len, 0);
}

/* Somebody on the LAN aired a packet — including a repeat the rx path drops.
 * If we were about to put that same packet on the BLE5 air, we no longer need
 * to (§13.2.1): the two bearers keep separate queues and this is how the LAN
 * one tells the radio one to stand down. */
static void xprs_heard_on_lan(const char *id, const char *wire, int len)
{
    /* Only a copy somebody has ALREADY relayed stands us down. The origin
     * repeating itself means nobody carried it yet, which is when a digipeater
     * is most useful — `via:` is the difference. */
    xprs_t hp;
    if (!xprs_parse(wire, len, &hp) || xprs_via_count(&hp) == 0) return;
    relay_cancel((uint32_t)strtoul(id, NULL, 16));
}

/* The same for ESP-NOW: a copy somebody already relayed there means our
 * Bluetooth copy is redundant.
 *
 * This callback also carries §23.7, and it has to. The receive path proper
 * swallows a packet whose identifier it has already heard, and §23.7's step 4
 * is the SAME signed acceptance aired a second time on the working channel —
 * "the first airing commits, the second locates". That repeat is exactly what
 * the dedup ring exists to throw away, and exactly what this callback exists to
 * preserve: it fires for every hearing, duplicates included. */
static void xprs_heard_on_now(const char *id, const char *wire, int len)
{
    xprs_t hp;
    if (!xprs_parse(wire, len, &hp)) return;

    char type[16];
    xprs_type(&hp, type, sizeof type);
    if (strcmp(type, "channel") == 0 || strcmp(type, "receipt") == 0) {
        xprschan_on_packet(&hp, wire, len);
        /* fall through: a receipt is still ordinary traffic to everything else */
    }

    if (xprs_via_count(&hp) == 0) return;
    relay_cancel((uint32_t)strtoul(id, NULL, 16));
}

/*
 * Sign a packet we are about to transmit (§9.1).
 *
 * `sig:` covers the packet with `sig:` and `via:` removed. A packet this
 * station originates has neither, so the text as it stands IS the signed text —
 * no reconstruction needed, and nothing to get wrong. `sig:` goes before `m:`
 * when there is one, because `m:` is greedy and must stay last (§4).
 *
 * Returns the new length, or the original when it cannot sign or would not fit.
 */
static int xprs_sign_wire(char *wire, int len, int cap)
{
    if (!s_xprs_can_sign || len <= 0) return len;
    const nostr_keys_t *keys = xprs_keys();
    if (!keys) return len;
    return xprsid_sign(wire, len, cap, keys->private_key);
}

/*
 * t:identity (XPRS.md §9.3) — the packet that makes every other signature
 * checkable.
 *
 *   t:identity f:X3JS7Y ts:... k:npub1... sig:<60 characters>
 *
 * A receiver stores the callsign→key binding and can then verify anything this
 * station signs. It is self-signed, which is not circular: the signature proves
 * the sender HOLDS the private half, and without one anybody could rebroadcast
 * a real npub with their own fields attached. The callsign derives from the key
 * (§3), so entitlement to an X3 callsign needs nothing further.
 */
static void xprs_identity_air(void)
{
    const nostr_keys_t *keys = xprs_keys();
    if (!s_xprs_can_sign || !keys || !keys->npub[0]) return;

    char wire[XPRS_MAX_WIRE + 1];
    char ts[24];
    xprs_time_field(ts, sizeof ts);
    int len = snprintf(wire, sizeof wire, "t:identity f:%s %s k:%s",
                       s_aprs_call, ts, keys->npub);
    if (len <= 0 || len > XPRS_MAX_WIRE) return;
    len = xprs_sign_wire(wire, len, (int)sizeof wire);

    xprs_air(wire, len, SUBTYPE_XPRS);
    xprslan_send(wire, len);
    xprsnow_send(wire, len);   /* §9.3 on every bearer: a peer that cannot
                                * learn our key cannot check anything we say */
    if (s_xprs_index) {
        xprsindex_add(s_xprs_index, wire, len, 0, true, (uint32_t)time(NULL));
    }
    ESP_LOGI(TAG, "announced identity %s = %s", s_aprs_call, keys->npub);
}

/* ── announcing that this station is an indexer (XPRS.md §36.9) ─────────── */

/* How often the service announcement goes out, and how many callsigns the
 * directory may list. 64 is a dongle's worth: an indexer with more archived
 * stations than that has outgrown this hardware. */

static uint32_t s_last_service;

/* `serve:archive` is how a station discovers an indexer at all (§36.9): heard
 * in a beacon or a t:service on the air, or passed on verbatim by another
 * indexer. One word carries the whole claim -- keeping a spool, answering
 * cmd:history, holding mail -- because section 24 folded the three it used to
 * take into that one. Until this went out, the dongle archived everything it heard and
 * answered questions faithfully, and nothing on the network had any way to
 * learn it existed.
 *
 * count: is the number of callsigns archived — §36.9 puts it in the
 * announcement so a peer knows the size of a directory before fetching it.
 *
 * NOT signed: this station holds no XPRS key (§37 says it transmits unsigned).
 * §36.9 expects a signed announcement, so a peer is entitled to ignore this
 * one; it is still what makes the station discoverable to the stations in
 * range, and signing is the next thing this needs.
 */
/* Does this station get to write `super` (XPRS.md 36.9.4)?
 *
 * The checkable commitment is DEEP -- "a spool measured in RECORDS, not in
 * whatever the board's flash happened to have spare" -- and this is the board
 * that has it: a gigabyte of card against the T-Deck's 10 MB of flash.
 *
 * What it does NOT have is reach. Its hub link is compiled out for heap (see
 * the budget below rns_tcp_start), so it is a super for the stations that can
 * hear its radios, which is a real thing to be and is not the always-on
 * internet archiver 36.12.2 leans on. That is a warning, not a refusal: the
 * word invites asks, and every ask this board can hear, it can answer.
 */
#define XPRS_SUPER_MIN_BYTES  (64ull * 1024 * 1024)   /* ~200k records */
static uint64_t s_xprs_budget;

static bool xprs_is_super(void)
{
    if (!xcfg_get_bool("index_super", false)) return false;
    static int said;
    if (s_xprs_budget < XPRS_SUPER_MIN_BYTES) {
        if (said != 2) {
            said = 2;
            ESP_LOGW(TAG, "index_super set, but this station declines `super`: "
                     "%llu MB of archive is not a deep spool (36.9.4)",
                     (unsigned long long)(s_xprs_budget / (1024ull * 1024)));
        }
        return false;
    }
    if (said != 1) {
        said = 1;
        ESP_LOGI(TAG, "announcing serve:archive,super — %llu MB of spool",
                 (unsigned long long)(s_xprs_budget / (1024ull * 1024)));
#ifndef RNS_HUB_LINK
        ESP_LOGW(TAG, "super without a hub link: reachable by the stations in "
                      "range, not by the ones 36.12.2 is about");
#endif
    }
    return true;
}

static void xprs_service_air(void)
{
    if (!s_xprs_index || !s_aprs_call[0]) return;

    int n = xprsindex_directory(s_xprs_index, s_dir, XPRS_DIR_MAX);

    /* count: is RECORDS held, never callsigns heard from (XPRS.md 24.0.1).
     *
     * This sent the directory's callsign count, and that number fails silently
     * in the one job the field has. A listener remembers it and asks for
     * history when it moves; a station that has heard from six peers for a
     * month reports six however much those six say, so the listener sees a
     * number that never changes, concludes there is nothing to fetch, and
     * stops -- while the archive fills up behind it. It looks like a working
     * poller, which is why this had to be settled rather than left. The
     * directory is still built above: it is what 36.9 exchanges, and it is a
     * different question from how much is held. */
    xprsidx_stats_t st;
    xprsindex_stats(s_xprs_index, &st);

    char wire[XPRS_MAX_WIRE + 1];
    char ts[24];
    xprs_time_field(ts, sizeof ts);
    int len = snprintf(wire, sizeof wire,
                       "t:service f:%s serve:archive%s count:%lu %s",
                       s_aprs_call, xprs_is_super() ? ",super" : "",
                       (unsigned long)st.count, ts);
    if (len > 0 && len < (int)sizeof wire)
        len += xdiag_beacon_fields(wire + len, (int)sizeof wire - len);
    if (len <= 0 || len > XPRS_MAX_WIRE) return;
    len = xprs_sign_wire(wire, len, (int)sizeof wire);

    xprs_air(wire, len, SUBTYPE_XPRS);
    xprslan_send(wire, len);
    xprsnow_send(wire, len);
    if (s_xprs_index) {
        xprsindex_add(s_xprs_index, wire, len, 0, true, (uint32_t)time(NULL));
    }
    ESP_LOGI(TAG, "announced serve:archive%s — %lu record(s) held, "
                  "%d callsign(s)", xprs_is_super() ? ",super" : "",
             (unsigned long)st.count, n);
}

/* This station, on the bearer it is describing (§10.6). Built on the bearer's
 * own task: deriving an identifier is a SHA-256 and a timer task's stack is not
 * sized for that. */
static int xprs_lan_beacon(char *out, int cap)
{
    if (!s_aprs_call[0]) return 0;
    int n = snprintf(out, (size_t)cap, "t:observation f:%s link:lan",
                     s_aprs_call);
    if (n <= 0 || n >= cap) return 0;

    /* `peers:` used to be a count of distinct source ADDRESSES, which is a
     * different quantity wearing the same name: §10.6.4 defines it as how many
     * stations this one can reach directly, and says `hears:` lists the ones
     * that fitted. Both now come from the same table, so they cannot disagree. */
    int total = 0;
    char hears[XPRS_MAX_WIRE];
    int room = XPRS_MAX_WIRE - n
             - 24                                    /* " peers:NN hears:" + slack */
             - (s_xprs_can_sign ? 5 + XPRSSIG_B85_LEN : 0);
    if (room > (int)sizeof hears) room = (int)sizeof hears;
    int hn = (room > 1) ? xprs_hears_render(XPRS_BEARER_LAN, hears, room, &total)
                        : 0;

    n += snprintf(out + n, (size_t)(cap - n), " peers:%d", total);
    if (hn > 0 && n < cap) {
        n += snprintf(out + n, (size_t)(cap - n), " hears:%s", hears);
    }
    if (n <= 0 || n >= cap) return 0;
    return xprs_sign_wire(out, n, cap);
}

/* The same beacon on the other short-range bearer. §10.6.1 is why this is a
 * separate packet rather than one with two link: values: a reading belongs to
 * the bearer it names, and who we hear over ESP-NOW is not who we hear on the
 * wire. `peers:` and `hears:` therefore come from the ESP-NOW half of the heard
 * ring, not the LAN's. */
static int xprs_now_beacon(char *out, int cap)
{
    if (!s_aprs_call[0]) return 0;
    int n = snprintf(out, (size_t)cap, "t:observation f:%s link:espnow",
                     s_aprs_call);
    if (n <= 0 || n >= cap) return 0;

    int total = 0;
    char hears[XPRS_MAX_WIRE];
    int room = XPRS_MAX_WIRE - n - 24
             - (s_xprs_can_sign ? 5 + XPRSSIG_B85_LEN : 0);
    if (room > (int)sizeof hears) room = (int)sizeof hears;
    int hn = (room > 1) ? xprs_hears_render(XPRS_BEARER_NOW, hears, room, &total)
                        : 0;

    n += snprintf(out + n, (size_t)(cap - n), " peers:%d", total);
    if (hn > 0 && n < cap) {
        n += snprintf(out + n, (size_t)(cap - n), " hears:%s", hears);
    }
    if (n <= 0 || n >= cap) return 0;
    return xprs_sign_wire(out, n, cap);
}

/* ---- meeting on a working channel (XPRS.md §23.7) ------------------------ */

/* Only a station whose signature we can CHECK gets to take this one off the
 * shared channel. §23.7 refuses an unsigned invitation for exactly this reason;
 * a station we hold no key for is no better placed to be believed. */
static bool xc_verified(const xprs_t *p)
{
    char from[10];
    if (!xprs_get_str(p, "f", from, sizeof from)) return false;
    const uint8_t *key = xprs_peer_key(from);
    return key && xprs_verify_sig(p, key);
}

static uint32_t xc_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t xc_epoch(void)
{
    time_t t = time(NULL);
    return (t > 1750000000) ? (uint32_t)t : 0;
}

/* This station gateways for other people, so leaving is a real cost — but it is
 * bounded, it only ever happens for a peer we can verify, and coming back is
 * enforced by a local clock the invitation cannot touch. */
static bool xc_may_move(void) { return true; }

static void xc_on_working(const char *peer, uint8_t channel, bool lr)
{
    ESP_LOGW(TAG, "working channel %u with %s%s — the iGate and the hub are "
                  "out of reach until we return", channel, peer,
             lr ? " on the long-range PHY" : "");
}

static void xc_on_home(const char *peer, bool worked)
{
    ESP_LOGW(TAG, "home again after %s (%s)", peer[0] ? peer : "nobody",
             worked ? "the pair met" : "nothing happened");
}

static const xc_ops_t k_chan_ops = {
    .sign = xprs_sign_wire,
    .verified = xc_verified,
    .air = xprsnow_send,
    .now_ms = xc_now_ms,
    .time_field = xprs_time_field,
    .epoch = xc_epoch,
    .hold_reconnect = xprs_wifi_hold_reconnect,
    .announce_identity = xprs_identity_air,
    .may_move = xc_may_move,
    .settle = xprsnow_settle,
    .trace = xprsnow_set_trace,
    .bluetooth = xc_bluetooth,
    .on_working = xc_on_working,
    .on_home = xc_on_home,
};

/* ---- serving cmd:history (XPRS.md sections 25.2.1 and 36) ---------------- */

/*
 * The dongle advertises `serve:archive` every ten minutes. This is the half
 * that makes the claim true over the air: until now `t:command` was
 * not even one of the types the receive path dispatched on, so the only way to
 * ask this indexer anything was HTTP — which is a bench convenience, not the
 * protocol.
 *
 * The shape is section 25.2.1's replay and it matches the Dart responder
 * (lib/services/xprs/xprs_history_server.dart) deliberately, down to the page
 * size and the budgets, so a station cannot tell the two apart by behaviour:
 *
 *   t:result f:<us> d:<asker> ts:... r:<command id> code:202     "coming"
 *   <the stored packets, verbatim — the author's bytes, the author's sig>
 *   t:result ... code:200            (or 206 when more is held)
 *
 * and 404 for an empty window, 429 over budget. One reply packet per relay-task
 * tick, which is 1500 ms — the same pacing, and free, because that task already
 * wakes on exactly that period.
 */
#define XPRS_HIST_PAGE          12
#define XPRS_HIST_KNOWN_PH       6      /* replays/hour for a station we know */
#define XPRS_HIST_STRANGER_PH    2
#define XPRS_HIST_GLOBAL_PH     12
#define XPRS_ASK_RING           16
#define XPRS_ANSWERED_RING       8

/* Callsign → signing key, learned from the `t:identity` packets other stations
 * air (section 9.3). Small on purpose: this exists to tell a station we have
 * met from a stranger when spending airtime, not to be a directory. */
#define XPRS_PEERKEYS_MAX       16
static struct { char call[10]; uint8_t pub[32]; } s_peer_keys[XPRS_PEERKEYS_MAX];

/* Append-only, and the count is published LAST. The receive tasks add entries
 * and the index's writer task reads them while verifying, with no lock between
 * them: that is safe only because an entry is finished before it is counted, so
 * a reader either does not see a slot at all or sees it whole. Nothing ever
 * rewrites or removes one. */
static volatile int s_peer_keys_n;

static int xprs_peer_key_count(void) { return s_peer_keys_n; }

static const uint8_t *xprs_peer_key(const char *call)
{
    for (int i = 0; i < s_peer_keys_n; i++) {
        if (strcasecmp(s_peer_keys[i].call, call) == 0) return s_peer_keys[i].pub;
    }
    return NULL;
}

/* Remember a binding the FIRST time we hear it and never overwrite it.
 * A callsign on an open bearer is not proof of anything, so a later packet
 * claiming the same callsign with a different key is somebody trying to become
 * that station — and since a wrong key turns every genuine packet from them
 * into a forgery, last-writer-wins would be a way to silence a station by
 * shouting. */
static void xprs_peer_key_learn(const char *call, const uint8_t pub[32])
{
    if (!call || !call[0] || xprs_peer_key(call)) return;
    int n = s_peer_keys_n;
    if (n >= XPRS_PEERKEYS_MAX) return;
    strncpy(s_peer_keys[n].call, call, sizeof s_peer_keys[0].call - 1);
    s_peer_keys[n].call[sizeof s_peer_keys[0].call - 1] = 0;
    memcpy(s_peer_keys[n].pub, pub, 32);
    s_peer_keys_n = n + 1;              /* published last: see the note above */
    ESP_LOGI(TAG, "XPRS: learned %s signs with %02x%02x%02x%02x...",
             call, pub[0], pub[1], pub[2], pub[3]);
}

/* A heard `t:identity f:<call> ... k:npub1...` (section 9.3), verified against
 * the very key it carries: a station claiming a key must show it holds it, and
 * the packet is signed with it, so it can. */
static void xprs_identity_heard(const xprs_t *p)
{
    char call[10], npub[80];
    if (!xprs_get_str(p, "f", call, sizeof call)) return;
    if (!xprs_get_str(p, "k", npub, sizeof npub)) return;
    if (strncmp(npub, "npub1", 5) != 0) return;
    if (xprs_peer_key(call)) return;          /* first speaker wins; cheap exit */

    char hrp[8];
    uint8_t data[64];
    size_t dlen = sizeof data;
    if (bech32_decode(npub, hrp, data, &dlen) != ESP_OK) return;
    if (dlen != 32 || strcmp(hrp, "npub") != 0) return;
    if (!xprs_verify_sig(p, data)) {
        ESP_LOGW(TAG, "XPRS: %s does not sign for the key it published", call);
        return;
    }
    xprs_peer_key_learn(call, data);
}

/* Check `sig:` (section 9.1) against [pub], the signer's 32-byte x-only key.
 * False for absent, malformed or wrong — a caller deciding whether to spend
 * airtime wants one answer, and "not proven" is the same as "no". */
static bool xprs_verify_sig(const xprs_t *p, const uint8_t pub[32])
{
    return xprsid_verify(p, pub);
}

/*
 * What the index calls to decide whether a stored packet is really from who it
 * says (XPRS.md section 9.1). Runs on the store's writer task — core 1, one
 * packet before it is written — never on the thread that heard it.
 *
 * Three answers, and the middle one matters: a station whose key we have never
 * heard is UNKNOWN, not a liar. Only a signature that fails against a key we
 * actually hold is evidence of anything.
 */
static int xprs_verify_for_index(const char *wire, int len, const char *from)
{
    const uint8_t *key = xprs_peer_key(from);
    if (!key) return 0;                    /* no key: cannot tell, and says so */
    xprs_t p;
    if (!xprs_parse(wire, len, &p)) return 0;
    return xprs_verify_sig(&p, key) ? 1 : -1;
}

/* Every `t:identity` this station ever stored is a callsign→key binding it can
 * have back for the cost of reading them (section 9.3). Without this the table
 * is empty at every boot, and the first ten minutes of traffic from stations we
 * have known for weeks reads as unverifiable.
 *
 * Boot only, and on the task that opens the store — the radios are not up yet,
 * so this is the one moment when reading the card costs nobody anything. */
static bool xprs_key_from_identity(const xprsidx_rec_t *rec, void *ctx)
{
    (void)ctx;
    xprs_t p;
    if (xprs_parse(rec->wire, rec->len, &p)) xprs_identity_heard(&p);
    return s_peer_keys_n < XPRS_PEERKEYS_MAX;
}

/* Set when the store opens, acted on by the relay task's first tick.
 *
 * NOT done where the store is opened. `app_main` has a 3584-byte stack and
 * checking a signature is an mbedtls_ecp_muladd, which wants several kilobytes
 * of its own: doing it there overflowed the main task and put the dongle in a
 * reboot loop — which over the network is indistinguishable from a flaky link,
 * exactly as docs/esp32.md warns. The relay task has 8 KB and lives on core 1,
 * so it does this once, on its first pass, before the radios are busy. */
static volatile bool s_keys_reload_due;

static void xprs_keys_reload(void)
{
    if (!s_xprs_index) return;
    xprsidx_query_t q = {
        .type = XI_T_IDENTITY,
        .asker = s_aprs_call,
        .limit = XPRS_PEERKEYS_MAX * 2,    /* the newest may repeat a station */
        .newest_first = true,
    };
    xprsindex_query(s_xprs_index, &q, xprs_key_from_identity, NULL);
    ESP_LOGI(TAG, "XPRS: %d signing key%s known from stored identities",
             s_peer_keys_n, s_peer_keys_n == 1 ? "" : "s");
}

/*
 * The replay, in two halves that run on two different processors.
 *
 * THE CARD MAY NOT BE TOUCHED FROM A RECEIVE PATH. `docs/esp32.md` says it twice
 * — "never write from a receive path" and "anything that blocks for
 * milliseconds must be pinned to core 1" — and it says what happens when you
 * do: SD work on core 0 took this station from 178 of 182 pings to 1 of 96,
 * with `wifi:m f null` as the symptom. A `t:command` arrives on the NimBLE host
 * task (priority 21, core 0, the same processor as WiFi and the controller) or
 * on the LAN bearer's task, and answering it inline put a FatFs query on both.
 *
 * So the receive task does only what is free: check the packet is a history ask
 * addressed to us, that we have not already answered it, and that the asker is
 * inside its budget. Then it copies the ask verbatim and sets a flag. Everything
 * with a cost — the signature check, the query, the signing of every reply — runs
 * on the relay task, which is pinned to core 1.
 *
 * The page holds record INDEXES, not wires: four bytes each rather than a
 * quarter of a kilobyte, read back at air time.
 */
enum { HIST_IDLE = 0, HIST_PENDING, HIST_REPLAY };

static struct {
    volatile uint8_t state;               /* the only cross-task handshake */
    uint8_t  bearer;
    char     ask[XPRS_MAX_WIRE + 1];      /* the command, verbatim, re-parsed on core 1 */
    int      n, i;
    bool     more;
    char     to[10];
    char     cmdid[XPRS_ID_LEN];
    uint32_t rec[XPRS_HIST_PAGE];
} s_hist;

/* A refusal owed to somebody. Composing one means signing it, which is crypto,
 * which belongs on core 1 with everything else — so even saying "no" is queued
 * rather than answered where the packet landed. */
static struct {
    volatile bool due;
    uint8_t bearer;
    int     code;
    char    to[10];
    char    cmdid[XPRS_ID_LEN];
} s_hist_refuse;

static struct { char call[10]; uint32_t t; } s_hist_asks[XPRS_ASK_RING];

/*
 * Asks that arrived while one was already being served.
 *
 * 36.9.4 lists `concurrent` among the things claiming `super` commits a
 * station to: "one replay at a time, with a page taking tens of seconds to
 * air, means the second asker of any minute is refused". This board takes
 * about eighteen seconds over a page, so on a busy minute the refusal was the
 * normal answer -- an honest 429, but a 429 to a station that had done
 * nothing wrong and would have been served a moment later.
 *
 * One replay still airs at a time: that is 31.4, and the channel needs it.
 * What changes is that the next asker WAITS instead of being turned away.
 * Only when this room is also full does the 429 come back, which is what it
 * is for. Depth applies only when this station claims `super`; an ordinary
 * archiver keeps its single slot.
 */
#define XPRS_ASKQ_MAX 3
static struct {
    uint8_t bearer;
    char    ask[XPRS_MAX_WIRE + 1];
    char    to[10];                        /* same width as s_hist.to */
    char    cmdid[XPRS_ID_LEN];
} s_askq[XPRS_ASKQ_MAX];
static volatile uint8_t s_askq_w, s_askq_r, s_askq_n;

static struct { uint32_t id; uint32_t t; } s_hist_answered[XPRS_ANSWERED_RING];

/* Air one packet on the bearer the ask arrived on. A reply that goes out
 * somewhere else is a reply the requester never hears. */
static void xprs_air_on(const char *wire, int len, uint8_t bearer)
{
    if (bearer == XPRS_BEARER_LAN)      xprslan_send(wire, len);
    else if (bearer == XPRS_BEARER_NOW) xprsnow_send(wire, len);
    else                                xprs_air(wire, len, SUBTYPE_XPRS);
}

/*
 * Gossip (36.9.4). This board had NONE: it rendered its own hears: list for
 * others to use and kept nothing of theirs, so every history miss was a dead
 * end -- a 404 with no m:try, when a station in the room very likely had what
 * was asked for. On the deep board that is the wrong way round; a super is
 * where the humble stations come to ask precisely this.
 */
/* An observation's hears: list, through the walls of 36.9.4. The quota is
 * checked before the signature on purpose: a verify is a curve operation on
 * the task that heard the packet, and the quota admits one per interval
 * whatever the verify says. */
static void xprs_gossip_heard(const xprs_t *p, uint8_t bearer)
{
    if (!s_goss) return;
    char type[16], from[10], hears[96], link[8];
    xprs_type(p, type, sizeof type);
    if (strcmp(type, "observation") != 0) return;
    if (!xprs_get_str(p, "f", from, sizeof from) || !from[0]) return;
    if (strcasecmp(from, s_aprs_call) == 0) return;
    if (!xprs_get_str(p, "hears", hears, sizeof hears)) return;

    const uint32_t now_s = now_sec();
    if (!xgossip_would_accept(s_goss, from, now_s)) return;
    if (!xprs_get_str(p, "link", link, sizeof link))
        snprintf(link, sizeof link, "%s",
                 bearer == XPRS_BEARER_LAN ? "lan" : "espnow");

    const char *list[8];
    char *tok = hears, *next;
    int n = 0;
    while (tok && *tok && n < 8) {
        next = strchr(tok, ',');
        if (next) *next = 0;
        if (*tok) list[n++] = tok;
        tok = next ? next + 1 : NULL;
    }
    xgossip_note_hears(s_goss, from, list, n, link, xc_verified(p), now_s);
}

/* As xprs_air_result, with the 404's `m:try <peers>` tail (36.9): a miss is
 * not a dead end when somebody this station has heard of has what was asked
 * for. Never names us -- the asker just asked us. */
static void xprs_air_result_try(const char *to, const char *cmdid, int code,
                                uint8_t bearer, const char *only)
{
    char list[64];
    if (!only || !only[0] ||
        xgossip_try_candidates(s_goss, only, s_aprs_call, list, sizeof list) <= 0)
        list[0] = 0;

    char wire[XPRS_MAX_WIRE + 1], ts[24];
    xprs_time_field(ts, sizeof ts);
    int n = snprintf(wire, sizeof wire, "t:result f:%s d:%s %s r:%s code:%d",
                     s_aprs_call, to, ts, cmdid, code);
    if (n > 0 && n < XPRS_MAX_WIRE && list[0])
        n += snprintf(wire + n, sizeof wire - (size_t)n, " m:try %s", list);
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = xprs_sign_wire(wire, n, (int)sizeof wire);
    xprs_air_on(wire, n, bearer);
}

static void xprs_air_result(const char *to, const char *cmdid, int code,
                            uint8_t bearer)
{
    char wire[XPRS_MAX_WIRE + 1], ts[24];
    xprs_time_field(ts, sizeof ts);
    int n = snprintf(wire, sizeof wire, "t:result f:%s d:%s %s r:%s code:%d",
                     s_aprs_call, to, ts, cmdid, code);
    if (n <= 0 || n > XPRS_MAX_WIRE) return;
    n = xprs_sign_wire(wire, n, (int)sizeof wire);
    xprs_air_on(wire, n, bearer);
}

/* Does [call] appear in [wire] as a whole token?
 *
 * Section 36.6: `only:` matches a callsign WHEREVER the packet carries it — as
 * author, as addressee, or inside a list field (`hears:`, `hold:`, `via:`,
 * `grant:`). That one reading rule is what makes "where can X be reached" an
 * askable question with no new vocabulary, so the match is over the whole
 * packet rather than over two named fields. Bounded by non-alphanumerics so
 * X1BO does not match X1BOA3. */
static bool xprs_wire_mentions(const char *wire, int len, const char *call)
{
    int cl = (int)strlen(call);
    if (cl <= 0) return false;
    for (int i = 0; i + cl <= len; i++) {
        if (strncasecmp(wire + i, call, (size_t)cl) != 0) continue;
        char before = (i == 0) ? 0 : wire[i - 1];
        char after  = (i + cl >= len) ? 0 : wire[i + cl];
        bool lb = !(isalnum((unsigned char)before) || before == '-');
        bool rb = !(isalnum((unsigned char)after)  || after  == '-');
        if (lb && rb) return true;
    }
    return false;
}

/* How many records a single ask may look at. An `only:` that matches nothing
 * otherwise walks the store until the query's own limit or the segments run
 * out, and every one of those is a read from a card the radios are waiting on. */
#define XPRS_HIST_SCAN_MAX 200

struct hist_ctx { const char *only; int n; int seen; bool more; bool capped; };

static bool hist_emit(const xprsidx_rec_t *rec, void *vctx)
{
    struct hist_ctx *c = (struct hist_ctx *)vctx;
    if (++c->seen > XPRS_HIST_SCAN_MAX) { c->capped = true; return false; }
    if (c->only && c->only[0] &&
        !xprs_wire_mentions(rec->wire, rec->len, c->only)) {
        return true;                       /* filtered, keep looking */
    }
    if (c->n >= XPRS_HIST_PAGE) { c->more = true; return false; }
    s_hist.rec[c->n] = rec->index;
    c->n++;
    return true;
}

/* Replays this station has already answered, so the requester's advert re-airing
 * the identical wire for its whole TTL gets one answer and then quiet. */
static bool xprs_hist_already_answered(uint32_t id, uint32_t t)
{
    int slot = 0;
    for (int i = 0; i < XPRS_ANSWERED_RING; i++) {
        if (s_hist_answered[i].id == id && t - s_hist_answered[i].t < 300) {
            return true;
        }
        if (s_hist_answered[i].t < s_hist_answered[slot].t) slot = i;
    }
    s_hist_answered[slot].id = id;
    s_hist_answered[slot].t = t;
    return false;
}

/* Section 31.3/31.4: a stranger gets less of the channel than a station we have
 * met, and everybody together gets a ceiling. Over it is 429, out loud, because
 * silence is indistinguishable from a dead indexer. */
static bool xprs_hist_budget_super(const char *call, uint32_t t);  /* below */

static bool xprs_hist_budget_allows(const char *call, uint32_t t)
{
    if (xprs_is_super()) return xprs_hist_budget_super(call, t);
    int global = 0, mine = 0;
    for (int i = 0; i < XPRS_ASK_RING; i++) {
        if (!s_hist_asks[i].t || t - s_hist_asks[i].t > 3600) continue;
        global++;
        if (strcasecmp(s_hist_asks[i].call, call) == 0) mine++;
    }
    if (global >= XPRS_HIST_GLOBAL_PH) return false;
    int cap = xprs_peer_key(call) ? XPRS_HIST_KNOWN_PH : XPRS_HIST_STRANGER_PH;
    return mine < cap;
}

/*
 * A super's budgets (36.9.4: "orders of magnitude above the reference
 * numbers"; 31.2's six-an-hour is explicitly a pocket device's figure, and
 * this board holds a gigabyte). A separate implementation from the ring
 * above, for the reason the shared app gives: the ring stores one timestamp
 * per permitted replay, which is what makes it an exact sliding hour and is
 * also why it cannot be scaled to thousands. At super scale the hour boundary
 * stops meaning anything -- the budget is there to stop a flood, not to
 * ration a neighbour -- so this counts within a window at twelve bytes an
 * asker, and the ordinary path stays exactly as precise as it was.
 */
#define XPRS_HIST_SUPER_MULT   1000
#define XPRS_HIST_SUPER_ASKERS   32
static struct { char call[16]; uint32_t win, n; }
    s_hist_super[XPRS_HIST_SUPER_ASKERS];
static struct { uint32_t win, n; } s_hist_super_global;

static bool xprs_hist_budget_super(const char *call, uint32_t t)
{
    if (t - s_hist_super_global.win < 3600 &&
        s_hist_super_global.n >= (uint32_t)XPRS_HIST_GLOBAL_PH * XPRS_HIST_SUPER_MULT)
        return false;
    const uint32_t cap = (uint32_t)(xprs_peer_key(call) ? XPRS_HIST_KNOWN_PH
                                                        : XPRS_HIST_STRANGER_PH)
                         * XPRS_HIST_SUPER_MULT;
    for (int i = 0; i < XPRS_HIST_SUPER_ASKERS; i++) {
        if (strcasecmp(s_hist_super[i].call, call) != 0) continue;
        if (t - s_hist_super[i].win >= 3600) return true;
        return s_hist_super[i].n < cap;
    }
    return true;
}

static void xprs_hist_record_super(const char *call, uint32_t t)
{
    if (t - s_hist_super_global.win >= 3600) {
        s_hist_super_global.win = t;
        s_hist_super_global.n = 0;
    }
    s_hist_super_global.n++;

    int slot = -1, oldest = 0;
    for (int i = 0; i < XPRS_HIST_SUPER_ASKERS; i++) {
        if (strcasecmp(s_hist_super[i].call, call) == 0) { slot = i; break; }
        if (!s_hist_super[i].call[0]) { slot = i; break; }
        if (s_hist_super[i].win < s_hist_super[oldest].win) oldest = i;
    }
    if (slot < 0) slot = oldest;
    if (strcasecmp(s_hist_super[slot].call, call) != 0) {
        snprintf(s_hist_super[slot].call, sizeof s_hist_super[0].call, "%s", call);
        s_hist_super[slot].win = t;
        s_hist_super[slot].n = 0;
    } else if (t - s_hist_super[slot].win >= 3600) {
        s_hist_super[slot].win = t;
        s_hist_super[slot].n = 0;
    }
    s_hist_super[slot].n++;
}

static void xprs_hist_record_ask(const char *call, uint32_t t)
{
    if (xprs_is_super()) { xprs_hist_record_super(call, t); return; }
    int slot = 0;
    for (int i = 0; i < XPRS_ASK_RING; i++) {
        if (!s_hist_asks[i].t) { slot = i; break; }
        if (s_hist_asks[i].t < s_hist_asks[slot].t) slot = i;
    }
    strncpy(s_hist_asks[slot].call, call, sizeof s_hist_asks[0].call - 1);
    s_hist_asks[slot].call[sizeof s_hist_asks[0].call - 1] = 0;
    s_hist_asks[slot].t = t;
}

/* ---- the receive half: free work only, on whatever task heard the packet --- */

/* Queue a refusal for the relay task to sign and air. */
static void xprs_hist_refuse(const char *to, const char *cmdid, int code,
                             uint8_t bearer)
{
    if (s_hist_refuse.due) return;         /* one owed at a time is enough */
    s_hist_refuse.bearer = bearer;
    s_hist_refuse.code = code;
    snprintf(s_hist_refuse.to, sizeof s_hist_refuse.to, "%s", to);
    memcpy(s_hist_refuse.cmdid, cmdid, XPRS_ID_LEN);
    s_hist_refuse.due = true;
}

/*
 * One `t:command cmd:history` addressed to us, as seen by the task that heard
 * it. Nothing here opens a file, signs anything, or blocks: it decides whether
 * the ask is ours to answer and hands it to core 1.
 */
static void xprs_hist_accept(const char *wire, int len, const xprs_t *p,
                             uint8_t bearer)
{
    if (!s_xprs_index || !s_aprs_call[0]) return;
    if (len <= 0 || len > XPRS_MAX_WIRE) return;

    char cmd[16], to[10], from[10];
    if (!xprs_get_str(p, "cmd", cmd, sizeof cmd) ||
        strcmp(cmd, "history") != 0) return;
    /* Addressed to US. A history ask with no `d:` is addressed to the whole
     * street, and every indexer on it answering at once is exactly the storm
     * section 13.2.1 exists to prevent. */
    if (!xprs_get_str(p, "d", to, sizeof to) ||
        strcasecmp(to, s_aprs_call) != 0) return;
    if (!xprs_get_str(p, "f", from, sizeof from) || !from[0]) return;

    char cmdid[XPRS_ID_LEN];
    xprs_id(p, cmdid);                     /* a SHA-256 over 250 bytes: cheap */
    uint32_t t = now_sec();
    if (xprs_hist_already_answered((uint32_t)strtoul(cmdid, NULL, 16), t)) return;

    if (!xprs_hist_budget_allows(from, t)) {
        ESP_LOGW(TAG, "XPRS: history for %s refused - over budget (429)", from);
        xprs_hist_refuse(from, cmdid, 429, bearer);
        return;
    }
    if (s_hist.state != HIST_IDLE) {
        const int depth = xprs_is_super() ? XPRS_ASKQ_MAX : 0;
        if (s_askq_n >= depth) {
            xprs_hist_refuse(from, cmdid, 429, bearer);
            return;
        }
        const uint8_t w = s_askq_w;
        memcpy(s_askq[w].ask, wire, (size_t)len);
        s_askq[w].ask[len] = 0;
        s_askq[w].bearer = bearer;
        snprintf(s_askq[w].to, sizeof s_askq[w].to, "%s", from);
        memcpy(s_askq[w].cmdid, cmdid, XPRS_ID_LEN);
        xprs_hist_record_ask(from, t);
        s_askq_w = (uint8_t)((w + 1) % XPRS_ASKQ_MAX);
        s_askq_n++;                        /* published last */
        return;
    }

    memcpy(s_hist.ask, wire, (size_t)len);
    s_hist.ask[len] = 0;
    s_hist.bearer = bearer;
    snprintf(s_hist.to, sizeof s_hist.to, "%s", from);
    memcpy(s_hist.cmdid, cmdid, XPRS_ID_LEN);
    xprs_hist_record_ask(from, t);
    /* Last, and after everything it describes: the relay task reads this. */
    s_hist.state = HIST_PENDING;
}

/* ---- the core-1 half: the signature, the card, and every reply ------------ */

/* Run the query the pending ask asks for. Returns the number of records found. */
static int xprs_hist_query(void)
{
    xprs_t p;
    int len = (int)strlen(s_hist.ask);
    if (!xprs_parse(s_hist.ask, len, &p)) return 0;

    /* A forged command deserves no airtime at all — checked HERE, because a
     * verify is a curve operation and the receive path is the wrong processor
     * for one. Unverifiable is not forged: a station whose key we have never
     * heard is a stranger, and strangers are metered rather than refused. */
    const uint8_t *key = xprs_peer_key(s_hist.to);
    if (key && xprs_get(&p, "sig", NULL) && !xprs_verify_sig(&p, key)) {
        ESP_LOGW(TAG, "XPRS: history ask from %s is forged - ignored", s_hist.to);
        return -1;
    }

    char since[24], until[24], only[10], kind[64] = "";
    /* `kind:` names a TYPE, or a comma-separated list of them (25.2 -- the
     * 36.9.3 neighbourhood ask lists six and runs 37 characters, which is why
     * the buffer is not 16). `only:` names a CALLSIGN (36.6) and is applied
     * by hist_emit below. With no kind: named, serve the talking rather than
     * the beacons an archiver also keeps -- otherwise a page of the newest
     * twelve is twelve presence records. */
    xprs_get_str(&p, "kind", kind, sizeof kind);
    xprsidx_query_t q = {
        .since_ts = xprs_get_str(&p, "since", since, sizeof since)
                        ? xprsindex_ts_to_epoch(since, (int)strlen(since)) : 0,
        .until_ts = xprs_get_str(&p, "until", until, sizeof until)
                        ? xprsindex_ts_to_epoch(until, (int)strlen(until)) : 0,
        .type     = -1,
        .types    = xprsidx_type_mask(kind),
        .talk_only = !kind[0],
        .from     = NULL,
        .asker    = s_hist.to,
        .limit    = XPRS_HIST_PAGE + 1,
        .newest_first = true,
    };
    struct hist_ctx ctx = {
        .only = xprs_get_str(&p, "only", only, sizeof only) ? only : NULL,
        .n = 0, .seen = 0, .more = false, .capped = false,
    };

    /* Hold the writer off the card for the length of the query and give it
     * straight back (docs/esp32.md, "the web server belongs there too"). Records
     * keep arriving into RAM meanwhile, so nothing is lost by the pause. */
    xprsindex_pause_writes(s_xprs_index, true);
    xprsindex_query(s_xprs_index, &q, hist_emit, &ctx);
    xprsindex_pause_writes(s_xprs_index, false);

    if (ctx.capped) {
        /* Say so. A silent cap reads as "that is everything I hold", which is a
         * different and untrue answer. */
        ESP_LOGW(TAG, "XPRS: history for %s stopped at %d records examined",
                 s_hist.to, XPRS_HIST_SCAN_MAX);
        ctx.more = true;
    }
    s_hist.more = ctx.more;
    return ctx.n;
}

/* Move the oldest waiting ask into the slot the pump reads. Core 1, called
 * only when the machine is idle. */
static void xprs_hist_take_next(void)
{
    if (!s_askq_n || s_hist.state != HIST_IDLE) return;
    const uint8_t r = s_askq_r;
    memcpy(s_hist.ask, s_askq[r].ask, sizeof s_hist.ask);
    s_hist.bearer = s_askq[r].bearer;
    snprintf(s_hist.to, sizeof s_hist.to, "%s", s_askq[r].to);
    memcpy(s_hist.cmdid, s_askq[r].cmdid, XPRS_ID_LEN);
    s_askq_r = (uint8_t)((r + 1) % XPRS_ASKQ_MAX);
    s_askq_n--;
    s_hist.state = HIST_PENDING;           /* published last */
}

/* One step of the replay, called once per relay-task tick (1500 ms) — the same
 * inter-packet pacing the Dart responder uses, and free here because that task
 * already wakes on exactly that period. Runs on core 1. */
static void xprs_hist_pump(void)
{
    /* Gossip's card work, on core 1 with the rest of it. Queued by whichever
     * task heard the packet; written here (esp32.md: never write from the
     * task that heard it). */
    xgossip_pump(s_goss);

    /* Somebody is waiting to be told no. */
    if (s_hist_refuse.due) {
        xprs_air_result(s_hist_refuse.to, s_hist_refuse.cmdid,
                        s_hist_refuse.code, s_hist_refuse.bearer);
        s_hist_refuse.due = false;
        return;
    }

    /* Idle with somebody waiting: start them before doing anything else. */
    xprs_hist_take_next();

    if (s_hist.state == HIST_PENDING) {
        int n = xprs_hist_query();
        if (n < 0) {                        /* forged: no reply, no airtime */
            s_hist.state = HIST_IDLE;
            return;
        }
        if (n == 0) {
            ESP_LOGI(TAG, "XPRS: history for %s - nothing in that window (404)",
                     s_hist.to);
            {
                /* 36.9: name whoever gossip says has been near the callsign
                 * that was asked about, so the miss points somewhere. */
                xprs_t ap;
                char only[10] = "";
                if (xprs_parse(s_hist.ask, (int)strlen(s_hist.ask), &ap))
                    xprs_get_str(&ap, "only", only, sizeof only);
                xprs_air_result_try(s_hist.to, s_hist.cmdid, 404,
                                    s_hist.bearer, only);
            }
            s_hist.state = HIST_IDLE;
            return;
        }
        s_hist.n = n;
        s_hist.i = 0;
        s_hist.state = HIST_REPLAY;
        xprs_air_result(s_hist.to, s_hist.cmdid, 202, s_hist.bearer);
        ESP_LOGI(TAG, "XPRS: history for %s - %d packet%s%s", s_hist.to, n,
                 n == 1 ? "" : "s", s_hist.more ? ", more held" : "");
        return;
    }

    if (s_hist.state != HIST_REPLAY) return;

    if (s_hist.i < s_hist.n) {
        /* Read it back now and air it byte for byte: the author's packet and
         * the author's signature (sections 25.2.1 and 36.2). Not re-signed, not
         * re-framed — a replay that rewrote anything would invalidate the very
         * thing it was replaying.
         *
         * On the STACK, not in BSS: every static byte is a byte the heap does
         * not have, and this task's 8 KB is sized for signing already. */
        xprsidx_rec_t rec;
        uint32_t want = s_hist.rec[s_hist.i++];
        if (xprsindex_get(s_xprs_index, want, &rec) && rec.len > 0) {
            xprs_air_on(rec.wire, rec.len, s_hist.bearer);
        }
        return;
    }
    s_hist.state = HIST_IDLE;
    xprs_air_result(s_hist.to, s_hist.cmdid, s_hist.more ? 206 : 200,
                    s_hist.bearer);
}

static void handle_xprs(const uint8_t *payload, int len, int rssi,
                        uint8_t subtype)
{
    /* Static: this only ever runs on the NimBLE host task. */
    static char buf[XPRS_MAX_WIRE + 1];
    static char rewired[XPRS_MAX_WIRE + 1];
    static xprs_t p;

    if (len <= 0 || len > XPRS_MAX_WIRE) return;
    memcpy(buf, payload, len);
    buf[len] = 0;
    if (!xprs_parse(buf, len, &p)) return;

    char type[16] = "", from[10] = "", to[12] = "";
    xprs_type(&p, type, sizeof type);
    xprs_get_str(&p, "f", from, sizeof from);
    xprs_get_str(&p, "d", to, sizeof to);
    if (!from[0]) return;                                  /* unattributable */
    if (strcasecmp(from, s_aprs_call) == 0) return;        /* our own echo */
    xdiag_park_parsed(&p, buf, len, "ble");

    xprs_update_maybe(&p, "ble");
    xprs_catchup_maybe(&p, 0);
    /* The screen's stores: devices, chat, rx/device stats (xprs_station,
     * shared with the m5stack). Cheap ring writes, so from this task too. */
    xst_ingest_parsed(&p, "ble", rssi);

    /* Keep it, and offer it to the other bearer. The index refuses what must
     * not be stored (ping/pong, duplicates) and holds mail privately; the LAN
     * bearer appends via:, honours the §13.1 hop budget, waits a random moment
     * and drops its copy if somebody else airs the packet first (§13.2.1). */
    if (s_xprs_index) {
        xprsindex_add(s_xprs_index, buf, len, rssi, false, (uint32_t)time(NULL));
    }
    xprslan_offer(buf, len);
    xprsnow_offer(buf, len);

    /* Dedup by the DERIVED identifier (§5), not by content: via: grows at
     * every hop, so the same packet has a different content hash at each —
     * and the same identifier at all of them. This is also what makes our
     * own relayed copy (and its echo off a phone) inert. */
    char id[XPRS_ID_LEN];
    xprs_id(&p, id);
    uint32_t idh = (uint32_t)strtoul(id, NULL, 16);
    /* If somebody has already relayed this packet, our queued copy is
     * pointless (§13.2.1). Checked ahead of the seen-ring so a repeat still
     * counts — but only when it carries a via:, because the origin saying it
     * again is a reason to digipeat rather than to stand down. */
    if (xprs_via_count(&p) > 0) relay_cancel(idh);
    if (xprs_seen(idh)) {
        /* Already handled once. The one thing a repeat sighting may do is
         * extend the digipeat — and only when it is the ORIGIN repeating
         * (no via:), only for something we actually repeated before, and
         * only inside the rate/lifetime bounds above. Advert rotation and
         * mesh echoes fall through all three gates and die here. */
        if (xprs_via_count(&p) != 0) return;   /* an echo, not the sender */
        xprs_digi_t *e = digi_find(idh);
        uint32_t now = now_sec();
        if (!e || e->times >= XPRS_DIGI_TIMES_MAX ||
            now - e->last_digi < XPRS_DIGI_REPEAT_SEC) return;
        int rl = xprs_append_via(buf, len, s_aprs_call, rewired, 249);
        if (rl <= 0) return;
        uint8_t ad[256];
        int an = build_ad(subtype, (const uint8_t *)rewired, rl, ad);
        if (an <= 0) return;
        relay_enqueue(ad, an);
        digi_record(idh, now);
        s_digi_repeats++;
        ESP_LOGI(TAG, "digipeat again: %s id=%s — the origin is still "
                 "transmitting (repeat %u)", type, id, (unsigned)e->times);
        return;
    }
    xprs_seen_remember(idh);

    ESP_LOGI(TAG, "RX XPRS %s %s>%s id=%s rssi=%d %dB",
             type, from, to[0] ? to : "*", id, rssi, len);

    /* The sender was just heard: deliver anything parked for it, and feed
     * the APRS-IS filter the same way the compact path does. */
    igate_heard_add(from, XPRS_BEARER_BLE);
    if (s_mesh_up) mesh_deliver_pending(from);

    /* ping: answer when it is for us or for anyone (§11.6). Protocol
     * machinery — never parked, never relayed (§6.5.1 bottom row). */
    if (strcmp(type, "command") == 0) {
        xprs_hist_accept((const char *)buf, len, &p, XPRS_BEARER_BLE);
        return;
    }
    if (strcmp(type, "identity") == 0) {
        xprs_identity_heard(&p);
        return;
    }
    if (strcmp(type, "ping") == 0) {
        if (!to[0] || strcasecmp(to, s_aprs_call) == 0) xprs_pong(from, rssi);
        return;
    }
    if (strcmp(type, "pong") == 0) return;   /* logged above; a measurement */

    /* receipt: r: names the delivered packet, s:ack|read means the target
     * has it — release every parked copy (§13.3). The receipt itself then
     * relays below: "a receipt is worth repeating even after the sender has
     * seen it", because it releases OTHER carriers too. */
    if (strcmp(type, "receipt") == 0) {
        char r[8] = "", s[24] = "";
        if (xprs_get_str(&p, "r", r, sizeof r) && strlen(r) == 6 &&
            xprs_get_str(&p, "s", s, sizeof s) &&
            (xprs_words_has(s, "ack") || xprs_words_has(s, "read"))) {
            int purged = blemesh_scf_ack(r);
            if (purged) ESP_LOGI(TAG, "SCF: receipt %s purged %d", r, purged);
        }
    }

    bool relay = true;

    /* observation: a beacon — the sighting above was its whole value. Its
     * readings (link:, rssi measured here) are local to the SENDER's spot;
     * relaying would assert reach the relayer has, not the sender. */
    if (strcmp(type, "observation") == 0) relay = false;

    if (strcmp(type, "message") == 0 && to[0]) {
        if (strcasecmp(to, s_aprs_call) == 0) {
            relay = false;                    /* delivered; nothing to extend */
        } else if (s_mesh_up && xprs_is_station(to, (int)strlen(to))) {
            /* Store-and-forward custody. scope:local is refused AT ADMISSION
             * (§13.11.3): parking now and airing later is carrying, which is
             * exactly what local excludes. (Relaying stays allowed — a re-air
             * on the same short-range bearer never leaves it.)
             * Urgency (§13.5): the sender states what it wants, the carrier
             * decides what it may have — a reachable target's mail may claim
             * any level, a stranger's is capped below urgent and defaults to
             * low when it states nothing (docs/store-and-forward.md §4). */
            if (!xprs_scope_local(&p)) {
                bool known = blemesh_reachable(to);
                int vl = 0;
                bool stated = xprs_get(&p, "urg", &vl) != NULL;
                int urg = xprs_urg(&p);
                if (!known)
                    urg = stated
                        ? (urg > XPRS_URG_HIGH ? XPRS_URG_HIGH : urg)
                        : XPRS_URG_LOW;
                if (blemesh_scf_offer(to, id, payload, len, now_sec(),
                                      (uint8_t)urg))
                    ESP_LOGI(TAG, "SCF: parked XPRS %dB for %s (id=%s urg=%d, %d held)",
                             len, to, id, urg, blemesh_scf_count());
            } else {
                ESP_LOGI(TAG, "SCF: refused scope:local %s -> %s", from, to);
            }
        }
    }

    /* Relay with the §13 discipline: append ourselves to via:. The codec
     * refuses when we are already in the path (§13.2), when the type's relay
     * budget is spent (§13.1: sos/warning 9, everything else 3), or when the
     * result would not fit one AD — all three mean "do not re-air". The
     * identifier and any sig: survive unchanged (§5, §9.1). */
    if (relay) {
        int rl = xprs_append_via(buf, len, s_aprs_call, rewired, 249);
        if (rl > 0) {
            uint8_t ad[256];
            int an = build_ad(subtype, (const uint8_t *)rewired, rl, ad);
            if (an > 0) {
                relay_enqueue_id(ad, an, idh);
                s_relayed_count++;
                /* Remember WHAT we repeated: only these ids may be repeated
                 * again when the origin keeps transmitting (above). */
                digi_record(idh, now_sec());
                ESP_LOGI(TAG, "relayed XPRS %s id=%s +via:%s (#%u)",
                         type, id, s_aprs_call, (unsigned)s_relayed_count);
            }
        }
    }
}

/* Our XPRS presence beacon (§10.6, same fields and order as the phone's
 * mesh_service.dart): who we are, on which bearer, how many neighbours, and
 * how much mail we hold — mail:N is what invites a neighbour that can reach
 * the recipient to dial a custody session. Zero-valued fields are omitted. */
static void xprs_beacon_air(void)
{
    /* 248 is the ceiling build_ad() will accept in one AD (254 minus its own
     * six-byte header), and a wire longer than that is aired NOWHERE — the
     * builder returns 0 and the beacon disappears without a word. */
    char wire[249];
    int n = snprintf(wire, sizeof wire, "t:observation f:%s link:ble",
                     s_aprs_call[0] ? s_aprs_call : "TDONGLE");
    /* Who we hear on THIS radio, and how many that is in full (§10.6.4). The
     * mesh neighbour count is a different table (DV route beacons, subtype
     * 0x4D) and a station can be in one and not the other, so `peers:` comes
     * from the same place `hears:` does or the two contradict each other. */
    int total = 0;
    char hears[120];
    int room = (int)sizeof wire - n
             - 24
             - (s_xprs_can_sign ? 5 + XPRSSIG_B85_LEN : 0)
             - 48;                    /* uptime:/lifetime:/mail: still to come */
    if (room > (int)sizeof hears) room = (int)sizeof hears;
    int hn = (room > 1) ? xprs_hears_render(XPRS_BEARER_BLE, hears, room, &total)
                        : 0;
    int peers = total > 0 ? total : blemesh_neighbor_count();
    if (peers > 0 && n < (int)sizeof wire)
        n += snprintf(wire + n, sizeof wire - n, " peers:%d", peers);
    if (hn > 0 && n < (int)sizeof wire)
        n += snprintf(wire + n, sizeof wire - n, " hears:%s", hears);
    int mail = blemesh_scf_count();
    if (mail > 0 && n < (int)sizeof wire)
        n += snprintf(wire + n, sizeof wire - n, " mail:%d", mail);
    /* Stability account (§10.5): how long this run, how long in total. */
    char up[16], life[16];
    xprs_fmt_duration(now_sec(), up, sizeof up);
    xprs_fmt_duration(s_life_base + now_sec(), life, sizeof life);
    if (n < (int)sizeof wire)
        n += snprintf(wire + n, sizeof wire - n, " uptime:%s lifetime:%s",
                      up, life);
    if (n <= 0 || n >= (int)sizeof wire) return;
    n = xprs_sign_wire(wire, n, (int)sizeof wire);
    uint8_t ad[256];
    int an = build_ad(SUBTYPE_XPRS, (const uint8_t *)wire, n, ad);
    if (an > 0) air_raw_ad(ad, an);
}

/* ---- street mesh (aurora docs/mesh.md): beacon + DV + SCF ----------------- */

/* Re-air every parked frame for [target] (it was just seen). Each goes back on
 * the normal relay rotation as a plain 0x41 broadcast; the receiver dedups.
 * An XPRS frame goes out with our callsign appended to via: (§13.3 — a
 * carrier appends itself when it finally transmits); when the append is
 * refused (we are already in the path, the budget is spent, or it would not
 * fit) the frame airs UNMODIFIED — delivery to a sighted target outranks the
 * relay budget, which governs relays, not the final handover. */
static void mesh_deliver_pending(const char *target)
{
    static uint8_t frames[4][BLEMESH_SCF_FRAME_MAX];
    static int lens[4];
    int n = blemesh_scf_pop_for(target, now_sec(), frames, lens, 4);
    for (int i = 0; i < n; i++) {
        const uint8_t *out = frames[i];
        int olen = lens[i];
        static char rewired[XPRS_MAX_WIRE + 1];
        if (xprs_looks_like(frames[i], lens[i])) {
            int rl = xprs_append_via((const char *)frames[i], lens[i],
                                     s_aprs_call, rewired, 249);
            if (rl > 0) { out = (const uint8_t *)rewired; olen = rl; }
            /* Re-remember the identifier: the original sighting may be past
             * the dedup window, and the echo of this re-air must not read as
             * a fresh packet. */
            char id[XPRS_ID_LEN];
            if (xprs_id_of((const char *)out, olen, id))
                xprs_seen_remember((uint32_t)strtoul(id, NULL, 16));
        }
        uint8_t ad[256];
        int an = build_aprs_ad(out, olen, ad);
        if (an > 0) { relay_enqueue(ad, an); s_relayed_count++; }
    }
    if (n > 0)
        ESP_LOGI(TAG, "SCF: %s back in range -> re-airing %d parked frame(s)", target, n);
}

/* A phone's (or another dongle's) route beacon: learn it, and treat the sender
 * as "in range" for parked mail. */
static void handle_mesh(const uint8_t *payload, int len, int rssi)
{
    if (!s_mesh_up) return;
    blemesh_beacon_t b;
    if (!blemesh_beacon_decode(payload, len, &b)) return;
    bool changed = blemesh_table_ingest(&b, rssi, now_sec());
    if (changed) {
        s_mesh_dirty = true;
        ESP_LOGI(TAG, "mesh: %s (%s%s, %ddBm, reaches %d) — %d neighbor(s)",
                 b.callsign,
                 b.dev_class == BLEMESH_CLASS_PHONE ? "phone" :
                 b.dev_class == BLEMESH_CLASS_ESP32 ? "esp32" : "node",
                 b.powered ? ", powered" : "", rssi, b.dv_count,
                 blemesh_neighbor_count());
    }
    mesh_deliver_pending(b.callsign);
}

/* Build + air our route beacon: class esp32, always powered + stationary (a
 * plugged dongle is the street's natural base station), storage headroom from
 * the SD card, DV digest from the table. */
static void mesh_beacon_air(void)
{
    if (!s_mesh_up) return;
    blemesh_beacon_t b = {0};
    snprintf(b.callsign, sizeof(b.callsign), "%s",
             s_aprs_call[0] ? s_aprs_call : "TDONGLE");
    b.dev_class = BLEMESH_CLASS_ESP32;
    b.powered = true;
    b.uptime_bucket = blemesh_uptime_bucket(now_sec());
    b.mobility = 1;                       /* stationary */
    b.storage_bucket = sdcard_is_mounted() ? 3 : 0;
    b.dv_count = (uint8_t)blemesh_table_export(b.dv, 48);
    /* M2 trailer: invite dial-ins while we carry mail/files (we cannot dial). */
    /* GATT is retired with the move to tinynimble, so there is no bulk spool
     * and no session to dial: mail still counts, files no longer exist here. */
    int pm = blemesh_scf_count();
    b.pending_msgs = (uint8_t)(pm > 255 ? 255 : pm);
    b.pending_bulk = 0;

    uint8_t payload[200];
    int pn = blemesh_beacon_encode(&b, payload, sizeof(payload));
    if (pn <= 0) return;
    uint8_t ad[256];
    int n = 0;
    ad[n++] = 0;
    ad[n++] = 0xFF;
    ad[n++] = COMPANY_LO;
    ad[n++] = COMPANY_HI;
    ad[n++] = MARKER;
    ad[n++] = SUBTYPE_MESH;
    memcpy(ad + n, payload, pn); n += pn;
    ad[0] = n - 1;
    air_raw_ad(ad, n);
}

/* Owns ext-adv instance 0: rotates between queued relays and our own announce. */
static void relay_task(void *arg)
{
    (void)arg;
    static uint8_t pick[256];
    /* Created first, run last. The stack comes out of a heap nobody has touched
     * yet; the work waits for ble_bring_up() to release it. */
    while (!s_relay_may_run) vTaskDelay(pdMS_TO_TICKS(50));
    announce("tdongle-s3 online", 17);   /* configures instance 0 + first announce */
    uint32_t last_own = now_sec();
    uint32_t last_sweep = now_sec();
    uint32_t last_beacon = 0;
    int tick = 0;
    int own_rot = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        s_relay_ticks++;
        /* An install, if one was asked for: minutes of flash work on the
         * one task here with a big stack, on core 1, off the radios. */
        xprs_update_answer();   /* the verify and the answer, on core 1 */
        xdiag_pump((uint32_t)(esp_timer_get_time() / 1000));
        xota_poll();
        /* Rollback self-test (25.8): two minutes of the API listening, a
         * bearer up and no panic behind us, then this image is trusted. */
        if (s_relay_ticks == 80) {
#ifdef XOTA_FAIL_SELFTEST
            /* A build that condemns itself on purpose. The rollback path is
             * the only part of the updater that cannot be exercised by a
             * working image, and it is the part that decides whether a bad
             * push means a ladder. Build one of these, sign it, install it,
             * and watch the previous image come back by itself:
             *     pio run -e rns_ble5 --build-flag=-DXOTA_FAIL_SELFTEST
             * Never ship it -- the version string should say so too. */
            ESP_LOGE(TAG, "XOTA_FAIL_SELFTEST: condemning this image");
            xota_mark_unhealthy();
#else
            if (s_api && (xprslan_is_active() || s_mesh_up) &&
                esp_reset_reason() != ESP_RST_PANIC) xota_mark_healthy();
            else xota_mark_unhealthy();
#endif
        }
        uint32_t t = now_sec();
        xprs_catchup_air(); /* parked 36.10 ask: signed on THIS stack */

        /* A hub learns nothing about a station that has not spoken since it
         * connected, so a fresh socket asks for an announce here — on the task
         * that owns the signing buffers. */
        /* Say we are an indexer, on both bearers. The first goes out shortly
         * after boot rather than a full period later: a station that has just
         * come up is exactly the one its neighbours have not heard of. */
        bool service_due = (s_last_service == 0)
                               ? (t >= XPRS_SERVICE_FIRST_SEC)
                               : (t - s_last_service >= XPRS_SERVICE_EVERY_SEC);
        if (service_due) {
            s_last_service = t;
            /* Identity first: a peer that hears "I am an indexer" a moment
             * before it can check the signature has to keep the claim around
             * and re-judge it later. */
            xprs_identity_air();
            xprs_service_air();
        }

        /* Once, early: read our stored identities back into the key table, on
         * a task with the stack for the curve work that verifying them takes. */
        if (s_keys_reload_due) {
            s_keys_reload_due = false;
            xprs_keys_reload();
        }

        /* One packet of a cmd:history replay per tick — 1500 ms between
         * packets, which is the pacing section 31.4 asks for and the period
         * this loop already runs at. */
        xprs_hist_pump();

        /* §23.7's deadline. Cheap, and the one thing that guarantees a station
         * that moved comes back. */
        xprschan_tick();

        if (s_hub_announce_pending) {
            s_hub_announce_pending = false;
            announce("tdongle-s3 online", 17);
        }

        /* Housekeeping: age out dead neighbors/routes + expired parked mail. */
        if (t - last_sweep >= 60) {
            last_sweep = t;
            blemesh_table_sweep(t);
            blemesh_scf_sweep(t);
        }
        /* lifetime: accumulate service time into NVS every 15 min (§10.5).
         * A power pull loses at most that tail; ~96 writes/day is nothing to
         * a wear-leveled NVS partition. */
        static uint32_t last_life_save;
        if (t - last_life_save >= LIFE_SAVE_SEC) {
            last_life_save = t;
            nvs_handle_t h;
            if (nvs_open("rns", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u32(h, "lifesec", s_life_base + t);
                nvs_commit(h);
                nvs_close(h);
            }
        }
        /* Scan watchdog (same lesson as the phones): a controller that has
         * delivered nothing for 60 s gets its discovery torn down and
         * re-armed. A healthy desk hears street traffic well within that. */
        static uint32_t last_scan_kick;
        if (t - (s_last_disc ? s_last_disc : 0) > 60 &&
            t - last_scan_kick > 60) {
            last_scan_kick = t;
            ESP_LOGW(TAG, "scan silent %lus (disc=%lu) - restarting discovery",
                     (unsigned long)(t - s_last_disc),
                     (unsigned long)s_disc_count);
            tn_scan_stop();
            start_scan();
        }
        /* Triggered update: topology changed -> beacon early (light debounce
         * via the 1.5 s loop period), same as the phones. */
        if (s_mesh_dirty && t - last_beacon >= 4) {
            s_mesh_dirty = false;
            last_beacon = t;
            mesh_beacon_air();
            continue;
        }

        /* Our own frames get a GUARANTEED slot every 8 s — a busy street keeps
         * the relay queue non-empty for minutes at a time, and a beacon that
         * only airs when idle is never heard (the phones then never learn we
         * exist, so no routes ever point through us). Rotate the mesh route
         * beacon, the XPRS presence beacon (the readable half of discovery:
         * phones' XprsMonitor + peer sighting) and the signed RNS announce —
         * each airs every ~24 s; relays fill every other slot. */
        if (t - last_own >= 8) {
            own_rot = (own_rot + 1) % 3;
            if (own_rot == 0 && s_mesh_up) {
                mesh_beacon_air();
                last_beacon = t;
            } else if (own_rot == 1) {
                xprs_beacon_air();
            } else {
                char msg[48];
                int l = snprintf(msg, sizeof(msg), "tdongle-s3 #%d", ++tick);
                announce(msg, l);         /* keep our own announce fresh (re-signs) */
            }
            last_own = t;
            continue;
        }
        int n = relay_pick(pick);
        if (n > 0) {
            air_raw_ad(pick, n);          /* re-air a relayed packet */
        }
    }
}

static void start_scan(void)
{
    tn_scan_cfg_t cfg = {
        .own_addr_type = s_own_addr_type,
        .passive       = 1,          /* XPRS never scan-requests */
        .itvl          = 0x0060,     /* ~83% duty, unchanged */
        .window        = 0x0050,
        .phy           = TN_PHY_1M,
    };
    esp_err_t err = tn_scan_start(&cfg, tn_report, NULL);
    if (err != ESP_OK) ESP_LOGE(TAG, "tn_scan_start: %s", esp_err_to_name(err));
    else ESP_LOGI(TAG, "extended scanning...");
}

/* ---- the screen: three views, rotating hands-off ------------------------- */
/* Devices -> Stats -> Chat on a 10 s dwell (xprs_ui_mini). The one button
 * (BOOT, GPIO0) is barely reachable in most mountings, so the tour runs by
 * itself: a short press advances now, a long press (>= 700 ms) freezes the
 * current view until the next long press. The console offers the same:
 * `view <1..3>` and `dump` (a FRAMEDUMP screenshot).
 *
 * name = the callsign a Reticulum announce advertised in its plaintext
 * app_data; those sightings are not XPRS wires, so they enter the devices
 * list through xst_dev_note rather than the ingest path. */
typedef struct {
    char name[CALLSIGN_MAX];
    uint8_t prefix[4];
} ui_msg_t;
static QueueHandle_t s_ui_q;

/* T-Dongle-S3 pushbutton = the BOOT strap pin (GPIO0, active low; no BTN_* in
 * xprs_model_tdongle_s3 -- the board has no other button). */
#define UI_BTN_GPIO    GPIO_NUM_0
#define UI_INRANGE_SEC 300          /* "in reach" = heard in the last 5 min */
#define UI_DWELL_MS    10000        /* per-view stop on the rotating tour */

static volatile int s_ui_force = -1;   /* console `view <n>`: 0..2, -1 idle */

/* Called from the NimBLE host task -- only enqueues (LVGL is single-task). */
static void ui_log_packet(const uint8_t *dest_hash, int hops, int rssi,
                          const char *name)
{
    (void)hops; (void)rssi;
    if (!s_ui_q) return;
    ui_msg_t m;
    if (name && name[0]) {
        snprintf(m.name, sizeof(m.name), "%s", name);
    } else {
        hexn(dest_hash, 4, m.name);
    }
    memcpy(m.prefix, dest_hash, 4);
    xQueueSend(s_ui_q, &m, 0);   /* drop if full; the next announce refreshes */
}

/* Rebuild the visible view from the xprs_station snapshots. ui_task only. */
static void ui_render(void)
{
    /* TX totals into the stats rings, whichever view is up: what the radio
     * finished with on each bearer, plus what this station relayed. */
    {
        uint32_t nissued = 0, ndone = 0, nfail = 0;
        uint32_t lrx = 0, ltx = 0, lcancel = 0;
        xprsnow_tx_stats(&nissued, &ndone, &nfail);
        xprslan_stats(&lrx, &ltx, &lcancel);
        xst_tx_total(ndone + ltx + s_relayed_count);
    }

    /* Street-mesh beacon neighbours are sightings too. */
    uint32_t now = now_sec();
    for (int i = 0; i < blemesh_neighbor_count(); i++) {
        const blemesh_neighbor_t *nb = blemesh_neighbor_at(i);
        if (!nb || now - nb->last_heard >= UI_INRANGE_SEC) continue;
        xst_dev_note(nb->callsign, "ble", 0);
    }

    switch (xum_view()) {
    case XUM_VIEW_DEVICES: {
        xst_dev_t devs[XUM_DEV_ROWS];
        xum_dev_t rows[XUM_DEV_ROWS];
        uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        int n = xst_devices(devs, XUM_DEV_ROWS, UI_INRANGE_SEC);
        for (int i = 0; i < n; i++) {
            snprintf(rows[i].call, sizeof rows[i].call, "%s", devs[i].call);
            snprintf(rows[i].bearer, sizeof rows[i].bearer, "%s",
                     devs[i].bearer);
            rows[i].dist_m = devs[i].rssi
                ? (int)(xst_est_distance_m(devs[i].rssi) + 0.5f) : -1;
            rows[i].age_s = (int)((now_ms - devs[i].last_ms) / 1000);
        }
        xum_devices(rows, n);
        break;
    }
    case XUM_VIEW_STATS: {
        uint16_t dev[XUM_STATS_POINTS], rxv[XUM_STATS_POINTS],
                 txv[XUM_STATS_POINTS];
        int np = xst_stats_series(1, dev, rxv, txv, XUM_STATS_POINTS);
        xum_stats(dev, rxv, txv, np, "hour");
        break;
    }
    default: {  /* chat */
        xst_chat_t rows[XUM_CHAT_ROWS];
        xum_chat_t out[XUM_CHAT_ROWS];
        int n = xst_chat(rows, XUM_CHAT_ROWS);
        for (int i = 0; i < n; i++) {
            snprintf(out[i].from, sizeof out[i].from, "%s", rows[i].from);
            snprintf(out[i].text, sizeof out[i].text, "%.60s", rows[i].text);
            out[i].kind = rows[i].kind;
        }
        xum_chat(out, n);
        break;
    }
    }
    xum_set_count(xst_devices_in_range(UI_INRANGE_SEC));
}

/* Owns ALL LVGL calls: drains the announce queue into the devices list,
 * polls the button, rotates the tour and refreshes the visible view. */
static void ui_task(void *arg)
{
    (void)arg;
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << UI_BTN_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    int64_t last_render_us = 0, dwell_at_us = esp_timer_get_time();
    int64_t press_us = 0;
    bool pressed = false, long_fired = false, held = false, dirty = true;
    for (;;) {
        ui_msg_t m;
        while (xQueueReceive(s_ui_q, &m, 0) == pdTRUE)
            xst_dev_note(m.name, "ble", 0);

        int64_t now_us = esp_timer_get_time();

        /* Button (~10 ms poll): release before 700 ms = advance the tour
         * now; holding past 700 ms = freeze/unfreeze the current view. */
        if (gpio_get_level(UI_BTN_GPIO) == 0) {
            if (!pressed) { pressed = true; long_fired = false;
                            press_us = now_us; }
            else if (!long_fired && now_us - press_us >= 700000) {
                long_fired = true;
                held = !held;
                xum_set_held(held);
                dirty = true;
            }
        } else if (pressed) {
            pressed = false;
            if (!long_fired && now_us - press_us >= 30000) {
                xum_show((xum_view() + 1) % XUM_VIEW_COUNT);
                dwell_at_us = now_us;
                dirty = true;
            }
        }

        /* Console `view <n>` jumps the tour and resets the dwell. */
        int forced = s_ui_force;
        if (forced >= 0) {
            s_ui_force = -1;
            xum_show(forced % XUM_VIEW_COUNT);
            dwell_at_us = now_us;
            dirty = true;
        }

        /* The tour: next view every UI_DWELL_MS unless held. */
        if (!held && now_us - dwell_at_us >= (int64_t)UI_DWELL_MS * 1000) {
            xum_show((xum_view() + 1) % XUM_VIEW_COUNT);
            dwell_at_us = now_us;
            dirty = true;
        }

        if (dirty || now_us - last_render_us >= 2000000) {
            dirty = false;
            last_render_us = now_us;
            ui_render();
        }
        xum_update();
        /* At the default 100 Hz tick pdMS_TO_TICKS(5) rounds to 0 ticks, so
         * vTaskDelay would never block and this task would starve IDLE0 (task
         * watchdog). Always delay at least one tick so the idle task can run. */
        TickType_t d = pdMS_TO_TICKS(10);
        vTaskDelay(d ? d : 1);
    }
}

/*
 * With tinynimble there is no host task and no sync callback: the controller
 * is up when tn_start() returns, so this is called directly rather than waited
 * for. Kept as a function because two paths need it -- first boot, and coming
 * back from the ESP-NOW working-channel exchange.
 */
static void ble_bring_up(void)
{
    /* One address for the life of the station. Recreating the advertising set
     * would rotate it and fragment every peer's address book. */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    mac[0] |= 0xC0;                     /* static random */
    tn_set_random_addr(mac);
    s_own_addr_type = 0x01;

    start_scan();
    xh_set(XH_BLE, true);
    s_relay_may_run = true;
}


/*
 * Take Bluetooth off the air for the length of a section 23.7 exchange, and put
 * it back afterwards.
 *
 * This is not a power optimisation and it is not optional. Measured on
 * esp32/espnow_probe, one variable at a time: with the BLE controller running,
 * a WiFi station that is NOT ASSOCIATED receives nothing at all -- while
 * transmitting perfectly, which is why it read for so long as the far side
 * going quiet. Cancelling the scan does not give the radio back; only taking
 * the controller down does. An associated station with the same controller up
 * is fine, because association is what keeps the WiFi side scheduled: it has
 * beacons it may not miss.
 *
 * Moving to a working channel means leaving the access point. So for the length
 * of the move this station is not a Bluetooth station, and the mesh treats that
 * as the ordinary absence section 23.7 already describes -- bounded by the same
 * local deadline that guarantees the return, because a station that fails to
 * come back does not lose a bearer, it loses all of them.
 */
static void ble_stack_down(void)
{
    if (!s_ble_up) return;
    s_ble_up = false;               /* first: stop anyone else airing into it */
    tn_scan_stop();
    tn_stop();                      /* gives the controller's DRAM back too */
    s_adv_configured = false;       /* the set is gone with the controller */
    ESP_LOGW(TAG, "Bluetooth off for the exchange, heap %u",
             (unsigned)esp_get_free_heap_size());
}

static void ble_stack_up(void)
{
    if (s_ble_up) return;
    esp_err_t err = tn_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "tn_start failed on the way back (%s) -- this station "
                      "has lost Bluetooth until it reboots", esp_err_to_name(err));
        return;
    }
    s_ble_up = true;
    ble_bring_up();                 /* address, scan, relay release */
    ESP_LOGW(TAG, "Bluetooth back, heap %u",
             (unsigned)esp_get_free_heap_size());
}


/* ---- APRS-IS iGate (WiFi STA -> APRS-IS, gating BLE5 APRS both ways) ----- */

static char s_aprs_call[10];     /* station callsign (X3xxxx) used with APRS-IS */

/* Callsigns heard on this station's own radios — the iGate filters APRS-IS for
 * traffic to these (and relays such traffic back down), and it is also what
 * `hears:` publishes (§10.6.3). Touched by the NimBLE host task
 * (igate_heard_add) and the aprsis task (igate_get_heard) → mutex-guarded.
 *
 * The last-heard time is kept PER BEARER, not once. §10.6.1 is explicit that a
 * reading belongs to the bearer it names: a `link:lan` beacon claiming to hear
 * a station that only ever spoke over Bluetooth would be a false statement
 * about the wire, and the station on the other end would draw a map from it.
 * `t` stays "heard on anything", which is the question the APRS-IS filter asks. */
#define IG_HEARD_MAX 24
static struct {
    char     call[8];
    uint32_t t;          /* heard on ANY local bearer */
    uint32_t t_ble;
    uint32_t t_lan;
    uint32_t t_now;      /* ESP-NOW */
} s_ig_heard[IG_HEARD_MAX];
static SemaphoreHandle_t s_ig_heard_mtx;

static void igate_heard_add(const char *call, uint8_t bearer)
{
    if (!s_ig_heard_mtx || !call) return;
    char c[8]; int n = 0;                     /* normalise: upper, strip -SSID */
    for (const char *p = call; *p && *p != '-' && n < 7; p++) {
        char u = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
        if ((u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9')) c[n++] = u;
        else break;
    }
    c[n] = 0;
    if (n < 3) return;                         /* too short to be a callsign */
    xSemaphoreTake(s_ig_heard_mtx, portMAX_DELAY);
    uint32_t t = now_sec();
    int slot = -1, oldest = 0;
    for (int i = 0; i < IG_HEARD_MAX; i++) {
        if (strcmp(s_ig_heard[i].call, c) == 0) { slot = i; break; }
        if (s_ig_heard[i].t == 0) { slot = i; break; }
        if (s_ig_heard[i].t < s_ig_heard[oldest].t) oldest = i;
    }
    if (slot < 0) slot = oldest;
    if (strcmp(s_ig_heard[slot].call, c) != 0) {
        /* A new tenant of this slot inherits none of the old one's history. */
        s_ig_heard[slot].t_ble = 0;
        s_ig_heard[slot].t_lan = 0;
        s_ig_heard[slot].t_now = 0;
    }
    strncpy(s_ig_heard[slot].call, c, sizeof s_ig_heard[slot].call - 1);
    s_ig_heard[slot].call[sizeof s_ig_heard[slot].call - 1] = 0;
    if (!t) t = 1;
    s_ig_heard[slot].t = t;
    if (bearer == XPRS_BEARER_BLE)      s_ig_heard[slot].t_ble = t;
    else if (bearer == XPRS_BEARER_LAN) s_ig_heard[slot].t_lan = t;
    else if (bearer == XPRS_BEARER_NOW) s_ig_heard[slot].t_now = t;
    xSemaphoreGive(s_ig_heard_mtx);
}

/* How long a station stays in `hears:` without saying anything. Long enough to
 * span the slowest beacon on either bearer (the LAN's is 300 s), short enough
 * that the claim is about now. */
#define XPRS_HEARS_TTL_SEC 600

/* Who this station hears DIRECTLY on [bearer], most recent first — the value of
 * `hears:` (§10.6.3), with no key and no trailing comma.
 *
 * The iGate's heard ring is exactly the right table to build this from: every
 * entry got there because a packet arrived on one of our own radios, so
 * "directly heard" holds by construction rather than by a rule somebody has to
 * remember. It is also, precisely, what the gateway itself believes about who
 * is on the air here — which is the thing a station asking "can you reach X"
 * wants to know.
 *
 * Writes as many callsigns as fit in [cap] and reports the FULL fresh count in
 * [total]: §10.6.4 makes `peers:` the true total even when the list is cut,
 * because without it a short list cannot be told from a small network.
 */
static int xprs_hears_render(uint8_t bearer, char *out, int cap, int *total)
{
    if (total) *total = 0;
    if (!out || cap <= 0) return 0;
    out[0] = 0;
    if (!s_ig_heard_mtx) return 0;

    typedef struct { char call[8]; uint32_t t; } heard_t;
    heard_t snap[IG_HEARD_MAX];
    int n = 0;
    uint32_t now = now_sec();

    xSemaphoreTake(s_ig_heard_mtx, portMAX_DELAY);
    for (int i = 0; i < IG_HEARD_MAX; i++) {
        /* Per bearer, and NOT with a default that silently answers for another
         * one: a `link:espnow` beacon that listed the stations heard over
         * Bluetooth is a false statement about that radio, which is the whole
         * thing §10.6.1 exists to prevent. It shipped that way for one build. */
        uint32_t t;
        switch (bearer) {
            case XPRS_BEARER_LAN: t = s_ig_heard[i].t_lan; break;
            case XPRS_BEARER_NOW: t = s_ig_heard[i].t_now; break;
            case XPRS_BEARER_BLE: t = s_ig_heard[i].t_ble; break;
            default: continue;                /* an unknown bearer hears nobody */
        }
        if (!t || (now - t) >= XPRS_HEARS_TTL_SEC) continue;
        memcpy(snap[n].call, s_ig_heard[i].call, sizeof snap[n].call);
        snap[n].t = t;
        n++;
    }
    xSemaphoreGive(s_ig_heard_mtx);

    if (total) *total = n;
    if (n == 0) return 0;

    /* Most relevant first (§10.6.3), and for a gateway relevant means most
     * recently heard: a station that spoke a minute ago is likelier to answer
     * than one that spoke nine. Insertion sort — n is at most IG_HEARD_MAX. */
    for (int i = 1; i < n; i++) {
        for (int j = i; j > 0 && snap[j].t > snap[j - 1].t; j--) {
            heard_t tmp = snap[j];
            snap[j] = snap[j - 1];
            snap[j - 1] = tmp;
        }
    }

    int len = 0;
    for (int i = 0; i < n; i++) {
        int cl = (int)strlen(snap[i].call);
        if (len + (len ? 1 : 0) + cl >= cap) break;   /* keep room for the NUL */
        if (len) out[len++] = ',';
        memcpy(out + len, snap[i].call, (size_t)cl);
        len += cl;
        out[len] = 0;
    }
    return len;
}

/* aprsis hook: fill calls[][8] with up to [max] callsigns heard within [age]. */
static int igate_get_heard(char calls[][8], int max, uint32_t max_age_sec)
{
    if (!s_ig_heard_mtx) return 0;
    int out = 0;
    uint32_t t = now_sec();
    xSemaphoreTake(s_ig_heard_mtx, portMAX_DELAY);
    for (int i = 0; i < IG_HEARD_MAX && out < max; i++) {
        if (s_ig_heard[i].t && (t - s_ig_heard[i].t) < max_age_sec) {
            strncpy(calls[out], s_ig_heard[i].call, 7);
            calls[out][7] = 0;
            out++;
        }
    }
    xSemaphoreGive(s_ig_heard_mtx);
    return out;
}

/* aprsis hook (downlink): re-air an APRS frame from the Internet over BLE5 so
 * local phones receive it. Built as the same from<0x1F>to<0x1F>text parcel and
 * content-remembered so we don't re-gate our own downlink back up (loop guard). */
static bool igate_relay(const char *from, const char *to, const char *text)
{
    uint8_t pl[300];
    int n = 0;
    for (const char *p = from; *p && n < 280; p++) pl[n++] = (uint8_t)*p;
    pl[n++] = 0x1F;
    for (const char *p = to; *p && n < 290; p++) pl[n++] = (uint8_t)*p;
    pl[n++] = 0x1F;
    for (const char *p = text; *p && n < 299; p++) pl[n++] = (uint8_t)*p;

    uint8_t ad[256];
    int adn = build_aprs_ad(pl, n, ad);
    if (adn <= 0) return false;
    relay_remember(fnv1a(pl, n));       /* loop guard: ignore our own downlink on RX */
    relay_enqueue(ad, adn);
    s_relayed_count++;
    return true;
}

/* The base36 derivation earlier builds used — kept ONLY so provisioning can
 * recognise (and replace) a stored callsign that was auto-derived by it. Its
 * alphabet is wrong per XPRS.md §3: an XPRS callsign's four characters come
 * from the bech32 charset, where b, i, o and 1 never appear. */
static void derive_x3_base36(char *out, int cap)
{
    static const char B36[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v = (v << 8) | s_id_hash[i];
    if (cap < 7) { out[0] = 0; return; }
    out[0] = 'X'; out[1] = '3';
    for (int i = 0; i < 4; i++) { out[2 + i] = B36[v % 36]; v /= 36; }
    out[6] = 0;
}

/* Derive the station's X3 callsign (XPRS.md §3): X3 + the first 20 bits of
 * the signing public key through the bech32 charset, uppercased — the same
 * arithmetic as the phone's X1 (nostr_key_generator.dart: the first four data
 * characters of the key's bech32 form). */
/* The pre-NOSTR form: an X3 callsign from the RETICULUM key. Kept only so a
 * station that was given one can be recognised and migrated below. */
static void derive_x3_from_ed(char *out, int cap)
{
    static const char CS[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    if (cap < 7) { if (cap > 0) out[0] = 0; return; }
    uint32_t bits = ((uint32_t)s_ed_pk[0] << 12) |
                    ((uint32_t)s_ed_pk[1] << 4) |
                    ((uint32_t)s_ed_pk[2] >> 4);         /* 20 bits */
    out[0] = 'X'; out[1] = '3';
    for (int i = 0; i < 4; i++) {
        char c = CS[(bits >> (15 - 5 * i)) & 31];
        out[2 + i] = (char)(c >= 'a' ? c - 32 : c);      /* uppercase letters */
    }
    out[6] = 0;
}

/* §3: an X3 callsign is derived from the NPUB — the key that signs — which is
 * what lets a receiver re-derive it and see that callsign and signature belong
 * together. xprs_nostr already does this (nostr_keys_derive_callsign); the
 * Reticulum-key form below is only a fallback for a station with no NOSTR key. */
static void derive_x3_callsign(char *out, int cap)
{
    if (s_xprs_can_sign && nostr_keys_get_callsign()[0]) {
        snprintf(out, (size_t)cap, "%s", nostr_keys_get_callsign());
        return;
    }
    derive_x3_from_ed(out, cap);
}

/* First-boot provisioning: write WiFi creds + callsign into NVS if absent, then
 * load the callsign into s_aprs_call. NVS is the source of truth thereafter. */
static void igate_provision(void)
{
    nvs_handle_t h;
    /* WiFi creds in the namespace xprs_wifi reads ("wifi_config"). */
    if (nvs_open("wifi_config", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = 0;
        bool have = (nvs_get_str(h, "ssid", NULL, &len) == ESP_OK && len > 1);
        if (!have && IGATE_WIFI_SSID[0]) {
            nvs_set_str(h, "ssid", IGATE_WIFI_SSID);
            nvs_set_str(h, "password", IGATE_WIFI_PASSWORD);
            nvs_commit(h);
            ESP_LOGI(TAG, "provisioned WiFi credentials into NVS");
        }
        nvs_close(h);
    }
    /* Callsign in the RNS namespace ("rns"/"aprs_call"). */
    s_aprs_call[0] = 0;
    if (nvs_open("rns", NVS_READWRITE, &h) == ESP_OK) {
        size_t len = sizeof s_aprs_call;
        if (!(nvs_get_str(h, "aprs_call", s_aprs_call, &len) == ESP_OK && s_aprs_call[0])) {
            if (IGATE_CALLSIGN[0])
                snprintf(s_aprs_call, sizeof s_aprs_call, "%s", IGATE_CALLSIGN);
            else
                derive_x3_callsign(s_aprs_call, sizeof s_aprs_call);
            nvs_set_str(h, "aprs_call", s_aprs_call);
            nvs_commit(h);
        } else {
            /* Migration: a stored callsign equal to the old base36 derivation
             * was auto-derived, not operator-chosen — replace it with the
             * bech32 form XPRS.md §3 requires. Provisioned callsigns differ
             * from the derivation and are left alone. */
            char old[10];
            derive_x3_base36(old, sizeof old);
            /* An X3 callsign that does not derive from this station's key is
             * not this station's name (§3): a receiver re-deriving it from the
             * announced npub gets something else, so every signed packet would
             * read as a claim to a callsign we cannot substantiate. Earlier
             * derivations — base36, and one from the Reticulum key — are both
             * fossils of that kind, and rotating the Reticulum identity orphans
             * the second. An operator-provisioned callsign (IGATE_CALLSIGN) is
             * never touched, and neither is anything that is not an X3. */
            bool x3 = (s_aprs_call[0] == 'X' && s_aprs_call[1] == '3');
            if (x3 && IGATE_CALLSIGN[0] == 0 && s_xprs_can_sign &&
                nostr_keys_get_callsign()[0] &&
                strcmp(s_aprs_call, nostr_keys_get_callsign()) != 0) {
                ESP_LOGW(TAG, "callsign migrated %s -> %s (derived from the "
                              "signing key, XPRS.md §3)",
                         s_aprs_call, nostr_keys_get_callsign());
                snprintf(s_aprs_call, sizeof s_aprs_call, "%s",
                         nostr_keys_get_callsign());
                nvs_set_str(h, "aprs_call", s_aprs_call);
                nvs_commit(h);
            } else if (strcmp(s_aprs_call, old) == 0) {
                derive_x3_callsign(s_aprs_call, sizeof s_aprs_call);
                nvs_set_str(h, "aprs_call", s_aprs_call);
                nvs_commit(h);
                ESP_LOGI(TAG, "callsign migrated %s -> %s (XPRS bech32 alphabet)",
                         old, s_aprs_call);
            }
        }
        nvs_close(h);
    }
    if (!s_aprs_call[0]) derive_x3_callsign(s_aprs_call, sizeof s_aprs_call);
}

/* Bring up the APRS-IS iGate, then WiFi STA. aprsis_init() starts its own task
 * that waits for WiFi internally — starting it first means its uplink queue
 * exists before the first BLE frame arrives, so traffic heard during the WiFi
 * connect window is buffered and gated once connected (not lost). No-op (warns)
 * if there are no WiFi credentials. */
/* Waits for the first sync and says so. "SNTP started" is not the same thing as
 * "the clock is right", and the difference is invisible until an announce is
 * quietly dropped by every hub on the network for carrying a 1970 timestamp. */
static void sntp_wait_task(void *arg)
{
    (void)arg;
    for (int attempt = 0; attempt < 20; attempt++) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
            time_t now = time(NULL);
            struct tm tm;
            gmtime_r(&now, &tm);
            ESP_LOGW(TAG, "clock set: %04d-%02d-%02d %02d:%02d:%02dZ (epoch %lld)",
                     tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                     tm.tm_hour, tm.tm_min, tm.tm_sec, (long long)now);
            vTaskDelete(NULL);
        }
        ESP_LOGW(TAG, "clock still not set (attempt %d)", attempt + 1);
    }
    vTaskDelete(NULL);
}

static void igate_start(void)
{
    s_ig_heard_mtx = xSemaphoreCreateMutex();
    igate_provision();

    char ssid[33] = {0}, pass[65] = {0};
    bool have_creds = false;
    if (xprs_wifi_init() == ESP_OK &&
        xprs_wifi_load_credentials(ssid, pass) == ESP_OK && ssid[0]) {
        have_creds = true;
    }
    if (!have_creds) {
        ESP_LOGW(TAG, "iGate: no WiFi credentials in NVS — iGate disabled");
        return;
    }

    /* Start APRS-IS first (queue ready for early frames); it waits for WiFi. */
    aprsis_set_stores(NULL, NULL);             /* no SD archive on this firmware */
    aprsis_set_ble_hooks(igate_get_heard, igate_relay);
    aprsis_init(s_aprs_call);
    ESP_LOGI(TAG, "iGate: APRS-IS started as %s; connecting WiFi STA to %s",
             s_aprs_call, ssid);

    xprs_wifi_config_t cfg = {0};
    strncpy(cfg.ssid, ssid, sizeof cfg.ssid - 1);
    strncpy(cfg.password, pass, sizeof cfg.password - 1);
    cfg.callback = NULL;
    xprs_wifi_connect(&cfg);

    /* A clock, at last. Reticulum stamps every announce with the time and hubs
     * drop the ones that look stale, so a station with no clock is a station
     * the network ignores. It is also what makes the `ts:` on packets this
     * dongle originates mean anything — until now they carried seconds since
     * boot, which is a timestamp in the early 1970s. */
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&sntp) == ESP_OK) {
        xTaskCreate(sntp_wait_task, "sntp_wait", 3072, NULL, 2, NULL);
    } else {
        ESP_LOGW(TAG, "SNTP failed to start — announces will be ignored");
    }

    /* LAN reach: listen for the XPRS app UDP discovery broadcast (announces) so
     * the dashboard can count xprs devices on the same network. Passive
     * (receive-only); datagrams start flowing once the STA has an IP. */
    lanwatch_start(LANWATCH_DEFAULT_PORT);
}

/* ---- serial console (USB-Serial-JTAG stdin) ------------------------------ *
 * Debug/control without the app: type into `pio device monitor` / serial.sh.
 *   status                   dump identity, neighbors, routes, parked mail
 *   msg <to> <text...>       air a compact APRS 1:1/group frame from our call
 *   xmsg <to> <text...>      air an XPRS t:message from our call (0x41)
 *   xping <call>             air an XPRS t:ping (0x58)
 *   xid <wire>               print a packet's derived identifier (XPRS.md §5)
 *   beacon | xbeacon         air the mesh route / XPRS presence beacon now
 *   ack <6hex>               simulate an overheard ?ACK (purges parked mail)
 */
static void console_recv_begin(const char *path);

static void console_handle(char *line)
{
    if (xdiag_console(line)) return;  /* test hooks, only when built in */
    if (xcfg_console(line)) return;   /* cfg get/set/del: the shared cable */
    if (strncmp(line, "chan ", 5) == 0) {
        /* chan <peer> <channel> [seconds] [lr] — §23.7 step 1. */
        char peer[10] = {0};
        int ch = 0, secs = 20, lr = 0;
        char lrs[8] = {0};
        int got = sscanf(line + 5, "%9s %d %d %7s", peer, &ch, &secs, lrs);
        if (got >= 2) {
            lr = (strcmp(lrs, "lr") == 0);
            printf("inviting %s to channel %d for %ds%s\n", peer, ch, secs,
                   lr ? " on the long-range PHY" : "");
            if (!xprschan_invite(peer, (uint8_t)ch, (uint32_t)secs, lr)) {
                printf("refused: busy, or this station will not move\n");
            }
        } else {
            printf("usage: chan <peer> <channel> [seconds] [lr]\n");
        }
        return;
    }
    if (strcmp(line, "unchan") == 0) { xprschan_abort("asked to"); return; }
    if (strcmp(line, "status") == 0) {
        printf("callsign=%s mesh=%d neigh=%d routes=%d scf=%d sd=%d "
               "disc=%lu last_rx=%lus ago epoch=%u.%u\n",
               s_aprs_call[0] ? s_aprs_call : "TDONGLE", (int)s_mesh_up,
               blemesh_neighbor_count(), blemesh_route_count(),
               blemesh_scf_count(), (int)sdcard_is_mounted(),
               (unsigned long)s_disc_count,
               (unsigned long)(now_sec() - s_last_disc),
               (unsigned)s_boot_epoch, (unsigned)now_sec());
        char up[16], life[16];
        xprs_fmt_duration(now_sec(), up, sizeof up);
        xprs_fmt_duration(s_life_base + now_sec(), life, sizeof life);
        printf("uptime=%s lifetime=%s (life base %us, saved every %ds)\n",
               up, life, (unsigned)s_life_base, LIFE_SAVE_SEC);
        printf("digipeat: %u origin-follow repeat(s), window %ds, cap %d\n",
               (unsigned)s_digi_repeats, XPRS_DIGI_REPEAT_SEC,
               XPRS_DIGI_TIMES_MAX);
        if (s_rssi_n) {
            printf("rx rssi: min=%d max=%d avg=-%lu n=%lu\n", s_rssi_min,
                   s_rssi_max, (unsigned long)(s_rssi_sum / s_rssi_n),
                   (unsigned long)s_rssi_n);
        }
        for (int i = 0; i < blemesh_neighbor_count(); i++) {
            const blemesh_neighbor_t *n = blemesh_neighbor_at(i);
            printf("  neigh %-9s class=%d rssi=%d bidi=%d reach=%d age=%us\n",
                   n->callsign, n->dev_class, n->rssi, (int)n->bidirectional,
                   n->reach, (unsigned)(now_sec() - n->last_heard));
        }
        return;
    }
    if (strncmp(line, "msg ", 4) == 0) {
        char *to = line + 4;
        char *sp = strchr(to, ' ');
        if (!sp) { printf("usage: msg <to> <text>\n"); return; }
        *sp = 0;
        const char *text = sp + 1;
        uint8_t payload[BLEMESH_SCF_FRAME_MAX];
        int n = snprintf((char *)payload, sizeof(payload), "%s\x1f%s\x1f%s",
                         s_aprs_call[0] ? s_aprs_call : "TDONGLE", to, text);
        if (n <= 0 || n >= (int)sizeof(payload)) { printf("too long\n"); return; }
        uint8_t ad[256];
        int an = build_aprs_ad(payload, n, ad);
        if (an > 0) {
            /* Remember our own content hash BEFORE airing: when a phone
             * re-airs (bridges) this frame back to us, handle_aprs must treat
             * it as already-handled — otherwise the echo gets uplinked to
             * APRS-IS and the "BLE-only" message leaks onto the internet. */
            relay_remember(fnv1a(payload, n));
            relay_enqueue(ad, an);
            printf("queued %dB to %s\n", n, to);
        }
        return;
    }
    if (strcmp(line, "scf") == 0) {
        int n = blemesh_scf_count();
        printf("scf %d/%d\n", n, BLEMESH_SCF_MAX);
        for (int i = 0; i < n; i++) {
            const char *tg = "", *am = "";
            int ln = 0; uint32_t age = 0; uint8_t urg = 0;
            if (blemesh_scf_at(i, &tg, &am, &ln, &age, now_sec(), &urg))
                printf("  [%d] for=%-9s id=%-6s %dB age=%us urg=%d\n", i, tg,
                       am[0] ? am : "-", ln, (unsigned)age, urg);
        }
        return;
    }
    if (strcmp(line, "scfclear") == 0) {
        blemesh_scf_clear();
        printf("scf cleared\n");
        return;
    }
    if (strcmp(line, "beacon") == 0) { mesh_beacon_air(); printf("beacon aired\n"); return; }
    if (strcmp(line, "xbeacon") == 0) { xprs_beacon_air(); printf("xprs beacon aired\n"); return; }
    if (strncmp(line, "xping ", 6) == 0) {
        char tf[32], wire[XPRS_MAX_WIRE + 1];
        xprs_time_field(tf, sizeof tf);
        int n = snprintf(wire, sizeof wire, "t:ping f:%s d:%s %s",
                         s_aprs_call[0] ? s_aprs_call : "TDONGLE", line + 6, tf);
        if (n <= 0 || n >= (int)sizeof wire) { printf("too long\n"); return; }
        xprs_air(wire, n, SUBTYPE_XPRS);        /* remembers own id (echo guard) */
        printf("queued: %s\n", wire);
        return;
    }
    if (strncmp(line, "xmsg ", 5) == 0) {
        char *to = line + 5;
        char *sp = strchr(to, ' ');
        if (!sp) { printf("usage: xmsg <to> <text>\n"); return; }
        *sp = 0;
        char tf[32], wire[XPRS_MAX_WIRE + 1];
        xprs_time_field(tf, sizeof tf);
        int n = snprintf(wire, sizeof wire, "t:message f:%s d:%s %s m:%s",
                         s_aprs_call[0] ? s_aprs_call : "TDONGLE", to, tf, sp + 1);
        if (n <= 0 || n >= (int)sizeof wire) { printf("too long\n"); return; }
        xprs_air(wire, n, SUBTYPE_APRS);        /* messages ride 0x41 */
        xprslan_send(wire, n);                  /* and the other bearers, */
        xprsnow_send(wire, n);                  /* like the identity does */
        {   /* our own words show in the Chat view (no echo comes back) */
            xprs_t lp;
            if (xprs_parse(wire, n, &lp)) xst_chat_note(&lp);
        }
        printf("queued %dB to %s\n", n, to);
        return;
    }
    if (strncmp(line, "xpark ", 6) == 0) {
        /* Inject a message straight into the custody store (test/demo): park
         * an XPRS 1:1 as if it had been heard on the air — delivered by the
         * ordinary sighting/custody machinery when the recipient appears.
         * [from] is explicit because a receiver's inbox maps the author's
         * published key; mail authored by a key-less station is carried but
         * not displayed. */
        char *from = line + 6;
        char *sp = strchr(from, ' ');
        if (!sp) { printf("usage: xpark <from> <to> <text>\n"); return; }
        *sp = 0;
        char *to = sp + 1;
        sp = strchr(to, ' ');
        if (!sp) { printf("usage: xpark <from> <to> <text>\n"); return; }
        *sp = 0;
        char tf[32], wire[XPRS_MAX_WIRE + 1];
        xprs_time_field(tf, sizeof tf);
        int n = snprintf(wire, sizeof wire, "t:message f:%s d:%s %s m:%s",
                         from, to, tf, sp + 1);
        if (n <= 0 || n >= (int)sizeof wire) { printf("too long\n"); return; }
        char id[XPRS_ID_LEN];
        if (!xprs_id_of(wire, n, id)) { printf("bad wire\n"); return; }
        if (blemesh_scf_offer(to, id, (const uint8_t *)wire, n, now_sec(),
                              BLEMESH_URG_NORMAL))
            printf("parked %s for %s (%dB)\n", id, to, n);
        else
            printf("not parked (duplicate or store full)\n");
        return;
    }
    if (strncmp(line, "xid ", 4) == 0) {
        char id[XPRS_ID_LEN];
        if (xprs_id_of(line + 4, (int)strlen(line + 4), id))
            printf("id=%s\n", id);
        else
            printf("not an XPRS packet\n");
        return;
    }
    if (strncmp(line, "xhear ", 6) == 0) {
        /* Feed a wire into the XPRS front door as if heard on the air —
         * deterministic digipeater tests over serial (repeat the SAME line
         * to exercise the origin-follow policy; the radio makes identical
         * repeats hard to stage on demand). Test-only: it runs on the
         * console task while real traffic runs on the host task, so use it
         * on a quiet bench. */
        const char *w = line + 6;
        handle_xprs((const uint8_t *)w, (int)strlen(w), 0, SUBTYPE_XPRS);
        printf("heard %dB\n", (int)strlen(w));
        return;
    }
    if (strncmp(line, "ack ", 4) == 0) {
        printf("purged %d\n", blemesh_scf_ack(line + 4));
        return;
    }
    if (strncmp(line, "sendfile ", 9) == 0 ||
        strcmp(line, "transfers") == 0 || strcmp(line, "spool") == 0) {
        printf("gone: this station is broadcast-only since the move to "
               "tinynimble -- no GATT, so no bulk transfer\n");
        return;
    }
    if (strcmp(line, "wifioff") == 0) {
        /* Coex experiment: does BLE RX sensitivity recover without WiFi? */
        esp_wifi_stop();
        printf("wifi stopped\n");
        return;
    }
    if (strcmp(line, "scankick") == 0) {
        tn_scan_stop(); start_scan(); printf("scan restarted\n"); return;
    }
    if (strncmp(line, "recv ", 5) == 0) {
        /* Preload a file onto the SD over the (fast, native-USB) console:
         *   recv /sdcard/foo.bin
         * then base64 lines, then a line "END". */
        console_recv_begin(line + 5);
        return;
    }
    if (strncmp(line, "view ", 5) == 0) {
        int v = atoi(line + 5);
        if (v >= 1 && v <= XUM_VIEW_COUNT) {
            s_ui_force = v - 1;
            printf("view %d\n", v);
        } else {
            printf("usage: view <1..%d> (devices, stats, chat)\n",
                   XUM_VIEW_COUNT);
        }
        return;
    }
    if (strcmp(line, "dump") == 0) {
        /* FRAMEDUMP over this console -- tools/scripts/framedump.py
         * decodes it into a PNG (--cmd "dump\n"). */
        xum_framedump();
        return;
    }
    printf("commands: status | view <1..3> | dump | msg <to> <text> | xmsg <to> <text> | "
           "xping <call> | xid <wire> | beacon | xbeacon | ack <am> | "
           "sendfile <to> <path> | transfers\n");
}

/* recv mode: base64 lines stream into a file until an "END" line. */
#include "mbedtls/base64.h"
static FILE *s_recv_f;
static uint32_t s_recv_bytes;
static void console_recv_begin(const char *path)
{
    if (s_recv_f) { fclose(s_recv_f); s_recv_f = NULL; }
    mkdir("/sdcard/mesh", 0775);
    s_recv_f = fopen(path, "wb");
    s_recv_bytes = 0;
    printf(s_recv_f ? "recv: streaming to %s (base64 lines, END to finish)\n"
                    : "recv: cannot open %s\n", path);
}

static void console_recv_line(const char *line)
{
    if (strcmp(line, "END") == 0) {
        fclose(s_recv_f);
        s_recv_f = NULL;
        printf("recv: done, %lu bytes\n", (unsigned long)s_recv_bytes);
        return;
    }
    unsigned char buf[192];
    size_t out = 0;
    if (mbedtls_base64_decode(buf, sizeof(buf), &out,
                              (const unsigned char *)line, strlen(line)) == 0) {
        fwrite(buf, 1, out, s_recv_f);
        s_recv_bytes += out;
        if ((s_recv_bytes & 0xFFFFF) < out) {  /* ~per-MB progress */
            printf("recv: %lu bytes\n", (unsigned long)s_recv_bytes);
        }
    } else {
        printf("recv: bad base64 line, aborting\n");
        fclose(s_recv_f);
        s_recv_f = NULL;
    }
}

static void console_task(void *arg)
{
    (void)arg;
    static char line[260];
    int n = 0;
    for (;;) {
        int c = fgetc(stdin);
        if (c == EOF) { vTaskDelay(pdMS_TO_TICKS(s_recv_f ? 2 : 50)); continue; }
        if (c == '\r' || c == '\n') {
            if (n > 0) {
                line[n] = 0;
                if (s_recv_f) console_recv_line(line);
                else console_handle(line);
                n = 0;
            }
            continue;
        }
        if (n < (int)sizeof(line) - 1) line[n++] = (char)c;
    }
}

/* Free heap after each subsystem comes up.
 *
 * The board reached the point where esp_now_send() returned ESP_ERR_ESPNOW_NO_MEM
 * on every call and the BLE controller could not allocate advert data — with
 * about eight kilobytes free seventeen seconds after boot. That is not a leak
 * and no amount of watching the radio explains it; it needs the one measurement
 * nothing here was taking. */
static void heap_mark(const char *stage)
{
    ESP_LOGW(TAG, "heap after %-12s free=%u big=%u", stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                        MALLOC_CAP_8BIT));
}

/* xum's flush onto the board's panel driver -- the whole board glue. */
static void lcd_flush_adapter(int x1, int y1, int x2, int y2,
                              const uint16_t *px, void *ctx)
{
    st7735_flush((st7735_handle_t)ctx, x1, y1, x2, y2, px);
}

void app_main(void)
{
    heap_mark("boot");

    /* THE RELAY TASK'S STACK, CLAIMED FIRST.
     *
     * This is eight kilobytes in one piece, and it used to be asked for from
     * ble_bring_up() — after WiFi, BLE, the SD card, the HTTP server and the
     * Reticulum hub had each taken their share. Measured there: 15,308 bytes
     * free but the largest block only 7,680, so the creation failed, and the
     * station ran on with no beacons, no service announcements, no history
     * replay and no section 23.7 tick while every other task carried on and
     * the board looked healthy. One boot line was the only sign.
     *
     * Here the heap is untouched (172 KB in one block) and it cannot fail for
     * want of room. The task blocks on s_relay_may_run until the BLE host has
     * synced, because it owns extended-advertising instance 0. */
    if (xTaskCreatePinnedToCore(relay_task, "rns_relay", 8192, NULL, 5, NULL, 1)
        != pdPASS) {
        ESP_LOGE(TAG, "relay task did NOT start (heap %u, largest block %u) - "
                      "this station will not beacon, announce or answer",
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(
                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    /* The screen's stores exist before anything can be heard; the callsign
     * firms up in igate_start() and is set below. */
    xst_init("", 0);

    /* model_init() initialises NVS + the ST7735 LCD. */
    if (model_init() != ESP_OK) {
        ESP_LOGW(TAG, "model_init failed (no display?)");
    } else if (xum_init(ST7735_WIDTH, ST7735_HEIGHT, lcd_flush_adapter,
                        model_get_lcd()) != ESP_OK) {
        ESP_LOGW(TAG, "mini UI failed to start (no RAM for the buffer?)");
    }

    s_relay_mtx = xSemaphoreCreateMutex();
    /* Declare the roster before starting anything. A part that is only
     * registered when it succeeds can never be reported missing, and
     * "never started at all" is precisely the failure this catches. */
    xh_expect(XH_HTTP,  true);
    xh_expect(XH_BLE,   true);
    xh_expect(XH_LAN,   true);
    xh_expect(XH_NOW,   true);
    xh_expect(XH_RELAY, true);
    xh_expect(XH_CARD,  false);   /* a station without a card still works */

    /* The controller only. There is no NimBLE host on this station any more:
     * tinynimble speaks HCI to the controller directly, which is where the
     * host's msys pools, its ACL transport buffers and its 5,120-byte task
     * stack used to go -- and this board has no PSRAM to hide them in. */
    heap_mark("before ble_init");
    {
        esp_err_t berr = tn_start();
        if (berr != ESP_OK) {
            ESP_LOGE(TAG, "tn_start failed: %s", esp_err_to_name(berr));
            return;
        }
    }
    heap_mark("before identity");
    identity_init();

    /* Boot epoch counter (XPRS.md §10.7): a clockless station dates its
     * packets epoch:<boots>.<uptime-seconds>; a receiver holding a clock
     * anchors the epoch when it first hears it. */
    {
        nvs_handle_t h;
        if (nvs_open("rns", NVS_READWRITE, &h) == ESP_OK) {
            nvs_get_u32(h, "bootcnt", &s_boot_epoch);   /* absent -> stays 0 */
            s_boot_epoch++;
            nvs_set_u32(h, "bootcnt", s_boot_epoch);
            nvs_get_u32(h, "lifesec", &s_life_base);    /* absent -> stays 0 */
            nvs_commit(h);
            nvs_close(h);
        } else {
            s_boot_epoch = 1;
        }
    }

    /* Start the dashboard UI task (owns all LVGL calls). */
    heap_mark("before ui_task");
    s_ui_q = xQueueCreate(12, sizeof(ui_msg_t));
    if (xTaskCreate(ui_task, "ui", 5120, NULL, 4, NULL) != pdPASS)
        ESP_LOGE(TAG, "ui task did NOT start");

    /* WiFi STA + APRS-IS iGate, started BEFORE the BLE host runs so the uplink
     * queue exists for the first frames heard during the WiFi connect window. */
    heap_mark("before igate");
    igate_start();
    xst_set_call(s_aprs_call[0] ? s_aprs_call : "TDONGLE");

    /* The HTTP server, claimed here rather than after the bearers.
     *
     * Its 5 KB task stack is heap like any other, and it used to be asked
     * for last -- after WiFi, NimBLE and the SD card had each taken their
     * tens of kilobytes. Measured on this board, it reached that point
     * with 5,312 bytes free and 2,816 in the largest block, so
     * httpd_start() failed and the station spent the whole session
     * gossiping happily over ESP-NOW while being completely unreachable
     * on the LAN -- the worst shape of failure, because from the air it
     * looks healthy. Here the heap is still one 49 KB block.
     *
     * Nothing can arrive in the gap: the handlers read the card and the
     * bearers, and WiFi does not have an address for another second.
     * docs/esp32.md: take a big stack at boot or do not take it at all. */
    heap_mark("before httpd");
    api_start();

    /* Updating without a ladder (XPRS.md 25.8). The config holds the key
     * that may approve an image and the npubs that may command this
     * station; both are seeded once from the compiled-in defaults and are
     * the operator's afterwards -- changeable with a cable, so a lost key
     * is a ladder and never a brick. */
    xcfg_init();
    if (!xcfg_get("fwkey", "")[0] && FW_DEFAULT_KEY[0])
        xcfg_set("fwkey", FW_DEFAULT_KEY);
    if (!xcfg_get("own1", "")[0] && FW_DEFAULT_OWNER[0])
        xcfg_set("own1", FW_DEFAULT_OWNER);
    {
        static xota_cfg_t oc;
        oc.board = "tdongle-s3";
        oc.callsign = s_aprs_call;
        oc.air = ota_air;
        oc.quiesce = ota_quiesce;
        xota_start(&oc);
    }
    {
        /* Diagnostics over the air (xprs_diag), the shared implementation.
         * No log on flash here, so it keeps its own tail and hooks esp_log. */
        static xdiag_cfg_t dc;
        dc.callsign = s_aprs_call;
        dc.sign = xprs_sign_wire;
        dc.air = xdiag_air;
        dc.stats = xdiag_stats;
        dc.log_cur = NULL;
        dc.log_prev = NULL;
        dc.epoch_now = xst_epoch_now;
        dc.budget = xprs_hist_budget_allows;
        dc.budget_record = xprs_hist_record_ask;
        dc.hook_log = true;
        xdiag_init(&dc);
    }
    /* Did the bootloader put us back? Then say so: a failed update
     * reporting its own failure, with nobody on the roof. */
    xota_report_rollback();

    /* Street mesh: identity from the iGate callsign (NVS). SD card (if present)
     * persists parked store-and-forward mail across reboots; RAM-only without. */
    heap_mark("before blemesh");
    blemesh_table_init(s_aprs_call[0] ? s_aprs_call : "TDONGLE");

    /* BLE, brought up here rather than at the end of app_main.
     *
     * The old reason was the NimBLE host task: 5,120 bytes of contiguous
     * stack, which started last found 5,256 free in a largest block of 3,584
     * and silently did not start -- leaving the station with no BLE while
     * relay_task, which waits on the host, waited forever, taking the firmware
     * self-test and the cmd:update answer with it. `relay=0` on the alive line
     * was the tell.
     *
     * tinynimble removes that whole failure mode: there is no host task to
     * fail to allocate, and no sync callback to race, because the controller
     * is up when tn_start() returns. The receive TASK is ours and is claimed
     * here, early, while the heap is still one large block -- docs/esp32.md:
     * claim the big stacks early or do not claim them.
     *
     * 6144 bytes because the receive path verifies secp256k1 signatures, and
     * that is precisely what overflowed the borrowed controller stack on the
     * T-Deck. */
    heap_mark("before ble_rx_task");
    s_ad_q = xQueueCreate(AD_Q_DEPTH, sizeof(ad_item_t));
    if (!s_ad_q ||
        xTaskCreate(ble_rx_task, "ble_rx", 6144, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "BLE receive task did not start -- no BLE this boot");
    } else {
        s_ble_up = true;
        ble_bring_up();          /* address, scan, relay release */
    }
    const char *scf_path = NULL;
    heap_mark("before sdcard");
    if (sdcard_init() == ESP_OK && sdcard_is_mounted()) {
        mkdir("/sdcard/mesh", 0775);
        scf_path = "/sdcard/mesh/pending.bin";
        ESP_LOGI(TAG, "mesh: SD store-and-forward at %s", scf_path);
    } else {
        ESP_LOGW(TAG, "mesh: no SD card — store-and-forward is RAM-only");
    }
    blemesh_scf_init(scf_path);
    s_mesh_up = true;

    /* The XPRS index, on by default when there is a card (XPRS.md §36). Its
     * writer runs on core 1: the BLE controller, the NimBLE host and WiFi are
     * all on core 0 and an SD transaction is long — writing from a receive path
     * cost the other firmware its ability to transmit at all. */
    if (sdcard_is_mounted()) {
        s_xprs_index = xprsindex_open("/sdcard/xprs");
        xprsindex_set_own(s_xprs_index, s_aprs_call);
        /* An SD card is roomy; a quarter gigabyte is still weeks of air.
         * The card is the whole point of this board: a super sizes its
         * archive to the volume rather than to this default. */
        s_xprs_budget = xprsindex_budget("/sdcard", 256ull * 1024 * 1024,
                                         xcfg_get_bool("index_super", false));
        xprsindex_set_max_bytes(s_xprs_index, s_xprs_budget);
        s_goss = xgossip_open("/sdcard/xprs");
        xgossip_set_super(s_goss, xprs_is_super());
        xst_stats_load("/sdcard/xprs/stats.bin");
        if (xprsindex_ready(s_xprs_index)) {
            xprsidx_stats_t xs;
            xprsindex_stats(s_xprs_index, &xs);
            ESP_LOGI(TAG, "XPRS indexer ready — %u records, epoch %c, %u segments",
                     (unsigned)xs.count, xs.epoch, (unsigned)xs.segments);
            /* Judge everything from here on (§9.1). The keys to judge WITH are
             * fetched by the relay task on its first tick — see the note on
             * s_keys_reload_due; doing it here overflowed the main task. */
            xprsindex_set_verifier(s_xprs_index, xprs_verify_for_index);
            s_keys_reload_due = true;
        } else {
            ESP_LOGW(TAG, "XPRS indexer unavailable — packets relayed, none kept");
            s_xprs_index = NULL;
        }
    }

    /* XPRS on the LAN (docs/lan.md): broadcast to and from everyone on this
     * network, on its own UDP port. Not Reticulum and not the internet. */
    heap_mark("before xprslan");
    if (xprslan_start(s_aprs_call[0] ? s_aprs_call : "TDONGLE") == ESP_OK) {
        xprslan_set_rx_cb(xprs_from_lan);
        xprslan_set_heard_cb(xprs_heard_on_lan);
        xprslan_set_beacon(xprs_lan_beacon, 300, 20);
        ESP_LOGI(TAG, "XPRS LAN bearer up on UDP %d", XPRSLAN_PORT);
    } else {
        ESP_LOGW(TAG, "XPRS LAN bearer failed to start");
    }

    /* XPRS over ESP-NOW (docs/espnow.md): the same 250-byte packet, on the WiFi
     * radio with no access point in the middle. Started AFTER the LAN bearer
     * because that bearer's task is what pumps both — see xb_register_ticked().
     *
     * It rides whatever channel the station is on, so two devices only hear
     * each other when they are on the same one. Nothing reports otherwise: the
     * symptom is a peer count that stays at zero. */
    heap_mark("before xprsnow");
    if (xprsnow_start(s_aprs_call[0] ? s_aprs_call : "TDONGLE") == ESP_OK) {
        xprsnow_set_rx_cb(xprs_from_now);
        xprsnow_set_heard_cb(xprs_heard_on_now);
        xprsnow_set_beacon(xprs_now_beacon, 300, 25);
        xprschan_init(s_aprs_call[0] ? s_aprs_call : "TDONGLE", &k_chan_ops);
    }

    /* The Reticulum interface. One socket to one hub: the station is then
     * reachable from anywhere on the network rather than only from the room it
     * is in. It reconnects on its own, so it is started whether or not WiFi has
     * finished associating. */
    rns_tcp_set_rx_cb(rns_from_hub, NULL);
#ifdef RNS_HUB_HOST
    rns_tcp_add_hub(RNS_HUB_HOST, RNS_TCP_DEFAULT_PORT);
#endif
    /* -DRNS_HUB_ONLY pins the station to RNS_HUB_HOST alone. The rotation is
     * right in the field and wrong on the bench: the first dial happens seconds
     * after boot, before WiFi has an address, so a single failure moves the
     * station off the hub under test and it never comes back. */
#ifndef RNS_HUB_ONLY
    for (size_t i = 0; i < sizeof k_rns_hubs / sizeof k_rns_hubs[0]; i++) {
        rns_tcp_add_hub(k_rns_hubs[i].host, k_rns_hubs[i].port);
    }
#endif
    heap_mark("before rns_tcp");
#ifdef RNS_HUB_LINK
    if (rns_tcp_start(NULL, 0) != ESP_OK) {
        ESP_LOGW(TAG, "Reticulum interface failed to start");
    }
#else
    /* No hub link on this board, deliberately.
     *
     * The T-Dongle-S3 cannot run everything at once. Measured, with each
     * subsystem started and none of them failing quietly: WiFi and the
     * iGate 55.7 KB, NimBLE and the controller 59.3 KB, the SD card 32 KB
     * at three open files, the LAN and ESP-NOW bearers 10.5 KB, the HTTP
     * server 11 KB, LVGL 16 KB -- against a heap that starts at 208 KB.
     * That is roughly 12 KB more than exists, and the shortfall does not
     * announce itself: whatever is created last simply fails, silently,
     * and the station runs on looking healthy. It cost this bench a full
     * session to find that BLE had not started for want of one contiguous
     * 5,120-byte stack, which parked relay_task, which took the firmware
     * self-test and the answer to an over-the-air cmd:update with it.
     *
     * One socket to one hub is 12,676 bytes -- a 4 KB task stack and, most
     * of it, that connection's two lwip windows. It is almost exactly the
     * shortfall, and it is the one thing here whose job another station
     * already does: the m5stack keeps its hub link, and this board is a
     * local-RF station and archiver -- BLE mesh, ESP-NOW, LAN, the card,
     * the screen, the API and its own updates.
     *
     * Build with -DRNS_HUB_LINK to put it back, and expect to give up
     * something else in the same breath. */
    ESP_LOGI(TAG, "no hub link on this board -- local RF and the card "
                  "(see the budget in main.c)");
#endif

    xTaskCreatePinnedToCore(heartbeat_task, "heartbeat", 3072, NULL, 1, NULL, 1);

    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);

    ESP_LOGI(TAG, "RNS-BLE5 full node + repeater + UI + APRS-IS iGate up");

    /* Everything above has now had its chance. Name whatever did not take
     * it, and complain if this board came up with less room than
     * docs/esp32.md records for it -- the check that would have caught a
     * whole sdkconfig block going missing in a repo move. The BLE host is
     * still associating at this point, so it is not judged yet; the
     * heartbeat picks it up within fifteen seconds. */
    xh_set(XH_LAN, xprslan_is_active());
    xh_set(XH_NOW, xprsnow_channel() != 0);
    xh_set(XH_CARD, sdcard_is_mounted());
    xh_heap_floor(TDONGLE_HEAP_FLOOR);
}
