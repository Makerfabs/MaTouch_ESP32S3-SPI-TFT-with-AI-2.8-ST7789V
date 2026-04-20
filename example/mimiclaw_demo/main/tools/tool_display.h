#ifndef TOOL_DISPLAY_H
#define TOOL_DISPLAY_H

#include "esp_err.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t tool_display_show_peach_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_display_show_image_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_display_show_web_image_execute(const char *input_json, char *output, size_t output_size);
esp_err_t tool_display_generate_image_execute(const char *input_json, char *output, size_t output_size);

#ifdef __cplusplus
}
#endif

#endif
