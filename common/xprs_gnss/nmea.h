/*
 * NMEA 0183, the two sentences a position needs: RMC (time, date, where,
 * whether the receiver trusts it) and GGA (fix quality, satellites, HDOP,
 * altitude). Any talker (GP, GN, GA, GL, GB, BD). Fed one byte at a time
 * from a UART; nothing allocated, nothing blocking, no floating point (the
 * coordinates are kept in millionths of a degree).
 *
 * Platform-free; host test: test_nmea_host.sh.
 */
#ifndef XPRS_NMEA_H
#define XPRS_NMEA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     valid;        /* RMC said A and GGA reported a fix */
    int32_t  lat_e6;       /* millionths of a degree, north positive */
    int32_t  lon_e6;       /* east positive */
    int32_t  alt_dm;       /* decimetres above mean sea level (GGA) */
    uint16_t hdop_x10;     /* HDOP in tenths; 0 = not reported */
    uint8_t  sats;
    uint8_t  quality;      /* GGA fix quality: 0 none, 1 GPS, 2 DGPS... */
    uint32_t utc;          /* seconds since 1970 from RMC, 0 = not known */
} nmea_fix_t;

typedef struct {
    char       line[100];
    int        n;
    bool       rmc_ok;     /* the last RMC said A */
    nmea_fix_t fix;
    uint32_t   sentences, bad;
} nmea_t;

void nmea_init(nmea_t *p);
/* One byte from the receiver. True when a sentence ended that changed the
 * fix (an RMC or a GGA with a good checksum). */
bool nmea_feed(nmea_t *p, char c);
/* One whole sentence, '$' to the checksum, without the line ending. */
bool nmea_sentence(nmea_t *p, const char *s);

/* Accuracy in metres from HDOP, the usual rule of thumb (a user range error
 * of about 5 m): what acc: is given when the receiver says nothing better. */
int nmea_acc_m(const nmea_fix_t *f);

#ifdef __cplusplus
}
#endif
#endif /* XPRS_NMEA_H */
