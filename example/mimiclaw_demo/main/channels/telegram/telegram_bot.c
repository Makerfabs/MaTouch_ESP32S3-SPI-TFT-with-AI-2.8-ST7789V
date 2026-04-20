#include "telegram_bot.h"
#include "mimi_config.h"
#include "bus/message_bus.h"
#include "proxy/http_proxy.h"
#include "asr/asr_client.h"
#include "lvgl_lcd/lvgl_lcd.h"

#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/stat.h>
#include <inttypes.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "telegram";

static char s_bot_token[128] = MIMI_SECRET_TG_TOKEN;
static int64_t s_update_offset = 0;
static int64_t s_last_saved_offset = -1;
static int64_t s_last_offset_save_us = 0;

#define TG_OFFSET_NVS_KEY "update_offset"
#define TG_DEDUP_CACHE_SIZE 64
#define TG_OFFSET_SAVE_INTERVAL_US (5LL * 1000 * 1000)
#define TG_OFFSET_SAVE_STEP 10

static uint64_t s_seen_msg_keys[TG_DEDUP_CACHE_SIZE] = {0};
static size_t s_seen_msg_idx = 0;

/* HTTP response accumulator */
typedef struct
{
    char *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static uint64_t fnv1a64(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    if (!s)
    {
        return h;
    }
    while (*s)
    {
        h ^= (unsigned char)(*s++);
        h *= 1099511628211ULL;
    }
    return h;
}

static uint64_t make_msg_key(const char *chat_id, int msg_id)
{
    uint64_t h = fnv1a64(chat_id);
    return (h << 16) ^ (uint64_t)(msg_id & 0xFFFF) ^ ((uint64_t)msg_id << 32);
}

static bool seen_msg_contains(uint64_t key)
{
    for (size_t i = 0; i < TG_DEDUP_CACHE_SIZE; i++)
    {
        if (s_seen_msg_keys[i] == key)
        {
            return true;
        }
    }
    return false;
}

static void seen_msg_insert(uint64_t key)
{
    s_seen_msg_keys[s_seen_msg_idx] = key;
    s_seen_msg_idx = (s_seen_msg_idx + 1) % TG_DEDUP_CACHE_SIZE;
}

static void save_update_offset_if_needed(bool force)
{
    if (s_update_offset <= 0)
    {
        return;
    }

    int64_t now = esp_timer_get_time();
    bool should_save = force;
    if (!should_save && s_last_saved_offset >= 0)
    {
        if ((s_update_offset - s_last_saved_offset) >= TG_OFFSET_SAVE_STEP)
        {
            should_save = true;
        }
        else if ((now - s_last_offset_save_us) >= TG_OFFSET_SAVE_INTERVAL_US)
        {
            should_save = true;
        }
    }
    else if (!should_save)
    {
        should_save = true;
    }

    if (!should_save)
    {
        return;
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs) != ESP_OK)
    {
        return;
    }

    if (nvs_set_i64(nvs, TG_OFFSET_NVS_KEY, s_update_offset) == ESP_OK)
    {
        if (nvs_commit(nvs) == ESP_OK)
        {
            s_last_saved_offset = s_update_offset;
            s_last_offset_save_us = now;
        }
    }
    nvs_close(nvs);
}

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *resp = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA)
    {
        if (resp->len + evt->data_len >= resp->cap)
        {
            size_t new_cap = resp->cap * 2;
            if (new_cap < resp->len + evt->data_len + 1)
            {
                new_cap = resp->len + evt->data_len + 1;
            }
            char *tmp = realloc(resp->buf, new_cap);
            if (!tmp)
                return ESP_ERR_NO_MEM;
            resp->buf = tmp;
            resp->cap = new_cap;
        }
        memcpy(resp->buf + resp->len, evt->data, evt->data_len);
        resp->len += evt->data_len;
        resp->buf[resp->len] = '\0';
    }
    return ESP_OK;
}

/* ── Proxy path: manual HTTP over CONNECT tunnel ────────────── */
// 代理连接
static char *tg_api_call_via_proxy(const char *path, const char *post_data)
{
    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443,
                                         (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000);
    if (!conn)
        return NULL;

    /* Build HTTP request */
    char header[512];
    int hlen;
    if (post_data)
    {
        hlen = snprintf(header, sizeof(header),
                        "POST /bot%s/%s HTTP/1.1\r\n"
                        "Host: api.telegram.org\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n\r\n",
                        s_bot_token, path, (int)strlen(post_data));
    }
    else
    {
        hlen = snprintf(header, sizeof(header),
                        "GET /bot%s/%s HTTP/1.1\r\n"
                        "Host: api.telegram.org\r\n"
                        "Connection: close\r\n\r\n",
                        s_bot_token, path);
    }

    if (proxy_conn_write(conn, header, hlen) < 0)
    {
        proxy_conn_close(conn);
        return NULL;
    }
    if (post_data && proxy_conn_write(conn, post_data, strlen(post_data)) < 0)
    {
        proxy_conn_close(conn);
        return NULL;
    }

    /* Read response — accumulate until connection close */
    size_t cap = 4096, len = 0;
    char *buf = calloc(1, cap);
    if (!buf)
    {
        proxy_conn_close(conn);
        return NULL;
    }

    int timeout = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000;
    while (1)
    {
        if (len + 1024 >= cap)
        {
            cap *= 2;
            char *tmp = realloc(buf, cap);
            if (!tmp)
                break;
            buf = tmp;
        }
        int n = proxy_conn_read(conn, buf + len, cap - len - 1, timeout);
        if (n <= 0)
            break;
        len += n;
    }
    buf[len] = '\0';
    proxy_conn_close(conn);

    /* Skip HTTP headers — find \r\n\r\n */
    char *body = strstr(buf, "\r\n\r\n");
    if (!body)
    {
        free(buf);
        return NULL;
    }
    body += 4;

    /* Return just the body */
    char *result = strdup(body);
    free(buf);
    return result;
}

/* ── Direct path: esp_http_client ───────────────────────────── */
// 直接连接
static char *tg_api_call_direct(const char *method, const char *post_data)
{
    char url[256];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", s_bot_token, method);

    http_resp_t resp = {
        .buf = calloc(1, 4096),
        .len = 0,
        .cap = 4096,
    };
    if (!resp.buf)
        return NULL;

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .timeout_ms = (MIMI_TG_POLL_TIMEOUT_S + 5) * 1000,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        free(resp.buf);
        return NULL;
    }

    if (post_data)
    {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_data, strlen(post_data));
    }

    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        free(resp.buf);
        return NULL;
    }

    return resp.buf;
}

// 判断走代理还是直连
static char *tg_api_call(const char *method, const char *post_data)
{
    if (http_proxy_is_enabled())
    {
        return tg_api_call_via_proxy(method, post_data);
    }
    return tg_api_call_direct(method, post_data);
}

// 判断是否响应ok
static bool tg_response_is_ok(const char *resp, const char **out_desc)
{
    if (out_desc)
    {
        *out_desc = NULL;
    }
    if (!resp)
    {
        return false;
    }

    cJSON *root = cJSON_Parse(resp);
    if (root)
    {
        cJSON *ok_field = cJSON_GetObjectItem(root, "ok");
        bool ok = cJSON_IsTrue(ok_field);
        if (!ok && out_desc)
        {
            cJSON *desc = cJSON_GetObjectItem(root, "description");
            if (desc && cJSON_IsString(desc))
            {
                *out_desc = desc->valuestring;
            }
        }
        cJSON_Delete(root);
        return ok;
    }

    /* Proxy or gateway can occasionally return non-standard payload framing. */
    if (strstr(resp, "\"ok\":true") != NULL)
    {
        return true;
    }

    return false;
}

// static bool is_peach_command(const char *text)
// {
//     if (!text) {
//         return false;
//     }

//     return strcmp(text, "桃") == 0 ||
//            strcmp(text, "/tao") == 0 ||
//            strcmp(text, "/peach") == 0 ||
//            strcmp(text, "显示桃") == 0;
// }

// 解析音频数据
static bool tg_extract_voice_info(cJSON *message, char *file_id, size_t file_id_size,
                                  int *duration_s, int *file_size)
{
    cJSON *voice = cJSON_GetObjectItem(message, "voice");
    if (!voice)
        return false;

    cJSON *voice_file_id = cJSON_GetObjectItem(voice, "file_id");
    if (!cJSON_IsString(voice_file_id) || !voice_file_id->valuestring || !voice_file_id->valuestring[0])
    {
        return false;
    }

    strncpy(file_id, voice_file_id->valuestring, file_id_size - 1);
    file_id[file_id_size - 1] = '\0';

    if (duration_s)
    {
        cJSON *duration = cJSON_GetObjectItem(voice, "duration");
        *duration_s = cJSON_IsNumber(duration) ? (int)duration->valuedouble : 0;
    }
    if (file_size)
    {
        cJSON *size = cJSON_GetObjectItem(voice, "file_size");
        *file_size = cJSON_IsNumber(size) ? (int)size->valuedouble : 0;
    }
    return true;
}

// 获取纸飞机文件路径
static esp_err_t tg_get_file_path(const char *file_id, char *out_path, size_t out_size)
{
    char method[256];
    snprintf(method, sizeof(method), "getFile?file_id=%s", file_id);
    char *resp = tg_api_call(method, NULL);
    if (!resp)
    {
        return ESP_FAIL;
    }

    esp_err_t err = ESP_FAIL;
    cJSON *root = cJSON_Parse(resp);
    if (root)
    {
        cJSON *ok = cJSON_GetObjectItem(root, "ok");
        cJSON *result = cJSON_GetObjectItem(root, "result");
        cJSON *file_path = result ? cJSON_GetObjectItem(result, "file_path") : NULL;
        if (cJSON_IsTrue(ok) && cJSON_IsString(file_path) && file_path->valuestring)
        {
            strncpy(out_path, file_path->valuestring, out_size - 1);
            out_path[out_size - 1] = '\0';
            err = ESP_OK;
        }
        cJSON_Delete(root);
    }
    free(resp);
    return err;
}

// 把 chunked 格式的数据，转换成普通连续字符串
//  4\r\n
//  Wiki\r\n
//  5\r\n
//  pedia\r\n
//  0\r\n
//  \r\n
//  解码成这样Wikipedia
// 分几次进行解码的
static void tg_decode_chunked_body(char *buf, size_t *len)
{
    // 判空 & 特殊情况
    if (!buf || !len || *len == 0)
    {
        return;
    }

    size_t i = 0;
    // 判断是不是 JSON（不是 chunked）
    while (i < *len && (buf[i] == ' ' || buf[i] == '\t'))
    {
        i++;
    }
    if (i < *len && (buf[i] == '{' || buf[i] == '['))
    {
        return;
    }

    // 初始化指针
    char *src = buf;
    char *dst = buf;
    char *end = buf + *len;
    // 逐个 chunk 解析
    while (src < end)
    {
        // 找 chunk header 行  在 src 开始的字符串里，找到第一个 \r\n 出现的位置
        char *line_end = strstr(src, "\r\n");
        if (!line_end)
        {
            break;
        }
        // 解析 chunk 大小
        unsigned long chunk_size = strtoul(src, NULL, 16);
        // 把"4"转成数字4
        // 判断结束
        if (chunk_size == 0)
        {
            break;
        }
        // 跳到真正的数据
        src = line_end + 2;
        // src当前读的位置   chunk_size想读的数据量   end buffer结尾（最后一个字节的后面）  dst  → 当前写位置（解码后的纯数据）
        // 我想读 chunk_size 个字节，但 buffer 里不够
        if (src + chunk_size > end)
        {
            // 只拷贝已有数据（有效数据）
            size_t avail = end - src;
            memmove(dst, src, avail);
            dst += avail;
            break;
        }
        // 正常拷贝 chunk 数据
        memmove(dst, src, chunk_size);
        // 移动指针
        dst += chunk_size;
        src += chunk_size;
        // 跳过 chunk 结尾 \r\n
        if (src + 2 <= end && src[0] == '\r' && src[1] == '\n')
        {
            src += 2;
        }
    }
    // 最后处理  更新长度  加字符串结束符
    *len = (size_t)(dst - buf);
    buf[*len] = '\0';
}

static esp_err_t tg_download_file_direct(const char *remote_path, const char *local_path)
{
    // 拼接下载 URL
    char url[384];
    // 地址是官方的
    snprintf(url, sizeof(url), "https://api.telegram.org/file/bot%s/%s", s_bot_token, remote_path);

    // 配置 HTTP 客户端
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = MIMI_TG_VOICE_DOWNLOAD_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    // 创建客户端
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        return ESP_FAIL;
    }

    // 打开连接
    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        return err;
    }

    // 获取响应头
    int status = esp_http_client_fetch_headers(client);
    if (status < 0 || esp_http_client_get_status_code(client) != 200)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // 打开本地文件
    //  w = write（写）
    //  b = binary（二进制）
    FILE *f = fopen(local_path, "wb");
    // 打开失败 → 退出
    if (!f)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    // 开始下载数据 每次读 1KB  记录总大小
    char buf[1024];
    int total = 0;
    // 循环读取
    while (1)
    {
        // 从网络读取数据
        int n = esp_http_client_read(client, buf, sizeof(buf));
        // 读取错误
        if (n < 0)
        {
            err = ESP_FAIL;
            break;
        }
        // 读完了
        if (n == 0)
        {
            err = ESP_OK;
            break;
        }
        // 累加大小
        total += n;
        // 限制文件大小 1MB
        if (total > MIMI_TG_VOICE_MAX_FILE_SIZE)
        {
            err = ESP_ERR_NO_MEM;
            break;
        }
        // 写入文件
        if (fwrite(buf, 1, n, f) != (size_t)n)
        {
            // 写失败就退出
            err = ESP_FAIL;
            break;
        }
    }

    // 结束处理
    // 关闭文件
    fclose(f);
    // 关闭连接 + 释放资源
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err;
}

static esp_err_t tg_download_file_via_proxy(const char *remote_path, const char *local_path)
{
    proxy_conn_t *conn = proxy_conn_open("api.telegram.org", 443, MIMI_TG_VOICE_DOWNLOAD_TIMEOUT_MS);
    if (!conn)
    {
        return ESP_FAIL;
    }

    char header[512];
    int hlen = snprintf(header, sizeof(header),
                        "GET /file/bot%s/%s HTTP/1.1\r\n"
                        "Host: api.telegram.org\r\n"
                        "Connection: close\r\n\r\n",
                        s_bot_token, remote_path);
    if (hlen <= 0 || (size_t)hlen >= sizeof(header))
    {
        proxy_conn_close(conn);
        return ESP_ERR_INVALID_SIZE;
    }

    if (proxy_conn_write(conn, header, hlen) < 0)
    {
        proxy_conn_close(conn);
        return ESP_FAIL;
    }

    size_t cap = 4096;
    size_t len = 0;
    char *raw = calloc(1, cap);
    if (!raw)
    {
        proxy_conn_close(conn);
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    char buf[1024];
    while (1)
    {
        int n = proxy_conn_read(conn, buf, sizeof(buf), MIMI_TG_VOICE_DOWNLOAD_TIMEOUT_MS);
        if (n < 0)
        {
            err = ESP_FAIL;
            break;
        }
        if (n == 0)
        {
            break;
        }
        if (len + (size_t)n + 1 > cap)
        {
            size_t new_cap = cap * 2;
            while (new_cap < len + (size_t)n + 1)
            {
                new_cap *= 2;
            }
            char *tmp = realloc(raw, new_cap);
            if (!tmp)
            {
                err = ESP_ERR_NO_MEM;
                break;
            }
            raw = tmp;
            cap = new_cap;
        }
        memcpy(raw + len, buf, (size_t)n);
        len += (size_t)n;
        raw[len] = '\0';
        if (len > (size_t)(MIMI_TG_VOICE_MAX_FILE_SIZE + 4096))
        {
            err = ESP_ERR_NO_MEM;
            break;
        }
    }
    proxy_conn_close(conn);
    if (err != ESP_OK)
    {
        free(raw);
        return err;
    }

    int status = 0;
    if (sscanf(raw, "HTTP/%*s %d", &status) != 1 || status != 200)
    {
        free(raw);
        return ESP_FAIL;
    }

    bool is_chunked = strstr(raw, "Transfer-Encoding: chunked") != NULL;
    char *body = strstr(raw, "\r\n\r\n");
    if (!body)
    {
        free(raw);
        return ESP_FAIL;
    }
    body += 4;
    size_t body_len = len - (size_t)(body - raw);
    memmove(raw, body, body_len);
    raw[body_len] = '\0';
    if (is_chunked)
    {
        tg_decode_chunked_body(raw, &body_len);
    }
    if (body_len > MIMI_TG_VOICE_MAX_FILE_SIZE)
    {
        free(raw);
        return ESP_ERR_NO_MEM;
    }

    FILE *f = fopen(local_path, "wb");
    if (!f)
    {
        free(raw);
        return ESP_FAIL;
    }
    if (body_len > 0 && fwrite(raw, 1, body_len, f) != body_len)
    {
        fclose(f);
        free(raw);
        return ESP_FAIL;
    }
    fclose(f);
    free(raw);
    return ESP_OK;
}

// 下载到flash，选择方法下载
static esp_err_t tg_download_file_to_spiffs(const char *remote_path, const char *local_path)
{
    esp_err_t err = http_proxy_is_enabled()
                        ? tg_download_file_via_proxy(remote_path, local_path)
                        : tg_download_file_direct(remote_path, local_path);
    // 只要不是成功 → 都算失败
    if (err != ESP_OK)
    {
        // 移除文件
        remove(local_path);
    }
    return err;
}

// 删除本地的临时语音文件
static void tg_cleanup_temp_voice(const char *local_path)
{
    if (local_path && local_path[0])
    {
        remove(local_path);
    }
}

// 处理音频信息 Telegram JSON消息  聊天ID    update_id（唯一标识）   消息ID
static esp_err_t tg_handle_voice_message(cJSON *message, const char *chat_id, int64_t uid, int msg_id_val)
{
    // file_id（下载用）
    char file_id[160] = {0};
    // 语音时长
    int duration_s = 0;
    // 文件大小
    int file_size = 0;
    // 解析语音信息   file_id / duration / file_size
    if (!tg_extract_voice_info(message, file_id, sizeof(file_id), &duration_s, &file_size))
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 音频数据过大
    if (file_size > MIMI_TG_VOICE_MAX_FILE_SIZE)
    {
        return telegram_send_message(chat_id, "语音文件过大，当前仅支持较短语音。");
    }

    // 获取 file_path（关键步骤🔥）
    char remote_path[256] = {0};
    // 获取路径存到remote_path里面  voice/file_xxx.ogg
    if (tg_get_file_path(file_id, remote_path, sizeof(remote_path)) != ESP_OK)
    {
        ESP_LOGW(TAG, "getFile failed for voice update_id=%" PRId64 " message_id=%d", uid, msg_id_val);
        return telegram_send_message(chat_id, "已收到语音，但获取文件路径失败。");
    }

    // 生成本地路径  /spiffs/tg_voice_12345_67.ogg
    char local_path[192] = {0};
    snprintf(local_path, sizeof(local_path), MIMI_TG_VOICE_TMP_DIR "/tg_voice_%" PRId64 "_%d.ogg", uid, msg_id_val);

    // Telegram → HTTP → 写入Flash 从远程路径下载到本地路径
    esp_err_t err = tg_download_file_to_spiffs(remote_path, local_path);
    // 下载失败
    if (err != ESP_OK)
    {
        ESP_LOGW(TAG, "Voice download failed for %s: %s", remote_path, esp_err_to_name(err));
        // 删除临时文件
        tg_cleanup_temp_voice(local_path);
        return telegram_send_message(chat_id, err == ESP_ERR_NO_MEM ? "语音文件过大，下载已取消。" : "已收到语音，但下载失败。");
    }

    // 调用 ASR（语音转文字） 把 .ogg → 转成文本
    char transcript[MIMI_ASR_TEXT_MAX_LEN] = {0};
    // 转写文件 将语音转成文本
    err = asr_client_transcribe_file(local_path, transcript, sizeof(transcript));
    // 删除临时文件（非常关键）成功也删不成功也删，
    tg_cleanup_temp_voice(local_path);
    // 转写不成功
    if (err != ESP_OK)
    {
        const char *reason = asr_client_get_unavailable_reason();
        char reply[256];
        snprintf(reply, sizeof(reply), "语音已收到（%d 秒），转写失败：%s", duration_s, (reason && reason[0]) ? reason : "未知错误");
        return telegram_send_message(chat_id, reply);
    }

    // 转写成功但是没有内容
    if (transcript[0] == '\0')
    {
        return telegram_send_message(chat_id, "语音已收到，但暂未识别出文本内容。");
    }

    // 转写成功，开始处理
    // 初始化消息结构体
    mimi_msg_t msg = {0};
    // 设置来源 标记来自 Telegram
    strncpy(msg.channel, MIMI_CHAN_TELEGRAM, sizeof(msg.channel) - 1);
    // 设置聊天ID
    strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
    // 设置内容
    msg.content = strdup(transcript);
    // 内存失败
    if (!msg.content)
    {
        return ESP_ERR_NO_MEM;
    }
    // 推入消息总线
    // LLM / AI处理模块
    if (message_bus_push_inbound(&msg) != ESP_OK)
    {
        free(msg.content);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 你拿到了一份信封（字符串 JSON），
// 先看看信封打开没坏（解析 JSON），
// 再看信封里写的‘ok’是不是成功（ok 字段），
// 然后把里面的每封信（result 数组）取出来准备读。
static void process_updates(const char *json_str)
{
    // 把字符串解析成 JSON 树
    // 是你从 Telegram 拉取到的更新数据，是一个 字符串
    cJSON *root = cJSON_Parse(json_str);
    if (!root)
        return;

    // 检查 ok 字段
    cJSON *ok = cJSON_GetObjectItem(root, "ok");
    // 如果不是 true：
    // 释放 JSON
    // 退出
    if (!cJSON_IsTrue(ok))
    {
        cJSON_Delete(root);
        return;
    }

    // 获取 result 数组   Telegram 返回的 result 是一个 数组，每个元素是一条更新（update）
    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (!cJSON_IsArray(result))
    {
        cJSON_Delete(root);
        return;
    }

    // update_id 是单调递增的整数
    // 每次拉取消息，如果没有明确告诉 Telegram “我收到了哪条消息”，下一次拉取可能会把旧消息再返回一次
    // 所以我们需要“记住上次处理到的 update_id”，避免重复处理
    // 遍历每一条 update
    cJSON *update;
    cJSON_ArrayForEach(update, result)
    {
        /* Track offset and skip stale/duplicate updates */
        // 处理 update_id 初始化 update_id
        cJSON *update_id = cJSON_GetObjectItem(update, "update_id");
        int64_t uid = -1;
        // 读取 update_id（Telegram用double存）
        if (cJSON_IsNumber(update_id))
        {
            uid = (int64_t)update_id->valuedouble;
        }
        if (uid >= 0)
        {
            // 如果是旧消息 → 跳过
            if (uid < s_update_offset)
            {
                continue;
            }
            // 更新 offset（避免重复处理）
            s_update_offset = uid + 1;
            save_update_offset_if_needed(false);
        }

        /* Extract message */
        // 提取 message
        cJSON *message = cJSON_GetObjectItem(update, "message");
        if (!message)
            continue;

        // cJSON *text = cJSON_GetObjectItem(message, "text");
        // if (!text || !cJSON_IsString(text)) continue;

        // 获取 chat
        cJSON *chat = cJSON_GetObjectItem(message, "chat");
        if (!chat)
            continue;

        // 获取聊天ID
        cJSON *chat_id = cJSON_GetObjectItem(chat, "id");
        if (!chat_id)
            continue;

        // 获取 message_id
        int msg_id_val = -1;
        cJSON *message_id = cJSON_GetObjectItem(message, "message_id");
        // 提取 message_id
        if (cJSON_IsNumber(message_id))
        {
            msg_id_val = (int)message_id->valuedouble;
        }

        // 把 chat_id 转成字符串
        char chat_id_str[32];
        // chat_id 是字符串
        if (cJSON_IsString(chat_id) && chat_id->valuestring)
        {
            strncpy(chat_id_str, chat_id->valuestring, sizeof(chat_id_str) - 1);
            chat_id_str[sizeof(chat_id_str) - 1] = '\0';
        }
        // chat_id 是数字
        else if (cJSON_IsNumber(chat_id))
        {
            snprintf(chat_id_str, sizeof(chat_id_str), "%.0f", chat_id->valuedouble);
        }
        // 否则跳过
        else
        {
            continue;
        }

        // 消息级去重
        if (msg_id_val >= 0)
        {
            // 生成唯一key（chat_id + message_id）
            uint64_t msg_key = make_msg_key(chat_id_str, msg_id_val);
            // 如果已经处理过 → 跳过
            if (seen_msg_contains(msg_key))
            {
                ESP_LOGW(TAG, "Drop duplicate message update_id=%" PRId64 " chat=%s message_id=%d",
                         uid, chat_id_str, msg_id_val);
                continue;
            }
            // 记录已处理
            seen_msg_insert(msg_key);
        }

        //处理文本消息
        cJSON *text = cJSON_GetObjectItem(message, "text");
        if (text && cJSON_IsString(text) && text->valuestring)
        {
            ESP_LOGI(TAG, "Text update_id=%" PRId64 " message_id=%d from chat %s: %.40s...",
                     uid, msg_id_val, chat_id_str, text->valuestring);
            //构造消息
            mimi_msg_t msg = {0};
            //标记来源
            strncpy(msg.channel, MIMI_CHAN_TELEGRAM, sizeof(msg.channel) - 1);
            //设置聊天ID
            strncpy(msg.chat_id, chat_id_str, sizeof(msg.chat_id) - 1);
            //深拷贝文本内容
            msg.content = strdup(text->valuestring);
            //推入消息队列
            if (msg.content)
            {
                if (message_bus_push_inbound(&msg) != ESP_OK)
                {
                    //队列满
                    ESP_LOGW(TAG, "Inbound queue full, drop telegram text message");
                    free(msg.content);
                }
            }
            continue;
        }

        //处理语音消息
        if (cJSON_GetObjectItem(message, "voice"))
        {
            ESP_LOGI(TAG, "Voice update_id=%" PRId64 " message_id=%d from chat %s",
                     uid, msg_id_val, chat_id_str);
            //调用语音处理函数  下载 → ASR → 推送
            esp_err_t voice_err = tg_handle_voice_message(message, chat_id_str, uid, msg_id_val);
            //错误处理
            if (voice_err != ESP_OK)
            {
                ESP_LOGW(TAG, "Voice handling failed: %s", esp_err_to_name(voice_err));
            }
        }
        // ESP_LOGI(TAG, "Message update_id=%" PRId64 " message_id=%d from chat %s: %.40s...",
        //          uid, msg_id_val, chat_id_str, text->valuestring);

        // if (is_peach_command(text->valuestring)) {
        //     esp_err_t display_err = bsp_display_show_peach();
        //     if (display_err == ESP_OK) {
        //         telegram_send_message(chat_id_str, "已在屏幕显示桃子图案");
        //     } else {
        //         ESP_LOGW(TAG, "Failed to render peach: %s", esp_err_to_name(display_err));
        //         telegram_send_message(chat_id_str, "显示桃子失败，请确认屏幕已初始化");
        //     }
        //     continue;
        // }

        /* Push to inbound bus */
        // mimi_msg_t msg = {0};
        // strncpy(msg.channel, MIMI_CHAN_TELEGRAM, sizeof(msg.channel) - 1);
        // strncpy(msg.chat_id, chat_id_str, sizeof(msg.chat_id) - 1);
        // msg.content = strdup(text->valuestring);
        // if (msg.content) {
        //     if (message_bus_push_inbound(&msg) != ESP_OK) {
        //         ESP_LOGW(TAG, "Inbound queue full, drop telegram message");
        //         free(msg.content);
        //     }
        // }
    }

    //循环结束后释放 JSON
    cJSON_Delete(root);
}

static void telegram_poll_task(void *arg)
{
    ESP_LOGI(TAG, "Telegram polling task started");

    // 无限循环，不停轮询 Telegram 消息。
    // 轮询任务通常就是这种永久运行的任务
    while (1)
    {
        //检查是否已经配置了 Bot Token（s_bot_token 是 Telegram 的 token）。
        if (s_bot_token[0] == '\0')
        {
            ESP_LOGW(TAG, "No bot token configured, waiting...");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        //构造调用 Telegram API 的参数字符串
        char params[128];
        snprintf(params, sizeof(params),
                 "getUpdates?offset=%" PRId64 "&timeout=%d",
                 s_update_offset, MIMI_TG_POLL_TIMEOUT_S);

        //调用 tg_api_call 发送 HTTP 请求给 Telegram
        char *resp = tg_api_call(params, NULL);
        if (resp)
        {
            //调用 process_updates(resp) → 把 JSON 字符串解析、去重、分发到消息队列 处理完后释放 resp 内存
            process_updates(resp);
            free(resp);
        }
        else
        {
            /* Back off on error */
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
    }
}

/* --- Public API --- */

esp_err_t telegram_bot_init(void)
{
    /* NVS overrides take highest priority (set via CLI) */
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_TG, NVS_READONLY, &nvs) == ESP_OK)
    {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TG_TOKEN, tmp, &len) == ESP_OK && tmp[0])
        {
            strncpy(s_bot_token, tmp, sizeof(s_bot_token) - 1);
        }

        int64_t offset = 0;
        if (nvs_get_i64(nvs, TG_OFFSET_NVS_KEY, &offset) == ESP_OK && offset > 0)
        {
            s_update_offset = offset;
            s_last_saved_offset = offset;
            ESP_LOGI(TAG, "Loaded Telegram update offset: %" PRId64, s_update_offset);
        }
        nvs_close(nvs);
    }

    /* s_bot_token is already initialized from MIMI_SECRET_TG_TOKEN as fallback */

    if (s_bot_token[0])
    {
        ESP_LOGI(TAG, "Telegram bot token loaded (len=%d)", (int)strlen(s_bot_token));
    }
    else
    {
        ESP_LOGW(TAG, "No Telegram bot token. Use CLI: set_tg_token <TOKEN>");
    }
    return ESP_OK;
}

esp_err_t telegram_bot_start(void)
{
    BaseType_t ret = xTaskCreatePinnedToCore(
        telegram_poll_task, "tg_poll",
        MIMI_TG_POLL_STACK, NULL,
        MIMI_TG_POLL_PRIO, NULL, MIMI_TG_POLL_CORE);

    return (ret == pdPASS) ? ESP_OK : ESP_FAIL;
}

esp_err_t telegram_send_message(const char *chat_id, const char *text)
{
    if (s_bot_token[0] == '\0')
    {
        ESP_LOGW(TAG, "Cannot send: no bot token");
        return ESP_ERR_INVALID_STATE;
    }

    /* Split long messages at 4096-char boundary */
    size_t text_len = strlen(text);
    size_t offset = 0;
    int all_ok = 1;

    while (offset < text_len)
    {
        size_t chunk = text_len - offset;
        if (chunk > MIMI_TG_MAX_MSG_LEN)
        {
            chunk = MIMI_TG_MAX_MSG_LEN;
        }

        /* Build JSON body */
        cJSON *body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "chat_id", chat_id);

        /* Create null-terminated chunk */
        char *segment = malloc(chunk + 1);
        if (!segment)
        {
            cJSON_Delete(body);
            return ESP_ERR_NO_MEM;
        }
        memcpy(segment, text + offset, chunk);
        segment[chunk] = '\0';

        cJSON_AddStringToObject(body, "text", segment);
        cJSON_AddStringToObject(body, "parse_mode", "Markdown");

        char *json_str = cJSON_PrintUnformatted(body);
        cJSON_Delete(body);
        free(segment);

        if (!json_str)
        {
            all_ok = 0;
            offset += chunk;
            continue;
        }

        ESP_LOGI(TAG, "Sending telegram chunk to %s (%d bytes)", chat_id, (int)chunk);
        char *resp = tg_api_call("sendMessage", json_str);
        free(json_str);

        int sent_ok = 0;
        bool markdown_failed = false;
        if (resp)
        {
            const char *desc = NULL;
            sent_ok = tg_response_is_ok(resp, &desc);
            if (!sent_ok)
            {
                markdown_failed = true;
                ESP_LOGI(TAG, "Markdown rejected by Telegram for %s: %s",
                         chat_id, desc ? desc : "unknown");
            }
        }

        if (!sent_ok)
        {
            /* Retry without parse_mode */
            cJSON *body2 = cJSON_CreateObject();
            cJSON_AddStringToObject(body2, "chat_id", chat_id);
            char *seg2 = malloc(chunk + 1);
            if (seg2)
            {
                memcpy(seg2, text + offset, chunk);
                seg2[chunk] = '\0';
                cJSON_AddStringToObject(body2, "text", seg2);
                free(seg2);
            }
            char *json2 = cJSON_PrintUnformatted(body2);
            cJSON_Delete(body2);
            if (json2)
            {
                char *resp2 = tg_api_call("sendMessage", json2);
                free(json2);
                if (resp2)
                {
                    const char *desc2 = NULL;
                    sent_ok = tg_response_is_ok(resp2, &desc2);
                    if (!sent_ok)
                    {
                        ESP_LOGE(TAG, "Plain send failed: %s", desc2 ? desc2 : "unknown");
                        ESP_LOGE(TAG, "Telegram raw response: %.300s", resp2);
                    }
                    free(resp2);
                }
                else
                {
                    ESP_LOGE(TAG, "Plain send failed: no HTTP response");
                }
            }
            else
            {
                ESP_LOGE(TAG, "Plain send failed: no JSON body");
            }
        }

        if (!sent_ok)
        {
            all_ok = 0;
        }
        else
        {
            if (markdown_failed)
            {
                ESP_LOGI(TAG, "Plain-text fallback succeeded for %s", chat_id);
            }
            ESP_LOGI(TAG, "Telegram send success to %s (%d bytes)", chat_id, (int)chunk);
        }

        free(resp);
        offset += chunk;
    }

    return all_ok ? ESP_OK : ESP_FAIL;
}

esp_err_t telegram_set_token(const char *token)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_TG, NVS_READWRITE, &nvs));
    ESP_ERROR_CHECK(nvs_set_str(nvs, MIMI_NVS_KEY_TG_TOKEN, token));
    ESP_ERROR_CHECK(nvs_commit(nvs));
    nvs_close(nvs);

    strncpy(s_bot_token, token, sizeof(s_bot_token) - 1);
    ESP_LOGI(TAG, "Telegram bot token saved");
    return ESP_OK;
}
