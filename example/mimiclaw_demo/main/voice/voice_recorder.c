#include "voice_recorder.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "driver/i2s_std.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "mimi_config.h"

#define RECORDER_SAMPLE_RATE 16000
#define RECORDER_BITS_PER_SAMPLE 16
#define RECORDER_CHANNELS 1
#define RECORDER_BCLK_GPIO 42
#define RECORDER_WS_GPIO 2
#define RECORDER_DIN_GPIO 41
#define RECORDER_BUFFER_BYTES 1024
#define RECORDER_PHRASE_START_THRESHOLD 1200
#define RECORDER_PHRASE_STOP_THRESHOLD 780
#define RECORDER_PHRASE_START_CONSECUTIVE_FRAMES 2
#define RECORDER_PHRASE_STOP_SILENCE_FRAMES 16
#define RECORDER_PHRASE_PREROLL_FRAMES 12
#define RECORDER_PHRASE_MIN_HOLD_BYTES 32000
#define RECORDER_PHRASE_MIN_BYTES 24000

static const char *TAG = "voice_recorder";
static char s_reason[128] = "voice recorder ready";
static i2s_chan_handle_t s_i2s_rx = NULL;

static void set_reason(const char *reason)
{
    if (!reason) {
        s_reason[0] = '\0';
        return;
    }
    snprintf(s_reason, sizeof(s_reason), "%s", reason);
}

const char *voice_recorder_get_unavailable_reason(void)
{
    return s_reason;
}

void voice_recorder_release(void)
{
    if (!s_i2s_rx) {
        return;
    }
    i2s_channel_disable(s_i2s_rx);
    i2s_del_channel(s_i2s_rx);
    s_i2s_rx = NULL;
}

static esp_err_t ensure_i2s_rx_init(void)
{
    if (s_i2s_rx) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = 6,
        .dma_frame_num = 240,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };

    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &s_i2s_rx);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = RECORDER_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = {
            .data_bit_width = I2S_DATA_BIT_WIDTH_16BIT,
            .slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT,
            .slot_mode = I2S_SLOT_MODE_MONO,
            .slot_mask = I2S_STD_SLOT_LEFT,
            .ws_width = I2S_DATA_BIT_WIDTH_16BIT,
            .ws_pol = true,
            .bit_shift = true,
#ifdef I2S_HW_VERSION_2
            .left_align = false,
            .big_endian = false,
            .bit_order_lsb = false,
#endif
        },
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = RECORDER_BCLK_GPIO,
            .ws = RECORDER_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din = RECORDER_DIN_GPIO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_i2s_rx, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }

    return i2s_channel_enable(s_i2s_rx);
}

static esp_err_t write_wav_header(FILE *f, uint32_t data_size)
{
    if (!f) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t chunk_size = 36 + data_size;
    uint32_t byte_rate = RECORDER_SAMPLE_RATE * RECORDER_CHANNELS * (RECORDER_BITS_PER_SAMPLE / 8);
    uint16_t block_align = RECORDER_CHANNELS * (RECORDER_BITS_PER_SAMPLE / 8);
    uint16_t audio_format = 1;
    uint32_t sample_rate = RECORDER_SAMPLE_RATE;
    uint16_t channels = RECORDER_CHANNELS;
    uint16_t bits_per_sample = RECORDER_BITS_PER_SAMPLE;
    uint32_t subchunk1_size = 16;

    if (fseek(f, 0, SEEK_SET) != 0) {
        return ESP_FAIL;
    }

    if (fwrite("RIFF", 1, 4, f) != 4 ||
        fwrite(&chunk_size, sizeof(chunk_size), 1, f) != 1 ||
        fwrite("WAVE", 1, 4, f) != 4 ||
        fwrite("fmt ", 1, 4, f) != 4 ||
        fwrite(&subchunk1_size, sizeof(subchunk1_size), 1, f) != 1 ||
        fwrite(&audio_format, sizeof(audio_format), 1, f) != 1 ||
        fwrite(&channels, sizeof(channels), 1, f) != 1 ||
        fwrite(&sample_rate, sizeof(sample_rate), 1, f) != 1 ||
        fwrite(&byte_rate, sizeof(byte_rate), 1, f) != 1 ||
        fwrite(&block_align, sizeof(block_align), 1, f) != 1 ||
        fwrite(&bits_per_sample, sizeof(bits_per_sample), 1, f) != 1 ||
        fwrite("data", 1, 4, f) != 4 ||
        fwrite(&data_size, sizeof(data_size), 1, f) != 1) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static int16_t compute_peak_amplitude(const uint8_t *buf, size_t bytes_read)
{
    int16_t peak = 0;
    const int16_t *samples = (const int16_t *)buf;
    size_t sample_count = bytes_read / sizeof(int16_t);
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t v = samples[i];
        if (v < 0) {
            v = -v;
        }
        if (v > peak) {
            peak = (int16_t)v;
        }
    }
    return peak;
}

static esp_err_t finalize_wav_file(FILE *f, uint32_t total_written)
{
    esp_err_t err = write_wav_header(f, total_written);
    fclose(f);
    if (err != ESP_OK) {
        set_reason("wav header finalize failed");
        return err;
    }
    if (total_written == 0) {
        set_reason("recorded wav is empty");
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t voice_recorder_record_wav(const char *file_path, uint32_t duration_ms)
{
    if (!file_path || duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_i2s_rx_init();
    if (err != ESP_OK) {
        set_reason("i2s rx init failed");
        return err;
    }

    FILE *f = fopen(file_path, "wb+");
    if (!f) {
        set_reason("cannot open record file");
        return ESP_FAIL;
    }

    err = write_wav_header(f, 0);
    if (err != ESP_OK) {
        fclose(f);
        set_reason("wav header init failed");
        return err;
    }

    uint8_t buf[RECORDER_BUFFER_BYTES];
    uint32_t target_bytes = (RECORDER_SAMPLE_RATE * (RECORDER_BITS_PER_SAMPLE / 8) * duration_ms) / 1000;
    uint32_t total_written = 0;

    while (total_written < target_bytes) {
        size_t bytes_to_read = RECORDER_BUFFER_BYTES;
        if (target_bytes - total_written < bytes_to_read) {
            bytes_to_read = target_bytes - total_written;
        }

        size_t bytes_read = 0;
        err = i2s_channel_read(s_i2s_rx, buf, bytes_to_read, &bytes_read, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            fclose(f);
            voice_recorder_release();
            set_reason("i2s rx read failed");
            return err;
        }
        if (bytes_read == 0) {
            continue;
        }
        if (fwrite(buf, 1, bytes_read, f) != bytes_read) {
            fclose(f);
            voice_recorder_release();
            set_reason("record file write failed");
            return ESP_FAIL;
        }
        total_written += (uint32_t)bytes_read;
    }

    err = finalize_wav_file(f, total_written);
    voice_recorder_release();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Recorded wav saved: %s (%lu bytes)", file_path, (unsigned long)total_written);
    set_reason("voice recorder ready");
    return ESP_OK;
}

esp_err_t voice_recorder_record_phrase_wav(const char *file_path, uint32_t max_duration_ms)
{
    if (!file_path || max_duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_i2s_rx_init();
    if (err != ESP_OK) {
        set_reason("i2s rx init failed");
        return err;
    }

    FILE *f = fopen(file_path, "wb+");
    if (!f) {
        set_reason("cannot open record file");
        return ESP_FAIL;
    }

    err = write_wav_header(f, 0);
    if (err != ESP_OK) {
        fclose(f);
        set_reason("wav header init failed");
        return err;
    }

    uint8_t *pre_roll = (uint8_t *)calloc(RECORDER_PHRASE_PREROLL_FRAMES, RECORDER_BUFFER_BYTES);
    size_t *pre_roll_sizes = (size_t *)calloc(RECORDER_PHRASE_PREROLL_FRAMES, sizeof(size_t));
    uint8_t *buf = (uint8_t *)malloc(RECORDER_BUFFER_BYTES);
    if (!pre_roll || !pre_roll_sizes || !buf) {
        free(pre_roll);
        free(pre_roll_sizes);
        free(buf);
        fclose(f);
        set_reason("record buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    size_t pre_roll_index = 0;
    size_t total_frames = (max_duration_ms + 31U) / 32U;
    if (total_frames == 0) {
        total_frames = 1;
    }

    uint32_t total_written = 0;
    int speech_frames = 0;
    int silence_frames = 0;
    bool speech_started = false;
    int16_t max_peak = 0;
    int logged_wait_frames = 0;

    for (size_t frame = 0; frame < total_frames; ++frame) {
        size_t bytes_read = 0;
        err = i2s_channel_read(s_i2s_rx, buf, RECORDER_BUFFER_BYTES, &bytes_read, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            free(pre_roll);
            free(pre_roll_sizes);
            free(buf);
            fclose(f);
            voice_recorder_release();
            set_reason("i2s rx read failed");
            return err;
        }
        if (bytes_read == 0) {
            continue;
        }

        memcpy(pre_roll + (pre_roll_index * RECORDER_BUFFER_BYTES), buf, bytes_read);
        pre_roll_sizes[pre_roll_index] = bytes_read;
        pre_roll_index = (pre_roll_index + 1U) % RECORDER_PHRASE_PREROLL_FRAMES;

        int16_t peak = compute_peak_amplitude(buf, bytes_read);
        if (peak > max_peak) {
            max_peak = peak;
        }
        bool is_speech = speech_started ? (peak >= RECORDER_PHRASE_STOP_THRESHOLD)
                                        : (peak >= RECORDER_PHRASE_START_THRESHOLD);

        if (!speech_started) {
            if ((logged_wait_frames % 32) == 0) {
                ESP_LOGI(TAG, "phrase wait peak=%d start_th=%d max_peak=%d", peak, RECORDER_PHRASE_START_THRESHOLD, max_peak);
            }
            logged_wait_frames++;
            speech_frames = is_speech ? (speech_frames + 1) : 0;
            if (speech_frames < RECORDER_PHRASE_START_CONSECUTIVE_FRAMES) {
                continue;
            }

            speech_started = true;
            silence_frames = 0;
            ESP_LOGI(TAG, "phrase start peak=%d max_peak=%d", peak, max_peak);
            for (size_t i = 0; i < RECORDER_PHRASE_PREROLL_FRAMES; ++i) {
                size_t idx = (pre_roll_index + i) % RECORDER_PHRASE_PREROLL_FRAMES;
                size_t preroll_size = pre_roll_sizes[idx];
                if (preroll_size == 0) {
                    continue;
                }
                const uint8_t *pre_roll_frame = pre_roll + (idx * RECORDER_BUFFER_BYTES);
                if (fwrite(pre_roll_frame, 1, preroll_size, f) != preroll_size) {
                    free(pre_roll);
                    free(pre_roll_sizes);
                    free(buf);
                    fclose(f);
                    voice_recorder_release();
                    set_reason("record file write failed");
                    return ESP_FAIL;
                }
                total_written += (uint32_t)preroll_size;
                pre_roll_sizes[idx] = 0;
            }
            continue;
        }

        if (fwrite(buf, 1, bytes_read, f) != bytes_read) {
            free(pre_roll);
            free(pre_roll_sizes);
            free(buf);
            fclose(f);
            voice_recorder_release();
            set_reason("record file write failed");
            return ESP_FAIL;
        }
        total_written += (uint32_t)bytes_read;

        if (is_speech || total_written < RECORDER_PHRASE_MIN_HOLD_BYTES) {
            silence_frames = 0;
        } else {
            silence_frames++;
            if (silence_frames >= RECORDER_PHRASE_STOP_SILENCE_FRAMES) {
                ESP_LOGI(TAG, "phrase stop peak=%d total_written=%lu max_peak=%d", peak, (unsigned long)total_written, max_peak);
                break;
            }
        }
    }

    if (!speech_started) {
        free(pre_roll);
        free(pre_roll_sizes);
        free(buf);
        fclose(f);
        voice_recorder_release();
        remove(file_path);
        set_reason("no speech detected");
        return ESP_ERR_NOT_FOUND;
    }

    if (total_written < RECORDER_PHRASE_MIN_BYTES) {
        free(pre_roll);
        free(pre_roll_sizes);
        free(buf);
        fclose(f);
        voice_recorder_release();
        remove(file_path);
        set_reason("recorded phrase too short");
        return ESP_ERR_INVALID_SIZE;
    }

    free(pre_roll);
    free(pre_roll_sizes);
    free(buf);

    err = finalize_wav_file(f, total_written);
    voice_recorder_release();
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Recorded phrase wav saved: %s (%lu bytes)", file_path, (unsigned long)total_written);
    set_reason("voice recorder ready");
    return ESP_OK;
}
