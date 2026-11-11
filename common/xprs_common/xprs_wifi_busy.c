/* See xprs_wifi_busy.h. */
#include "xprs_wifi_busy.h"
#include "esp_timer.h"

static volatile uint32_t s_until_ms, s_last_ms;
static volatile bool     s_any;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void xprs_wifi_busy(uint32_t ms)
{
    uint32_t now = now_ms(), until = now + ms;
    if (!s_any || (int32_t)(until - s_until_ms) > 0) s_until_ms = until;
    s_last_ms = now;
    s_any = true;
}

bool xprs_wifi_is_busy(void)
{
    return s_any && (int32_t)(s_until_ms - now_ms()) > 0;
}

uint32_t xprs_wifi_busy_since_ms(void)
{
    return s_any ? now_ms() - s_last_ms : UINT32_MAX;
}
