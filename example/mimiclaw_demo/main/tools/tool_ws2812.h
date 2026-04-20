#pragma once

#include "esp_err.h"

esp_err_t tool_ws2812_init(void);
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size);

esp_err_t tool_ws2812_get_status(const char *input_json, char *output, size_t output_size);