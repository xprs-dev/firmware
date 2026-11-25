/* The neighbours' clock hint (xprs_auth.h, XPRS.md 11.10): platform-free,
 * no dependencies, so the host test can take it alone (test_xauth_hint_host.sh). */
#include <stdio.h>
#include <string.h>
#include "xprs_auth.h"

/* The rule xprs_app.c's clock_hint() applies on the ESP32 boards, as one
 * function both chips can call (xprs_auth.h). */
#define HINT_NEED      3
#define HINT_WINDOW_MS 600000u      /* a sample is kept ten minutes ... */
#define HINT_AGREE_S   600u
uint32_t xauth_hint_note(xauth_hint_t ring[XAUTH_HINT_N], const char *from,
                         uint32_t epoch, uint32_t now_ms)
{
    if (!ring || !from || !from[0] || !epoch) return 0;
    int slot = -1, oldest = 0;
    for (int i = 0; i < XAUTH_HINT_N; i++) {
        if (ring[i].call[0] && strcmp(ring[i].call, from) == 0) { slot = i; break; }
        if (!ring[i].call[0]) { if (slot < 0) slot = i; }
        else if (now_ms - ring[i].ms > now_ms - ring[oldest].ms) oldest = i;
    }
    if (slot < 0) slot = oldest;
    snprintf(ring[slot].call, sizeof ring[slot].call, "%s", from);
    ring[slot].epoch = epoch;
    ring[slot].ms = now_ms;
    uint32_t v[XAUTH_HINT_N];
    int n = 0;
    /* ... and carried forward by the time since it was heard, so samples
     * minutes apart compare as readings of the same moment: on a channel
     * where only a few packets say what time it is NOW (a direct result, a
     * command), three authors rarely speak inside one minute. */
    for (int i = 0; i < XAUTH_HINT_N; i++)
        if (ring[i].call[0] && now_ms - ring[i].ms <= HINT_WINDOW_MS)
            v[n++] = ring[i].epoch + (now_ms - ring[i].ms) / 1000u;
    if (n < HINT_NEED) return 0;
    for (int i = 1; i < n; i++) {                 /* small: insertion sort */
        uint32_t x = v[i];
        int j = i;
        while (j > 0 && v[j - 1] > x) { v[j] = v[j - 1]; j--; }
        v[j] = x;
    }
    uint32_t med = v[n / 2];
    int agree = 0;
    for (int i = 0; i < n; i++)
        if (v[i] + HINT_AGREE_S >= med && v[i] <= med + HINT_AGREE_S) agree++;
    return agree >= HINT_NEED ? med : 0;
}
