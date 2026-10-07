/* What the card can tell about itself (sensors.h). */
#include <Arduino.h>
#include <math.h>

#include "board.h"
#include "sensors.h"

#define SENS_SAMPLES   8
#define SENS_AREF_MV   3000.0f   /* AR_INTERNAL_3_0 */
#define SENS_FULL      4096.0f   /* 12 bits */
#define NTC_RAIL_MV    3000      /* the sensor rail's LDO */

static float read_mv(uint8_t pin)
{
    uint32_t sum = 0;
    for (int i = 0; i < SENS_SAMPLES; i++) sum += analogRead(pin);
    return (float)sum / SENS_SAMPLES * SENS_AREF_MV / SENS_FULL;
}

void sens_begin(void)
{
    analogReference(AR_INTERNAL_3_0);
    analogReadResolution(12);
    pinMode(T1_CHG_DET, INPUT);
    pinMode(T1_EXT_PWR, INPUT);
}

int sens_batt_mv(void)
{
    return (int)(read_mv(T1_VBAT) * T1_VBAT_MULT + 0.5f);
}

/* A single-cell LiPo at rest, by the usual curve: good to a few percent in
 * the middle, which is where a percentage is read. */
int sens_batt_pct(int mv)
{
    static const int16_t k[][2] = {
        { 4180, 100 }, { 4100, 90 }, { 4000, 79 }, { 3900, 64 }, { 3800, 49 },
        { 3730, 34 }, { 3680, 22 }, { 3600, 12 }, { 3500, 5 }, { 3300, 0 },
    };
    if (mv >= k[0][0]) return 100;
    for (unsigned i = 1; i < sizeof k / sizeof k[0]; i++)
        if (mv >= k[i][0])
            return k[i][1] + (mv - k[i][0]) * (k[i - 1][1] - k[i][1]) / (k[i - 1][0] - k[i][0]);
    return 0;
}

bool sens_ext_power(void) { return digitalRead(T1_EXT_PWR) == HIGH; }
bool sens_charging(void)  { return digitalRead(T1_CHG_DET) == LOW; }

/* The divider (Meshtastic T1000xSensor.cpp): NTC from the sensor rail to the
 * pin, 8.25 kOhm from the pin to ground, so Rt = Rs (Vrail / Vpin - 1). The
 * rail is 3.0 V from its LDO, or the battery when that is lower. */
bool sens_temp_c(float *out)
{
    digitalWrite(T1_3V3_EN, HIGH);
    digitalWrite(T1_SENSOR_EN, HIGH);
    delay(5);
    float rail = read_mv(T1_VBAT) * T1_VBAT_MULT;
    if (rail > NTC_RAIL_MV) rail = NTC_RAIL_MV;
    float pin = read_mv(T1_NTC);
    digitalWrite(T1_SENSOR_EN, LOW);
    digitalWrite(T1_3V3_EN, LOW);
    if (pin < 10.0f || pin >= rail - 10.0f) return false;
    float rt = T1_NTC_RSERIES * (rail / pin - 1.0f);
    float t = 1.0f / (1.0f / 298.15f + logf(rt / T1_NTC_R25) / T1_NTC_B) - 273.15f;
    if (t < -30.0f || t > 105.0f) return false;
    *out = t;
    return true;
}
