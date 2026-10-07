/*
 * What deserves LoRa airtime when it is not our own: shared by the ESP32
 * stations (xprs_app.c) and the nRF52 cards, so the rule is one rule.
 * Platform-free, token walks only: these run for every packet heard.
 */
#ifndef XPRS_LR_WORTH_H
#define XPRS_LR_WORTH_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Is this wire worth a LoRa transmission on a shared mesh channel? A
 * message, sos, warning, receipt, reaction, command, result, identity,
 * mailbox, file or request: what somebody is waiting for. Presence
 * (observation, service, status) is left to the cheap bearers. On XPRS's
 * own channel ([xprs_channel] true) everything is worth it. */
bool lr_worth(const char *wire, int len, bool xprs_channel);

/* scope:local (XPRS.md 9.11.1), read before m:, never inside the text. */
bool lr_scope_local(const char *wire, int len);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_LR_WORTH_H */
