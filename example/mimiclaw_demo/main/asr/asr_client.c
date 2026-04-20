#include "asr/asr_client.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "nvs.h"
#include "cJSON.h"

static const char *TAG = "asr";
static const char *ASR_PROVIDER_GLADIA = "gladia";                             // 支持 Gladia 服务商
static const char *ASR_BOUNDARY = "----MimiClawGladiaBoundary7MA4YWxkTrZu0gW"; // 文件上传分隔符（固定格式）
static const char *GLADIA_AUTH_HEADER = "x-gladia-key";                        // API 密钥请求头
static const char *GLADIA_UPLOAD_PATH = "/v2/upload";                          // 上传接口
static const char *GLADIA_PRERECORDED_PATH = "/v2/pre-recorded";               // 转写接口
static const int ASR_POLL_INTERVAL_MS = 1000;

static char s_api_key[128] = MIMI_SECRET_ASR_API_KEY;
static char s_provider[32] = MIMI_SECRET_ASR_PROVIDER;
static char s_model[64] = MIMI_SECRET_ASR_MODEL;
static char s_base_url[192] = MIMI_SECRET_ASR_BASE_URL;
static char s_unavailable_reason[128] = "ASR provider not connected yet";

// 用来接收 HTTP 返回数据
typedef struct
{
    char *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

// 工具函数：安全复制字符串
// 防止字符串溢出，永远保证结尾合法
static void safe_copy(char *dst, size_t dst_size, const char *src)
{
    if (!dst || dst_size == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0'; // 确保结尾是 \0
}

// 初始化缓冲区
static esp_err_t resp_buf_init(resp_buf_t *rb, size_t cap)
{
    if (!rb || cap == 0)
        return ESP_ERR_INVALID_ARG;
    rb->buf = calloc(1, cap);
    if (!rb->buf)
        return ESP_ERR_NO_MEM;
    rb->len = 0; // 分配内存并清零
    rb->cap = cap;
    return ESP_OK;
}

// 释放内存
static void resp_buf_free(resp_buf_t *rb)
{
    if (!rb)
        return;
    free(rb->buf);
    rb->buf = NULL;
    rb->len = 0;
    rb->cap = 0;
}

// 追加数据 + 自动扩容
static esp_err_t resp_buf_append(resp_buf_t *rb, const char *data, size_t len)
{
    if (!rb || !data)
        return ESP_ERR_INVALID_ARG;
    if (rb->len + len + 1 > rb->cap)
    {
        size_t new_cap = rb->cap ? rb->cap * 2 : 1024;
        while (new_cap < rb->len + len + 1)
        {
            new_cap *= 2;
        }
        char *tmp = realloc(rb->buf, new_cap);
        if (!tmp)
            return ESP_ERR_NO_MEM;
        rb->buf = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->buf + rb->len, data, len);
    rb->len += len;
    rb->buf[rb->len] = '\0';
    return ESP_OK;
}

// 解析 HTTP chunked 分块传输，把服务器发的流式数据拼成完整 JSON
//  4\r\n
//  test\r\n
//  3\r\n
//  123\r\n
//  0\r\n
// test123
static void resp_buf_decode_chunked(resp_buf_t *rb)
{
    if (!rb || !rb->buf || rb->len == 0)
    {
        return;
    }

    size_t i = 0;
    while (i < rb->len && (rb->buf[i] == ' ' || rb->buf[i] == '\t'))
    {
        i++;
    }
    if (i < rb->len && (rb->buf[i] == '{' || rb->buf[i] == '['))
    {
        return;
    }

    char *src = rb->buf;
    char *dst = rb->buf;
    char *end = rb->buf + rb->len;
    while (src < end)
    {
        char *line_end = strstr(src, "\r\n");
        if (!line_end)
        {
            break;
        }

        unsigned long chunk_size = strtoul(src, NULL, 16);
        if (chunk_size == 0)
        {
            break;
        }

        src = line_end + 2;
        if (src + chunk_size > end)
        {
            size_t avail = end - src;
            memmove(dst, src, avail);
            dst += avail;
            break;
        }

        memmove(dst, src, chunk_size);
        dst += chunk_size;
        src += chunk_size;
        if (src + 2 <= end && src[0] == '\r' && src[1] == '\n')
        {
            src += 2;
        }
    }

    rb->len = (size_t)(dst - rb->buf);
    rb->buf[rb->len] = '\0';
}

// HTTP 事件处理 处理事件
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    // 取用户数据
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    // 判断是不是“数据事件” rb 是否有效  判断数据是否存在 判断长度 > 0
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb && evt->data && evt->data_len > 0)
    {
        //// 追加数据 + 自动扩容
        return resp_buf_append(rb, (const char *)evt->data, (size_t)evt->data_len);
    }
    return ESP_OK;
}

// 把 API Key、URL 等保存到 ESP32 闪存
static esp_err_t asr_save_str(const char *key, const char *value)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open(MIMI_NVS_ASR, NVS_READWRITE, &nvs)); // 打开 NVS
    ESP_ERROR_CHECK(nvs_set_str(nvs, key, value ? value : ""));   // 保存字符串
    ESP_ERROR_CHECK(nvs_commit(nvs));                             // 提交
    nvs_close(nvs);                                               // 关闭
    return ESP_OK;
}

// 只支持 Gladia，所以判断是不是它
static bool provider_is_gladia(void)
{
    return s_provider[0] != '\0' && strcmp(s_provider, ASR_PROVIDER_GLADIA) == 0;
}

// 设置不可用原因
static void set_unavailable_reason(const char *reason)
{
    safe_copy(s_unavailable_reason, sizeof(s_unavailable_reason), reason);
}

// 检查配置是否完整 自动判断是否可以使用 ASR
static void refresh_config_state(void)
{
    if (!provider_is_gladia())
    {
        set_unavailable_reason("ASR provider must be gladia");
    }
    else if (s_api_key[0] == '\0')
    {
        set_unavailable_reason("Gladia API key missing");
    }
    else if (s_base_url[0] == '\0')
    {
        set_unavailable_reason("Gladia base URL missing");
    }
    else
    {
        set_unavailable_reason("Gladia ASR ready");
    }
}

// 取文件名（不带路径）
static const char *file_basename(const char *path)
{
    // 找最后一个 /
    const char *slash = strrchr(path, '/');
    /*如果找到了 '/'：
    返回 '/' 后面的内容
    否则：
    直接返回原字符串 */
    return slash ? slash + 1 : path;
}

// 解析 HTTPS 地址   代理模式必须用
/*输入：url = "https://api.example.com/v2/upload"输出:host = "api.example.com" path = "/v2/upload"*/
static bool parse_https_url(const char *url, char *host, size_t host_size, char *path, size_t path_size)
{
    // 只处理 https:// 开头的 URL
    const char *prefix = "https://";
    // 校验 URL 是否合法
    size_t prefix_len = strlen(prefix);
    if (!url || strncmp(url, prefix, prefix_len) != 0)
        return false;

    // 找 host 起点（关键）api.example.com
    const char *host_start = url + prefix_len;
    // 找 path 起点 /v2/upload
    const char *path_start = strchr(host_start, '/');
    // 有路径和没路径
    size_t host_len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);
    // 检查长度是否合法
    if (host_len == 0 || host_len >= host_size)
        return false;
    // 拷贝 host
    // 手动复制 + 加字符串结束符
    // 为什么不用 strcpy   host 不是以 \0 结尾的一段  只是“中间一截”
    memcpy(host, host_start, host_len);
    host[host_len] = '\0';

    // 处理 path
    if (path_start)
    {
        // 有 path
        strncpy(path, path_start, path_size - 1);
        path[path_size - 1] = '\0';
    }
    else
    {
        // 没 path
        strncpy(path, "/", path_size - 1);
        path[path_size - 1] = '\0';
    }
    return true;
}

// 拼接 URL  https://abc.com + /upload  自动处理斜杠，避免错误
// 基础 URL（如 https://api.com）
// 路径（如 /upload）
// 输出结果
// 输出缓冲区大小
static bool join_url(const char *base, const char *suffix, char *out, size_t out_size)
{
    // 参数合法性检查
    if (!base || !base[0] || !suffix || !suffix[0] || !out || out_size == 0)
    {
        return false;
    }
    // 获取 base 长度
    size_t base_len = strlen(base);
    // 判断 base 是否以 / 结尾
    // base = "https://api.com/"
    bool base_has_slash = base_len > 0 && base[base_len - 1] == '/';
    // 判断 suffix 是否以 / 开头
    bool suffix_has_slash = suffix[0] == '/';
    // 定义返回值
    int n = 0;
    // 两个都有 /
    if (base_has_slash && suffix_has_slash)
    {
        // base   = "https://api.com/"
        // suffix = "/upload" 如果直接拼https://api.com//upload  ❌（多了一个 /） 处理这种情况
        n = snprintf(out, out_size, "%.*s%s", (int)(base_len - 1), base, suffix);
    }
    // 两个都没有 /
    else if (!base_has_slash && !suffix_has_slash)
    {
        n = snprintf(out, out_size, "%s/%s", base, suffix);
    }
    // 刚好一个有 /
    else
    {
        n = snprintf(out, out_size, "%s%s", base, suffix);
    }
    return n > 0 && (size_t)n < out_size;
}

// 确保数据全部发送  循环发送，直到发完  不需要代理
// client	HTTP连接
// data	要发送的数据
// len	数据总长度
static esp_err_t write_http_all(esp_http_client_handle_t client, const char *data, size_t len)
{
    // 初始化已发送长度
    size_t sent = 0;
    // 循环发送
    while (sent < len)
    {
            //发送数据（关键行）
            // 网络发送不是一次完成的
            // data = "HELLO_WORLD"
            /*sent = 0 → data + 0 = "HELLO_WORLD" 第一次 sent = 5 → data + 5 = "_WORLD"第二次 从“还没发送的地方”继续发 len - sent剩余还要发送的长度*/
            int n = esp_http_client_write(client, data + sent, len - sent);
        if (n <= 0)
            return ESP_FAIL;
        // 更新已发送长度
        sent += (size_t)n;
    }
    return ESP_OK;
}

// 确保数据全部发送 循环发送，直到发完  需要代理
static esp_err_t write_proxy_all(proxy_conn_t *conn, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len)
    {
        int n = proxy_conn_write(conn, data + sent, (int)(len - sent));
        if (n <= 0)
            return ESP_FAIL;
        sent += (size_t)n;
    }
    return ESP_OK;
}

// 流式读取文件并发送  读取本地音频文件
// HTTP连接 文件指针（音频文件）
static esp_err_t stream_file_to_http(esp_http_client_handle_t client, FILE *f)
{
    // 定义缓冲区
    char buf[1024];
    while (1)
    {
        // 读取文件（核心） 存数据的地方 每次读1字节 最多读1024字节 文件
        size_t n = fread(buf, 1, sizeof(buf), f);
        // 如果读到数据
        if (n > 0)
        {
            // 发送数据  把这一块数据发出去
            esp_err_t err = write_http_all(client, buf, n);
            if (err != ESP_OK)
                return err;
        }
        if (n < sizeof(buf))
        {
            if (feof(f))
                return ESP_OK;
            return ESP_FAIL;
        }
    }
}

// 流式读取文件并发送 代理读取本地音频文件
static esp_err_t stream_file_to_proxy(proxy_conn_t *conn, FILE *f)
{
    char buf[1024];
    while (1)
    {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n > 0)
        {
            esp_err_t err = write_proxy_all(conn, buf, n);
            if (err != ESP_OK)
                return err;
        }
        if (n < sizeof(buf))
        {
            if (feof(f))
                return ESP_OK;
            return ESP_FAIL;
        }
    }
}

// 从 JSON 提取错误信息  从 Gladia 返回 JSON 里找 error/message  给用户友好提示
// JSON根对象   输出字符串   输出缓冲区大小
static void extract_error_message(cJSON *root, char *out, size_t out_size)
{
    // 检查输出缓冲区
    if (!out || out_size == 0)
        return;
    // 先清空输出
    out[0] = '\0';
    // 检查 JSON 是否存在
    if (!root)
        return;
    // 尝试获取 error 字段
    cJSON *error = cJSON_GetObjectItem(root, "error");
    // 初始化 message
    cJSON *message = NULL;
    // 有 error 字段  是对象
    if (error && cJSON_IsObject(error))
    {
        message = cJSON_GetObjectItem(error, "message");
    }
    // 如果不是字符串
    if (!cJSON_IsString(message))
    {
        // 尝试：root.message
        message = cJSON_GetObjectItem(root, "message");
    }
    if (!cJSON_IsString(message))
    {
        message = cJSON_GetObjectItem(root, "status_message");
    }
    // 最终判断并拷贝 是字符串 有值 非空字符串
    if (cJSON_IsString(message) && message->valuestring && message->valuestring[0])
    {
        // 拷贝到输出
        safe_copy(out, out_size, message->valuestring);
    }
}

// 代理模式下，从响应里抽状态码 + 正文
// 原始HTTP响应（含头+body）  提取后的body     HTTP状态码
static esp_err_t proxy_response_to_body(resp_buf_t *raw, resp_buf_t *body, int *out_status)
{
    // 参数检查
    if (!raw || !raw->buf || !body || !out_status)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 解析 HTTP 状态码  HTTP/1.1 200 OK找到200 %*s会读到1.1，读一个字符串，但丢弃（不保存）
    if (sscanf(raw->buf, "HTTP/%*s %d", out_status) != 1)
    {
        set_unavailable_reason("Failed to parse proxy HTTP status");
        return ESP_FAIL;
    }
    // 判断是不是 chunked 是的话要解码，不是则直接用
    bool is_chunked = strstr(raw->buf, "Transfer-Encoding: chunked") != NULL;
    // 找到 body 起始位置（核心🔥）
    char *resp_body = strstr(raw->buf, "\r\n\r\n");
    // 如果没找到
    if (!resp_body)
    {
        set_unavailable_reason("Proxy response missing body");
        return ESP_FAIL;
    }
    // 跳过分界符
    resp_body += 4;
    // 计算 body 长度
    size_t body_len = raw->len - (size_t)(resp_body - raw->buf);
    memmove(raw->buf, resp_body, body_len);
    // 更新长度
    raw->len = body_len;
    raw->buf[raw->len] = '\0';

    // 如果是 chunked → 解码
    if (is_chunked)
    {
        // 解码
        resp_buf_decode_chunked(raw);
    }

    // 写入输出 buffer
    return resp_buf_append(body, raw->buf, raw->len);
}

// 发送 JSON 请求 直连
static esp_err_t http_json_direct(const char *method, const char *url, const char *body, resp_buf_t *rb, int *out_status)
{
    // http的客户端配置
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = rb,
        .timeout_ms = MIMI_ASR_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    // http的初始化 创建
    esp_http_client_handle_t client = esp_http_client_init(&config);
    // 创建失败
    if (!client)
    {
        set_unavailable_reason("Failed to init HTTP client");
        return ESP_FAIL;
    }

    // 设置http的请求方法
    esp_http_client_set_method(client, (strcmp(method, "GET") == 0) ? HTTP_METHOD_GET : HTTP_METHOD_POST);
    // 设置http的请求头
    esp_http_client_set_header(client, GLADIA_AUTH_HEADER, s_api_key);
    // 设置http的请求头
    esp_http_client_set_header(client, "Accept", "application/json");
    // 如果有请求体
    if (body)
    {
        // 设置类型
        esp_http_client_set_header(client, "Content-Type", "application/json");
        // 设置内容
        esp_http_client_set_post_field(client, body, (int)strlen(body));
    }

    // 真正发送请求
    esp_err_t err = esp_http_client_perform(client);
    // 获取响应码
    *out_status = esp_http_client_get_status_code(client);
    // 释放资源
    esp_http_client_cleanup(client);
    // 错误处理
    if (err != ESP_OK && rb->len == 0)
    {
        set_unavailable_reason("HTTP JSON request failed");
    }
    return err;
}

// 代理
static esp_err_t http_json_via_proxy(const char *method, const char *url, const char *body, resp_buf_t *rb, int *out_status)
{
    char host[96];
    char path[224];
    if (!parse_https_url(url, host, sizeof(host), path, sizeof(path)))
    {
        set_unavailable_reason("Invalid Gladia URL");
        return ESP_ERR_INVALID_ARG;
    }

    proxy_conn_t *conn = proxy_conn_open(host, 443, MIMI_ASR_TIMEOUT_MS);
    if (!conn)
    {
        set_unavailable_reason("Failed to open proxy tunnel to Gladia");
        return ESP_FAIL;
    }

    char header[1024];
    int header_len;
    if (body)
    {
        header_len = snprintf(header, sizeof(header),
                              "%s %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "%s: %s\r\n"
                              "Content-Type: application/json\r\n"
                              "Accept: application/json\r\n"
                              "Content-Length: %u\r\n"
                              "Connection: close\r\n\r\n",
                              method, path, host, GLADIA_AUTH_HEADER, s_api_key,
                              (unsigned)strlen(body));
    }
    else
    {
        header_len = snprintf(header, sizeof(header),
                              "%s %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "%s: %s\r\n"
                              "Accept: application/json\r\n"
                              "Connection: close\r\n\r\n",
                              method, path, host, GLADIA_AUTH_HEADER, s_api_key);
    }
    if (header_len <= 0 || (size_t)header_len >= sizeof(header))
    {
        proxy_conn_close(conn);
        set_unavailable_reason("Failed to build proxy JSON header");
        return ESP_ERR_INVALID_SIZE;
    }

    esp_err_t err = write_proxy_all(conn, header, (size_t)header_len);
    if (err == ESP_OK && body)
    {
        err = write_proxy_all(conn, body, strlen(body));
    }
    if (err != ESP_OK)
    {
        proxy_conn_close(conn);
        set_unavailable_reason("Failed while sending proxy JSON request");
        return err;
    }

    resp_buf_t raw = {0};
    err = resp_buf_init(&raw, 4096);
    if (err != ESP_OK)
    {
        proxy_conn_close(conn);
        return err;
    }

    char buf[1024];
    while (1)
    {
        int n = proxy_conn_read(conn, buf, sizeof(buf), MIMI_ASR_TIMEOUT_MS);
        if (n < 0)
        {
            err = ESP_FAIL;
            break;
        }
        if (n == 0)
        {
            break;
        }
        err = resp_buf_append(&raw, buf, (size_t)n);
        if (err != ESP_OK)
            break;
    }
    proxy_conn_close(conn);
    if (err != ESP_OK)
    {
        resp_buf_free(&raw);
        set_unavailable_reason("Failed to read proxy JSON response");
        return err;
    }

    err = proxy_response_to_body(&raw, rb, out_status);
    resp_buf_free(&raw);
    return err;
}

// 自动判断用直连还是代理
static esp_err_t http_json_call(const char *method, const char *url, const char *body, resp_buf_t *rb, int *out_status)
{
    return http_proxy_is_enabled()
               ? http_json_via_proxy(method, url, body, rb, out_status)
               : http_json_direct(method, url, body, rb, out_status);
}

// 构建文件上传格式
static esp_err_t build_gladia_upload_envelope(const char *file_path,
                                              char *prefix, size_t prefix_size,
                                              char *suffix, size_t suffix_size,
                                              size_t *total_len)
{
    // 定义一个结构体，用来存文件信息（大小、时间等）
    struct stat st;
    // 文件和文件内容
    if (stat(file_path, &st) != 0 || st.st_size <= 0)
    {
        set_unavailable_reason("Audio file missing or empty");
        return ESP_FAIL;
    }

    // 取文件名 不带路径
    const char *name = file_basename(file_path);
    // 构造 prefix  把字符串写进 prefix 缓冲区
    int prefix_len = snprintf(prefix, prefix_size,
                              "--%s\r\n"                                                            // 标记一个“分段开始”
                              "Content-Disposition: form-data; name=\"audio\"; filename=\"%s\"\r\n" /*这是一个文件字段
                               字段名叫 audio
                               文件名 test.ogg*/
                                  "Content-Type: audio/ogg\r\n\r\n",
                              ASR_BOUNDARY, name);
    // 构造 suffix（结尾🔥）
    int suffix_len = snprintf(suffix, suffix_size, "\r\n--%s--\r\n", ASR_BOUNDARY);
    // 检查长度是否合法
    if (prefix_len <= 0 || suffix_len <= 0 || (size_t)prefix_len >= prefix_size || (size_t)suffix_len >= suffix_size)
    {
        set_unavailable_reason("Multipart envelope build failed");
        return ESP_ERR_INVALID_SIZE;
    }
    // 计算总长度
    *total_len = (size_t)prefix_len + (size_t)st.st_size + (size_t)suffix_len;
    return ESP_OK;
}

// 直连上传
static esp_err_t multipart_upload_direct(const char *url,
                                         const char *file_path,
                                         const char *prefix,
                                         const char *suffix,
                                         size_t total_len,
                                         resp_buf_t *rb,
                                         int *out_status)
{
    // 构造 Content-Type（带 boundary） 这是文件上传，而且用 boundary 来分段
    char content_type[128];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", ASR_BOUNDARY);

    // 配置 HTTP 客户端
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = MIMI_ASR_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    // 创建 HTTP 客户端
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client)
    {
        set_unavailable_reason("Failed to init upload client");
        return ESP_FAIL;
    }

    // 设置请求信息
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, GLADIA_AUTH_HEADER, s_api_key);
    esp_http_client_set_header(client, "Content-Type", content_type);
    esp_http_client_set_header(client, "Accept", "application/json");

    // 打开连接
    esp_err_t err = esp_http_client_open(client, total_len);
    if (err != ESP_OK)
    {
        esp_http_client_cleanup(client);
        set_unavailable_reason("Failed to open upload connection");
        return err;
    }

    // 打开本地文件
    FILE *f = fopen(file_path, "rb");
    // 文件打不开
    if (!f)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        set_unavailable_reason("Failed to open audio file");
        return ESP_FAIL;
    }

    // 发送数据
    // 发送 prefix
    err = write_http_all(client, prefix, strlen(prefix));
    // 发送文件内容
    if (err == ESP_OK)
        err = stream_file_to_http(client, f);
    // 发送 suffix
    if (err == ESP_OK)
        err = write_http_all(client, suffix, strlen(suffix));
    // 关闭文件
    fclose(f);
    // 错误处理
    if (err != ESP_OK)
    {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        set_unavailable_reason("Failed while uploading audio to Gladia");
        return err;
    }

    // 读取服务器响应
    esp_http_client_fetch_headers(client);
    // 获取状态码
    *out_status = esp_http_client_get_status_code(client);

    // 循环读取响应 body
    char buf[1024];
    while (1)
    {
        int n = esp_http_client_read(client, buf, sizeof(buf));
        if (n < 0)
        {
            err = ESP_FAIL;
            break;
        }
        if (n == 0)
        {
            err = ESP_OK;
            break;
        }
        err = resp_buf_append(rb, buf, (size_t)n);
        if (err != ESP_OK)
            break;
    }
    // 关闭连接
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    // 最终错误判断
    if (err != ESP_OK && rb->len == 0)
    {
        set_unavailable_reason("Failed to read Gladia upload response");
    }
    return err;
}

// 代理上传
static esp_err_t multipart_upload_via_proxy(const char *url,
                                            const char *file_path,
                                            const char *prefix,
                                            const char *suffix,
                                            size_t total_len,
                                            resp_buf_t *rb,
                                            int *out_status)
{
    char host[96];
    char path[224];
    if (!parse_https_url(url, host, sizeof(host), path, sizeof(path)))
    {
        set_unavailable_reason("Invalid Gladia upload URL");
        return ESP_ERR_INVALID_ARG;
    }

    proxy_conn_t *conn = proxy_conn_open(host, 443, MIMI_ASR_TIMEOUT_MS);
    if (!conn)
    {
        set_unavailable_reason("Failed to open proxy tunnel to Gladia");
        return ESP_FAIL;
    }

    char header[1024];
    int header_len = snprintf(header, sizeof(header),
                              "POST %s HTTP/1.1\r\n"
                              "Host: %s\r\n"
                              "%s: %s\r\n"
                              "Content-Type: multipart/form-data; boundary=%s\r\n"
                              "Accept: application/json\r\n"
                              "Content-Length: %u\r\n"
                              "Connection: close\r\n\r\n",
                              path, host, GLADIA_AUTH_HEADER, s_api_key, ASR_BOUNDARY,
                              (unsigned)total_len);
    if (header_len <= 0 || (size_t)header_len >= sizeof(header))
    {
        proxy_conn_close(conn);
        set_unavailable_reason("Failed to build proxy upload header");
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(file_path, "rb");
    if (!f)
    {
        proxy_conn_close(conn);
        set_unavailable_reason("Failed to open audio file");
        return ESP_FAIL;
    }

    esp_err_t err = write_proxy_all(conn, header, (size_t)header_len);
    if (err == ESP_OK)
        err = write_proxy_all(conn, prefix, strlen(prefix));
    if (err == ESP_OK)
        err = stream_file_to_proxy(conn, f);
    if (err == ESP_OK)
        err = write_proxy_all(conn, suffix, strlen(suffix));
    fclose(f);
    if (err != ESP_OK)
    {
        proxy_conn_close(conn);
        set_unavailable_reason("Failed while uploading audio to Gladia via proxy");
        return err;
    }

    resp_buf_t raw = {0};
    err = resp_buf_init(&raw, 4096);
    if (err != ESP_OK)
    {
        proxy_conn_close(conn);
        return err;
    }

    char buf[1024];
    while (1)
    {
        int n = proxy_conn_read(conn, buf, sizeof(buf), MIMI_ASR_TIMEOUT_MS);
        if (n < 0)
        {
            err = ESP_FAIL;
            break;
        }
        if (n == 0)
        {
            break;
        }
        err = resp_buf_append(&raw, buf, (size_t)n);
        if (err != ESP_OK)
            break;
    }
    proxy_conn_close(conn);
    if (err != ESP_OK)
    {
        resp_buf_free(&raw);
        set_unavailable_reason("Failed to read Gladia proxy upload response");
        return err;
    }

    err = proxy_response_to_body(&raw, rb, out_status);
    resp_buf_free(&raw);
    return err;
}

// 统一入口
static esp_err_t multipart_upload_call(const char *url,
                                       const char *file_path,
                                       const char *prefix,
                                       const char *suffix,
                                       size_t total_len,
                                       resp_buf_t *rb,
                                       int *out_status)
{
    return http_proxy_is_enabled()
               ? multipart_upload_via_proxy(url, file_path, prefix, suffix, total_len, rb, out_status)
               : multipart_upload_direct(url, file_path, prefix, suffix, total_len, rb, out_status);
}

// 上传本地音频文件 → 返回云端音频地址
static esp_err_t gladia_upload_file(const char *file_path, char *out_audio_url, size_t out_audio_url_size)
{
    char upload_url[256];
    // 拼接地址
    // https://api.gladia.io + /v2/upload
    if (!join_url(s_base_url, GLADIA_UPLOAD_PATH, upload_url, sizeof(upload_url)))
    {
        set_unavailable_reason("Failed to build Gladia upload URL");
        return ESP_ERR_INVALID_ARG;
    }

    // 构造 multipart  HTTP头 + boundary  结束标志  总长度
    char prefix[256];
    char suffix[64];
    size_t total_len = 0;
    // 生成 HTTP 上传格式（文件上传必须用这个）
    // 生成 multipart 格式
    // 计算 Content-Length
    esp_err_t err = build_gladia_upload_envelope(file_path, prefix, sizeof(prefix), suffix, sizeof(suffix), &total_len);
    if (err != ESP_OK)
        return err;
    // 初始化响应缓冲区
    resp_buf_t rb = {0};
    // 初始化接收缓冲区
    err = resp_buf_init(&rb, 2048);
    if (err != ESP_OK)
    {
        set_unavailable_reason("Out of memory while preparing upload");
        return err;
    }

    //HTTP 状态码
    int status = 0;
    // 真正上传文件  发送 prefix + 文件 + suffix
    err = multipart_upload_call(upload_url, file_path, prefix, suffix, total_len, &rb, &status);
    if (err != ESP_OK)
    {
        resp_buf_free(&rb);
        return err;
    }

    // 解析返回 JSON
    cJSON *root = cJSON_Parse(rb.buf);
    //如果失败
    if (!root)
    {
        resp_buf_free(&rb);
        set_unavailable_reason("Gladia upload returned invalid JSON");
        return ESP_FAIL;
    }
    // 检查 HTTP 状态码
    if (status != 200 && status != 201)
    {
        char reason[128] = {0};
        //从 JSON 里提取错误信息
        extract_error_message(root, reason, sizeof(reason));
        if (reason[0])
        {
            set_unavailable_reason(reason);
        }
        else
        {   
            //设置错误
            set_unavailable_reason("Gladia upload failed");
        }
        cJSON_Delete(root);
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    // 提取关键字段  提取 audio_url
    cJSON *audio_url = cJSON_GetObjectItem(root, "audio_url");
    //是否合法
    if (!cJSON_IsString(audio_url) || !audio_url->valuestring || !audio_url->valuestring[0])
    {
        //清理：
        cJSON_Delete(root);
        resp_buf_free(&rb);
        set_unavailable_reason("Gladia upload missing audio_url");
        return ESP_FAIL;
    }
    //拷贝结果
    safe_copy(out_audio_url, out_audio_url_size, audio_url->valuestring);
    //清理资源
    cJSON_Delete(root);
    resp_buf_free(&rb);
    return ESP_OK;
}

// 创建转写任务 → 得到 job_id   告诉 Gladia 转写这个音频   返回任务 ID
static esp_err_t gladia_create_job(const char *audio_url, char *out_job_id, size_t out_job_id_size)
{

    //拼接 create URL
    char create_url[256];

    //https://api.gladia.io/v2/pre-recorded
    if (!join_url(s_base_url, GLADIA_PRERECORDED_PATH, create_url, sizeof(create_url)))
    {
        set_unavailable_reason("Failed to build Gladia create URL");
        return ESP_ERR_INVALID_ARG;
    }
    //构造 JSON 请求体
    char body[768];
    int body_len = snprintf(body, sizeof(body), "{\"audio_url\":\"%s\"}", audio_url);
    //检查是否溢出
    if (body_len <= 0 || (size_t)body_len >= sizeof(body))
    {
        set_unavailable_reason("Gladia request body too large");
        return ESP_ERR_INVALID_SIZE;
    }

    //初始化响应缓冲区
    resp_buf_t rb = {0};
    esp_err_t err = resp_buf_init(&rb, 2048);
    if (err != ESP_OK)
    {
        set_unavailable_reason("Out of memory while creating Gladia job");
        return err;
    }
    
    //发送 HTTP 请求
    int status = 0;
    err = http_json_call("POST", create_url, body, &rb, &status);
    if (err != ESP_OK)
    {
        resp_buf_free(&rb);
        return err;
    }

    //解析 JSON
    cJSON *root = cJSON_Parse(rb.buf);
    if (!root)
    {
        resp_buf_free(&rb);
        set_unavailable_reason("Gladia create job returned invalid JSON");
        return ESP_FAIL;
    }

    //检查状态码
    if (status != 200 && status != 201)
    {
        char reason[128] = {0};
        extract_error_message(root, reason, sizeof(reason));
        if (reason[0])
        {
            set_unavailable_reason(reason);
        }
        else
        {
            set_unavailable_reason("Gladia create job failed");
        }
        cJSON_Delete(root);
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    //获取 job_id
    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (cJSON_IsString(id) && id->valuestring && id->valuestring[0])
    {
        safe_copy(out_job_id, out_job_id_size, id->valuestring);
        cJSON_Delete(root);
        resp_buf_free(&rb);
        return ESP_OK;
    }

    //fallback：从 result_url 提取
    cJSON *result_url = cJSON_GetObjectItem(root, "result_url");
    //如果 API 没给 id，而是给//https://.../result/abc123
    if (cJSON_IsString(result_url) && result_url->valuestring && result_url->valuestring[0])
    {
        //abc123
        const char *tail = strrchr(result_url->valuestring, '/');
        if (tail && tail[1])
        {
            safe_copy(out_job_id, out_job_id_size, tail + 1);
            cJSON_Delete(root);
            resp_buf_free(&rb);
            return ESP_OK;
        }
    }
    //如果都没有
    cJSON_Delete(root);
    resp_buf_free(&rb);
    set_unavailable_reason("Gladia create job missing id");
    return ESP_FAIL;
}

// 轮询一次结果
static esp_err_t gladia_poll_job_once(const char *job_id, char *out_text, size_t out_size)
{
    //基础地址
    char base_poll_url[256];
    //最终请求地址
    char poll_url[320];
    //https://api.gladia.io + /v2/pre-recorded
    if (!join_url(s_base_url, GLADIA_PRERECORDED_PATH, base_poll_url, sizeof(base_poll_url)))
    {
        set_unavailable_reason("Failed to build Gladia poll URL");
        return ESP_ERR_INVALID_ARG;
    }
    //https://api.gladia.io/v2/pre-recorded/{job_id}
    int n = snprintf(poll_url, sizeof(poll_url), "%s/%s", base_poll_url, job_id);
    //拼接失败 或 超出缓冲区
    if (n <= 0 || (size_t)n >= sizeof(poll_url))
    {
        set_unavailable_reason("Gladia poll URL too long");
        return ESP_ERR_INVALID_SIZE;
    }

    //准备接收数据
    resp_buf_t rb = {0};
    esp_err_t err = resp_buf_init(&rb, 4096);
    if (err != ESP_OK)
    {
        set_unavailable_reason("Out of memory while polling Gladia job");
        return err;
    }

    //发起 HTTP 请求
    int status = 0;
    err = http_json_call("GET", poll_url, NULL, &rb, &status);
    if (err != ESP_OK)
    {
        resp_buf_free(&rb);
        return err;
    }

    //解析 JSON
    cJSON *root = cJSON_Parse(rb.buf);
    if (!root)
    {
        resp_buf_free(&rb);
        set_unavailable_reason("Gladia poll returned invalid JSON");
        return ESP_FAIL;
    }

    //检查 HTTP 状态码
    if (status != 200)
    {
        char reason[128] = {0};
        extract_error_message(root, reason, sizeof(reason));
        if (reason[0])
        {
            set_unavailable_reason(reason);
        }
        else
        {
            set_unavailable_reason("Gladia poll failed");
        }
        cJSON_Delete(root);
        resp_buf_free(&rb);
        return ESP_FAIL;
    }

    //尝试拿“最终结果”
    cJSON *result = cJSON_GetObjectItem(root, "result");
    /*如果 result 存在 → 取 transcription
      否则 → NULL*/
    cJSON *transcription = result ? cJSON_GetObjectItem(result, "transcription") : NULL;
    //result.transcription.full_transcript
    cJSON *full_transcript = transcription ? cJSON_GetObjectItem(transcription, "full_transcript") : NULL;
    //判断是否成功
    if (cJSON_IsString(full_transcript) && full_transcript->valuestring && full_transcript->valuestring[0])
    {
        //把识别结果写出去
        safe_copy(out_text, out_size, full_transcript->valuestring);
        cJSON_Delete(root);
        resp_buf_free(&rb);
        //状态更新
        set_unavailable_reason("Gladia transcription ready");
        return ESP_OK;
    }
    
    //检查任务状态
    cJSON *status_field = cJSON_GetObjectItem(root, "status");
    //确认是字符串
    if (cJSON_IsString(status_field) && status_field->valuestring)
    {
        const char *value = status_field->valuestring;
        //判断失败状态
        if (strcmp(value, "error") == 0 || strcmp(value, "failed") == 0 || strcmp(value, "canceled") == 0 || strcmp(value, "cancelled") == 0)
        {
            //处理
            char reason[128] = {0};
            extract_error_message(root, reason, sizeof(reason));
            if (reason[0])
            {
                set_unavailable_reason(reason);
            }
            else
            {
                set_unavailable_reason("Gladia job failed");
            }
            cJSON_Delete(root);
            resp_buf_free(&rb);
            return ESP_FAIL;
        }
    }
    //还在处理中
    cJSON_Delete(root);
    resp_buf_free(&rb);
    set_unavailable_reason("Gladia transcription still processing");
    return ESP_ERR_NOT_FOUND;
}

// 等待最终结果
static esp_err_t gladia_wait_for_result(const char *job_id, char *out_text, size_t out_size)
{
    //计算最多轮询次数
    int max_attempts = (MIMI_ASR_TIMEOUT_MS + ASR_POLL_INTERVAL_MS - 1) / ASR_POLL_INTERVAL_MS;//46次
    //保证至少轮询一次
    if (max_attempts < 1)
    {
        max_attempts = 1;
    }

    //开始轮询循环
    for (int i = 0; i < max_attempts; ++i)
    {
        //每次轮询
        esp_err_t err = gladia_poll_job_once(job_id, out_text, out_size);
        //如果成功
        if (err == ESP_OK)
        {
            return ESP_OK;
        }
        //如果不是“处理中”
        if (err != ESP_ERR_NOT_FOUND)
        {
            return err;
        }
        //等待一段时间再继续
        vTaskDelay(pdMS_TO_TICKS(ASR_POLL_INTERVAL_MS));
    }
    //如果循环结束（超时🔥）
    set_unavailable_reason("Gladia transcription timed out");
    return ESP_ERR_TIMEOUT;
}

// 初始化
esp_err_t asr_client_init(void)
{
    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_ASR, NVS_READONLY, &nvs) == ESP_OK)
    {
        size_t len;

        len = sizeof(s_api_key);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_ASR_API_KEY, s_api_key, &len) != ESP_OK)
        {
            safe_copy(s_api_key, sizeof(s_api_key), MIMI_SECRET_ASR_API_KEY);
        }

        len = sizeof(s_provider);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_ASR_PROVIDER, s_provider, &len) != ESP_OK)
        {
            safe_copy(s_provider, sizeof(s_provider), MIMI_SECRET_ASR_PROVIDER);
        }

        len = sizeof(s_model);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_ASR_MODEL, s_model, &len) != ESP_OK)
        {
            safe_copy(s_model, sizeof(s_model), MIMI_SECRET_ASR_MODEL);
        }

        len = sizeof(s_base_url);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_ASR_BASE_URL, s_base_url, &len) != ESP_OK)
        {
            safe_copy(s_base_url, sizeof(s_base_url), MIMI_SECRET_ASR_BASE_URL);
        }

        nvs_close(nvs);
    }

    refresh_config_state();
    ESP_LOGI(TAG, "ASR initialized (provider=%s, configured=%s)",
             s_provider[0] ? s_provider : "(empty)",
             asr_client_is_configured() ? "yes" : "no");
    return ESP_OK;
}

// 判断是否配置完成
bool asr_client_is_configured(void)
{
    return provider_is_gladia() && s_api_key[0] != '\0' && s_base_url[0] != '\0';
}

// 转写文件（主函数！）
esp_err_t asr_client_transcribe_file(const char *file_path, char *out_text, size_t out_size)
{
    if (out_text && out_size > 0)
    {
        out_text[0] = '\0';
    }

    if (!file_path || !file_path[0])
    {
        set_unavailable_reason("ASR request missing audio file");
        return ESP_ERR_INVALID_ARG;
    }

    if (!provider_is_gladia())
    {
        set_unavailable_reason("Only gladia provider is supported currently");
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!asr_client_is_configured())
    {
        set_unavailable_reason("Gladia config missing; set ASR key and base URL first");
        return ESP_ERR_INVALID_STATE;
    }

    char audio_url[384] = {0};
    esp_err_t err = gladia_upload_file(file_path, audio_url, sizeof(audio_url));
    if (err != ESP_OK)
    {
        return err;
    }

    char job_id[128] = {0};
    err = gladia_create_job(audio_url, job_id, sizeof(job_id));
    if (err != ESP_OK)
    {
        return err;
    }

    return gladia_wait_for_result(job_id, out_text, out_size);
}

// 设置配置（密钥 / URL / 模型）
esp_err_t asr_client_set_api_key(const char *api_key)
{
    safe_copy(s_api_key, sizeof(s_api_key), api_key);
    refresh_config_state();
    return asr_save_str(MIMI_NVS_KEY_ASR_API_KEY, s_api_key);
}

esp_err_t asr_client_set_provider(const char *provider)
{
    safe_copy(s_provider, sizeof(s_provider), provider);
    refresh_config_state();
    return asr_save_str(MIMI_NVS_KEY_ASR_PROVIDER, s_provider);
}

esp_err_t asr_client_set_model(const char *model)
{
    safe_copy(s_model, sizeof(s_model), model);
    refresh_config_state();
    return asr_save_str(MIMI_NVS_KEY_ASR_MODEL, s_model);
}

esp_err_t asr_client_set_base_url(const char *base_url)
{
    safe_copy(s_base_url, sizeof(s_base_url), base_url);
    refresh_config_state();
    return asr_save_str(MIMI_NVS_KEY_ASR_BASE_URL, s_base_url);
}

// 获取不可用原因
const char *asr_client_get_unavailable_reason(void)
{
    return s_unavailable_reason;
}
