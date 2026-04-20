#include "voice_auto_chat.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "mimi_config.h"
#include "bus/message_bus.h"
#include "asr/asr_client.h"
#include "wifi/wifi_manager.h"
#include "voice/voice_recorder.h"

#define VOICE_AUTO_CHAT_STACK (16 * 1024)
#define VOICE_AUTO_CHAT_PRIO 4
#define VOICE_AUTO_CHAT_MAX_RECORD_MS 8000
#define VOICE_AUTO_CHAT_IDLE_MS 1200
#define VOICE_AUTO_CHAT_RETRY_MS 3000
#define VOICE_AUTO_CHAT_REPLY_TIMEOUT_MS 60000

static const char *TAG = "voice_auto_chat";
static bool s_started = false;
static volatile bool s_waiting_reply = false;

void voice_auto_chat_notify_cycle_done(void)
{
    s_waiting_reply = false;
}

static void voice_auto_chat_task(void *arg)
{
    (void)arg;

    const char *input_path = MIMI_SPIFFS_BASE "/voice_input.wav";
    char text[1024];

    while (1) {
        if (!wifi_manager_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(VOICE_AUTO_CHAT_RETRY_MS));
            continue;
        }
        if (s_waiting_reply) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        text[0] = '\0';

        esp_err_t err = voice_recorder_record_phrase_wav(input_path, VOICE_AUTO_CHAT_MAX_RECORD_MS);
        if (err == ESP_ERR_NOT_FOUND || err == ESP_ERR_INVALID_SIZE) {
            vTaskDelay(pdMS_TO_TICKS(VOICE_AUTO_CHAT_IDLE_MS));
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "record phrase failed: %s (%s)", esp_err_to_name(err), voice_recorder_get_unavailable_reason());
            vTaskDelay(pdMS_TO_TICKS(VOICE_AUTO_CHAT_RETRY_MS));
            continue;
        }

        err = asr_client_transcribe_file(input_path, text, sizeof(text));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "voice asr failed: %s (%s)", esp_err_to_name(err), asr_client_get_unavailable_reason());
            vTaskDelay(pdMS_TO_TICKS(VOICE_AUTO_CHAT_RETRY_MS));
            continue;
        }
        if (!text[0]) {
            ESP_LOGW(TAG, "voice asr returned empty text");
            vTaskDelay(pdMS_TO_TICKS(VOICE_AUTO_CHAT_IDLE_MS));
            continue;
        }

        mimi_msg_t msg = {0};
        strncpy(msg.channel, MIMI_CHAN_VOICE, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "voice_auto", sizeof(msg.chat_id) - 1);
        msg.content = strdup(text);
        if (!msg.content) {
            ESP_LOGW(TAG, "voice text alloc failed");
            vTaskDelay(pdMS_TO_TICKS(VOICE_AUTO_CHAT_RETRY_MS));
            continue;
        }

        s_waiting_reply = true;
        err = message_bus_push_inbound(&msg);
        if (err != ESP_OK) {
            free(msg.content);
            s_waiting_reply = false;
            ESP_LOGW(TAG, "voice inbound queue failed: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(VOICE_AUTO_CHAT_RETRY_MS));
            continue;
        }

        ESP_LOGI(TAG, "voice text queued: %s", text);
        uint32_t waited_ms = 0;
        while (s_waiting_reply && waited_ms < VOICE_AUTO_CHAT_REPLY_TIMEOUT_MS) {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited_ms += 200;
        }
        if (s_waiting_reply) {
            ESP_LOGW(TAG, "voice reply timeout");
            s_waiting_reply = false;
        }

        vTaskDelay(pdMS_TO_TICKS(VOICE_AUTO_CHAT_IDLE_MS));
    }
}

esp_err_t voice_auto_chat_init(void)
{
    return ESP_OK;
}

esp_err_t voice_auto_chat_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    if (xTaskCreatePinnedToCore(voice_auto_chat_task,
                                "voice_auto_chat",
                                VOICE_AUTO_CHAT_STACK,
                                NULL,
                                VOICE_AUTO_CHAT_PRIO,
                                NULL,
                                0) != pdPASS) {
        return ESP_FAIL;
    }

    s_started = true;
    return ESP_OK;
}
