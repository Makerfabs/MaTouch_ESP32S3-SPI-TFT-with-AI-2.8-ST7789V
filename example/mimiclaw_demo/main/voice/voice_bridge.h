#pragma once

#include <stdbool.h>

#include "esp_err.h"

esp_err_t voice_bridge_init(void);
bool voice_bridge_is_configured(void);
const char *voice_bridge_get_unavailable_reason(void);
esp_err_t voice_bridge_set_base_url(const char *base_url);
esp_err_t voice_bridge_send_file(const char *file_path, char *out_text, size_t out_text_size, char *out_audio_path, size_t out_audio_path_size);
esp_err_t voice_bridge_send_and_play_file(const char *file_path, char *out_text, size_t out_text_size, char *out_audio_path, size_t out_audio_path_size);
esp_err_t voice_bridge_speak_text(const char *text);
esp_err_t voice_bridge_play_wav_file(const char *file_path);
