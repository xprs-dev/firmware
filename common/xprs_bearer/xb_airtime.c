/* xb_airtime.c -- see the header. AN1200.13, in microseconds, then rounded
 * up to whole milliseconds at the door. */

#include "xb_airtime.h"

uint32_t xb_lora_airtime_ms(const xb_lora_air_t *p, int payload_len)
{
    if (!p || p->sf < 5 || p->sf > 12 || !p->bw_hz || payload_len <= 0)
        return 0;

    /* One symbol, in microseconds: 2^SF / BW. At SF7/125k that is 1024 us. */
    uint32_t tsym_us = (uint32_t)((((uint64_t)1u << p->sf) * 1000000u +
                                   p->bw_hz / 2) / p->bw_hz);

    /* The preamble is n + 4.25 symbols; kept in quarters to stay integral. */
    uint32_t tpre_us = (uint32_t)(((uint64_t)(p->preamble * 4u + 17u) *
                                   tsym_us) / 4u);

    /* Low-data-rate optimisation is mandatory when a symbol exceeds 16 ms
     * (SF11 and SF12 at 125 kHz) -- derived, per the header. */
    int de = tsym_us > 16000u ? 1 : 0;
    int ih = p->implicit_header ? 1 : 0;
    int crc = p->crc ? 1 : 0;

    /* payloadSymb = 8 + max(ceil((8*PL - 4*SF + 28 + 16*CRC - 20*IH)
     *                            / (4*(SF - 2*DE))) * (CR + 4), 0) */
    int32_t num = 8 * payload_len - 4 * p->sf + 28 + 16 * crc - 20 * ih;
    int32_t den = 4 * (p->sf - 2 * de);
    uint32_t nsym = 8;
    if (num > 0)
        nsym += (uint32_t)((num + den - 1) / den) * (uint32_t)(p->cr + 4);

    uint64_t total_us = (uint64_t)tpre_us + (uint64_t)nsym * tsym_us;
    return (uint32_t)((total_us + 999u) / 1000u);
}
