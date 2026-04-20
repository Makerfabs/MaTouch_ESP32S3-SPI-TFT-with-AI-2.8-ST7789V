#pragma once
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tool_dht11_init(void);
esp_err_t tool_dht11_read_execute(const char *params, char *result, size_t max_len);

#ifdef __cplusplus
}
#endif