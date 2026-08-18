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

#include <math.h>
#include "driver/gpio.h"
#include "lwip/sockets.h"
#include "ili9342.h"
#include "m5ui.h"

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
} flow_t;
static flow_t s_flow[FLOW_MAX];
static int s_flow_n;      /* total ever, ring position = s_flow_n % FLOW_MAX */

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

    flow_t *fl = &s_flow[s_flow_n % FLOW_MAX];
    snprintf(fl->call, sizeof fl->call, "%s", call);
    if (!xprs_get_str(&sp, "d", fl->to, sizeof fl->to)) fl->to[0] = 0;
    xprs_type(&sp, fl->type, sizeof fl->type);
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
    /* A station with two bearers is a bridge: what arrived here goes out there,
     * under the ordinary relay rules (the bearer refuses when we are already in
     * via: or the hop budget is spent). */
    xprslan_offer(wire, len);
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
    xprsnow_offer(wire, len);
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

    if (WIFI_SSID[0]) {
        wifi_config_t wc = {0};
        snprintf((char *)wc.sta.ssid, sizeof wc.sta.ssid, "%s", WIFI_SSID);
        snprintf((char *)wc.sta.password, sizeof wc.sta.password, "%s", WIFI_PASS);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_LOGI(TAG, "associating to \"%s\" — this is what puts us on the "
                      "dongle's channel", WIFI_SSID);
    } else {
        ESP_LOGW(TAG, "no WiFi credentials: staying unassociated on channel %d. "
                      "That only meets another unassociated board — an "
                      "associated dongle is on its access point's channel.",
                 ESPNOW_FALLBACK_CHANNEL);
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    if (!WIFI_SSID[0]) {
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

#define UI_PANEL_COUNT 5
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
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);

    body[0] = 0;
    m5ui_show_home(s_panel == 0);
    m5ui_show_flow(s_panel == 1);

    switch (s_panel) {
    case 0: {   /* Links: the graphic home panel */
        char d[48];
        snprintf(d, sizeof d, "ch %u, %d peers",
                 xprsnow_channel(), xprsnow_peer_count(600));
        m5ui_home_row(0, "ESP-NOW", xprsnow_is_active(), d);

        if (s_ip_str[0])
            snprintf(d, sizeof d, "%s", s_ip_str);
        else
            snprintf(d, sizeof d, "%s", WIFI_SSID[0] ? "joining..." : "down");
        m5ui_home_row(1, "WiFi / LAN", s_ip_str[0] != 0, d);

        if (!s_ip_str[0])       snprintf(d, sizeof d, "no address");
        else if (!s_inet_known) snprintf(d, sizeof d, "probing...");
        else                    snprintf(d, sizeof d, "%s",
                                          s_inet_up ? "reachable" : "down");
        m5ui_home_row(2, "Internet", s_inet_known && s_inet_up, d);

        m5ui_home_heard(s_heard_count);

        /* The scope: everybody in reach, at their estimated distance. */
        m5ui_blip_t blips[M5UI_BLIP_MAX];
        int nb = 0;
        for (int i = 0; i < SEEN_MAX && nb < M5UI_BLIP_MAX; i++) {
            if (!s_seen[i].call[0]) continue;
            if ((now - s_seen[i].last_ms) / 1000 >= UI_INRANGE_SEC) continue;
            snprintf(blips[nb].label, sizeof blips[nb].label, "%s",
                     s_seen[i].call);
            blips[nb].meters = est_distance_m(s_seen[i].rssi);
            nb++;
        }
        m5ui_radar_blips(blips, nb);
        m5ui_set_title("links 1/5");
        break;
    }
    case 1: {   /* Flow: the packets going past, newest first */
        m5ui_flow_t rows[M5UI_FLOW_ROWS];
        int nr = 0;
        for (int i = 0; i < FLOW_MAX && nr < M5UI_FLOW_ROWS; i++) {
            int idx = (s_flow_n - 1 - i);
            if (idx < 0) break;
            flow_t *fl = &s_flow[idx % FLOW_MAX];
            if (!fl->call[0]) continue;
            m5ui_flow_t *r = &rows[nr++];
            snprintf(r->from, sizeof r->from, "%s", fl->call);
            snprintf(r->to, sizeof r->to, "%s", fl->to);
            snprintf(r->type, sizeof r->type, "%s", fl->type);
            snprintf(r->link, sizeof r->link, "%s", fl->link);
            r->dist_m = fl->rssi ? est_distance_m(fl->rssi) : -1.0f;
            r->age_s = (now - fl->ms) / 1000;
        }
        m5ui_flow_rows(rows, nr);
        m5ui_set_title("flow 2/5");
        break;
    }
    case 2: {   /* Traffic */
        uint32_t rx = 0, tx = 0, cancelled = 0, dropped = 0;
        uint32_t issued = 0, done = 0, failed = 0;
        uint32_t lrx = 0, ltx = 0, lcancel = 0;
        xprsnow_stats(&rx, &tx, &cancelled, &dropped);
        xprsnow_tx_stats(&issued, &done, &failed);
        xprslan_stats(&lrx, &ltx, &lcancel);
        char last[48] = "--";
        if (s_last_call[0]) {
            snprintf(last, sizeof last, "%s, %lus ago", s_last_call,
                     (unsigned long)((now - s_last_rx_ms) / 1000));
        }
        snprintf(body, sizeof body,
                 "ESPNOW rx %lu  tx %lu/%lu  fail %lu\n"
                 "       cancel %lu  drop %lu\n"
                 "LAN    rx %lu  tx %lu  cancel %lu\n"
                 "Heard  %lu packets\n"
                 "Last   %s",
                 (unsigned long)rx, (unsigned long)done, (unsigned long)issued,
                 (unsigned long)failed,
                 (unsigned long)cancelled, (unsigned long)dropped,
                 (unsigned long)lrx, (unsigned long)ltx, (unsigned long)lcancel,
                 (unsigned long)s_heard_count, last);
        m5ui_set_title("traffic 3/5");
        break;
    }
    case 3: {   /* Devices */
        int n = seen_in_range();
        int pos = snprintf(body, sizeof body, "In reach (%d):", n);
        for (int i = 0; i < SEEN_MAX && pos < (int)sizeof body - 40; i++) {
            if (!s_seen[i].call[0]) continue;
            uint32_t age = (now - s_seen[i].last_ms) / 1000;
            if (age >= UI_INRANGE_SEC) continue;
            if (s_seen[i].rssi)
                pos += snprintf(body + pos, sizeof body - pos,
                                "\n%-8s %-6s %4d dBm ~%dm  %lus",
                                s_seen[i].call, s_seen[i].bearer,
                                s_seen[i].rssi,
                                (int)(est_distance_m(s_seen[i].rssi) + 0.5f),
                                (unsigned long)age);
            else
                pos += snprintf(body + pos, sizeof body - pos,
                                "\n%-8s %-6s          %lus",
                                s_seen[i].call, s_seen[i].bearer,
                                (unsigned long)age);
        }
        if (n == 0) snprintf(body, sizeof body,
                             "In reach (0):\nnobody heard yet");
        m5ui_set_title("devices 4/5");
        break;
    }
    default: {  /* Node */
        const char *npub = nostr_keys_get_npub();
        char np[20] = "none";
        if (npub && npub[0]) snprintf(np, sizeof np, "%.12s...", npub);
        snprintf(body, sizeof body,
                 "Callsign  %s\n"
                 "Key       %s\n"
                 "Heap      %u free\n"
                 "Rendezvous %s%s\n"
                 "Peer keys %d learned",
                 s_call, np,
                 (unsigned)esp_get_free_heap_size(),
                 xprschan_busy() ? "busy" : "idle",
                 xprschan_busy() ? " (off home channel)" : "",
                 s_peers_n);
        m5ui_set_title("node 5/5");
        break;
    }
    }
    m5ui_set_body(body);
    m5ui_set_device_count(seen_in_range());

    /* Bottom-left: rotate through in-reach callsigns, then the heard tally —
     * the T-Dongle's habit. */
    static int rot;
    char info[40] = "";
    int shown = 0, want = -1;
    int alive = seen_in_range();
    if (alive > 0) want = rot % (alive + 1);
    for (int i = 0; i < SEEN_MAX && want >= 0; i++) {
        if (!s_seen[i].call[0]) continue;
        if ((now - s_seen[i].last_ms) / 1000 >= UI_INRANGE_SEC) continue;
        if (shown++ == want) {
            snprintf(info, sizeof info, "%s via %s", s_seen[i].call,
                     s_seen[i].bearer);
            break;
        }
    }
    if (!info[0]) snprintf(info, sizeof info, "heard %lu",
                           (unsigned long)s_heard_count);
    rot++;
    m5ui_set_info(info);
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

    static btn_t ba = { BTN_A, 0, true };
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

    uint64_t next_render_us = 0;
    for (;;) {
        bool force = false;
        if (btn_pressed(&ba)) {
            s_panel = (s_panel + UI_PANEL_COUNT - 1) % UI_PANEL_COUNT;
            force = true;
            ESP_LOGI(TAG, "button A: panel %d", s_panel);
        }
        if (btn_pressed(&bc)) {
            s_panel = (s_panel + 1) % UI_PANEL_COUNT;
            force = true;
            ESP_LOGI(TAG, "button C: panel %d", s_panel);
        }
        if (btn_pressed(&bb)) {
            force = true;
            ESP_LOGI(TAG, "button B: refresh");
        }

        /* A new packet flashes the RX dot the moment it lands; the panels
         * themselves keep their cadence -- an instantly-updating list is
         * distracting to read. */
        static uint32_t last_heard;
        if (s_heard_count != last_heard) {
            last_heard = s_heard_count;
            m5ui_pulse();
        }

        /* Console debug: 'S' = screenshot over the UART, '1'..'5' = jump
         * to a panel (the buttons, but reachable from a script). */
        int ch = getchar();
        if (ch == 'S') m5ui_framedump();
        if (ch >= '1' && ch <= '0' + UI_PANEL_COUNT) {
            s_panel = ch - '1';
            force = true;
        }

        uint64_t now_us = esp_timer_get_time();
        if (force || now_us >= next_render_us) {
            ui_render();
            /* Scope and flow settle every 10 s; the counter panels at 2 s. */
            next_render_us = now_us +
                ((s_panel == 0 || s_panel == 1) ? 10000000ULL : 2000000ULL);
        }

        m5ui_update();

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

    if (nostr_keys_init() != ESP_OK || !nostr_keys_available()) {
        ESP_LOGE(TAG, "no signing key — this station cannot take part in §23.7, "
                      "which follows only invitations it can check");
    }
    derive_callsign();
    /* §3: an X3 callsign derives from the signing key, so a receiver can
     * re-derive it. This board keeps its MAC-derived X5 name only when there is
     * no key to derive from. */
    if (nostr_keys_get_callsign() && nostr_keys_get_callsign()[0]) {
        snprintf(s_call, sizeof s_call, "%s", nostr_keys_get_callsign());
    }
    ESP_LOGI(TAG, "M5Stack XPRS station %s (ESP-NOW + LAN, no BLE5 on this chip)",
             s_call);

    wifi_up();

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

    if (xprsnow_start(s_call) == ESP_OK) {
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
    if (ili9342_init(&lcd_cfg, &lcd) == ESP_OK && m5ui_init(lcd) == ESP_OK) {
        xTaskCreate(ui_task, "ui", 4096, NULL, 4, NULL);
    } else {
        ESP_LOGE(TAG, "display init failed — running headless");
    }
}
