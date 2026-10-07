/*
 * The card as a post office (XPRS.md 9.12, 12.8, 12.11): it holds mail for
 * the people it meets and hands it over the moment it hears them again.
 * The store is xprs_mailbox on the 24 KB of flash below the filesystem;
 * this file is the card's side: which packets become mail, whose keys
 * verify a receipt or a declaration, who counts as a contact, and when to
 * deliver.
 */
#pragma once
#include <stdint.h>

#include "xprsbearer.h"

typedef struct {
    const char *self;
    /* Heard directly (no via:) on any link within [ms]? */
    bool (*heard_directly)(const char *call, uint32_t ms);
} mail_cfg_t;

/* Before the SoftDevice when possible: the first open may erase pages. */
void mail_begin(const mail_cfg_t *cfg);
/* Every packet heard, on bearer [b]. [direct]: no via:, so its author is in
 * reach on [b]. May answer q:mail at once; anything that writes flash is
 * queued for mail_tick(). */
void mail_heard(const char *wire, int len, xb_t *b, bool direct);
/* From the station loop: the queued writes, then deliveries due. */
void mail_tick(void);
/* Anything queued or due? (The loop keeps its short sleep.) */
bool mail_busy(void);
/* Mail held for everybody, for the beacon's mail:. */
int  mail_count(void);
/* The adopted LoRa network kept across a restart, -1 when none is. */
int  mail_net_get(void);
void mail_net_set(int net);
/* One line of counters for the console. */
void mail_report(void);
/* Say why each packet was or was not held (console). */
void mail_trace(bool on);
