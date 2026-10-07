/* Host test for nmea.c. Run: sh test_nmea_host.sh */
#include <stdio.h>
#include <string.h>

#include "nmea.h"

static int fails, checks;
#define CHECK(c, what) do { checks++; if (!(c)) { fails++; \
    printf("  FAIL %s:%d  %s\n", __func__, __LINE__, what); } } while (0)

static void feed(nmea_t *p, const char *s)
{
    for (; *s; s++) nmea_feed(p, *s);
}

int main(void)
{
    printf("nmea host tests\n");
    nmea_t p;
    nmea_init(&p);
    /* A receiver still searching: time and date, no position. */
    feed(&p, "$GNRMC,142640.000,V,,,,,,,071026,,,N*54\r\n");
    CHECK(!p.fix.valid, "no fix while RMC says V");
    CHECK(p.fix.utc == 1791383200u, "the clock comes from RMC before the fix");
    feed(&p, "$GNGGA,142641.000,3843.338,N,00908.358,W,1,07,1.2,85.4,M,50.0,M,,*67\r\n");
    feed(&p, "$GNRMC,142641.000,A,3843.338,N,00908.358,W,0.0,0.0,071026,,,A*6F\r\n");
    CHECK(p.fix.valid, "a fix once RMC is A and GGA has quality");
    CHECK(p.fix.lat_e6 == 38722300, "latitude 38 deg 43.338 min");
    CHECK(p.fix.lon_e6 == -9139300, "longitude west is negative");
    CHECK(p.fix.sats == 7 && p.fix.hdop_x10 == 12 && p.fix.alt_dm == 854, "GGA extras");
    CHECK(nmea_acc_m(&p.fix) == 6, "acc from HDOP");
    uint32_t bad = p.bad;
    feed(&p, "$GNGGA,142642.000,3843.338,N,00908.358,W,1,07,1.2,85.4,M,50.0,M,,*00\r\n");
    CHECK(p.bad == bad + 1, "a bad checksum is refused");
    feed(&p, "$GNGGA,142643.000,,,,,0,00,99.9,,M,,M,,*77\r\n");
    CHECK(!p.fix.valid, "quality 0 drops the fix");
    feed(&p, "$GPGSV,3,1,12,01,40,083,46*44\r\n");
    CHECK(p.sentences >= 4, "other sentences are counted, not parsed");
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
