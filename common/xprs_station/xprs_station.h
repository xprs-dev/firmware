/*
 * xprs_station -- the station's generic RAM stores: who was heard, what
 * was said, and how much traffic went by. Moved out of the m5stack
 * firmware so every board with a screen (or an API) reads the same
 * picture from the same rings.
 *
 * RAM-only and radio-task safe: every call is at most one parse plus
 * small ring writes under a spinlock. Board-specific fan-out (indexer
 * enqueue, history asks, digipeat, flow logs) stays in each firmware,
 * called BESIDE xst_ingest, never through it.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "xprs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XST_SEEN_MAX  16
#define XST_CHAT_MAX  8
#define XST_SB10_N    144        /* 10-minute buckets: one day  */
#define XST_SBDAY_N   30         /* daily buckets: one month    */

typedef struct {
    char     call[10];
    char     bearer[7];          /* "espnow", "lan", "ble" ... */
    int      rssi;               /* 0 when the bearer has none (LAN) */
    uint32_t last_ms;            /* esp_timer ms when last heard */
} xst_dev_t;

typedef struct {
    char     from[10];
    char     text[120];
    char     id[XPRS_ID_LEN];    /* section 5, so replies can find it */
    char     r[XPRS_ID_LEN];     /* parent id when this is a reply (6.4) */
    uint8_t  kind;               /* 0 global, 1 scope:local, 2 direct (d:) */
    uint32_t ep;                 /* epoch when heard, 0 before NTP */
} xst_chat_t;

/* Once at boot. [own_call] may be "" when the callsign firms up later
 * (then use xst_set_call); [tz_off_sec] shifts the daily buckets so a
 * "day" is the operator's day, not UTC's. */
void xst_init(const char *own_call, int tz_off_sec);
void xst_set_call(const char *own_call);

/* Ingest one heard wire (any task; cheap). Parses internally and feeds
 * devices + chat + rx/device stats. Returns false when the wire does not
 * parse, carries no f:, or is our own echo. */
bool xst_ingest(const char *wire, int len, const char *bearer, int rssi);
/* Same, when the caller already parsed. Returns false on own echo/no f:. */
bool xst_ingest_parsed(const xprs_t *p, const char *bearer, int rssi);

/* Chat ring only -- for locally-sent messages (the API/console send path
 * hears no echo of its own wire). */
void xst_chat_note(const xprs_t *p);

/* A sighting that is not an XPRS wire (an RNS announce, a blemesh
 * neighbour): keeps the devices list honest about everything in reach. */
void xst_dev_note(const char *call, const char *bearer, int rssi);

/* Bank transmitted-packet totals into the stats rings. Call once per UI
 * tick with the CUMULATIVE total across the board's bearers; the primed
 * delta is kept here. */
void xst_tx_total(uint32_t tx_total_now);

/* Snapshots (copied out under the lock). Devices: freshest first, only
 * rows heard within [in_range_sec]. Chat: newest first. */
int  xst_devices(xst_dev_t *out, int max, int in_range_sec);
int  xst_devices_in_range(int in_range_sec);
int  xst_chat(xst_chat_t *out, int max);
/* Find a chat row by its section-5 id (reply-parent lookup). 1 = found. */
int  xst_chat_find(const char *id, xst_chat_t *out);

/* Series for the stats charts. view 0 = last 24 ten-minute buckets,
 * 1 = last 24 hours, 2 = last 30 days. Returns the point count written
 * (<= max), 0 before NTP has spoken. */
int  xst_stats_series(int view, uint16_t *dev, uint16_t *rx, uint16_t *tx,
                      int max);

/* 0 until NTP has spoken -- nothing is banked before then. */
uint32_t xst_epoch_now(void);

/* Distance from signal strength (log-distance path loss, A=-40 dBm,
 * n=2.7): honestly rough, but -46 and -85 are a room and a street apart.
 * -1 when rssi is 0 (bearer without RSSI). */
float xst_est_distance_m(int rssi);

/* Stats persistence, whole-blob (magic XST1, same format the m5stack has
 * been writing). The caller picks the path and the task; the write must
 * obey the board's storage discipline (single writer, core 1). */
void xst_stats_load(const char *path);
void xst_stats_save(const char *path);

#ifdef __cplusplus
}
#endif
