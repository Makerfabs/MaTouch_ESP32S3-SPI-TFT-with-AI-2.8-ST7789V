#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t voice_recorder_record_wav(const char *file_path, uint32_t duration_ms);
esp_err_t voice_recorder_record_phrase_wav(const char *file_path, uint32_t max_duration_ms);
void voice_recorder_release(void);
const char *voice_recorder_get_unavailable_reason(void);
