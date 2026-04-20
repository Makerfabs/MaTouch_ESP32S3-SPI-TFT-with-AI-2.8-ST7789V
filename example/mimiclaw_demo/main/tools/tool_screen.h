#ifndef TOOL_SCREEN_BACKLIGHT_H
#define TOOL_SCREEN_BACKLIGHT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tool_screen_bl_init(void);
esp_err_t tool_screen_bl_set_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_screen_bl_get_status(const char *input_json, char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif