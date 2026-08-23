/* xprs_health.c -- see the header. Small, allocation-free, and noisy only
 * when something changes. */
#include "xprs_health.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "health";

static struct {
    const char *name;
    bool        required;
    bool        up;
} s_parts[XH_MAX];

static int  s_n;
static bool s_last_ok;
static bool s_reported;
static bool s_below_floor;
static bool s_floor_reported;

void xh_expect(const char *name, bool required)
{
    if (!name || s_n >= XH_MAX) return;
    for (int i = 0; i < s_n; i++)
        if (strcmp(s_parts[i].name, name) == 0) return;   /* idempotent */
    s_parts[s_n].name     = name;
    s_parts[s_n].required = required;
    s_parts[s_n].up       = false;
    s_n++;
}

void xh_set(const char *name, bool up)
{
    if (!name) return;
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_parts[i].name, name) != 0) continue;
        s_parts[i].up = up;
        return;
    }
}

void xh_masks(uint16_t *up, uint16_t *required)
{
    uint16_t u = 0, r = 0;
    for (int i = 0; i < s_n && i < 16; i++) {
        if (s_parts[i].up)       u |= (uint16_t)(1u << i);
        if (s_parts[i].required) r |= (uint16_t)(1u << i);
    }
    if (up) *up = u;
    if (required) *required = r;
}

bool xh_all_ok(void)
{
    for (int i = 0; i < s_n; i++)
        if (s_parts[i].required && !s_parts[i].up) return false;
    return true;
}

void xh_report(bool force)
{
    bool ok = xh_all_ok();
    /* Only on a change, or when asked. A station that is fine should not
     * narrate it every fifteen seconds; a station that breaks should say
     * so at the moment it breaks, which is what the change edge is. */
    if (!force && s_reported && ok == s_last_ok) return;
    s_last_ok = ok;
    s_reported = true;

    if (!ok) {
        for (int i = 0; i < s_n; i++) {
            if (!s_parts[i].required || s_parts[i].up) continue;
            /* Named, at ERROR, one line each. The whole point is that
             * this is greppable and impossible to read past. */
            ESP_LOGE(TAG, "NOT RUNNING: %s", s_parts[i].name);
        }
    }

    /* The summary always carries the roster, so a log fragment from the
     * field is enough to tell what the board thought it had. */
    char line[160];
    int n = 0;
    for (int i = 0; i < s_n && n < (int)sizeof line - 24; i++)
        n += snprintf(line + n, sizeof line - n, "%s%s%s",
                      i ? " " : "", s_parts[i].name,
                      s_parts[i].up ? "+" : (s_parts[i].required ? "!" : "-"));
    if (n <= 0) return;

    if (ok) ESP_LOGW(TAG, "station up: %s", line);
    else    ESP_LOGE(TAG, "STATION DEGRADED: %s", line);
}

void xh_heap_floor(unsigned expected_free)
{
    /* INTERNAL, deliberately, and this is not a refinement -- it is the
     * difference between this check working and not existing.
     *
     * esp_get_free_heap_size() counts every byte the allocator can hand
     * out, and on a board with PSRAM that is eight megabytes. The floor
     * would then never be crossed and this function would go quiet
     * forever, which is precisely the failure it was written to catch.
     *
     * Measured on the T-Deck the day PSRAM was switched on: the heartbeat
     * reported heap=8,367,348 while the HTTP server and the index writer
     * had both failed to get a task stack, because a task stack cannot
     * live in PSRAM. Free memory was never the problem; free memory OF
     * THE RIGHT KIND was.
     *
     * On a board without PSRAM this is the same number as before. */
    unsigned free_now = (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    unsigned largest  =
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    unsigned min_ever =
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    bool below = expected_free && free_now < expected_free;

    /* Edge-triggered, because this is called both at the end of boot and
     * from the heartbeat forever after. The periodic call is not
     * decoration: a board can start every subsystem cleanly and then run
     * out, and when it does, the parts do not vanish -- they stop
     * WORKING. The HTTP handle stays valid while every handler's
     * response malloc fails, which is a station that looks present and
     * answers nothing. Free heap is the honest signal for that, and this
     * is the only thing watching it. */
    if (s_floor_reported && below == s_below_floor) return;
    s_floor_reported = true;
    s_below_floor = below;

    if (!below) {
        ESP_LOGW(TAG, "heap: internal free=%u largest=%u min-ever=%u (floor %u)",
                 free_now, largest, min_ever, expected_free);
        return;
    }
    /* Loud, because this is the shape of the bug that hides every other
     * bug: the board still boots, still talks, and has quietly lost the
     * margin something else was relying on. */
    ESP_LOGE(TAG, "INTERNAL HEAP BELOW THE DOCUMENTED FLOOR: free=%u "
                  "largest=%u, expected at least %u -- something that used "
                  "to be configured is not (min-ever %u). Check this board's "
                  "sdkconfig against the tables in docs/esp32.md before "
                  "chasing anything else. NOTE: on a PSRAM board the total "
                  "heap can be megabytes while this number is the one that "
                  "decides whether a task stack or a DMA buffer can be had.",
             free_now, largest, expected_free, min_ever);
}
