/**
 * @file xprs_tz.h
 * @brief Where in the world is this station: its UTC offset, found out.
 *
 * A station only has a clock once NTP has answered, and NTP needs the
 * internet. So whenever there is a time worth showing, the internet is
 * there to ask which zone the station is in, and that is all this does: it
 * asks a public time service over plain HTTP (these boards carry no TLS)
 * and hands back the zone name, the offset in force now, and, when the
 * service says, when that offset next changes.
 *
 * Two services, asked in order: worldtimeapi.org, which also names the next
 * daylight-saving change, and ip-api.com when the first does not answer.
 * Both see the station's public address; that is the price of the answer,
 * and the operator can decline it (`tz_auto = no`, or a pinned `tz`).
 *
 * The parsers are plain C with no ESP-IDF in them, so the host test can
 * feed them the services' real answers.
 */
#ifndef XPRS_TZ_H
#define XPRS_TZ_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int     off_s;          /**< seconds east of UTC, in force now          */
    char    zone[48];       /**< "Europe/Berlin", or "" when not given      */
    const char *source;     /**< which service answered                     */
    bool    has_change;     /**< the service named the next change          */
    int64_t change_utc;     /**< when, as a Unix time                       */
    int     off_after;      /**< the offset from then on                    */
} xtz_t;

/** "+02:00", "-05:30", "-00:30", "+0545", "+2" into seconds east of UTC.
 *  False for anything else, or an offset no zone uses (beyond +-14 h, or
 *  not a whole quarter hour). */
bool xtz_parse_offset(const char *s, int *off_s);

/** Render seconds east of UTC as "+02:00". */
void xtz_format_offset(int off_s, char *out, int cap);

/** worldtimeapi.org's /api/ip answer (the JSON body). */
bool xtz_parse_worldtime(const char *body, xtz_t *out);

/** ip-api.com's /json answer with fields status,timezone,offset. */
bool xtz_parse_ipapi(const char *body, xtz_t *out);

/**
 * Ask the services, in order, until one answers. Blocks for up to a few
 * seconds per service (3 s timeouts), so call it from a task that can wait
 * and is not the UI's or an event loop. Uses one static buffer: one caller.
 */
bool xtz_lookup(xtz_t *out);

#ifdef __cplusplus
}
#endif

#endif /* XPRS_TZ_H */
