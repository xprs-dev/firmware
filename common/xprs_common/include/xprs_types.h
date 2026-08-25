#ifndef XPRS_TYPES_H
#define XPRS_TYPES_H

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Board model definitions
#define MODEL_ESP32S3_EPAPER_1IN54  1
#define MODEL_ESP32_GENERIC         99

// XPRS error codes
typedef enum {
    XPRS_OK = 0,
    XPRS_ERR_INVALID_ARG = -1,
    XPRS_ERR_NO_MEM = -2,
    XPRS_ERR_NOT_FOUND = -3,
    XPRS_ERR_NOT_SUPPORTED = -4,
    XPRS_ERR_TIMEOUT = -5,
    XPRS_ERR_INVALID_STATE = -6,
    XPRS_ERR_IO = -7,
} xprs_err_t;

// Common result type
typedef struct {
    esp_err_t err;
    const char *message;
} xprs_result_t;

#ifdef __cplusplus
}
#endif

#endif // XPRS_TYPES_H
