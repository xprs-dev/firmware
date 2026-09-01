/* Two T-Decks, BLE5 extended advertising, and nothing else.
 *
 * The question this answers: does talking to the controller directly, with no
 * NimBLE host, put the same bytes on the air that the real firmware does -- and
 * can the radio be handed back afterwards?
 *
 * One binary for both boards. Each derives its own callsign from its MAC and
 * advertises an XPRS-framed manufacturer advert; each scans and prints what it
 * heard. If A hears B and B hears A with the framing intact, the transport
 * works. If it does not, the failure is in 254 bytes of buffer and six
 * commands rather than somewhere in a 64 KB host.
 *
 * Serial keys (the console is native USB-JTAG on this board):
 *   a  advertise one frame        s  start scanning
 *   A  advertise every 2 s        S  stop scanning
 *   x  full teardown (tn_stop)    r  bring the controller back up
 *   g  serve the mesh channel: the set turns connectable, a peer that dials
 *      in and writes to FFF2 gets the same bytes back on FFF1 with "echo:"
 *      in front (docs/ble5-gatt.md)
 *   m  send "hello from <call>" down the link, if one is up
 *   ?  status and counters
 */

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"

#include "esp_ota_ops.h"
#include "radio.h"
#include "esp_heap_caps.h"
#include "driver/usb_serial_jtag.h"
#include "esp_vfs_dev.h"
#include "esp_vfs_usb_serial_jtag.h"

/* tn_port_esp diagnostics (weak elsewhere); see tinynimble.h. */
void tn_acl_stats(uint32_t *nocp_events, uint32_t *nocp_packets, int *credits, uint32_t *in_dropped);
#include "xprs_blob.h"

static const char *TAG = "probe";

/* The XPRS advert framing, verbatim from docs/esp32.md: manufacturer data,
 * company 0xFFFF, marker 0x3E, one subtype byte. 0x41 is an APRS parcel. */
#define XPRS_COMPANY_LO 0xFF
#define XPRS_COMPANY_HI 0xFF
#define XPRS_MARKER     0x3E
#define XPRS_SUB_APRS   0x41

static char s_call[10];
static volatile uint32_t s_heard, s_heard_xprs, s_sent;
static char s_last_from[32];
static int  s_last_rssi;

/* Who has actually been heard, by name. "last peer" is not proof that the
 * other deck was heard -- a third XPRS device on the bench answers to that
 * too, and one did. */
#define PEERS_MAX 6
#define PEER_CALL_MAX 32     /* same width as s_last_from, so no truncation */
static struct { char call[PEER_CALL_MAX]; uint32_t n; int rssi; } s_peer[PEERS_MAX];

static void note_peer(const char *call, int rssi)
{
    for (int i = 0; i < PEERS_MAX; i++) {
        if (s_peer[i].call[0] == 0) {
            snprintf(s_peer[i].call, sizeof s_peer[i].call, "%s", call);
            s_peer[i].n = 1; s_peer[i].rssi = rssi;
            return;
        }
        if (strcmp(s_peer[i].call, call) == 0) {
            s_peer[i].n++; s_peer[i].rssi = rssi;
            return;
        }
    }
}

/* May run in CONTROLLER context (tinynimble) or on the host task (NimBLE).
 * Either way: parse, count, and get out. */
static void on_ad(const uint8_t *data, int data_len, int rssi)
{
    s_heard++;

    /* Walk the AD structures looking for our manufacturer frame. */
    const uint8_t *p = data;
    const uint8_t *end = data + data_len;
    while (p + 2 <= end) {
        uint8_t len = p[0];
        if (len == 0 || p + 1 + len > end) break;
        uint8_t type = p[1];
        if (type == 0xFF && len >= 4 &&
            p[2] == XPRS_COMPANY_LO && p[3] == XPRS_COMPANY_HI &&
            len >= 5 && p[4] == XPRS_MARKER) {
            s_heard_xprs++;
            s_last_rssi = rssi;
            int n = len - 5;              /* after company + marker + subtype */
            if (n > (int)sizeof s_last_from - 1) n = sizeof s_last_from - 1;
            if (n > 0) {
                memcpy(s_last_from, p + 6, n);
                s_last_from[n] = 0;
                note_peer(s_last_from, rssi);
            }
            break;
        }
        p += 1 + len;
    }
}

static void advertise_once(void)
{
    /* [len][0xFF][company lo][company hi][marker][subtype][callsign...] */
    uint8_t ad[32];
    /* The length byte counts everything after itself: the AD type, the two
     * company bytes, the marker and the subtype -- five -- plus the callsign.
     * Writing 4 here truncated the callsign by one byte on the air, and the
     * only symptom was a peer name one character short. */
    int body = 5 + (int)strlen(s_call);
    int i = 0;
    ad[i++] = (uint8_t)body;
    ad[i++] = 0xFF;
    ad[i++] = XPRS_COMPANY_LO;
    ad[i++] = XPRS_COMPANY_HI;
    ad[i++] = XPRS_MARKER;
    ad[i++] = XPRS_SUB_APRS;
    memcpy(ad + i, s_call, strlen(s_call));
    i += (int)strlen(s_call);

    esp_err_t err = radio_advertise(ad, i);
    if (err == ESP_OK) s_sent++;
    else ESP_LOGE(TAG, "advertise failed: %s", esp_err_to_name(err));
}

/* Runs on the pump, i.e. this task. Answer in kind so the far end can see
 * its own bytes came back through us -- the whole point of the test. */
/* ── XBLOB server (docs/ble5-gatt.md) ───────────────────────────────────
 * The bench stand-in for a station that holds a firmware image and streams it
 * to the P1 over the 1:1 GATT link. The laptop loads the image + sha + approval
 * over serial ('I'), forwards the signed cmd:update to the P1 over GATT ('F'),
 * and the P1 then dials in and START-s; we blast. */
static xblob_t   s_srv;
static uint8_t  *s_img;
static uint32_t  s_img_size;
static uint8_t   s_img_sha[32];
static char      s_img_sig[64];
static bool      s_have_img, s_serving;

static uint32_t s_srv_ok, s_srv_busy;
static int srv_send(void *c, const uint8_t *f, int n)
{
    (void)c;
    if (!radio_gatt_connected()) return -1;
    esp_err_t e = radio_gatt_send(f, n);
    if (e == 0) { s_srv_ok++; return XBLOB_SEND_OK; }
    s_srv_busy++;
    return XBLOB_SEND_BUSY;
}
static int srv_read(void *c, uint32_t off, uint8_t *dst, int cap)
{
    (void)c;
    if (!s_have_img || off >= s_img_size) return 0;
    int n = cap; if (off + (uint32_t)n > s_img_size) n = (int)(s_img_size - off);
    memcpy(dst, s_img + off, n); return n;
}
static void srv_done(void *c, bool ok)
{
    (void)c; s_serving = false;
    radio_scan_on();
    printf("xblob-srv: transfer %s\n", ok ? "OK" : "gave up");
}
static const xblob_ops_t SRV_OPS = { NULL, srv_send, srv_read, NULL, srv_done };

/* blocking read of exactly [n] bytes from the console, ~20 s guard. */
static int read_n(uint8_t *buf, int n)
{
    int got = 0; int64_t t0 = esp_timer_get_time();
    while (got < n) {
        int c = getchar();
        if (c == EOF) { if (esp_timer_get_time() - t0 > 20000000) return got; continue; }
        buf[got++] = (uint8_t)c; t0 = esp_timer_get_time();
    }
    return got;
}

static volatile uint32_t s_gatt_rx, s_gatt_tx;
static bool s_pipe;    /* transparent serial<->GATT bridge, for OTA-over-GATT */

static void on_gatt_rx(const uint8_t *d, int n)
{
    s_gatt_rx++;
    uint8_t sha[32];
    if (xblob_is_start(d, n, sha)) {
        if (s_have_img && memcmp(sha, s_img_sha, 32) == 0) {
            printf("xblob-srv: START -> serving %lu bytes\n", (unsigned long)s_img_size);
            radio_scan_off();     /* the 83%%-duty scanner starves the link; serve first */
            xblob_server_start(&s_srv, &SRV_OPS, s_img_sha, s_img_size, 240, s_img_sig, 0);
            s_serving = true;
        } else printf("xblob-srv: START for an image we do not hold\n");
        return;
    }
    if (xblob_is_frame(d, n)) { xblob_rx(&s_srv, d, n, 0); return; }
    if (s_pipe) {
        /* A frame from the far end, verbatim, one line. The pusher on the
         * other end of this UART reads "RX>" lines as the station's answers
         * (tools/push_firmware_p1.py --gatt). XPRS wires are printable and
         * carry no newline, so a line is a frame. */
        printf("RX>%.*s\n", n, (const char *)d);
        return;
    }
    printf("  gatt rx %dB: %.*s\n", n, n > 80 ? 80 : n, (const char *)d);
    uint8_t echo[256];
    int m = snprintf((char *)echo, sizeof echo, "echo:");
    int room = radio_gatt_mtu() - m;
    if (n > room) n = room;
    memcpy(echo + m, d, n);
    esp_err_t err = radio_gatt_send(echo, m + n);
    if (err == ESP_OK) s_gatt_tx++;
    else printf("  echo failed: %s\n", esp_err_to_name(err));
}

static void status(void)
{
    printf("\n  callsign     %s\n", s_call);
    printf("  stack        %s\n", radio_name());
    printf("  controller   %s\n", radio_is_up() ? "up" : "DOWN");
    printf("  sent         %u\n", (unsigned)s_sent);
    printf("  reports      %u  (xprs-framed %u)\n",
           (unsigned)s_heard, (unsigned)s_heard_xprs);
    for (int i = 0; i < PEERS_MAX; i++)
        if (s_peer[i].call[0])
            printf("  heard        %-10s x%-6u  %d dBm\n",
                   s_peer[i].call, (unsigned)s_peer[i].n, s_peer[i].rssi);
    printf("  xblob srv ok=%u busy=%u serving=%d have=%d nb=%u state=%u sent=%u ack=%u cur=%u\n",
           (unsigned)s_srv_ok,(unsigned)s_srv_busy,(int)s_serving,(int)s_have_img,
           (unsigned)s_srv.nblocks,(unsigned)s_srv.state,
           (unsigned)s_srv.frames_sent,(unsigned)s_srv.ack,(unsigned)s_srv.cursor);
    { uint32_t ne,np,dr; int cr; tn_acl_stats(&ne,&np,&cr,&dr);
      printf("  acl nocp_evts=%u nocp_pkts=%u credits=%d in_dropped=%u\n",(unsigned)ne,(unsigned)np,cr,(unsigned)dr); }
    printf("  link         %s, mtu %d, gatt rx %u tx %u\n",
           radio_gatt_connected() ? "UP" : "none", radio_gatt_mtu(),
           (unsigned)s_gatt_rx, (unsigned)s_gatt_tx);
    printf("  internal heap %u free, largest %u, min-ever %u\n\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
}

static esp_err_t radio_up(void)
{
    return radio_start(on_ad);
}

static void scan_on(void)
{
    esp_err_t err = radio_scan_on();
    printf("  scan %s\n", err == ESP_OK ? "on" : esp_err_to_name(err));
}

void app_main(void)
{
    /* Confirm this image so the T-Deck's OTA does not roll it back to its
     * normal firmware on the first reboot -- the probe has no health check of
     * its own and would otherwise be reverted, taking the GATT server with it. */
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t ost;
    if (run && esp_ota_get_state_partition(run, &ost) == ESP_OK && ost == ESP_OTA_IMG_PENDING_VERIFY)
        esp_ota_mark_app_valid_cancel_rollback();

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_call, sizeof s_call, "X%02X%02X", mac[4], mac[5]);

    printf("\n=== BLE probe [%s] :: %s ===\n", radio_name(), s_call);
    printf("heap before controller: %u internal\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (radio_up() != ESP_OK) {
        ESP_LOGE(TAG, "radio would not come up -- nothing to test");
        return;
    }
    printf("heap after  controller: %u internal\n",
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    scan_on();
    advertise_once();
    printf("advertising and scanning. keys: a A s S x r ?\n");

    bool repeat = true;
    int64_t next = 0;
    for (;;) {
        int c = getchar();
        if (c > 0 && s_pipe) {
            /* Pipe mode: every line the pusher writes becomes one GATT frame.
             * '.' alone on a line leaves pipe mode. */
            static char line[256];
            static int  ln;
            if (c == '\r' || c == '\n') {        /* CR or LF ends a line */
                if (ln == 1 && line[0] == '.') { s_pipe = false; printf("  pipe off\n"); }
                else if (ln > 0) {
                    /* radio_gatt_send blocks up to 500 ms for the controller
                     * to take the packet (tn_port_esp gatt_send_l2cap), so
                     * one call is the flow control; a failure is real. */
                    esp_err_t e = radio_gatt_send((const uint8_t *)line, ln);
                    if (e == ESP_OK) { s_gatt_tx++; printf("K\n"); }   /* ACK: sent, send the next */
                    else printf("  send failed: %s\n", esp_err_to_name(e));
                }
                ln = 0;
            } else if (ln < (int)sizeof line) line[ln++] = (char)c;
            radio_gatt_pump();
            continue;
        }
        if (c > 0) {
            switch (c) {
            case 'I': {   /* load image: u32 size, 32 sha, u8 siglen, sig, then size raw bytes */
                uint8_t hdr[37];
                if (read_n(hdr, 37) != 37) { printf("I: short header\n"); break; }
                uint32_t sz = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
                memcpy(s_img_sha, hdr + 4, 32);
                int sl = hdr[36];
                if (sl > 63) sl = 63;
                if (read_n((uint8_t *)s_img_sig, sl) != sl) { printf("I: short sig\n"); break; }
                s_img_sig[sl] = 0;
                if (s_img) { free(s_img); s_img = NULL; }
                s_img = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
                if (!s_img) s_img = malloc(sz);
                if (!s_img) { printf("I: no memory for %lu\n", (unsigned long)sz); break; }
                printf("I: loading %lu bytes...\n", (unsigned long)sz);
                /* BINARY-SAFE: the console VFS translates CR/LF on input by
                 * default, which silently rewrites bytes of a raw image --
                 * same length, different content, and the manifest hashes are
                 * then computed over the corruption, so the receiver verifies
                 * every block and the whole-file sha still fails. */
                esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
                int got = read_n(s_img, (int)sz);
                esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
                s_img_size = sz;
                s_have_img = (got == (int)sz);
                if (s_have_img) {
                    /* prove the copy before serving it */
                    extern void xprs_sha256(const uint8_t *in, size_t len, uint8_t out[32]);
                    uint8_t h[32];
                    xprs_sha256(s_img, sz, h);
                    if (memcmp(h, s_img_sha, 32) != 0) {
                        printf("I: sha MISMATCH after load -- serial not binary-safe\n");
                        s_have_img = false;
                    }
                }
                printf("I: %s (%d/%lu)\n", s_have_img ? "loaded, sha ok" : "FAILED", got, (unsigned long)sz);
                break; }
            case 'F': {   /* forward a text wire over GATT: u16 len, then len bytes */
                uint8_t lh[2];
                if (read_n(lh, 2) != 2) break;
                int wl = lh[0] | (lh[1] << 8);
                if (wl <= 0 || wl > 250) { printf("F: bad len %d\n", wl); break; }
                uint8_t w[256];
                if (read_n(w, wl) != wl) break;
                printf("F: forward %dB -> %s\n", wl, esp_err_to_name(radio_gatt_send(w, wl)));
                break; }
            case 'P': s_pipe = true; printf("  pipe on -- lines -> GATT, RX> lines <- GATT; '.' to exit\n"); break;
            case 'a': advertise_once(); printf("  sent one\n"); break;
            case 'A': repeat = !repeat; printf("  repeat %s\n", repeat ? "on" : "off"); break;
            case 's': scan_on(); break;
            case 'S': radio_scan_off(); printf("  scan off\n"); break;
            case 'x':
                /* The teardown that matters: with the controller up an
                 * unassociated WiFi station receives nothing. */
                printf("  tearing down: %s\n", esp_err_to_name(radio_stop()));
                status();
                break;
            case 'r': printf("  bring-up: %s\n", esp_err_to_name(radio_up()));
                      scan_on(); break;
            case 'g': printf("  serve: %s\n", esp_err_to_name(radio_gatt_serve(on_gatt_rx)));
                      advertise_once(); break;
            case 'm': {
                char msg[48];
                int n = snprintf(msg, sizeof msg, "hello from %s", s_call);
                printf("  send: %s\n", esp_err_to_name(radio_gatt_send((const uint8_t *)msg, n)));
                break; }
            case '?': status(); break;
            default: break;
            }
        }
        radio_gatt_pump();
        if (s_serving) xblob_tx_ready(&s_srv);
        if (repeat && !s_serving && radio_is_up() && esp_timer_get_time() > next) {
            advertise_once();
            next = esp_timer_get_time() + 2000000;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
