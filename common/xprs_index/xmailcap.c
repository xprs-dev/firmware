/* xmailcap.c -- see the header. */
#include "xmailcap.h"

#include <string.h>
#include <strings.h>

static void roll(xmc_t *c, uint32_t hour)
{
    if (c->hour == hour) return;
    uint32_t rs = c->refused_sender, ra = c->refused_all;
    memset(c, 0, sizeof *c);
    c->hour = hour;
    c->refused_sender = rs;     /* the counters are for ever, the counts hourly */
    c->refused_all = ra;
}

static int find(const xmc_t *c, const char *from)
{
    for (int i = 0; i < c->n; i++)
        if (strcasecmp(c->s[i].call, from) == 0) return i;
    return -1;
}

bool xmc_may(xmc_t *c, const char *from, uint32_t hour)
{
    roll(c, hour);
    if (c->all >= XMC_ALL) { c->refused_all++; return false; }
    int i = from && from[0] ? find(c, from) : -1;
    if (i >= 0 && c->s[i].taken >= XMC_PER_SENDER) { c->refused_sender++; return false; }
    return true;
}

void xmc_took(xmc_t *c, const char *from, uint32_t hour)
{
    roll(c, hour);
    if (c->all < UINT16_MAX) c->all++;
    if (!from || !from[0]) return;
    int i = find(c, from);
    if (i < 0) {
        if (c->n >= XMC_SENDERS) return;   /* counted against the total only */
        i = c->n++;
        strncpy(c->s[i].call, from, XMC_CALL_LEN - 1);
        c->s[i].call[XMC_CALL_LEN - 1] = 0;
    }
    if (c->s[i].taken < UINT16_MAX) c->s[i].taken++;
}
