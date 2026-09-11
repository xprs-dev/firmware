/**
 * @file xprs_core.h
 * @brief Which core the blocking work goes on, on a chip with one core too.
 *
 * docs/esp32.md, "The two processors": the radios live on core 0, so
 * anything that blocks for milliseconds (the storage task, the index
 * writer, httpd, the Reticulum socket) is pinned to core 1. That is right
 * on the ESP32 and the S3, and on a single-core chip it is fatal: the C3
 * has no core 1, and FreeRTOS answers xTaskCreatePinnedToCore(..., 1) with
 * a failed assert and an abort, a boot loop before the station says a word.
 *
 * XPRS_WORK_CORE is core 1 where there is one and "no affinity" where there
 * is not. It fits both xTaskCreatePinnedToCore() and httpd_config_t.core_id.
 * On one core the isolation this codebase is built around does not exist,
 * and only task priorities stand between the radios and the storage.
 */
#ifndef XPRS_CORE_H
#define XPRS_CORE_H

#include "freertos/FreeRTOS.h"

#if CONFIG_FREERTOS_UNICORE
#  define XPRS_WORK_CORE tskNO_AFFINITY
#else
#  define XPRS_WORK_CORE 1
#endif

#endif /* XPRS_CORE_H */
