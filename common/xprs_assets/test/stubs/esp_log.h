#pragma once
#include <stdio.h>
/* Consume the tag so -Werror=unused-variable does not fire on the real
 * file's `static const char *TAG`, and print warnings so a failing test
 * shows why it failed. */
#define ESP_LOGI(t, ...) do { (void)(t); } while (0)
#define ESP_LOGD(t, ...) do { (void)(t); } while (0)
#define ESP_LOGW(t, ...) do { (void)(t); fprintf(stderr, "  [W] " __VA_ARGS__); fputc('\n', stderr); } while (0)
