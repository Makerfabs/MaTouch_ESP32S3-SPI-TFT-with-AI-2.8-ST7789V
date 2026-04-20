#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

esp_err_t asr_client_init(void);
bool asr_client_is_configured(void);
esp_err_t asr_client_transcribe_file(const char *file_path, char *out_text, size_t out_size);
esp_err_t asr_client_set_api_key(const char *api_key);
esp_err_t asr_client_set_provider(const char *provider);
esp_err_t asr_client_set_model(const char *model);
esp_err_t asr_client_set_base_url(const char *base_url);
const char *asr_client_get_unavailable_reason(void);