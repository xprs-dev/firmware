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
#include "xprs_psram.h"   /* xprs_big_calloc: the peer table off the .bss */

static const char *TAG = "xprsrns";

/* TweetNaCl, by its real names (the macro header is not included here). */
extern int crypto_sign_ed25519_tweet_keypair(unsigned char *, unsigned char *);
extern void randombytes(unsigned char *, unsigned long long);

static rns_identity_t     s_id;
static uint8_t            s_wapp_name[RNS_NAME_HASH_LEN];

/*
 * The wire this station announces itself with on connect (36.12.2).
 *
 * Set by the STATION, not built here: it is a signed t:identity, and an
 * Ed25519 signature is not something to put on the socket task's stack --
 * the same rule the announce builder below already follows.
 */
static char               s_hello[256];
static int                s_hello_len;
static uint8_t            s_wapp_dest[RNS_HASH_LEN];
static xprsrns_wire_cb_t  s_cb;
static bool               s_ready;
static SemaphoreHandle_t  s_tx_lock;
static int64_t            s_last_tx_us;
static int                s_pace_ms = 1100;

static uint32_t s_rx, s_tx, s_paced, s_other;

/*
 * An announce is dated, and the date decides whether anybody listens.
 *
 * The last five bytes of its random hash are the emission time in seconds
 * (rns_announce_build), and a reference transport replaces the path it
 * holds for a destination only with an announce emitted LATER than the one
 * it learned that path from (Transport.inbound: `announce_emitted >
 * path_timebase`). A station that announces before its clock is set dates
 * the announce a few seconds after 1970, so every transport that ever heard
 * this destination keeps its old path and drops the new announce without
 * passing it on. Measured on 2026-10-15 with a QEMU station whose clock never
 * set: its hub answered path requests with a stale route through another
 * node, three hops instead of two, and nothing addressed to the station
 * arrived, while the same build on a fresh local transport worked at once.
 *
 * So nothing is announced on a clock that is not set. The connect-time
 * hello waits for it (s_hello_due), and an ordinary wire is refused, which
 * every caller already treats as "not carried by Reticulum".
 */
static bool               s_hello_due;
static uint32_t           s_no_clock;

static bool clock_set(void) { return time(NULL) > 1700000000; }

/*
 * And a hub rations announces per destination.
 *
 * A reference transport with an announce rate target on an interface counts
 * a violation for every announce that comes sooner than the target after the
 * previous one, and past its grace stops re-broadcasting that destination
 * for target plus penalty (Transport.inbound, announce_rate_table). The path
 * is still learned; only the re-broadcast stops. This station announced
 * every wire it aired, one every seven seconds, so on rns.wisco.network
 * nothing it announced ever reached another client. Measured 2026-10-15
 * with reference clients: one announcing every ten seconds was passed six
 * times, in the first minute, and never again; a client listening for five
 * minutes heard nothing at all from this station.
 *
 * So on a public hub the station announces its hello and nothing else, once
 * per s_ann_gap_s (an hour, the usual target), and its wires go on the
 * directed lane or not at all, as XPRS.md 12.12.1 says of a filtered
 * transport. A hub at a private address is an XPRS node of the station's
 * own, which polices nothing, and keeps every wire. `rns_ann_s` overrides.
 */
static int     s_ann_gap_s;
static int64_t s_last_ann_us;           /* 0: nothing announced yet */
static int64_t s_hello_due_us;

static bool ann_slot_free(void)
{
    if (s_ann_gap_s <= 0 || s_last_ann_us == 0) return true;
    return esp_timer_get_time() - s_last_ann_us >= (int64_t)s_ann_gap_s * 1000000;
}

static bool host_is_private(const char *h)
{
    if (!strncmp(h, "10.", 3) || !strncmp(h, "192.168.", 8) ||
        !strncmp(h, "127.", 4) || !strncmp(h, "169.254.", 8) ||
        !strcasecmp(h, "localhost"))
        return true;
    if (!strncmp(h, "172.", 4)) {
        int b = atoi(h + 4);
        if (b >= 16 && b <= 31) return true;
    }
    size_t n = strlen(h);
    return n > 6 && !strcasecmp(h + n - 6, ".local");
}

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
    uint8_t  edpub[RNS_KEY_HALF];     /* the signing half: a link's proof is checked against it */
    uint32_t heard_s;
    uint8_t  id_hash[RNS_HASH_LEN];   /* SHA-256(pub)[:16]: the HKDF salt */
    /* The route, as the announce that taught us the peer told it: how far,
     * and through which transport node. A peer behind a hub is addressed
     * THROUGH the hub (HEADER_2, transport id = next hop), or the hub drops
     * the packet; reference RNS does the same for any path over one hop. */
    uint8_t  hops;
    uint8_t  next_hop[RNS_HASH_LEN];
    bool     have_next_hop;
} xrns_peer_t;

/* [XRNS_PEERS], big_calloc'd in init: off the .bss the RF sample bank caps
 * on the boards without PSRAM. NOT only reached once the bearer is up: on
 * the C3 a t:service heard over BLE two seconds into the boot had the
 * indexer asking xprsrns_can_address() before xprsrns_init() ran (a Load
 * access fault in idx, 2026-10-26), so every reader tolerates the table
 * not being there yet: nobody is addressable before init. */
static xrns_peer_t *s_peers;
static void (*s_link_cb)(const rns_packet_t *p);
static uint32_t s_addressed_tx, s_addressed_rx, s_no_peer;
/* Sent through a hub (HEADER_2), and path requests made. */
static uint32_t s_via_hub, s_path_req;
/* Addressed to us and would not open: a forgery, or a sender salting the
 * key differently (reference RNS salts with the identity hash). */
static uint32_t s_undecryptable;
/* Older than this, a route is asked for again before it is used. */
#define XRNS_PATH_STALE_S (30u * 60u)

static void peer_learn(const char *call, const uint8_t dest[RNS_HASH_LEN],
                       const uint8_t pub[RNS_PUB_LEN], const rns_packet_t *via)
{
    if (!call || !call[0] || !s_peers) return;
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
    memcpy(s_peers[slot].edpub, pub + RNS_KEY_HALF, RNS_KEY_HALF);
    s_peers[slot].heard_s = now;
    rns_identity_t idt;
    rns_identity_from_public(pub, &idt);
    memcpy(s_peers[slot].id_hash, idt.hash, RNS_HASH_LEN);
    /* Reference RNS counts a hop on receipt; this parser does not, so the
     * path is one longer than the field says. */
    s_peers[slot].hops = (uint8_t)(via->hops + 1);
    s_peers[slot].have_next_hop = via->have_transport_id;
    if (via->have_transport_id)
        memcpy(s_peers[slot].next_hop, via->transport_id, RNS_HASH_LEN);
}

/*
 * Neighbours we can name but not yet address.
 *
 * A station answers a wire on the destination the asker announced, and on a
 * public hub nobody's announces get through (s_ann_gap_s), so a phone
 * asking from the next room got no answer: measured 2026-10-15, the desktop
 * reached a station through rns.wisco.network and the answer counted
 * no_peer. What the phone's beacon does carry is `lx:`, its LXMF
 * destination. That is the same identity as its xprs.wapp destination, so a
 * path request for it is answered with an announce holding the key, and
 * xprs.wapp follows from the key. One ask per neighbour, again after ten
 * minutes if nothing came.
 */
#define XRNS_ASKS 4
#define XRNS_ASK_AGAIN_US (600LL * 1000000)
typedef struct {
    char     call[16];
    uint8_t  dest[RNS_HASH_LEN];
    int64_t  asked_us;                  /* 0: not asked yet */
} xrns_ask_t;
static xrns_ask_t s_asks[XRNS_ASKS];
static uint32_t   s_lx_learned;

static const xrns_peer_t *peer_find(const char *call);

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void xprsrns_note_address(const char *call, const char *lx_hex)
{
    if (!s_ready || !call || !call[0] || !lx_hex) return;
    if (strlen(lx_hex) != 2 * RNS_HASH_LEN || peer_find(call)) return;
    uint8_t d[RNS_HASH_LEN];
    for (int i = 0; i < RNS_HASH_LEN; i++) {
        int h = hexval(lx_hex[2 * i]), l = hexval(lx_hex[2 * i + 1]);
        if (h < 0 || l < 0) return;
        d[i] = (uint8_t)(h << 4 | l);
    }
    int slot = -1;
    for (int i = 0; i < XRNS_ASKS; i++) {
        if (strcasecmp(s_asks[i].call, call) == 0) { slot = i; break; }
        if (slot < 0 && !s_asks[i].call[0]) slot = i;
    }
    if (slot >= 0 && strcasecmp(s_asks[slot].call, call) == 0 &&
        memcmp(s_asks[slot].dest, d, RNS_HASH_LEN) == 0)
        return;                         /* already in hand */
    if (slot < 0) {                     /* full: the oldest ask goes */
        slot = 0;
        for (int i = 1; i < XRNS_ASKS; i++)
            if (s_asks[i].asked_us < s_asks[slot].asked_us) slot = i;
    }
    snprintf(s_asks[slot].call, sizeof s_asks[slot].call, "%s", call);
    memcpy(s_asks[slot].dest, d, RNS_HASH_LEN);
    s_asks[slot].asked_us = 0;
}

/* On the socket task: one due ask per frame, so a burst never blocks it. */
static void asks_send(void)
{
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < XRNS_ASKS; i++) {
        xrns_ask_t *a = &s_asks[i];
        if (!a->call[0]) continue;
        if (a->asked_us && now - a->asked_us < XRNS_ASK_AGAIN_US) continue;
        if (xSemaphoreTake(s_tx_lock, 0) != pdTRUE) return;
        static uint8_t prq[64];
        int qn = rns_path_request_build(a->dest, prq, sizeof prq);
        if (qn > 0 && rns_tcp_send(prq, (size_t)qn)) s_path_req++;
        xSemaphoreGive(s_tx_lock);
        a->asked_us = now;
        return;
    }
}

/*
 * Callsigns wanted by name (xprsrns_want). An XPRS app announces its LXMF
 * delivery destination with its callsign as the display name (XPRS.md
 * 12.12.2 asks every station to), and that identity's xprs.wapp destination
 * follows from the key. Only names on this list are worth a verify; every
 * other LXMF announce in the hub's flood costs a ten-byte compare.
 */
#define XRNS_WANTS 4
static char     s_want[XRNS_WANTS][16];
static uint8_t  s_want_next;
static uint8_t  s_lxmf_name[RNS_NAME_HASH_LEN];
static uint32_t s_named, s_deposited, s_deposit_nodest;

void xprsrns_want(const char *call)
{
    if (!s_ready || !call || !call[0] || peer_find(call)) return;
    for (int i = 0; i < XRNS_WANTS; i++)
        if (strcasecmp(s_want[i], call) == 0) return;
    snprintf(s_want[s_want_next], sizeof s_want[0], "%s", call);
    s_want_next = (uint8_t)((s_want_next + 1) % XRNS_WANTS);
}

/* The display name of an LXMF announce: msgpack [name, stamp cost] from
 * current LXMF, the bare UTF-8 name from older clients and this project's
 * app. */
static bool lxmf_name(const uint8_t *ad, size_t n, char *out, size_t cap)
{
    size_t at = 0, len = n;
    if (n >= 2 && ad[0] == 0x92) {
        uint8_t t = ad[1];
        if ((t == 0xc4 || t == 0xd9) && n >= 3) { len = ad[2]; at = 3; }
        else if ((t & 0xe0) == 0xa0)            { len = t & 0x1f; at = 2; }
        else return false;
    }
    if (len == 0 || at + len > n || len >= cap) return false;
    memcpy(out, ad + at, len);
    out[len] = 0;
    return true;
}

static int want_find(const char *name)
{
    for (int i = 0; i < XRNS_WANTS; i++)
        if (s_want[i][0] && strcasecmp(s_want[i], name) == 0) return i;
    return -1;
}

static int ask_for(const uint8_t dest[RNS_HASH_LEN])
{
    for (int i = 0; i < XRNS_ASKS; i++)
        if (s_asks[i].call[0] && memcmp(s_asks[i].dest, dest, RNS_HASH_LEN) == 0)
            return i;
    return -1;
}

static const xrns_peer_t *peer_find(const char *call)
{
    if (!call || !call[0] || !s_peers) return NULL;
    for (int i = 0; i < XRNS_PEERS; i++)
        if (strcasecmp(s_peers[i].call, call) == 0) return &s_peers[i];
    return NULL;
}

bool xprsrns_can_address(const char *call) { return peer_find(call) != NULL; }

int xprsrns_peer_count(void)
{
    int n = 0;
    if (!s_peers) return 0;
    for (int i = 0; i < XRNS_PEERS; i++) if (s_peers[i].call[0]) n++;
    return n;
}

/* Air the hello: once per connection, and only on a set clock (see
 * s_hello_due). Runs on the socket task. */
static void hello_air(void)
{
    if (!ann_slot_free()) return;
    /* A hello with no wire after the tag teaches nobody our callsign, and on
     * a public hub it spends the hour's one announce. The station sets the
     * identity wire a little after it connects; give it two minutes. */
    if (s_hello_len == 0 &&
        esp_timer_get_time() - s_hello_due_us < 120LL * 1000000)
        return;
    char hello[8 + sizeof s_hello];
    int hn = snprintf(hello, sizeof hello, "%cxprs", 4);
    if (s_hello_len > 0 && hn + s_hello_len < (int)sizeof hello) {
        memcpy(hello + hn, s_hello, (size_t)s_hello_len);
        hn += s_hello_len;
    }
    static uint8_t pkt[RNS_MTU + 64];
    /* The build's buffers are shared statics: every builder holds the
     * tx lock, this connect-time hello included. */
    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2500)) != pdTRUE) return;
    s_hello_due = false;
    int n = rns_announce_build(&s_id, s_wapp_name,
                               (const uint8_t *)hello, (size_t)hn,
                               (uint64_t)time(NULL), pkt, sizeof pkt);
    bool ok = n > 0 && rns_tcp_send(pkt, (size_t)n);
    if (ok) s_last_ann_us = esp_timer_get_time();
    xSemaphoreGive(s_tx_lock);
    if (n > 0) ESP_LOGI(TAG, "hello announce: %dB (%s), sent=%d", n,
                        s_hello_len > 0 ? "with identity" : "TAG ONLY -- "
                        "nobody will learn us from this", (int)ok);
    else       ESP_LOGE(TAG, "hello announce did not build (%d)", n);
}

static void on_frame(const uint8_t *frame, size_t len, void *ctx)
{
    (void)ctx;
    if (!frame && len == 0) {
        /* Freshly connected. Say we exist: the peer cannot address a station
         * it has never heard, and XPRS's ingest learns the wapp lane from
         * exactly this announce shape. */
        /*
         * This used to announce the tag "xprs" and NOTHING else, and the
         * receiving side needs a wire after the tag to reach peer_learn()
         * (`if (wl <= 0) return;` below). So the one announcement a station
         * made at the moment it joined a hub taught every listener nothing,
         * and destinations were learned only as a side effect of whatever
         * traffic happened to be aired later. Measured on the bench: a board
         * airing observations every thirty seconds was learned at once,
         * while the board airing a full wire once every ten minutes was
         * still unknown to its peer a quarter of an hour later -- with 326
         * frames received and all 326 rejected as somebody else's.
         *
         * 36.12.2 names identity announcements as one of only two lanes
         * guaranteed to cross a shared transport. This is that lane, so it
         * carries the identity.
         */
        s_hello_due = true;
        s_hello_due_us = esp_timer_get_time();
    }
    /* On a rationed hub the hello is also the hourly refresh: it is what
     * keeps this destination's path alive at the hub and tells the clients
     * that joined since who we are. */
    if (!s_hello_due && s_ann_gap_s > 0 && s_last_ann_us && ann_slot_free()) {
        s_hello_due = true;
        s_hello_due_us = esp_timer_get_time();
    }
    if (s_hello_due && clock_set()) hello_air();
    asks_send();
    if (!frame) return;

    rns_packet_t p;
    if (!rns_packet_parse(frame, len, &p)) return;

    /* A link this station opened (XPRS.md 11.2's bulk lane): its proof and
     * everything addressed to the link id go to whoever opened it. Cheap:
     * two byte compares, no crypto, and a station with no link open pays a
     * function call. */
    if (s_link_cb && (p.dest_type == RNS_DEST_LINK ||
                      (p.packet_type == RNS_PACKET_PROOF && p.context == RNS_CTX_LRPROOF))) {
        s_link_cb(&p);
        if (p.dest_type == RNS_DEST_LINK) return;
    }

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
        /* Salted with our IDENTITY hash, as reference RNS encrypts
         * (Identity.get_salt()). This passed the destination hash until
         * 2026-10-15, so only another station could write to us. */
        int pn = rns_decrypt_from(s_id.prv, s_id.hash, p.data, p.data_len,
                                  plain, sizeof plain - 1);
        if (pn <= 0) { s_other++; s_undecryptable++; return; }
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

    /* The answer to one of our asks (above): any destination of the
     * neighbour's identity, LXMF's as a rule. Rare, so worth the verify. */
    int ask = ask_for(a.dest);
    if (ask >= 0) {
        if (!rns_announce_verify(&p, &a)) { s_other++; return; }
        rns_identity_t idt;
        rns_identity_from_public(a.pub, &idt);
        uint8_t wd[RNS_HASH_LEN];
        rns_destination_hash(s_wapp_name, idt.hash, wd);
        peer_learn(s_asks[ask].call, wd, a.pub, &p);
        s_lx_learned++;
        ESP_LOGI(TAG, "%s is addressable: learned from its lx: (%d hops)",
                 s_asks[ask].call, (int)p.hops + 1);
        memset(&s_asks[ask], 0, sizeof s_asks[ask]);
        return;
    }

    if (memcmp(a.name_hash, s_lxmf_name, RNS_NAME_HASH_LEN) == 0) {
        char nm[24];
        int w = lxmf_name(a.app_data, a.app_len, nm, sizeof nm) ? want_find(nm) : -1;
        if (w >= 0 && rns_announce_verify(&p, &a)) {
            rns_identity_t idt;
            rns_identity_from_public(a.pub, &idt);
            uint8_t wd[RNS_HASH_LEN];
            rns_destination_hash(s_wapp_name, idt.hash, wd);
            peer_learn(s_want[w], wd, a.pub, &p);
            s_named++;
            ESP_LOGI(TAG, "%s is addressable: named by its LXMF announce",
                     s_want[w]);
            s_want[w][0] = 0;
            return;
        }
        s_other++;
        return;
    }

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
        peer_learn(from, a.dest, a.pub, &p);
    }

    if (s_cb) s_cb((const char *)wire, wl);
}

/* Which callsigns this station can address, by index. A COUNT was not
 * enough: a board reporting one learned peer and still broadcasting its asks
 * turned out to have learned a third station rather than the one it was
 * trying to reach, and nothing in the report could say so. */
bool xprsrns_peer_at(int i, char *call, size_t cap)
{
    if (i < 0 || i >= XRNS_PEERS || !call || !cap) return false;
    if (!s_peers || !s_peers[i].call[0]) return false;   /* a hole, not the end */
    snprintf(call, cap, "%s", s_peers[i].call);
    return true;
}

void xprsrns_set_hello(const char *wire, int len)
{
    if (!wire || len <= 0 || len >= (int)sizeof s_hello) return;
    if (s_tx_lock && xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(500)) != pdTRUE)
        return;
    memcpy(s_hello, wire, (size_t)len);
    s_hello_len = len;
    if (s_tx_lock) xSemaphoreGive(s_tx_lock);
}

/* ── Outbound ───────────────────────────────────────────────────────────── */

bool xprsrns_send(const char *wire, int len)
{
    if (!s_ready || !rns_tcp_is_up() || len <= 0) return false;
    if (len > RNS_MTU - 5 - 100) return false;   /* announce overhead */
    if (!clock_set()) { s_no_clock++; return false; }   /* see s_hello_due */
    if (s_ann_gap_s > 0) { s_paced++; return false; }    /* see s_ann_gap_s */

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

    /* A route learned a while ago may be gone from the hub's table: ask for
     * it again first. The answer is the peer's announce, relayed, which
     * refreshes the entry above; this packet goes on the route we have. */
    const uint32_t now_s = (uint32_t)time(NULL);
    if (now_s - pr->heard_s > XRNS_PATH_STALE_S) {
        static uint8_t prq[64];
        int qn = rns_path_request_build(pr->dest, prq, sizeof prq);
        if (qn > 0 && rns_tcp_send(prq, (size_t)qn)) s_path_req++;
    }

    /* The ciphertext carries an ephemeral public key, an IV and a MAC on top
     * of the wire, and the packet carries a header on top of that. */
    if (len > RNS_MTU - RNS_ENC_OVERHEAD - 32) return false;

    if (xSemaphoreTake(s_tx_lock, pdMS_TO_TICKS(2500)) != pdTRUE) {
        s_paced++;
        return false;
    }

    static uint8_t cipher[RNS_MTU];
    /* The salt is the recipient's IDENTITY hash, as reference RNS does
     * (Identity.get_salt()). It was their destination hash until
     * 2026-10-15, which only another station could open. */
    int cn = rns_encrypt_to(pr->xpub, pr->id_hash, (const uint8_t *)wire,
                            (size_t)len, NULL, NULL, cipher, sizeof cipher);
    bool ok = false;
    if (cn > 0) {
        rns_packet_t p;
        memset(&p, 0, sizeof p);
        if (pr->hops > 1 && pr->have_next_hop) {
            /* Through the hub that told us the route. */
            p.header_type       = RNS_HEADER_2;
            p.transport_type    = RNS_TRANSPORT_TRANSPORT;
            memcpy(p.transport_id, pr->next_hop, RNS_HASH_LEN);
            p.have_transport_id = true;
            s_via_hub++;
        } else {
            p.header_type    = RNS_HEADER_1;
            p.transport_type = RNS_TRANSPORT_BROADCAST;  /* a direct neighbour */
        }
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

void xprsrns_deposit(const char *wire, int len)
{
    if (!s_ready || !rns_tcp_is_up() || !wire || len <= 0) return;
    const char *list = xcfg_get("archivers", "");
    if (!list[0]) list = xcfg_get("supers", "");      /* the old key */
    char one[16];
    int w = 0;
    for (const char *c = list; ; c++) {
        if (*c && *c != ',' && *c != ' ' && w < (int)sizeof one - 1) {
            one[w++] = *c;
            continue;
        }
        one[w] = 0;
        if (w) {
            if (peer_find(one) && xprsrns_send_to(one, wire, len)) {
                s_deposited++;
            } else {
                s_deposit_nodest++;
                xprsrns_want(one);
            }
        }
        w = 0;
        if (!*c) break;
    }
}

void xprsrns_archive_stats(uint32_t *deposited, uint32_t *no_dest,
                           uint32_t *named)
{
    if (deposited) *deposited = s_deposited;
    if (no_dest) *no_dest = s_deposit_nodest;
    if (named) *named = s_named;
}

bool xprsrns_dest_hex(char out[33])
{
    if (!s_ready) return false;
    for (int i = 0; i < RNS_HASH_LEN; i++)
        snprintf(out + 2 * i, 3, "%02x", s_wapp_dest[i]);
    return true;
}

void xprsrns_addressed_stats(uint32_t *tx, uint32_t *rx, uint32_t *no_peer,
                             int *peers)
{
    if (tx) *tx = s_addressed_tx;
    if (rx) *rx = s_addressed_rx;
    if (no_peer) *no_peer = s_no_peer;
    if (peers) *peers = xprsrns_peer_count();
}

void xprsrns_route_stats(uint32_t *via_hub, uint32_t *path_req,
                         uint32_t *undecryptable, uint32_t *no_clock,
                         uint32_t *lx_learned)
{
    if (via_hub) *via_hub = s_via_hub;
    if (path_req) *path_req = s_path_req;
    if (undecryptable) *undecryptable = s_undecryptable;
    if (no_clock) *no_clock = s_no_clock;
    if (lx_learned) *lx_learned = s_lx_learned;
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
    /* Every wire unless set; xprsrns_init() rations a public hub. */
    s_ann_gap_s = atoi(xcfg_get("rns_ann_s", "0"));

    if (!identity_load()) {
        ESP_LOGE(TAG, "no identity -- bearer stays down");
        return false;
    }
    const char *aspects[] = { "wapp" };
    rns_name_hash("xprs", aspects, 1, s_wapp_name);
    const char *lx_aspects[] = { "delivery" };
    rns_name_hash("lxmf", lx_aspects, 1, s_lxmf_name);
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
    if (!s_peers) s_peers = xprs_big_calloc(XRNS_PEERS, sizeof *s_peers);
    if (!s_peers) { ESP_LOGE(TAG, "peer table: no memory"); return; }
    s_ready = true;
    ESP_LOGI(TAG, "attached to a running uplink, dest %02x%02x%02x%02x..",
             s_wapp_dest[0], s_wapp_dest[1], s_wapp_dest[2], s_wapp_dest[3]);
}

void xprsrns_feed(const uint8_t *frame, size_t len)
{
    /* (NULL, 0) is the CONNECT edge, not an empty frame -- it is what makes
     * on_frame air the hello. Rejecting it as malformed, which this did,
     * silently cost the caller the one announcement that makes it
     * addressable. */
    if (!s_ready) return;
    if (!frame || len == 0) { on_frame(NULL, 0, NULL); return; }
    on_frame(frame, len, NULL);
}

void xprsrns_init(xprsrns_wire_cb_t cb)
{
    const char *hub = xcfg_get("rns_hub", "");
    if (!hub || !hub[0]) {
        ESP_LOGI(TAG, "idle: no rns_hub configured");
        return;
    }
    if (!xprsrns_setup(cb)) return;
    if (!s_peers) s_peers = xprs_big_calloc(XRNS_PEERS, sizeof *s_peers);
    if (!s_peers) { ESP_LOGE(TAG, "peer table: no memory"); return; }

    /* "host" or "host:port". */
    char host[96];
    snprintf(host, sizeof host, "%s", hub);
    uint16_t port = RNS_TCP_DEFAULT_PORT;
    char *colon = strrchr(host, ':');
    if (colon && colon[1]) { *colon = 0; port = (uint16_t)atoi(colon + 1); }

    if (!xcfg_get("rns_ann_s", "")[0] && !host_is_private(host))
        s_ann_gap_s = 3600;

    rns_tcp_set_rx_cb(on_frame, NULL);
    rns_tcp_add_hub(host, port);
    if (rns_tcp_start(NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "uplink did not start");
        return;
    }
    s_ready = true;
    ESP_LOGI(TAG, "up: uplink %s:%u, dest %02x%02x%02x%02x.., announces: %s %ds",
             host, (unsigned)port,
             s_wapp_dest[0], s_wapp_dest[1], s_wapp_dest[2], s_wapp_dest[3],
             s_ann_gap_s > 0 ? "hello only, every" : "every wire, gap",
             s_ann_gap_s);
}

/* ── The bulk lane's needs (xprs_fwfetch) ────────────────────────────── */

void xprsrns_set_link_cb(void (*cb)(const rns_packet_t *p)) { s_link_cb = cb; }

bool xprsrns_send_packet(const uint8_t *pkt, size_t len)
{
    if (!s_ready || !rns_tcp_is_up() || !pkt || !len) return false;
    return rns_tcp_send(pkt, len);
}

bool xprsrns_peer_route(const char *call, uint8_t dest[RNS_HASH_LEN],
                        uint8_t pub[RNS_PUB_LEN], uint8_t next_hop[RNS_HASH_LEN],
                        bool *have_hop)
{
    if (!s_ready) return false;
    const xrns_peer_t *pr = peer_find(call);
    if (!pr) return false;
    if (dest) {
        /* Its `xprs/files` destination: the same identity, another name. */
        static uint8_t files_name[RNS_NAME_HASH_LEN];
        static bool have_name;
        if (!have_name) {
            const char *aspects[] = { "files" };
            rns_name_hash("xprs", aspects, 1, files_name);
            have_name = true;
        }
        rns_destination_hash(files_name, pr->id_hash, dest);
    }
    if (pub) {
        memcpy(pub, pr->xpub, RNS_KEY_HALF);
        memcpy(pub + RNS_KEY_HALF, pr->edpub, RNS_KEY_HALF);
    }
    bool hop = pr->hops > 1 && pr->have_next_hop;
    if (next_hop && hop) memcpy(next_hop, pr->next_hop, RNS_HASH_LEN);
    if (have_hop) *have_hop = hop;
    return true;
}

