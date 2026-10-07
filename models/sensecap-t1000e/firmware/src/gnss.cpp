/* The Airoha AG3335 receiver (gnss.h). */
#include <Arduino.h>

#include "board.h"
#include "gnss.h"

static bool   s_on, s_configured;
static nmea_t s_nmea;
static uint32_t s_bytes;

/* Meshtastic's sentences for this chip (GPS.cpp): GPS, GLONASS, Galileo and
 * BeiDou, which shortens a cold start; and only GGA and RMC, which is all
 * nmea.c reads and keeps the UART quiet. Not saved ($PAIR513): flash
 * writes on every power-up would wear the receiver's own. */
static const char *const k_setup[] = {
    "$PAIR066,1,1,1,1,0,0*3A\r\n",
    "$PAIR062,0,1*3F\r\n", "$PAIR062,1,0*3F\r\n", "$PAIR062,2,0*3C\r\n",
    "$PAIR062,3,0*3D\r\n", "$PAIR062,4,1*3B\r\n", "$PAIR062,5,0*3B\r\n",
    "$PAIR062,6,0*38\r\n",
};

void gnss_power(bool on)
{
    if (on == s_on) return;
    if (on) {
        nmea_init(&s_nmea);
        s_bytes = 0;
        s_configured = false;
        digitalWrite(T1_GNSS_EN, HIGH);
        /* A reset pulse (HIGH resets, LOW runs), as Meshtastic gives it. */
        digitalWrite(T1_GNSS_RESET, HIGH);
        delay(10);
        digitalWrite(T1_GNSS_RESET, LOW);
        Serial1.begin(T1_GNSS_BAUD);
    } else {
        Serial1.end();
        digitalWrite(T1_GNSS_EN, LOW);
    }
    s_on = on;
}

bool gnss_is_on(void) { return s_on; }

void gnss_poll(void)
{
    if (!s_on) return;
    int budget = 512;
    while (budget-- > 0 && Serial1.available()) {
        nmea_feed(&s_nmea, (char)Serial1.read());
        s_bytes++;
        /* The first sentence says it is awake and listening. */
        if (!s_configured && s_nmea.sentences > 0) {
            for (const char *c : k_setup) Serial1.write(c);
            s_configured = true;
        }
    }
}

const nmea_fix_t *gnss_fix(void) { return &s_nmea.fix; }

void gnss_counts(uint32_t *good, uint32_t *bad, uint32_t *bytes)
{
    *good = s_nmea.sentences;
    *bad = s_nmea.bad;
    *bytes = s_bytes;
}
