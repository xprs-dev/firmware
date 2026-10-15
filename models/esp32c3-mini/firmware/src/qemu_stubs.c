/*
 * Built only for the c3qemu environment (XPRS_QEMU): the hardware that
 * Espressif's QEMU does not model, answered so the station boots and can be
 * measured. Never in an image for the board.
 *
 * ADC2's self-calibration runs from a global constructor and polls the SAR
 * ADC for an event QEMU never raises, so the boot stopped there for good.
 * A fixed code is what an uncalibrated ADC would use anyway.
 */
#include <stdint.h>
#include <stdbool.h>

uint32_t __wrap_adc_hal_self_calibration(int adc_n, int atten, bool internal_gnd)
{
    (void)adc_n; (void)atten; (void)internal_gnd;
    return 0;
}
