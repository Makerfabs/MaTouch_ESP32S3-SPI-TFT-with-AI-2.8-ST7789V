#include "voice_bridge.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "driver/i2s_std.h"
#include "cJSON.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "mimi_config.h"

#define VOICE_NVS_NAMESPACE "voice_bridge"
#define VOICE_NVS_KEY_BASE_URL "base_url"
#define VOICE_BOUNDARY "----MimiVoiceBoundary7MA4YWxkTrZu0gW"
#define VOICE_DEFAULT_BASE_URL "http://192.168.1.100:8000"
#define VOICE_MAX_URL 256
#define VOICE_TEXT_MAX 1024
#define VOICE_HTTP_TIMEOUT_MS (60 * 1000)
#define VOICE_I2S_SAMPLE_RATE 16000
#define VOICE_I2S_PORT I2S_NUM_1
#define VOICE_I2S_BCLK_GPIO 20
#define VOICE_I2S_WS_GPIO 1
#define VOICE_I2S_DOUT_GPIO 19
#define VOICE_I2S_DMA_DESC_NUM 4
#define VOICE_I2S_DMA_FRAME_NUM 256

static const char *TAG = "voice_bridge";

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} voice_buf_t;

static char s_base_url[VOICE_MAX_URL] = VOICE_DEFAULT_BASE_URL;
static char s_unavailable_reason[128] = "voice bridge not configured";
static bool s_inited = false;
static i2s_chan_handle_t s_i2s_tx = NULL;

static void set_reason(const char *reason)
{
    if (!reason) {
        s_unavailable_reason[0] = '\0';
        return;
    }
    snprintf(s_unavailable_reason, sizeof(s_unavailable_reason), "%s", reason);
}

static esp_err_t voice_buf_init(voice_buf_t *rb, size_t cap)
{
    rb->buf = (char *)malloc(cap);
    if (!rb->buf) {
        return ESP_ERR_NO_MEM;
    }
    rb->len = 0;
    rb->cap = cap;
    rb->buf[0] = '\0';
    return ESP_OK;
}

static void voice_buf_free(voice_buf_t *rb)
{
    free(rb->buf);
    rb->buf = NULL;
    rb->len = 0;
    rb->cap = 0;
}

static esp_err_t voice_buf_append(voice_buf_t *rb, const char *data, size_t len)
{
    if (rb->len + len + 1 > rb->cap) {
        size_t new_cap = rb->cap * 2;
        while (new_cap < rb->len + len + 1) {
            new_cap *= 2;
        }
        char *new_buf = (char *)realloc(rb->buf, new_cap);
        if (!new_buf) {
            return ESP_ERR_NO_MEM;
        }
        rb->buf = new_buf;
        rb->cap = new_cap;
    }
    memcpy(rb->buf + rb->len, data, len);
    rb->len += len;
    rb->buf[rb->len] = '\0';
    return ESP_OK;
}

static const char *file_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) {
        slash = strrchr(path, '\\');
    }
    return slash ? slash + 1 : path;
}

static esp_err_t join_url(const char *base, const char *path, char *out, size_t out_size)
{
    if (!base || !path || !out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (path[0] == '/') {
        int n = snprintf(out, out_size, "%s%s", base, path);
        return (n > 0 && (size_t)n < out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
    }
    int n = snprintf(out, out_size, "%s/%s", base, path);
    return (n > 0 && (size_t)n < out_size) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t write_all(esp_http_client_handle_t client, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int n = esp_http_client_write(client, data + sent, len - sent);
        if (n <= 0) {
            return ESP_FAIL;
        }
        sent += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t stream_file(esp_http_client_handle_t client, FILE *f)
{
    char buf[1024];
    while (!feof(f)) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) {
            break;
        }
        esp_err_t err = write_all(client, buf, n);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t load_base_url_from_nvs(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(VOICE_NVS_NAMESPACE, NVS_READONLY, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    size_t required = sizeof(s_base_url);
    err = nvs_get_str(nvs, VOICE_NVS_KEY_BASE_URL, s_base_url, &required);
    nvs_close(nvs);
    return err;
}

esp_err_t voice_bridge_set_base_url(const char *base_url)
{
    if (!base_url || !base_url[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(VOICE_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(nvs, VOICE_NVS_KEY_BASE_URL, base_url);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        snprintf(s_base_url, sizeof(s_base_url), "%s", base_url);
        set_reason("voice bridge ready");
    }
    return err;
}

bool voice_bridge_is_configured(void)
{
    return s_base_url[0] != '\0';
}

const char *voice_bridge_get_unavailable_reason(void)
{
    return s_unavailable_reason;
}

static esp_err_t ensure_i2s_init(void)
{
    if (s_i2s_tx) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = {
        .id = VOICE_I2S_PORT,
        .role = I2S_ROLE_MASTER,
        .dma_desc_num = VOICE_I2S_DMA_DESC_NUM,
        .dma_frame_num = VOICE_I2S_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_i2s_tx, NULL);
    if (err != ESP_OK) {
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = VOICE_I2S_SAMPLE_RATE,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif
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
            .bclk = VOICE_I2S_BCLK_GPIO,
            .ws = VOICE_I2S_WS_GPIO,
            .dout = VOICE_I2S_DOUT_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_i2s_tx, &std_cfg);
    if (err != ESP_OK) {
        return err;
    }
    return i2s_channel_enable(s_i2s_tx);
}

esp_err_t voice_bridge_play_wav_file(const char *file_path)
{
    if (!file_path) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ensure_i2s_init();
    if (err != ESP_OK) {
        set_reason("i2s init failed");
        return err;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        set_reason("cannot open wav file");
        return ESP_FAIL;
    }

    uint8_t header[44];
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        set_reason("invalid wav header");
        return ESP_FAIL;
    }

    uint8_t buf[2048];
    while (!feof(f)) {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0) {
            break;
        }
        size_t written = 0;
        err = i2s_channel_write(s_i2s_tx, buf, n, &written, portMAX_DELAY);
        if (err != ESP_OK) {
            fclose(f);
            set_reason("i2s write failed");
            return err;
        }
    }
    fclose(f);
    return ESP_OK;
}

static esp_err_t download_file(const char *url, const char *local_path)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = VOICE_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        return ESP_FAIL;
    }
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    int status = esp_http_client_fetch_headers(client);
    if (status < 0) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    int http_status = esp_http_client_get_status_code(client);
    if (http_status != 200) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    FILE *f = fopen(local_path, "wb");
    if (!f) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t total_written = 0;
    char buf[1024];
    while (1) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        if (fwrite(buf, 1, n, f) != (size_t)n) {
            err = ESP_FAIL;
            break;
        }
        total_written += (size_t)n;
    }

    fclose(f);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        return err;
    }
    return (total_written > 44) ? ESP_OK : ESP_FAIL;
}

esp_err_t voice_bridge_send_file(const char *file_path, char *out_text, size_t out_text_size, char *out_audio_path, size_t out_audio_path_size)
{
    if (!file_path || !out_text || !out_audio_path) {
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    if (stat(file_path, &st) != 0 || st.st_size <= 0) {
        set_reason("voice source file missing or empty");
        return ESP_FAIL;
    }

    char endpoint[VOICE_MAX_URL];
    if (join_url(s_base_url, "/voice-chat", endpoint, sizeof(endpoint)) != ESP_OK) {
        set_reason("voice endpoint too long");
        return ESP_ERR_INVALID_SIZE;
    }

    const char *name = file_basename(file_path);
    const char *ext = strrchr(name, '.');
    const char *format = (ext && strcmp(ext, ".wav") == 0) ? "wav" : "pcm_s16le";

    char prefix[512];
    int prefix_len = snprintf(
        prefix, sizeof(prefix),
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"audio\"; filename=\"%s\"\r\n"
        "Content-Type: application/octet-stream\r\n\r\n",
        VOICE_BOUNDARY, name);
    char middle[512];
    int middle_len = snprintf(
        middle, sizeof(middle),
        "\r\n--%s\r\n"
        "Content-Disposition: form-data; name=\"device_id\"\r\n\r\nesp32s3-board\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"sample_rate\"\r\n\r\n16000\r\n"
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"format\"\r\n\r\n%s\r\n"
        "--%s--\r\n",
        VOICE_BOUNDARY, VOICE_BOUNDARY, VOICE_BOUNDARY, format, VOICE_BOUNDARY);
    if (prefix_len <= 0 || middle_len <= 0) {
        set_reason("multipart build failed");
        return ESP_FAIL;
    }

    size_t total_len = (size_t)prefix_len + (size_t)st.st_size + (size_t)middle_len;
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", VOICE_BOUNDARY);

    esp_http_client_config_t config = {
        .url = endpoint,
        .timeout_ms = VOICE_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        set_reason("http client init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Accept", "application/json");

    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        set_reason("voice endpoint open failed");
        return err;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        set_reason("cannot open source audio file");
        return ESP_FAIL;
    }

    err = write_all(client, prefix, (size_t)prefix_len);
    if (err == ESP_OK) {
        err = stream_file(client, f);
    }
    if (err == ESP_OK) {
        err = write_all(client, middle, (size_t)middle_len);
    }
    fclose(f);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        set_reason("voice upload failed");
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    voice_buf_t rb = {0};
    err = voice_buf_init(&rb, 2048);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    char buf[1024];
    while (1) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        err = voice_buf_append(&rb, buf, (size_t)n);
        if (err != ESP_OK) {
            break;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        voice_buf_free(&rb);
        set_reason("voice response read failed");
        return err;
    }

    if (status != 200) {
        set_reason(rb.buf);
        voice_buf_free(&rb);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(rb.buf);
    if (!root) {
        voice_buf_free(&rb);
        set_reason("voice response invalid json");
        return ESP_FAIL;
    }

    cJSON *text = cJSON_GetObjectItem(root, "text");
    cJSON *audio_url = cJSON_GetObjectItem(root, "audio_url");
    if (cJSON_IsString(text) && text->valuestring) {
        snprintf(out_text, out_text_size, "%s", text->valuestring);
    } else {
        out_text[0] = '\0';
    }

    if (!cJSON_IsString(audio_url) || !audio_url->valuestring || !audio_url->valuestring[0]) {
        cJSON_Delete(root);
        voice_buf_free(&rb);
        set_reason("voice response missing audio_url");
        return ESP_FAIL;
    }

    char absolute_audio_url[VOICE_MAX_URL];
    if (join_url(s_base_url, audio_url->valuestring, absolute_audio_url, sizeof(absolute_audio_url)) != ESP_OK) {
        cJSON_Delete(root);
        voice_buf_free(&rb);
        set_reason("audio url too long");
        return ESP_ERR_INVALID_SIZE;
    }

    snprintf(out_audio_path, out_audio_path_size, MIMI_SPIFFS_BASE "/voice_reply.wav");
    err = download_file(absolute_audio_url, out_audio_path);
    cJSON_Delete(root);
    voice_buf_free(&rb);
    if (err != ESP_OK) {
        set_reason("reply wav download invalid or empty");
        return err;
    }

    struct stat reply_st;
    if (stat(out_audio_path, &reply_st) != 0 || reply_st.st_size <= 44) {
        set_reason("reply wav too small");
        return ESP_FAIL;
    }

    set_reason("voice bridge ready");
    return ESP_OK;
}

esp_err_t voice_bridge_send_and_play_file(const char *file_path, char *out_text, size_t out_text_size, char *out_audio_path, size_t out_audio_path_size)
{
    if (!file_path || !out_text || !out_audio_path) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = voice_bridge_send_file(file_path, out_text, out_text_size, out_audio_path, out_audio_path_size);
    if (err != ESP_OK) {
        return err;
    }

    err = voice_bridge_play_wav_file(out_audio_path);
    if (err != ESP_OK) {
        return err;
    }

    set_reason("voice bridge ready");
    return ESP_OK;
}

esp_err_t voice_bridge_speak_text(const char *text)
{
    if (!text || !text[0]) {
        set_reason("speak text is empty");
        return ESP_ERR_INVALID_ARG;
    }

    char endpoint[VOICE_MAX_URL];
    if (join_url(s_base_url, "/tts", endpoint, sizeof(endpoint)) != ESP_OK) {
        set_reason("tts endpoint too long");
        return ESP_ERR_INVALID_SIZE;
    }

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        set_reason("tts request build failed");
        return ESP_ERR_NO_MEM;
    }
    cJSON_AddStringToObject(req, "text", text);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        set_reason("tts request build failed");
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = endpoint,
        .timeout_ms = VOICE_HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        cJSON_free(body);
        set_reason("http client init failed");
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "application/json");

    size_t body_len = strlen(body);
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        cJSON_free(body);
        set_reason("tts endpoint open failed");
        return err;
    }

    err = write_all(client, body, body_len);
    cJSON_free(body);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        set_reason("tts request write failed");
        return err;
    }

    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    voice_buf_t rb = {0};
    err = voice_buf_init(&rb, 1024);
    if (err != ESP_OK) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return err;
    }

    char buf[512];
    while (1) {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0) {
            err = ESP_FAIL;
            break;
        }
        if (n == 0) {
            err = ESP_OK;
            break;
        }
        err = voice_buf_append(&rb, buf, (size_t)n);
        if (err != ESP_OK) {
            break;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        voice_buf_free(&rb);
        set_reason("tts response read failed");
        return err;
    }
    if (status != 200) {
        set_reason(rb.buf);
        voice_buf_free(&rb);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(rb.buf);
    if (!root) {
        voice_buf_free(&rb);
        set_reason("tts response invalid json");
        return ESP_FAIL;
    }

    cJSON *audio_url = cJSON_GetObjectItem(root, "audio_url");
    if (!cJSON_IsString(audio_url) || !audio_url->valuestring || !audio_url->valuestring[0]) {
        cJSON_Delete(root);
        voice_buf_free(&rb);
        set_reason("tts response missing audio_url");
        return ESP_FAIL;
    }

    char absolute_audio_url[VOICE_MAX_URL];
    if (join_url(s_base_url, audio_url->valuestring, absolute_audio_url, sizeof(absolute_audio_url)) != ESP_OK) {
        cJSON_Delete(root);
        voice_buf_free(&rb);
        set_reason("tts audio url too long");
        return ESP_ERR_INVALID_SIZE;
    }

    char audio_path[128];
    snprintf(audio_path, sizeof(audio_path), MIMI_SPIFFS_BASE "/voice_reply.wav");
    err = download_file(absolute_audio_url, audio_path);
    cJSON_Delete(root);
    voice_buf_free(&rb);
    if (err != ESP_OK) {
        set_reason("tts wav download invalid or empty");
        return err;
    }

    err = voice_bridge_play_wav_file(audio_path);
    if (err != ESP_OK) {
        return err;
    }

    set_reason("voice bridge ready");
    return ESP_OK;
}

esp_err_t voice_bridge_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }

    esp_err_t err = load_base_url_from_nvs();
    if (err != ESP_OK) {
        snprintf(s_base_url, sizeof(s_base_url), "%s", VOICE_DEFAULT_BASE_URL);
    }
    set_reason("voice bridge ready");
    s_inited = true;
    return ESP_OK;
}
