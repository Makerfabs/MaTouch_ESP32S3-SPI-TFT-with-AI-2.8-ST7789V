#pragma once

#include "esp_err.h"

esp_err_t voice_auto_chat_init(void);
esp_err_t voice_auto_chat_start(void);
void voice_auto_chat_notify_cycle_done(void);
