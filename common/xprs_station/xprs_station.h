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

/* Rows are keyed on (callsign, bearer), so a station heard on two radios
 * occupies two -- which is the point. Keyed on the callsign alone, the second
 * bearer overwrote the first, and a table that cannot hold "X on ble AND X on
 * lan" cannot describe the one path that matters on a mixed mesh: in over
 * Bluetooth, out over the LAN. 24 rows at ~32 B is under a kilobyte. */
#define XST_SEEN_MAX  24
/* The tail of a row's via: chain, "X3SEP2,X1ABCD" -- two relays. A longer
 * path keeps its nearest two; the legs beyond are unknown and the path says
 * so rather than guess. */
#define XST_VIA_LEN   24
/* What other stations said they hear and how loud (their hears:/zhq:), one
 * (observer, callsign) pair a row: the far leg of every relayed distance. */
#define XST_FAR_MAX   24

/* How many callsigns we will take from somebody else's `hears:`. 10.6.3:
 * "about twenty-five callsigns fit a packet". Taking fewer than the sender
 * can say discards topology that was already paid for. */
#define XST_HEARS_MAX 25
/* Deep enough to hold a conversation, not just the last few sayings: a UI
 * that filters by room shows a fraction of what is here, so a ring of 8
 * left a room looking empty while the station was busy. ~150 B a row. */
#define XST_CHAT_MAX  40
#define XST_SB10_N    144        /* 10-minute buckets: one day  */
#define XST_SBDAY_N   30         /* daily buckets: one month    */

typedef struct {
    char     call[XPRS_CALL_LEN];
    char     bearer[7];          /* "espnow", "lan", "ble" ... */
    int      rssi;               /* 0 when the bearer has none (LAN) */
    uint8_t  hops;               /* via: entries when last heard; 0 = direct.
                                  * What separates a neighbour from a station
                                  * somebody's relay carried to us. This
                                  * station's own callsign in via: is not a
                                  * hop (a gateway's translation names the
                                  * gateway); a Meshtastic node's own relays
                                  * count through xst_ingest_parsed_ex. */
    char     via[XST_VIA_LEN];   /* the relays that carried the last packet,
                                  * comma-joined, oldest first, NEAREST LAST,
                                  * the tail that fits; empty when direct. What
                                  * xst_dev_path() walks. */
    uint8_t  q;                  /* the signal bucket last SAID about this row
                                  * (see xst_signal_bucket). Kept because the
                                  * bucket is hysteretic: without the previous
                                  * answer there is nothing to be sticky
                                  * against. 0xff before the first reading. */
    uint32_t last_ms;            /* esp_timer ms when last heard */
} xst_dev_t;

typedef struct {
    char     from[XPRS_CALL_LEN];
    char     to[XPRS_CALL_LEN];             /* d:, empty on a broadcast. Without it a
                                  * 1:1 cannot say which conversation it
                                  * belongs to -- only that it was one. */
    char     text[120];
    char     id[XPRS_ID_LEN];    /* section 5, so replies can find it */
    char     r[XPRS_ID_LEN];     /* parent id when this is a reply (6.4) */
    uint8_t  kind;               /* 0 global, 1 scope:local, 2 direct (d:),
                                  * 3 a published status (section 27),
                                  * 4 scope:regional */
    uint8_t  urg;                /* urg: as a rank, 0 low .. 3 urgent
                                  * (XPRS 13.5). What survives a full ring. */
    uint32_t seq;                /* arrival order. The ring no longer fills
                                  * in slot order -- the least urgent row
                                  * gives way, wherever it sits -- so this
                                  * is what says which saying came last. */
    uint32_t ep;                 /* epoch when said (its ts:), else when
                                  * heard; 0 with neither */
} xst_chat_t;

/* Once at boot. [own_call] may be "" when the callsign firms up later
 * (then use xst_set_call); [tz_off_sec] shifts the daily buckets so a
 * "day" is the operator's day, not UTC's. */
void xst_init(const char *own_call, int tz_off_sec);
void xst_set_call(const char *own_call);

/* The station's offset from UTC, the one every local time on the station
 * uses (the day boundary here, the clocks on the screens). It can change
 * while running: found out over the internet, a daylight-saving change, or
 * the operator pinning one. [known] false means nobody has said and the
 * offset is 0; a screen should then say its time is UTC. Any task. */
void xst_set_tz(int off_sec, bool known);
int  xst_tz(bool *known);

/* ── The clock (XPRS.md 4.8, 11.10, 15.7) ──────────────────────────────
 * Where the wall clock came from, ranked. A wrong clock is not cosmetic: it
 * mis-dates every packet and signature the station airs, breaks the 300 s
 * command window of 11.4, and turns the bridges' old-news gates inside out.
 * So a source never moves a clock a better one set, an equal one only moves
 * it forward, and a stranger's packet sets nothing. */
typedef enum {
    XST_CLK_NONE  = 0,   /* no clock: packets say epoch:<boots>.<uptime> */
    XST_CLK_HINT  = 1,   /* several verified neighbours agreed (opt-in) */
    XST_CLK_OWNER = 2,   /* the ts: of a command that verified as the owner's */
    XST_CLK_NTP   = 3,   /* the network's, which the station believes as it is */
} xst_clock_src_t;

/* Take [epoch] from [src] when the ladder allows. Refused: a source below
 * the one that set the clock; anything but NTP turning a valid clock back
 * (11.10: each accepted word is later than the last, and the chip keeps a
 * clock across a reset that nobody this boot can vouch for -- "kept");
 * anything before 2023-11 or after 2040, whoever says it. [who] is for the
 * log. True when the clock was set. Any task. */
bool xst_clock_set(xst_clock_src_t src, uint32_t epoch, const char *who);
/* The network set the clock itself (SNTP writes it): record the source. */
void xst_clock_from_ntp(void);
xst_clock_src_t xst_clock_src(void);
const char *xst_clock_src_name(void);   /* "none", "kept", "hint", "owner", "ntp" */

/* Ingest one heard wire (any task; cheap). Parses internally and feeds
 * devices + chat + rx/device stats. Returns false when the wire does not
 * parse, carries no f:, or is our own echo. */
bool xst_ingest(const char *wire, int len, const char *bearer, int rssi);
/* Same, when the caller already parsed. Returns false on own echo/no f:. */
bool xst_ingest_parsed(const xprs_t *p, const char *bearer, int rssi);
/* Same, with [hops_more] hops that via: cannot express (a Meshtastic node
 * whose frame reached the gateway through that network's own repeaters,
 * its hop_start - hop_limit) and [relay], the last of those repeaters when
 * the bridge could name it ("" or NULL otherwise): it becomes the row's
 * via when the wire itself names no relay but this station. The rssi is
 * the frame's as measured, the repeater's transmission when relayed. */
bool xst_ingest_parsed_ex(const xprs_t *p, const char *bearer, int rssi,
                          int hops_more, const char *relay);

/** Whether LoRa also counts as a local bearer here ([lora] local, XPRS.md
 *  9.11.1): a chat heard on a bearer wider than its scope is not shown in a
 *  room (9.11), and LoRa's class depends on it. */
void xst_set_lora_local(bool local);

/* Chat ring only -- for locally-sent messages (the API/console send path
 * hears no echo of its own wire). */
void xst_chat_note(const xprs_t *p);

/* A sighting that is not an XPRS wire (an RNS announce, a blemesh
 * neighbour): keeps the devices list honest about everything in reach. */
void xst_dev_note(const char *call, const char *bearer, int rssi);
/* Same, [hops] away through repeaters of another network that via: cannot
 * name (a bridge's sighting of a Meshtastic or MeshCore node). [rssi] is
 * what the radio measured either way -- the node's own when hops is 0,
 * the last repeater's otherwise, which is still a distance to whoever it
 * was heard from; [relay] names that repeater when the bridge could,
 * "" or NULL otherwise. */
void xst_dev_note_hops(const char *call, const char *bearer, int rssi,
                       int hops, const char *relay);

/* Bank transmitted-packet totals into the stats rings. Call once per UI
 * tick with the CUMULATIVE total across the board's bearers; the primed
 * delta is kept here. */
void xst_tx_total(uint32_t tx_total_now);

/* Snapshots (copied out under the lock). Devices: freshest first, only
 * rows heard within [in_range_sec]. Chat: newest first.
 *
 * xst_devices() is ONE ROW PER STATION -- what the radar plots, the
 * Reachable panel lists and /api/xprs/devices serves. A station heard on
 * several bearers wears the bearer, hops and age of its latest contact; its
 * rssi is the strongest radio reading it has inside the window, so a LAN
 * packet arriving last does not cost the blip its distance.
 *
 * xst_devices_links() is the table as kept, one row per (callsign, bearer):
 * for the per-link `hears:` of an observation (10.6.1) and the per-link
 * counts on the home panel, and nothing a person reads as "who is there". */
int  xst_devices(xst_dev_t *out, int max, int in_range_sec);
int  xst_devices_links(xst_dev_t *out, int max, int in_range_sec);

/**
 * @brief One neighbour's signal as a single digit, 9 loud to 0 barely there.
 *
 * About 7 dB a step over -30 dBm to -100 dBm. Coarse on purpose. The archive
 * drops a repeated observation by hashing the wire (`xi_presence_hash`), so a
 * figure that moves every minute would make every beacon a new record -- which
 * is the flood that function was written to stop. A bucket moves only when the
 * signal genuinely changes tier.
 *
 * [was] is the digit last said about this neighbour, or 0xff if none, and buys
 * the hysteresis: a reading sitting on a boundary keeps the old answer until it
 * is half a step clear of it. Returns 0..9; the caller must not call it for a
 * bearer with no signal (rssi 0) -- see xst_hears_render, which omits the whole
 * digit string in that case rather than inventing a number.
 */
int xst_signal_bucket(int rssi, uint8_t was);

/**
 * @brief `hears:` for a beacon (XPRS.md 10.6.3, 36.9.4) -- the callsigns this
 *        station heard DIRECTLY (hops == 0) on [bearer] within [ttl_sec],
 *        comma-joined into [calls], most useful first.
 *
 * Per-bearer truth: a `link:lan` beacon lists only stations heard on the LAN,
 * because a claim about one radio proven on another is the lie 10.6.3 warns
 * about. [*total] gets the FULL fresh count even when the list is truncated
 * to fit, so `peers:` stays honest (10.6.4) -- and note that this counts
 * CALLSIGNS, not transport addresses, which is the corrected quantity the
 * T-Dongle already reports.
 *
 * -- Most useful first, which 10.6.3 leaves to the sender --------------------
 *
 * Ranked by callsign class before signal: section 2 says `X3` is a station,
 * relay or unattended equipment and `X1` is a person, so when the list is cut
 * it is the relays that survive it. Then the loudest, then the freshest.
 *
 * -- The ladder --------------------------------------------------------------
 *
 * [budget] is how many bytes the whole suffix may take -- ` hears:<list>` and,
 * when it fits, ` zhq:<digits>`. Three outcomes, in the order they are tried:
 *
 *   1. every neighbour listed, each with its digit in [q]
 *   2. every neighbour listed, [q] empty -- signal is what gives way first
 *   3. the top-ranked that fit, [q] empty; `peers:` says how many were left
 *
 * [q] is one digit per callsign written, same order, same count, and is left
 * empty whenever any listed neighbour has no RSSI to report (the LAN has
 * none). Pass q_cap 0 or q NULL to never ask for digits.
 *
 * @return characters written to [calls] (0 = nobody fresh on that bearer).
 */
int xst_hears_render(const char *bearer, int ttl_sec, int budget,
                     char *calls, int calls_cap, int *total,
                     char *q, int q_cap);
/* Distinct stations heard within [in_range_sec]. */
int  xst_devices_in_range(int in_range_sec);
int  xst_chat(xst_chat_t *out, int max);
/* Find a chat row by its section-5 id (reply-parent lookup). 1 = found. */
int  xst_chat_find(const char *id, xst_chat_t *out);

/* The people this station has a 1:1 history with: distinct base callsigns of
 * `kind == 2` rows where [self] is the sender or the recipient, most-recent
 * exchange first, then sorted alphabetically so the order never depends on
 * who spoke last. Returns the count written to [out] (each `char[XPRS_CALL_LEN]`), at
 * most [max]. This is the chat rail's PEOPLE list -- who we have talked to,
 * not everyone the radio has heard. */
int  xst_chat_peers(const char *self, char out[][XPRS_CALL_LEN], int max);

/* Is [call] (base match, case-insensitive) a station we have heard on any
 * bearer within [in_range_sec]? The chat's green/grey presence dot. */
bool xst_heard(const char *call, int in_range_sec);

/* Series for the stats charts. view 0 = last 24 ten-minute buckets,
 * 1 = last 24 hours, 2 = last 30 days. Returns the point count written
 * (<= max), 0 before NTP has spoken. */
int  xst_stats_series(int view, uint16_t *dev, uint16_t *rx, uint16_t *tx,
                      int max);

/* 0 until NTP has spoken -- nothing is banked before then. */
uint32_t xst_epoch_now(void);

/* XPRS.md 36.10, two archivers meet: should this station ask [call] for a
 * catch-up replay? True when [call] was silent for at least [absent_sec]
 * (or never heard) and has not been asked this absence. Call BEFORE
 * ingesting the packet that carried the serve:archive announcement --
 * ingesting first would make the peer look freshly heard. The caller still
 * owns the ask itself (clock, since:, signing, which bearer). */
bool xst_catchup_due(const char *call, int absent_sec);

/* Distance from signal strength (log-distance path loss, A=-40 dBm,
 * n=2.7): honestly rough, but -46 and -85 are a room and a street apart.
 * -1 when rssi is 0 (bearer without RSSI). */
float xst_est_distance_m(int rssi);

/* ── How far a station is when somebody carried it here ───────────────
 *
 * A relayed row's rssi is the RELAY's transmission: it says how far the
 * relay is, not the sender. The sender's distance has two legs -- this
 * station to the nearest relay, measured here, and the relay to the sender,
 * measured by the relay and said in its observation (`hears:` with the
 * `zhq:` digits, one a neighbour, xst_signal_bucket's scale), which
 * xst_far_note() keeps. xst_dev_path() adds the legs up along the row's
 * via: tail; a leg nobody has said leaves the total unknown rather than
 * short. Bench 2026-10-26: a Meshtastic node the T-Deck bridged read as
 * being where the T-Deck was. */
typedef struct {
    int  total_m;                /* the whole way, -1 when a leg is unknown */
    int  near_m;                 /* this station to the nearest relay (or to
                                  * a neighbour: then it is the total); -1 */
    int  far_m;                  /* the legs beyond the nearest relay, 0 for
                                  * a neighbour, -1 when one is unknown */
    int  floor_m;                /* the legs that ARE known, added up: at
                                  * least this far. For a node behind a
                                  * Meshtastic repeater it is the measured
                                  * distance to that repeater, which is what
                                  * a person next to this station wants to
                                  * see. -1 only when nothing is known. */
    char relay[XPRS_CALL_LEN];   /* the nearest relay, empty for a neighbour
                                  * or a repeater the bridge could not name */
} xst_path_t;
/* [in_range_sec] bounds which rows and sayings count (the relay's own row
 * for the near leg; three times that for the relays' sayings, which are
 * aired every minute or slower in a crowd). */
void xst_dev_path(const xst_dev_t *d, int in_range_sec, xst_path_t *out);
/* [observer] said it hears [call] at signal digit [q] (0 barely there .. 9
 * loud). Any task. */
void xst_far_note(const char *observer, const char *call, int q);
/* The rssi that digit stands for (the middle of its 7 dB step), 0 when the
 * observer never said or said more than [ttl_sec] ago. */
int  xst_far_rssi(const char *observer, const char *call, int ttl_sec);

/* ── The conversation, kept ───────────────────────────────────────────
 *
 * The chat ring is small and lives in RAM, so every reboot used to show an
 * empty conversation. These put it on storage -- whichever storage the
 * board has, since the caller names the path: an SD card where there is
 * one, the internal flash where there is not.
 *
 * It is deliberately NOT the packet index. The index keeps everything and
 * is mostly machine chatter -- on a busy pair of stations, receipts and
 * catch-up commands outnumber human sayings by fifty to one and push them
 * out of any recent-window query. A conversation is small, precious and
 * worth its own few kilobytes.
 *
 * Save from ONE task that may block on flash (the indexer's writer, core
 * 1), never from a bearer callback. xst_chat_dirty() says when it is worth
 * doing; it clears on read. */
bool xst_chat_dirty(void);
void xst_chat_load(const char *path);
void xst_chat_save(const char *path);

/* Stats persistence, whole-blob (magic XST1, same format the m5stack has
 * been writing). The caller picks the path and the task; the write must
 * obey the board's storage discipline (single writer, core 1). */
void xst_stats_load(const char *path);
void xst_stats_save(const char *path);

#ifdef __cplusplus
}
#endif
