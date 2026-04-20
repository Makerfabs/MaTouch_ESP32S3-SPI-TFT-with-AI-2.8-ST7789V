#include "tools/tool_ws2812.h"
#include "mimi_config.h"

#include "driver/rmt_tx.h"
#include "led_strip.h"
#include "esp_log.h"
#include "cJSON.h"

#include <string.h>
#include <stdio.h>

// ==================== 配置 ====================
#define WS2812_PIN          0       // 你的数据引脚
#define LED_COUNT           1       // 灯珠数量
#define DEFAULT_BRIGHTNESS  30      // 默认亮度 0~100
// ==============================================

static const char *TAG = "tool_ws2812";
static led_strip_handle_t s_led_strip = NULL;
static bool s_led_on = false;
static uint8_t s_r = 255, s_g = 255, s_b = 255;
static uint8_t s_brightness = DEFAULT_BRIGHTNESS;

// 统一应用颜色/亮度/开关状态
static void ws2812_apply_state(void)
{
    if (!s_led_strip) return;

    if (s_led_on) {
        uint8_t r = s_r * s_brightness / 100;
        uint8_t g = s_g * s_brightness / 100;
        uint8_t b = s_b * s_brightness / 100;

        for (int i = 0; i < LED_COUNT; i++) {
            led_strip_set_pixel(s_led_strip, i, r, g, b);
        }
    } else {
        led_strip_clear(s_led_strip);
    }

    led_strip_refresh(s_led_strip);
}

// 初始化（和 GPIO 初始化风格一致）
esp_err_t tool_ws2812_init(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = WS2812_PIN,
        .max_leds = LED_COUNT,
    };

    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
    };

    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &s_led_strip));
    led_strip_clear(s_led_strip);

    ESP_LOGI(TAG, "WS2812 initialized (pin %d, %d LEDs)", WS2812_PIN, LED_COUNT);
    return ESP_OK;
}

// AI 调用：设置灯带（开关、颜色、亮度）
esp_err_t tool_ws2812_set_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    // 开关（必选）
    cJSON *enable_obj = cJSON_GetObjectItem(root, "enable");
    if (!cJSON_IsBool(enable_obj)) {
        snprintf(output, output_size, "Error: 'enable' required (true/false)");
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }
    s_led_on = cJSON_IsTrue(enable_obj);

    // 亮度（可选）
    if (cJSON_HasObjectItem(root, "brightness")) {
        cJSON *b_obj = cJSON_GetObjectItem(root, "brightness");
        if (cJSON_IsNumber(b_obj)) {
            int val = b_obj->valueint;
            if (val >= 0 && val <= 100) s_brightness = val;
        }
    }

    // RGB（可选）
    if (cJSON_HasObjectItem(root, "r")) s_r = cJSON_GetObjectItem(root, "r")->valueint;
    if (cJSON_HasObjectItem(root, "g")) s_g = cJSON_GetObjectItem(root, "g")->valueint;
    if (cJSON_HasObjectItem(root, "b")) s_b = cJSON_GetObjectItem(root, "b")->valueint;

    // 限制 0~255
    if (s_r > 255) s_r = 255;
    if (s_g > 255) s_g = 255;
    if (s_b > 255) s_b = 255;

    ws2812_apply_state();

    // 返回结果（和 GPIO 工具格式一致）
    snprintf(output, output_size,
             "WS2812 %s | brightness %d%% | RGB %d,%d,%d",
             s_led_on ? "ON" : "OFF",
             s_brightness,
             s_r, s_g, s_b);

    ESP_LOGI(TAG, "Set: %s", output);
    cJSON_Delete(root);
    return ESP_OK;
}


esp_err_t tool_ws2812_get_status(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"create json fail\"}");
        return ESP_ERR_NO_MEM;
    }

    // 组装状态 JSON
    cJSON_AddBoolToObject(root, "enable", s_led_on);
    cJSON_AddNumberToObject(root, "brightness", s_brightness);
    cJSON_AddNumberToObject(root, "r", s_r);
    cJSON_AddNumberToObject(root, "g", s_g);
    cJSON_AddNumberToObject(root, "b", s_b);

    // 转成字符串输出
    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        snprintf(output, output_size, "%s", json_str);
        cJSON_free(json_str);
    } else {
        snprintf(output, output_size, "{\"error\":\"print json fail\"}");
    }

    ESP_LOGI(TAG, "Get status: %s", output);
    cJSON_Delete(root);
    return ESP_OK;
}