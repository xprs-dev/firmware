#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef enum { ESP_PARTITION_TYPE_DATA = 1 } esp_partition_type_t;
typedef int esp_partition_subtype_t;
typedef struct { uint32_t address; uint32_t size; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(esp_partition_type_t, esp_partition_subtype_t, const char *);
esp_err_t esp_partition_read(const esp_partition_t *, size_t, void *, size_t);
