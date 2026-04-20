#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include "esp_rom_sys.h"
#include "esp_err.h"

#define TAG "tool_dht11"
#define DHT11_GPIO 4

static float temperature = 0.0f;
static float humidity = 0.0f;

esp_err_t tool_dht11_init(void)
{
    gpio_reset_pin(DHT11_GPIO);
    gpio_set_direction(DHT11_GPIO, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_level(DHT11_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "DHT11 init OK");
    return ESP_OK;
}

static bool dht11_read_raw(void)
{
    uint8_t data[5] = {0};
    gpio_set_level(DHT11_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(18));
    gpio_set_level(DHT11_GPIO, 1);
    esp_rom_delay_us(40);

    int timeout = 10000;
    while (gpio_get_level(DHT11_GPIO) == 0 && timeout-- > 0) esp_rom_delay_us(1);
    if (timeout <= 0) return false;

    timeout = 10000;
    while (gpio_get_level(DHT11_GPIO) == 1 && timeout-- > 0) esp_rom_delay_us(1);
    if (timeout <= 0) return false;

    for (int i = 0; i < 5; i++) {
        uint8_t byte = 0;
        for (int j = 0; j < 8; j++) {
            timeout = 10000;
            while (gpio_get_level(DHT11_GPIO) == 0 && timeout-- > 0) esp_rom_delay_us(1);
            esp_rom_delay_us(40);
            if (gpio_get_level(DHT11_GPIO) == 1) byte |= 1 << (7 - j);
            timeout = 10000;
            while (gpio_get_level(DHT11_GPIO) == 1 && timeout-- > 0) esp_rom_delay_us(1);
        }
        data[i] = byte;
    }

    if ((data[0] + data[1] + data[2] + data[3]) != data[4]) return false;

    humidity = (float)data[0];
    temperature = (float)data[2];
    return true;
}

// ===================== 这里完全匹配你工程的格式 =====================
esp_err_t tool_dht11_read_execute(const char *params, char *result, size_t max_len)
{
    bool ok = dht11_read_raw();
    if (!ok) {
        snprintf(result, max_len, "{\"success\":false,\"error\":\"read failed\"}");
        return ESP_OK;
    }

    snprintf(result, max_len,
             "{\"success\":true,\"temperature\":%.1f,\"humidity\":%.1f}",
             temperature, humidity);

    ESP_LOGI(TAG, "DHT11: %.1f°C, %.1f%%", temperature, humidity);
    return ESP_OK;
}