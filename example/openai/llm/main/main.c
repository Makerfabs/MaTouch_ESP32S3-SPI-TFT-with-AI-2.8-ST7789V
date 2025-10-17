#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "protocol_examples_common.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"


#include "OpenAI.h"

static const char *TAG = "main";


// OpenAI API key
#define OPENAI_API_KEY ""

extern uint8_t ca_pem_start[] asm("_binary_ca_pem_start");
extern uint8_t ca_pem_end[] asm("_binary_ca_pem_end");


void app_main(void)
{

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    OpenAI_t *openai = OpenAICreate(OPENAI_API_KEY);
    assert(openai);
    OpenAI_ChatCompletion_t *chatCompletion = openai->chatCreate(openai);
    assert(chatCompletion);
    chatCompletion->setModel(chatCompletion, "gpt-3.5-turbo");  //Model to use for completion. Default is gpt-3.5-turbo
    chatCompletion->setSystem(chatCompletion, "You are a helpful assistant.");     //Description of the required assistant
    chatCompletion->setMaxTokens(chatCompletion, 1024);         //The maximum number of tokens to generate in the completion.
    chatCompletion->setTemperature(chatCompletion, 0.2);        //float between 0 and 1. Higher value gives more random results.
    chatCompletion->setStop(chatCompletion, "\r");              //Up to 4 sequences where the API will stop generating further tokens.
    chatCompletion->setPresencePenalty(chatCompletion, 0);      //float between -2.0 and 2.0. Positive values increase the model's likelihood to talk about new topics.
    chatCompletion->setFrequencyPenalty(chatCompletion, 0);     //float between -2.0 and 2.0. Positive values decrease the model's likelihood to repeat the same line verbatim.
    chatCompletion->setUser(chatCompletion, "OpenAI-ESP32");    //A unique identifier representing your end-user, which can help OpenAI to monitor and detect abuse.

    OpenAI_StringResponse_t *result = chatCompletion->multiModalMessage(chatCompletion, "text", "tell me a joke", false);
    assert(result);
    if (result->getLen(result) == 1) {
        ESP_LOGI(TAG, "Received message. Tokens: %"PRIu32"", result->getUsage(result));
        char *response = result->getData(result, 0);
        ESP_LOGI(TAG, "%s", response);
    } else if (result->getLen(result) > 1) {
        ESP_LOGI(TAG, "Received %"PRIu32" messages. Tokens: %"PRIu32"", result->getLen(result), result->getUsage(result));
        for (int i = 0; i < result->getLen(result); ++i) {
            char *response = result->getData(result, i);
            ESP_LOGI(TAG, "Message[%d]: %s", i, response);
        }
    } else if (result->getError(result)) {
        ESP_LOGE(TAG, "Error! %s", result->getError(result));
    } else {
        ESP_LOGE(TAG, "Unknown error!");
    }

    result->deleteResponse(result);
    openai->chatDelete(chatCompletion);
    OpenAIDelete(openai);
}

