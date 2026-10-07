/*
 * What the card can tell about itself: the temperature at its NTC, the
 * battery, and whether it is on a charger. Each reading powers its divider
 * only for the few milliseconds it takes (the sensor rail and the NTC
 * divider would otherwise draw continuously).
 */
#pragma once
#include <stdint.h>

void sens_begin(void);
/* Degrees C at the NTC, or false when the reading is out of the thermistor's
 * range (open or shorted). Inside a pocket this is near body heat: it is the
 * card's temperature, said as such, not the weather's. */
bool sens_temp_c(float *out);
int  sens_batt_mv(void);
int  sens_batt_pct(int mv);
bool sens_ext_power(void);       /* USB or the charging cradle */
bool sens_charging(void);
