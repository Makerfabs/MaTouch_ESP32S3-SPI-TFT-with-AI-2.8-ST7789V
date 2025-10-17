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

#include "bsp/on_board.h"
#include "bsp/display.h"

static const char *TAG = "main";


// Replace with your OpenAI API key
#define OPENAI_API_KEY ""

extern uint8_t ca_pem_start[] asm("_binary_ca_pem_start");
extern uint8_t ca_pem_end[] asm("_binary_ca_pem_end");

#define SAMPLE_RATE     16000
#define BITS_PER_SAMPLE 16
#define CHANNELS        1
#define RECORD_TIME_SEC 5
#define I2S_READ_LEN    (1024)
#define AUDIO_BUFFER_SIZE  (SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8 * RECORD_TIME_SEC)

static uint8_t *audio_buffer = NULL;

// @brief audio event flags
#define AS_EVENT_RECORD_START    BIT0
#define AS_EVENT_RECORD_AI       BIT1
#define AS_EVENT_PLAY_INPUT      BIT2


// @brief wav header
typedef struct {
    char riff[4];              // "RIFF"
    uint32_t file_size;        // 文件大小 - 8
    char wave[4];              // "WAVE"
    char fmt[4];               // "fmt "
    uint32_t fmt_size;         // fmt块大小（16）
    uint16_t audio_format;     // 音频格式（1=PCM）
    uint16_t num_channels;     // 声道数
    uint32_t sample_rate;      // 采样率
    uint32_t byte_rate;        // 字节率
    uint16_t block_align;      // 块对齐
    uint16_t bits_per_sample;  // 位深
    char data[4];              // "data"
    uint32_t data_size;        // 数据大小
} __attribute__((packed)) wav_header_t;

static EventGroupHandle_t record_event_group = NULL;

static lv_obj_t * speak_button;
static lv_obj_t * button_label;
static lv_obj_t * text_display;

#define AUDIO_CODEC_DMA_DESC_NUM 6
#define AUDIO_CODEC_DMA_FRAME_NUM 240
#define AUDIO_CODEC_DEFAULT_MIC_GAIN 30.0

#define AUDIO_NODE_MAX 20
#define AUDIO_NODE_DATA_SIZE  2048

static i2s_chan_handle_t i2s_tx_chan = NULL;
static i2s_chan_handle_t i2s_rx_chan = NULL;

static void set_chat_text_status(const char *status)
{
    if (text_display == NULL) return;
    bsp_display_lock(0);
    char buf[512];
    snprintf(buf, sizeof(buf), "%s", status);
    lv_label_set_text(text_display, buf);
    bsp_display_unlock();
}

static void button_handler(lv_event_t * e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *target = lv_event_get_target(e);

    if(target == speak_button && code == LV_EVENT_CLICKED) {
        bsp_display_lock(0);
        lv_label_set_text(button_label, "RECORDING");
        bsp_display_unlock();
        xEventGroupSetBits(record_event_group, AS_EVENT_RECORD_START);
    }

}

static void audio_input_task(void *arg) 
{
    size_t bytes_read;
    uint32_t total_bytes;
    const size_t buffer_size = I2S_READ_LEN * 2;
    const uint8_t header_size = sizeof(wav_header_t);

    while (true) {
        xEventGroupWaitBits(
            record_event_group,
            AS_EVENT_RECORD_START,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );

        ESP_LOGI(TAG, "I2S read task started");

        total_bytes = 0;

        while (total_bytes < AUDIO_BUFFER_SIZE) {
            size_t bytes_to_read = (AUDIO_BUFFER_SIZE - total_bytes) > buffer_size ? buffer_size : (AUDIO_BUFFER_SIZE - total_bytes);
            esp_err_t ret = i2s_channel_read(i2s_rx_chan, audio_buffer + header_size + total_bytes, bytes_to_read, &bytes_read, portMAX_DELAY);
            if (ret != ESP_OK) {
                ESP_LOGE(TAG, "I2S read failed: %s", esp_err_to_name(ret));
                break;
            }

            if(bytes_read) total_bytes += bytes_read;
        }
        wav_header_t header;
        memcpy(header.riff, "RIFF", 4);
        header.file_size = total_bytes + sizeof(wav_header_t) - 8;
        memcpy(header.wave, "WAVE", 4);
        memcpy(header.fmt, "fmt ", 4);
        header.fmt_size = 16;
        header.audio_format = 1; // PCM
        header.num_channels = CHANNELS;
        header.sample_rate = SAMPLE_RATE;
        header.byte_rate = SAMPLE_RATE * CHANNELS * BITS_PER_SAMPLE / 8;
        header.block_align = CHANNELS * BITS_PER_SAMPLE / 8;
        header.bits_per_sample = BITS_PER_SAMPLE;
        memcpy(header.data, "data", 4);
        header.data_size = total_bytes;
        memcpy(audio_buffer, (uint8_t *)&header, sizeof(wav_header_t));

        xEventGroupSetBits(record_event_group, AS_EVENT_RECORD_AI);

        bsp_display_lock(0);
        lv_label_set_text(button_label, "RECORD");
        bsp_display_unlock();
        
        ESP_LOGI(TAG, "I2S read task finished, total bytes: %lu", total_bytes);
    }
}
static void ai_task(void *arg)
{
    OpenAI_t *openai = OpenAICreate(OPENAI_API_KEY);
    assert(openai);

    OpenAI_AudioTranscription_t *audioTranscription = openai->audioTranscriptionCreate(openai);
    assert(audioTranscription);

    audioTranscription->setResponseFormat(audioTranscription, OPENAI_AUDIO_RESPONSE_FORMAT_JSON);
    audioTranscription->setTemperature(audioTranscription, 0.2);                                                            //float between 0 and 1. Higher value gives more random results.
    audioTranscription->setLanguage(audioTranscription, "en");      

    while(true) {
        xEventGroupWaitBits(
            record_event_group,
            AS_EVENT_RECORD_AI,
            pdTRUE,
            pdFALSE,
            portMAX_DELAY
        );
        ESP_LOGI("AS", "Starting AI...");

        char *text = audioTranscription->file(audioTranscription, audio_buffer, sizeof(wav_header_t) + AUDIO_BUFFER_SIZE, OPENAI_AUDIO_INPUT_FORMAT_WAV);
        if(text == NULL) {
            ESP_LOGE(TAG, "Failed to transcribe audio");
            continue;
        }
        ESP_LOGI(TAG, "Text: %s", text);
        set_chat_text_status(text);

        free(text);
    }
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

    // Create a new channel for MIC
    chan_cfg.id = (i2s_port_t)1;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &i2s_rx_chan));
    std_cfg.clk_cfg.sample_rate_hz = (uint32_t)input_sample_rate;
    std_cfg.gpio_cfg.bclk = mic_sck; // 42
    std_cfg.gpio_cfg.ws = mic_ws; // 2
    std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    std_cfg.gpio_cfg.din = mic_din; // 41
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx_chan, &std_cfg));

    if(i2s_tx_chan != NULL) {
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx_chan));
    }

    if(i2s_rx_chan != NULL) {
        ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx_chan));
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

    bsp_no_audio_codec_simplex(16000, 24000, GPIO_NUM_20, GPIO_NUM_1, GPIO_NUM_19, GPIO_NUM_42, GPIO_NUM_2, GPIO_NUM_41);

    ESP_ERROR_CHECK(bsp_spiffs_mount());

    record_event_group = xEventGroupCreate();
    assert(record_event_group);

    xEventGroupClearBits(record_event_group, AS_EVENT_RECORD_START | AS_EVENT_PLAY_INPUT | AS_EVENT_RECORD_AI);

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = 1,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
        }
    };
    bsp_display_start_with_config(&cfg);
    bsp_display_backlight_on();

    bsp_display_lock(0);

    lv_obj_t * screen = lv_scr_act();
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t * main_area = lv_obj_create(screen);
    lv_obj_clear_flag(main_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_grow(main_area, 1);

    lv_obj_set_style_bg_color(main_area, lv_color_make(0,0,0), 0);
    lv_obj_set_style_bg_opa(main_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(main_area, 0, 0);
    lv_obj_set_style_pad_all(main_area, 0, 0);

    text_display = lv_label_create(main_area);
    lv_obj_set_width(text_display, LV_PCT(90));
    lv_label_set_long_mode(text_display, LV_LABEL_LONG_WRAP);
    lv_label_set_text(text_display, "Ready");
    lv_obj_set_style_text_align(text_display, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_set_style_bg_opa(text_display, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_display, 0, 0);
    lv_obj_set_style_pad_all(text_display, 0, 0);
    lv_obj_align(text_display, LV_ALIGN_CENTER, 0, 0);

    speak_button = lv_button_create(screen);
    lv_obj_set_width(speak_button, 150);
    lv_obj_set_height(speak_button, 50);
    lv_obj_set_style_radius(speak_button, 25, 0);
    lv_obj_set_style_bg_color(speak_button, lv_color_hex(0x007AFF), 0);
    lv_obj_add_event_cb(speak_button, button_handler, LV_EVENT_ALL, NULL);

    button_label = lv_label_create(speak_button);
    lv_label_set_text(button_label, "RECORD");
    lv_obj_center(button_label);
    lv_obj_set_style_text_align(button_label, LV_TEXT_ALIGN_CENTER, 0);

    bsp_display_unlock();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(example_connect());

    audio_buffer = heap_caps_malloc(AUDIO_BUFFER_SIZE + sizeof(wav_header_t), MALLOC_CAP_SPIRAM);
    assert(audio_buffer);


    xTaskCreate(audio_input_task, "audio_input", 1024 * 6, NULL, 10, NULL);

    xTaskCreate(ai_task, "ai_task", 4096, NULL, 5, NULL);
}

