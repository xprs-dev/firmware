/*
 * How much of somebody else's mail a station takes in an hour (2026-11-27).
 *
 * A station keeps every packet it hears (xprsindex.h), and the mail among
 * them is what it hands to the recipient on sighting (XPRS.md 36.8.1). That
 * is what makes a station left in a room useful. What it must not do is let
 * one sender, or a crowd of callsigns invented on the spot, push everybody
 * else's mail out of the store: the index evicts its oldest segment when full,
 * so a flood costs the mail that was already waiting.
 *
 * Two limits per clock hour, on mail that is neither ours nor for us:
 *   - XMC_PER_SENDER from one sender;
 *   - XMC_ALL from everybody together, which is what bounds the invented
 *     callsigns: past XMC_SENDERS distinct senders in an hour, a newcomer is
 *     counted against the total only.
 *
 * Counted when the store actually took the packet (xmc_took), so a duplicate
 * or a refusal costs the sender nothing. The phone's equivalent is
 * MeshStore.perSenderPerHour / othersPerHour in the core.
 *
 * Platform-free; host test: test_xmailcap_host.sh.
 */
#ifndef XPRS_XMAILCAP_H
#define XPRS_XMAILCAP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XMC_PER_SENDER 60
#define XMC_ALL        600
#define XMC_SENDERS    48
#define XMC_CALL_LEN   16

typedef struct {
    uint32_t hour;                         /* the hour the counts belong to */
    uint16_t all;                          /* taken from everybody */
    uint8_t  n;                            /* senders in the table */
    struct { char call[XMC_CALL_LEN]; uint16_t taken; } s[XMC_SENDERS];
    uint32_t refused_sender, refused_all;  /* what the two limits turned away */
} xmc_t;

/** May mail from @p from be taken now? @p hour: seconds / 3600 (any clock
 *  that counts up will do: a fresh hour starts the counts afresh). */
bool xmc_may(xmc_t *c, const char *from, uint32_t hour);

/** The store took mail from @p from: count it. */
void xmc_took(xmc_t *c, const char *from, uint32_t hour);

#ifdef __cplusplus
}
#endif

#endif
