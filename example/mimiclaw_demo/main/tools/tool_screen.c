#include "tools/tool_screen.h"
#include "mimi_config.h"
#include "lvgl_lcd/lvgl_lcd.h"

#include "driver/ledc.h"
#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

#define LCD_BL_GPIO 45
#define LEDC_CHANNEL LEDC_CHANNEL_0
#define LEDC_TIMER LEDC_TIMER_0
#define LEDC_FREQ 5000
#define LEDC_RESOLUTION LEDC_TIMER_8_BIT

static const char *TAG = "tool_screen_bl";

// 全局状态
static bool s_bl_on = true;
static int s_brightness = 80;

static void screen_bl_apply_state(void)
{
    uint32_t duty;

    if (!s_bl_on)
    {
        duty = 0; // 关闭
    }
    else
    {
        // 亮度范围保护
        if (s_brightness > 100)
            s_brightness = 100;
        if (s_brightness < 10)
            s_brightness = 10; // 最低 10% 防止屏幕全黑

        duty = (s_brightness * 255) / 100;
    }

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

esp_err_t tool_screen_bl_init(void)
{
    // PWM 定时器配置
    ledc_timer_config_t timer_conf = {
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz = LEDC_FREQ,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_conf);

    // PWM 通道配置
    ledc_channel_config_t channel_conf = {
        .channel = LEDC_CHANNEL,
        .duty = 0,
        .gpio_num = LCD_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .hpoint = 0,
    };
    ledc_channel_config(&channel_conf);

    // 初始化状态
    s_bl_on = true;
    s_brightness = 80;
    screen_bl_apply_state();

    ESP_LOGI(TAG, "Screen backlight initialized (pin %d)", LCD_BL_GPIO);
    return ESP_OK;
}

esp_err_t tool_screen_bl_set_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root)
    {
        snprintf(output, output_size, "Error: invalid JSON");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *enable_obj = cJSON_GetObjectItem(root, "enable");
    if (cJSON_IsBool(enable_obj))
    {
        s_bl_on = cJSON_IsTrue(enable_obj);
    }

    if (cJSON_HasObjectItem(root, "brightness"))
    {
        cJSON *b_obj = cJSON_GetObjectItem(root, "brightness");
        if (cJSON_IsNumber(b_obj))
        {
            int val = b_obj->valueint;
            if (val >= 0 && val <= 100)
                s_brightness = val;
        }
    }

    screen_bl_apply_state();

    snprintf(output, output_size, "Backlight: %s, Brightness: %d%%",
             s_bl_on ? "ON" : "OFF", s_brightness);

    cJSON_Delete(root);
    return ESP_OK;
}


esp_err_t tool_screen_bl_get_status(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enable", s_bl_on);
    cJSON_AddNumberToObject(root, "brightness", s_brightness);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        snprintf(output, output_size, "%s", json_str);
        free(json_str);
    }
    cJSON_Delete(root);
    return ESP_OK;
}



