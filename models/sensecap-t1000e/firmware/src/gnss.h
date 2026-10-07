/*
 * The Airoha AG3335 receiver: powered only while a fix is wanted. Its
 * backup domain (VRTC) stays up, so ephemeris and time survive between
 * fixes and most starts are hot (seconds, not a minute).
 */
#pragma once
#include "nmea.h"

void gnss_power(bool on);
bool gnss_is_on(void);
/* Drain the UART into the parser; call often while on. */
void gnss_poll(void);
const nmea_fix_t *gnss_fix(void);
/* Sentences parsed and refused (checksum, overlong) since power-up. */
void gnss_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes);
