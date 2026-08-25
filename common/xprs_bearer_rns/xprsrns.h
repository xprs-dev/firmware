/**
 * @file xprsrns.h
 * @brief The Reticulum bearer: this archiver, reachable over RNS.
 *
 * The station's XPRS wires ride Reticulum as signed wapp-datagram announces
 * -- app_data `[len]"xprs"[wire]` on the station's own `xprs.wapp`
 * destination -- which is byte-for-byte the lane the XPRS app already
 * speaks (`wappBroadcast` / `XprsIngest.reticulum`). Nothing on the phone or
 * desktop side changes: an ESP32 archiver simply starts appearing on the
 * same funnel, its `t:service` archived under the mailbox-declaration rule,
 * its `cmd:history` replies verified against the XPRS signatures they
 * already carry.
 *
 * The uplink is `xprs_rns`'s TCP interface, pointed wherever the operator
 * says: a public hub, or -- the configuration that actually makes a station
 * reachable, given that community hubs do not cross-forward announces
 * between their own clients -- a nearby XPRS node's TCP server on 4242,
 * which ingests directly and relays onward.
 *
 * config.ini:
 *   rns_hub      host[:port]  -- empty (the default) leaves the bearer idle
 *   rns_pace_ms  minimum gap between announces (default 1100; raise toward
 *                a public hub, which polices announce rates per destination)
 *
 * Compiled out entirely when CONFIG_XPRS_BEARER_RNS is off: every call
 * below becomes a no-op and a board without the memory pays nothing.
 */
#ifndef XPRS_BEARER_RNS_H
#define XPRS_BEARER_RNS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_XPRS_BEARER_RNS

/** One XPRS wire heard over Reticulum. Called on the uplink's own task;
 *  copy and return, exactly as the other bearer callbacks do. */
typedef void (*xprsrns_wire_cb_t)(const char *wire, int len);

/**
 * Bring the bearer up: load (or mint and persist) the RNS identity, dial
 * the configured uplink, deliver inbound wires to @p cb. Idles quietly when
 * no `rns_hub` is configured. Safe to call once from app start.
 */
void xprsrns_init(xprsrns_wire_cb_t cb);

/**
 * @brief Come up on a Reticulum transport somebody else already started.
 *
 * There is one receive-callback slot on the uplink, so a board whose own
 * code owns it cannot use xprsrns_init() -- that would unhook it silently.
 * Such a board calls this and then hands every hub frame to xprsrns_feed().
 */
void xprsrns_attach(xprsrns_wire_cb_t cb);

/** One frame off the uplink, for a board that owns the receive callback. */
void xprsrns_feed(const uint8_t *frame, size_t len);

/** Air one wire as a signed wapp announce. False when the uplink is down,
 *  the wire does not fit, or the bearer is idle. Paced internally.
 *
 *  This is the BROADCAST lane, and its reach is smaller than it looks: the
 *  public community hubs do not cross-forward announces between their own
 *  clients, so a wire aired this way reaches whatever shares our uplink and
 *  nothing beyond it. Use it for what is addressed to everybody. */
bool xprsrns_send(const char *wire, int len);

/**
 * Send one wire ADDRESSED to a callsign we have heard announce.
 *
 * An encrypted single packet to that station's own destination: the lane a
 * hub actually forwards, and therefore the only way one station reaches
 * another across the internet. No link is opened and no session is kept --
 * this codec has neither -- so there is no delivery receipt either. That is
 * the right trade for anything that will be asked again on its own schedule
 * (a poll, a replay request, a gossip query) and the wrong one for mail,
 * which travels under custody instead.
 *
 * False when the uplink is down, the wire does not fit, or -- the ordinary
 * case -- we have never heard that callsign announce and so cannot address
 * it. A caller that gets false may fall back to the broadcast lane.
 */
bool xprsrns_send_to(const char *callsign, const char *wire, int len);

/** True when [callsign] can be addressed right now (we hold its destination
 *  and public key from an announce we verified). */
bool xprsrns_can_address(const char *callsign);

/** How many peers we can currently address. */
int xprsrns_peer_count(void);

/** Counters for the addressed lane: packets out, packets in, sends refused
 *  for want of a known destination, and peers currently addressable. */
void xprsrns_addressed_stats(uint32_t *tx, uint32_t *rx, uint32_t *no_peer,
                             int *peers);

/** Up = socket connected. */
bool xprsrns_is_up(void);

/** Counters for a status line: wires in, wires out, announces refused by
 *  pacing, frames that were not for us. */
void xprsrns_stats(uint32_t *rx, uint32_t *tx, uint32_t *paced,
                   uint32_t *other);

#else /* the bearer is configured out: every call is a visible no-op */

typedef void (*xprsrns_wire_cb_t)(const char *wire, int len);
static inline void xprsrns_init(xprsrns_wire_cb_t cb) { (void)cb; }
static inline void xprsrns_attach(xprsrns_wire_cb_t cb) { (void)cb; }
static inline void xprsrns_feed(const uint8_t *f, size_t n) { (void)f; (void)n; }
static inline bool xprsrns_send(const char *wire, int len)
{ (void)wire; (void)len; return false; }
static inline bool xprsrns_send_to(const char *c, const char *wire, int len)
{ (void)c; (void)wire; (void)len; return false; }
static inline bool xprsrns_can_address(const char *c) { (void)c; return false; }
static inline int xprsrns_peer_count(void) { return 0; }
static inline void xprsrns_addressed_stats(uint32_t *tx, uint32_t *rx,
                                           uint32_t *np, int *peers)
{ if (tx) *tx = 0; if (rx) *rx = 0; if (np) *np = 0; if (peers) *peers = 0; }
static inline bool xprsrns_is_up(void) { return false; }
static inline void xprsrns_stats(uint32_t *rx, uint32_t *tx, uint32_t *paced,
                                 uint32_t *other)
{ if (rx) *rx = 0; if (tx) *tx = 0; if (paced) *paced = 0; if (other) *other = 0; }

#endif /* CONFIG_XPRS_BEARER_RNS */

#ifdef __cplusplus
}
#endif
#endif /* XPRS_BEARER_RNS_H */
