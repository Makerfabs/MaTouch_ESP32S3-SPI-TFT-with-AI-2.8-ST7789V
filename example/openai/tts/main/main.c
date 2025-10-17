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

// Set your OpenAI API key here
#define OPENAI_API_KEY ""

extern uint8_t ca_pem_start[] asm("_binary_ca_pem_start");
extern uint8_t ca_pem_end[] asm("_binary_ca_pem_end");

#define SAMPLE_RATE     16000
#define BITS_PER_SAMPLE 16
#define CHANNELS        1
#define RECORD_TIME_SEC 5
#define I2S_READ_LEN    (1024)
#define AUDIO_BUFFER_SIZE  (SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8 * RECORD_TIME_SEC)

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240
#define AUDIO_CODEC_DEFAULT_MIC_GAIN 30.0

#define AUDIO_NODE_MAX 20
#define AUDIO_NODE_DATA_SIZE  2048

static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;


static void on_stream(const uint8_t *data, size_t length)
{
    i2s_channel_write(i2s_tx_chan, data, length, NULL, pdMS_TO_TICKS(100));
}

void bsp_no_audio_codec_simplex(int input_sample_rate, int output_sample_rate, gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout, gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din) 
{
    // Create a new channel for speaker
    i2s_chan_config_t chan_cfg = {
        .id = (i2s_port_t)0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx_chan, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
			#ifdef   I2S_HW_VERSION_2
				.ext_clk_freq_hz = 0,
			#endif

        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = false,
            .bit_shift = true,
            #ifdef   I2S_HW_VERSION_2
                .left_align = true,
                .big_endian = false,
                .bit_order_lsb = false
            #endif

        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = spk_bclk, // 20
            .ws = spk_ws,   // 1
            .dout = spk_dout, // 19
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false
            }
        }
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx_chan, &std_cfg));

    if(i2s_tx_chan != NULL) {
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
    }

    ESP_LOGI(TAG, "Simplex channels created");
}

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


    bsp_no_audio_codec_simplex(16000, 24000, GPIO_NUM_20, GPIO_NUM_1, GPIO_NUM_19, GPIO_NUM_42, GPIO_NUM_2, GPIO_NUM_41);

    OpenAI_t *openai = OpenAICreate(OPENAI_API_KEY);
    assert(openai);

    OpenAI_AudioSpeech_t *audioSpeech = openai->audioSpeechCreate(openai);
    assert(audioSpeech);

    audioSpeech->setModel(audioSpeech, "tts-1-hd"); // openai tts-1-hd
    audioSpeech->setVoice(audioSpeech, "alloy"); // nova
    audioSpeech->setResponseFormat(audioSpeech, OPENAI_AUDIO_OUTPUT_FORMAT_PCM);
    audioSpeech->setSpeed(audioSpeech, 1.0);
    
    audioSpeech->speechStream(audioSpeech, "hello,tell me a joke", on_stream);

    openai->audioSpeechDelete(audioSpeech);
    OpenAIDelete(openai);
}

