/*
 * XPRS over the LAN — see xprslan.h for the bearer, and docs/lan.md for the
 * wire it shares with everything else that speaks it.
 *
 * What used to be most of this file — the re-air queue, the identifier rings,
 * the §13.2.1 cancel, the beacon scheduler — now lives in `xprs_bearer`,
 * because ESP-NOW wanted the same logic and the BLE relay had already written
 * it a second time. What is left here is what is actually about a LAN: a UDP
 * socket, a task to read it, and a mutex, wired to that core through five
 * function pointers.
 *
 * Everything except the socket still compiles on the host (XPRSLAN_HOST_TEST),
 * which is where the timing and the relay decisions are tested — none of that
 * needs a network, and all of it is what actually goes wrong.
 */

#include "xprslan.h"

#include <string.h>
#include <stdio.h>

#include "xprs.h"
#include "xprsbearer.h"

#ifdef XPRSLAN_HOST_TEST

#include <stdlib.h>
#define XL_LOGI(fmt, ...) ((void)0)
#define XL_LOGW(fmt, ...) ((void)0)
#define XL_LOGD(fmt, ...) ((void)0)

/* The host drives time and catches what would have gone on the wire. */
uint32_t xl_test_now_ms;
char     xl_test_aired[XPRSLAN_WIRE_MAX + 1];
int      xl_test_aired_len;
int      xl_test_air_count;
uint32_t xl_test_random = 12345;

static uint32_t xl_now_ms(void) { return xl_test_now_ms; }
static uint32_t xl_random(void) { return xl_test_random; }
static bool xl_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    memcpy(xl_test_aired, wire, (size_t)len);
    xl_test_aired[len] = 0;
    xl_test_aired_len = len;
    xl_test_air_count++;
    return true;
}
/* The host harness is single-threaded; the lock is a device concern. */
#define XL_LOCK_FN   NULL
#define XL_UNLOCK_FN NULL

#else /* on the device */

#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"

static const char *TAG = "xprslan";
#define XL_LOGI(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define XL_LOGW(fmt, ...) ESP_LOGW(TAG, fmt, ##__VA_ARGS__)
#define XL_LOGD(fmt, ...) ESP_LOGD(TAG, fmt, ##__VA_ARGS__)

static int s_fd = -1;

/* One writer on the socket at a time.
 *
 * Several tasks air on this bearer: the station's own beacons and service
 * announcements come from the relay task, a reply comes from whichever task
 * heard the ask, and the bearer's own task drains the re-air queue. They share
 * one socket, one already-aired ring and one counter, and none of the three
 * survives two writers. */
static SemaphoreHandle_t s_tx_mtx;
static void xl_lock(void *ctx)   { (void)ctx; if (s_tx_mtx) xSemaphoreTake(s_tx_mtx, portMAX_DELAY); }
static void xl_unlock(void *ctx) { (void)ctx; if (s_tx_mtx) xSemaphoreGive(s_tx_mtx); }
#define XL_LOCK_FN   xl_lock
#define XL_UNLOCK_FN xl_unlock

static uint32_t xl_now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static uint32_t xl_random(void) { return esp_random(); }

/* The station's own access point, when it has one: see
 * xprslan_set_extra_bcast(). Network byte order, 0 for none. The socket is
 * bound to the access point's own address, because once the station has
 * joined a network its default route is that network, and a subnet broadcast
 * sent from an unbound socket followed it there (the phone on the hotspot
 * heard nothing from then on, C3, 2026-09-11). ESP-IDF's lwIP routes a bound
 * socket's datagrams by their source address. */
static volatile uint32_t s_extra_bcast;
static int s_fd_ap = -1;
static xb_t s_lan;                       /* defined with the bearer, below */

/* One datagram to everyone on the wire. */
static bool xl_air(void *ctx, const char *wire, int len)
{
    (void)ctx;
    if (s_fd < 0) return false;
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(XPRSLAN_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    /* Both addresses are tried and either will do. A station that has only
     * its hotspot has no route for 255.255.255.255 (it leaves by the station
     * interface, which is down), so that send fails, and returning on it left
     * the hotspot's own subnet unsent: a phone on the hotspot heard nothing
     * from a station that had joined no network yet, which is every station
     * being set up (XPRS.md 11.10, seen on the C3, 2026-09-11). Once a network
     * is joined, 255.255.255.255 goes there, and the subnet send is what keeps
     * the hotspot's phone in the conversation. */
    int n = sendto(s_fd, wire, (size_t)len, 0, (struct sockaddr *)&to, sizeof to);
    bool ok = n == len;
    uint32_t extra = s_extra_bcast;
    if (extra) {
        to.sin_addr.s_addr = extra;
        int fd = s_fd_ap >= 0 ? s_fd_ap : s_fd;
        if (sendto(fd, wire, (size_t)len, 0, (struct sockaddr *)&to, sizeof to) == len)
            ok = true;
    }
    if (!ok) XL_LOGW("sendto failed: errno %d", errno);
    return ok;
}

bool xprslan_unicast(uint32_t ip, const char *wire, int len)
{
    if (s_fd < 0 || !ip || !wire || len <= 0) return false;
    struct sockaddr_in to = {
        .sin_family = AF_INET,
        .sin_port = htons(XPRSLAN_PORT),
        .sin_addr.s_addr = ip,
    };
    xl_lock(NULL);
    int n = sendto(s_fd, wire, (size_t)len, 0, (struct sockaddr *)&to, sizeof to);
    if (n == len) s_lan.tx_count++;     /* counted like any other datagram */
    xl_unlock(NULL);
    return n == len;
}

void xprslan_set_extra_bcast(uint32_t addr, uint32_t src)
{
    /* Under the transmit lock: xl_air may be sending on the old socket from
     * another task right now. */
    xl_lock(NULL);
    if (s_fd_ap >= 0) { close(s_fd_ap); s_fd_ap = -1; }
    if (addr && src) {
        int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (fd >= 0) {
            int one = 1;
            setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
            struct sockaddr_in me = { .sin_family = AF_INET, .sin_port = 0,
                                      .sin_addr.s_addr = src };
            if (bind(fd, (struct sockaddr *)&me, sizeof me) == 0) s_fd_ap = fd;
            else { XL_LOGW("hotspot socket: bind failed, errno %d", errno); close(fd); }
        } else {
            XL_LOGW("hotspot socket: socket() failed, errno %d", errno);
        }
    }
    /* Without a socket of its own the subnet send would leave by the default
     * route, the very fault this socket exists to stop: better not at all. */
    s_extra_bcast = s_fd_ap >= 0 ? addr : 0;
    xl_unlock(NULL);
}

#endif

/* ── This bearer ────────────────────────────────────────────────────────── */

static xprslan_rx_cb_t s_rx_cb;

/* The core speaks of a peer as an opaque 64-bit number so a MAC fits; on this
 * bearer it is an IPv4 address, which is what our callers already expect. */
static void xl_rx_shim(const char *wire, int len, uint64_t peer, int rssi)
{
    (void)rssi;                      /* a network reports no signal */
    if (s_rx_cb) s_rx_cb(wire, len, (uint32_t)peer);
}

static const xb_ops_t k_lan_ops = {
    .air = xl_air,
    .now_ms = xl_now_ms,
    .random = xl_random,
    .lock = XL_LOCK_FN,
    .unlock = XL_UNLOCK_FN,
    .ctx = NULL,
    .name = "lan",
};

void xprslan_offer(const char *wire, int len) { xb_offer(&s_lan, wire, len); }
void xprslan_echo(const char *wire, int len)  { xb_echo(&s_lan, wire, len); }
uint32_t xprslan_idle_ms(uint32_t now_ms) { return xb_idle_ms(&s_lan, now_ms); }
bool xprslan_send(const char *wire, int len)  { return xb_send(&s_lan, wire, len); }
bool xprslan_is_active(void)                  { return xb_is_active(&s_lan); }

void xprslan_set_rx_cb(xprslan_rx_cb_t cb)
{
    s_rx_cb = cb;
    xb_set_rx_cb(&s_lan, cb ? xl_rx_shim : NULL);
}
void xprslan_set_heard_cb(xprslan_heard_cb_t cb) { xb_set_heard_cb(&s_lan, cb); }

void xprslan_set_beacon(xprslan_beacon_cb_t cb, uint32_t interval_sec,
                        uint32_t first_delay_sec)
{
    xb_set_beacon(&s_lan, cb, interval_sec, first_delay_sec);
}

int xprslan_peer_count(uint32_t max_age_sec)
{
    return xb_peer_count(&s_lan, max_age_sec);
}

void xprslan_stats(uint32_t *out_rx, uint32_t *out_tx, uint32_t *out_cancelled)
{
    xb_stats(&s_lan, out_rx, out_tx, out_cancelled);
}

/* ── The socket and its task ────────────────────────────────────────────── */

#ifndef XPRSLAN_HOST_TEST

static void xprslan_task(void *arg)
{
    (void)arg;
    static char buf[XPRSLAN_WIRE_MAX + 32];
    for (;;) {
        struct sockaddr_in src;
        socklen_t slen = sizeof src;
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 100 ms */
        setsockopt(s_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

        int n = recvfrom(s_fd, buf, sizeof(buf) - 1, 0,
                         (struct sockaddr *)&src, &slen);
        if (n > 0) {
            /* Our own broadcast is not looped back to this socket by lwip, and
             * if a stack ever did, the identifier rings would drop it — so
             * there is no source-address check to get wrong. */
            buf[n] = 0;
            xb_on_wire(&s_lan, buf, n, (uint64_t)src.sin_addr.s_addr, 0);
        }
        /* The same task airs what is due, for EVERY bearer that asked to be
         * driven from here. 100 ms of granularity is well inside the 200–1200 ms
         * jitter it is implementing, and a second task would cost 5 KB of a heap
         * this board does not have. */
        xb_tick_all(xl_now_ms());
    }
}

esp_err_t xprslan_start(const char *callsign)
{
    if (xb_is_active(&s_lan)) return ESP_OK;

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (fd < 0) {
        XL_LOGW("socket() failed: errno %d", errno);
        return ESP_FAIL;
    }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(XPRSLAN_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        XL_LOGW("bind(%u) failed: errno %d", XPRSLAN_PORT, errno);
        close(fd);
        return ESP_FAIL;
    }

    s_fd = fd;
    if (!s_tx_mtx) s_tx_mtx = xSemaphoreCreateMutex();
    if (!s_tx_mtx) { close(fd); s_fd = -1; return ESP_ERR_NO_MEM; }
    xb_init(&s_lan, &k_lan_ops, callsign);
    xb_register_ticked(&s_lan);
    /* 5 KB. Every datagram costs two SHA-256 derivations (the identifier here,
     * and again when the index decides on it), a BLE re-air and a log line, all
     * on this stack — 4 KB overflowed under a burst and took the board down. */
    /* 7168 since LoRa: this task pumps EVERY registered bearer, and 5120 was
     * the floor measured when there were two. The third brought SX1262 SPI
     * transfers, a 251-byte receive buffer and its own log lines onto this
     * same stack, and a spike past the floor does not always trip the canary
     * -- it can silently corrupt the heap next door, which presented as the
     * UI task spinning inside an LVGL redraw ninety seconds later. */
    if (xTaskCreate(xprslan_task, "xprslan", 7168, NULL, 3, NULL) != pdPASS) {
        XL_LOGW("task create failed");
        close(fd);
        s_fd = -1;
        xb_stop(&s_lan);
        return ESP_FAIL;
    }
    /* This task is now the one that pumps every registered bearer. */
    xb_set_driver(true);
    XL_LOGI("XPRS on the LAN: UDP %u, callsign %s", XPRSLAN_PORT, callsign);
    return ESP_OK;
}

void xprslan_stop(void)
{
    xb_stop(&s_lan);
    if (s_fd >= 0) { close(s_fd); s_fd = -1; }
}

#else /* host: the test drives these directly */

esp_err_t xprslan_start(const char *callsign)
{
    xb_init(&s_lan, &k_lan_ops, callsign);
    xb_register_ticked(&s_lan);
    xb_set_driver(true);
    return 0;
}
void xprslan_stop(void) { xb_stop(&s_lan); }

/* Handles the host test reaches for. */
void xl_test_datagram(const char *wire, int len, uint32_t ip)
{
    xb_on_wire(&s_lan, wire, len, (uint64_t)ip, 0);
}
int xl_test_pump(uint32_t now)
{
    uint32_t before = 0;
    xb_stats(&s_lan, NULL, &before, NULL);
    xb_tick(&s_lan, now);          /* the device runs pump and beacon together */
    uint32_t after = 0;
    xb_stats(&s_lan, NULL, &after, NULL);
    return (int)(after - before);
}
void xl_test_reset(void)
{
    char call[16];
    snprintf(call, sizeof call, "%s", s_lan.call);
    xb_init(&s_lan, &k_lan_ops, call);
    xb_set_rx_cb(&s_lan, s_rx_cb ? xl_rx_shim : NULL);
    xl_test_air_count = 0;
    xl_test_aired_len = 0;
    xl_test_aired[0] = 0;
}
int xl_test_queue_len(void)
{
    int n = 0;
    for (int i = 0; i < XB_QUEUE_MAX; i++) if (s_lan.queue[i].used) n++;
    return n;
}
uint32_t xl_test_queue_due(int i) { return s_lan.queue[i].due_ms; }
/* The LAN is unmetered in the field (section 31.1: "the internet | nothing,
 * which is the trap"). The pacing it does not use is still the shared
 * engine's, so this bearer is where it gets tested. */
void xl_test_set_pace(uint32_t ms) { xb_set_pace(&s_lan, ms); }
/* 13.1's digipeater, the same engine call every bearer's *_digipeat() makes. */
void xl_test_digipeat(const char *w, int n) { xb_digipeat(&s_lan, w, n); }
uint32_t xl_test_owed_ms(void)     { return xb_owed_ms(&s_lan); }

#endif
