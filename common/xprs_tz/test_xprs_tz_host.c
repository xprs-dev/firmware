/*
 * Host test for the time-zone answers (xprs_tz.h).
 *
 * The bodies below are what the two services send: ip-api.com's is copied
 * from a live answer (2026-09-11, from Germany), worldtimeapi.org's follow
 * the shape its documentation gives, because the service was refusing
 * connections the day this was written. Offsets are checked where they
 * are easy to get wrong: a negative half hour, a quarter-hour zone, and
 * values no zone uses.
 */

#include <stdio.h>
#include <string.h>

#include "xprs_tz.h"

static int checks, failures;
#define CHECK(cond, fmt, ...) do {                                            \
    checks++;                                                                 \
    if (!(cond)) {                                                            \
        failures++;                                                           \
        printf("  FAIL %s:%d  " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
    }                                                                         \
} while (0)

static void offsets(void)
{
    int o = 0;
    CHECK(xtz_parse_offset("+02:00", &o) && o == 7200, "+02:00 -> %d", o);
    CHECK(xtz_parse_offset("-05:30", &o) && o == -19800, "-05:30 -> %d", o);
    /* The one sscanf("%d:%d") got wrong: zero hours, so the sign was lost. */
    CHECK(xtz_parse_offset("-00:30", &o) && o == -1800, "-00:30 -> %d", o);
    CHECK(xtz_parse_offset("+0545", &o) && o == 20700, "+0545 -> %d", o);
    CHECK(xtz_parse_offset("+2", &o) && o == 7200, "+2 -> %d", o);
    CHECK(xtz_parse_offset("00:00", &o) && o == 0, "00:00 -> %d", o);
    CHECK(!xtz_parse_offset("+15:00", &o), "+15:00 accepted");
    CHECK(!xtz_parse_offset("+02:10", &o), "+02:10 accepted (not a quarter hour)");
    CHECK(!xtz_parse_offset("+02:99", &o), "+02:99 accepted");
    CHECK(!xtz_parse_offset("CET", &o), "CET accepted");
    CHECK(!xtz_parse_offset("", &o), "empty accepted");
    CHECK(!xtz_parse_offset("+02:00x", &o), "trailing junk accepted");

    char b[8];
    xtz_format_offset(-1800, b, sizeof b);
    CHECK(strcmp(b, "-00:30") == 0, "format -1800 -> %s", b);
    xtz_format_offset(20700, b, sizeof b);
    CHECK(strcmp(b, "+05:45") == 0, "format 20700 -> %s", b);
}

static void ipapi(void)
{
    xtz_t r;
    const char *live = "{\"status\":\"success\",\"timezone\":\"Europe/Berlin\",\"offset\":7200}";
    CHECK(xtz_parse_ipapi(live, &r), "live answer refused");
    CHECK(r.off_s == 7200, "offset %d", r.off_s);
    CHECK(strcmp(r.zone, "Europe/Berlin") == 0, "zone %s", r.zone);
    CHECK(!r.has_change, "ip-api does not name a change");
    CHECK(strcmp(r.source, "ip-api.com") == 0, "source %s", r.source);

    CHECK(!xtz_parse_ipapi("{\"status\":\"fail\",\"message\":\"private range\"}", &r),
          "a failure accepted");
    CHECK(!xtz_parse_ipapi("{\"status\":\"success\",\"offset\":7201}", &r),
          "an impossible offset accepted");
}

static void worldtime(void)
{
    xtz_t r;
    const char *summer =
        "{\"utc_offset\":\"+02:00\",\"timezone\":\"Europe/Berlin\",\"day_of_week\":5,"
        "\"day_of_year\":254,\"datetime\":\"2026-09-11T11:26:03.123456+02:00\","
        "\"utc_datetime\":\"2026-09-11T09:26:03.123456+00:00\",\"unixtime\":1789118763,"
        "\"raw_offset\":3600,\"week_number\":37,\"dst\":true,\"abbreviation\":\"CEST\","
        "\"dst_offset\":3600,\"dst_from\":\"2026-03-29T01:00:00+00:00\","
        "\"dst_until\":\"2026-10-25T01:00:00+00:00\",\"client_ip\":\"192.0.2.1\"}";
    CHECK(xtz_parse_worldtime(summer, &r), "summer refused");
    CHECK(r.off_s == 7200, "offset %d", r.off_s);
    CHECK(strcmp(r.zone, "Europe/Berlin") == 0, "zone %s", r.zone);
    CHECK(r.has_change, "the end of summer time not found");
    CHECK(r.change_utc == 1792890000LL, "change at %lld", (long long)r.change_utc);
    CHECK(r.off_after == 3600, "after the change %d", r.off_after);

    const char *winter =
        "{\"utc_offset\":\"+01:00\",\"timezone\":\"Europe/Berlin\",\"raw_offset\":3600,"
        "\"dst\":false,\"dst_offset\":0,\"dst_from\":null,\"dst_until\":null}";
    CHECK(xtz_parse_worldtime(winter, &r), "winter refused");
    CHECK(r.off_s == 3600 && !r.has_change, "winter %d %d", r.off_s, r.has_change);

    const char *nepal =
        "{\"utc_offset\":\"+05:45\",\"timezone\":\"Asia\\/Kathmandu\",\"raw_offset\":20700,"
        "\"dst\":false,\"dst_until\":null}";
    CHECK(xtz_parse_worldtime(nepal, &r), "Kathmandu refused");
    CHECK(r.off_s == 20700, "Kathmandu %d", r.off_s);
    CHECK(strcmp(r.zone, "Asia/Kathmandu") == 0, "escaped slash kept as %s", r.zone);

    /* A US zone at the end of its summer: the change is at 06:00 UTC. */
    const char *chicago =
        "{\"utc_offset\":\"-05:00\",\"timezone\":\"America/Chicago\",\"raw_offset\":-21600,"
        "\"dst\":true,\"dst_until\":\"2026-11-01T07:00:00.000+01:00\"}";
    CHECK(xtz_parse_worldtime(chicago, &r), "Chicago refused");
    CHECK(r.off_s == -18000 && r.off_after == -21600, "Chicago %d then %d",
          r.off_s, r.off_after);
    CHECK(r.change_utc == 1793512800LL, "Chicago change %lld (offset in the stamp)",
          (long long)r.change_utc);

    CHECK(!xtz_parse_worldtime("<html>502 Bad Gateway</html>", &r), "HTML accepted");
}

int main(void)
{
    offsets();
    ipapi();
    worldtime();
    printf("xprs_tz: %d checks, %d failed\n", checks, failures);
    return failures ? 1 : 0;
}
