#include "tools/tool_display.h"
#include "lvgl_lcd/lvgl_lcd.h"
#include "mimi_config.h"
#include "proxy/http_proxy.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "nvs.h"

static const char *TAG = "tool_display";
static char s_image_search_key[128] = {0};
//生成图像
static char s_image_generation_key[128] = {0};
static char s_image_generation_model[128] = {0};

static void log_heap_state(const char *stage)
{
    ESP_LOGI(TAG,
             "[heap] %s: internal_free=%u internal_largest=%u spiram_free=%u spiram_largest=%u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} response_buf_t;

typedef struct {
    FILE *file;
    size_t written;
    size_t max_bytes;
    bool overflow;
    char content_type[64];
} download_ctx_t;

static bool parse_https_url(const char *url, char *host, size_t host_size, char *path_out, size_t path_size);

//去查找flash的路径
static bool is_supported_image_path(const char *path)
{
    if (!path) {
        return false;
    }

    size_t base_len = strlen(MIMI_SPIFFS_BASE);
    if (strncmp(path, MIMI_SPIFFS_BASE, base_len) != 0) {
        return false;
    }
    if (base_len > 0 && MIMI_SPIFFS_BASE[base_len - 1] != '/') {
        if (path[base_len] != '/') {
            return false;
        }
    }
    if (strstr(path, "..") != NULL) {
        return false;
    }

    const char *ext = strrchr(path, '.');
    if (!ext) {
        return false;
    }

    return strcasecmp(ext, ".png") == 0 || strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0;
}

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;

    for (; *src && pos < dst_size - 4; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = (char)c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

static esp_err_t ensure_parent_dir(const char *path)
{
    char dir_path[128];
    const char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        return ESP_OK;
    }

    size_t len = (size_t)(slash - path);
    if (len >= sizeof(dir_path)) {
        return ESP_ERR_INVALID_ARG;
    }

    memcpy(dir_path, path, len);
    dir_path[len] = '\0';

    if (strcmp(dir_path, MIMI_SPIFFS_BASE) == 0) {
        return ESP_OK;
    }

    struct stat st;
    if (stat(dir_path, &st) == 0) {
        return ESP_OK;
    }

    if (mkdir(dir_path, 0777) == 0 || errno == EEXIST) {
        return ESP_OK;
    }

    ESP_LOGE(TAG, "Failed to create dir %s (errno=%d)", dir_path, errno);
    return ESP_FAIL;
}

static void load_image_search_key(void)
{
    if (s_image_search_key[0] != '\0') {
        return;
    }

    if (MIMI_SECRET_IMAGE_SEARCH_KEY[0] != '\0') {
        strncpy(s_image_search_key, MIMI_SECRET_IMAGE_SEARCH_KEY, sizeof(s_image_search_key) - 1);
        return;
    }

    if (MIMI_SECRET_TAVILY_KEY[0] != '\0') {
        strncpy(s_image_search_key, MIMI_SECRET_TAVILY_KEY, sizeof(s_image_search_key) - 1);
    }

    nvs_handle_t nvs;
    if (nvs_open(MIMI_NVS_SEARCH, NVS_READONLY, &nvs) == ESP_OK) {
        char tmp[128] = {0};
        size_t len = sizeof(tmp);
        if (nvs_get_str(nvs, MIMI_NVS_KEY_TAVILY_KEY, tmp, &len) == ESP_OK && tmp[0]) {
            strncpy(s_image_search_key, tmp, sizeof(s_image_search_key) - 1);
        }
        nvs_close(nvs);
    }
}

// 生成图像
// 只在第一次调用时，把密钥复制到全局变量里，后续直接跳过，不重复复制。
static void load_image_generation_key(void)
{
    if (s_image_generation_key[0] != '\0') {
        return;
    }

    if (MIMI_SECRET_IMAGE_GENERATION_KEY[0] != '\0') {
        strncpy(s_image_generation_key, MIMI_SECRET_IMAGE_GENERATION_KEY, sizeof(s_image_generation_key) - 1);
    }
}

// 生成图像
// 加载图像生成模型
static void load_image_generation_model(void)
{
    if (s_image_generation_model[0] != '\0') {
        return;
    }

    if (MIMI_SECRET_IMAGE_GENERATION_MODEL[0] != '\0') {
        strncpy(s_image_generation_model, MIMI_SECRET_IMAGE_GENERATION_MODEL, sizeof(s_image_generation_model) - 1);
        return;
    }

    if (MIMI_IMAGE_GENERATION_DEFAULT_MODEL[0] != '\0') {
        strncpy(s_image_generation_model, MIMI_IMAGE_GENERATION_DEFAULT_MODEL, sizeof(s_image_generation_model) - 1);
    }
}

static esp_err_t response_event_handler(esp_http_client_event_t *evt)
{
    response_buf_t *buf = (response_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && buf && evt->data_len > 0) {
        size_t needed = buf->len + evt->data_len;
        if (needed < buf->cap) {
            memcpy(buf->data + buf->len, evt->data, evt->data_len);
            buf->len += evt->data_len;
            buf->data[buf->len] = '\0';
        }
    }
    return ESP_OK;
}

static esp_err_t download_event_handler(esp_http_client_event_t *evt)
{
    download_ctx_t *ctx = (download_ctx_t *)evt->user_data;
    if (!ctx) {
        return ESP_OK;
    }

    if (evt->event_id == HTTP_EVENT_ON_HEADER && evt->header_key && evt->header_value &&
        strcasecmp(evt->header_key, "Content-Type") == 0) {
        snprintf(ctx->content_type, sizeof(ctx->content_type), "%s", evt->header_value);
    }

    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->data_len > 0) {
        if (ctx->overflow || !ctx->file) {
            return ESP_FAIL;
        }
        if (ctx->written + evt->data_len > ctx->max_bytes) {
            ctx->overflow = true;
            return ESP_FAIL;
        }
        size_t written = fwrite(evt->data, 1, evt->data_len, ctx->file);
        if (written != (size_t)evt->data_len) {
            return ESP_FAIL;
        }
        ctx->written += written;
    }
    return ESP_OK;
}

static char *build_pexels_search_url(const char *query, int width, int height)
{
    (void)width;
    (void)height;

    char encoded_query[256];
    url_encode(query, encoded_query, sizeof(encoded_query));

    char *url = heap_caps_calloc(1, 768, MALLOC_CAP_SPIRAM);
    if (!url) {
        return NULL;
    }

    snprintf(url, 768,
             MIMI_IMAGE_SEARCH_API_URL
             "?query=%s&per_page=1&orientation=portrait&size=medium&locale=zh-CN",
             encoded_query);
    return url;
}

static const char *get_first_string_field(cJSON *item, const char *field_names[], size_t count)
{
    if (!item) {
        return NULL;
    }
    for (size_t i = 0; i < count; i++) {
        cJSON *field = cJSON_GetObjectItem(item, field_names[i]);
        if (cJSON_IsString(field) && field->valuestring[0]) {
            return field->valuestring;
        }
    }
    return NULL;
}

static char *duplicate_string_psram(const char *value)
{
    size_t len = strlen(value) + 1;
    char *copy = heap_caps_calloc(1, len, MALLOC_CAP_SPIRAM);
    if (!copy) {
        return NULL;
    }
    memcpy(copy, value, len);
    return copy;
}

static char *json_escape_psram(const char *value)
{
    if (!value) {
        return NULL;
    }

    size_t len = strlen(value);
    size_t cap = len * 2 + 1;
    char *escaped = heap_caps_calloc(1, cap, MALLOC_CAP_SPIRAM);
    if (!escaped) {
        return NULL;
    }

    size_t out = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)value[i];
        switch (ch) {
        case '\\':
        case '"':
            escaped[out++] = '\\';
            escaped[out++] = (char)ch;
            break;
        case '\n':
            escaped[out++] = '\\';
            escaped[out++] = 'n';
            break;
        case '\r':
            escaped[out++] = '\\';
            escaped[out++] = 'r';
            break;
        case '\t':
            escaped[out++] = '\\';
            escaped[out++] = 't';
            break;
        default:
            escaped[out++] = (char)ch;
            break;
        }
    }
    escaped[out] = '\0';
    return escaped;
}

static char *extract_json_https_url_psram(const char *json)
{
    static const char *patterns[] = {
        "\"image_url\":\"https://",
        "\"url\":\"https://",
        "\"jpg_url\":\"https://"
    };

    if (!json) {
        return NULL;
    }

    const char *start = NULL;
    for (size_t i = 0; i < (sizeof(patterns) / sizeof(patterns[0])); i++) {
        const char *found = strstr(json, patterns[i]);
        if (found) {
            start = found + strlen(patterns[i]) - strlen("https://");
            break;
        }
    }
    if (!start) {
        return NULL;
    }

    const char *end = start;
    while (*end) {
        if (*end == '"' && end > start && end[-1] != '\\') {
            break;
        }
        end++;
    }
    if (end == start || *end != '"') {
        return NULL;
    }

    size_t len = (size_t)(end - start);
    char *url = heap_caps_calloc(1, len + 1, MALLOC_CAP_SPIRAM);
    if (!url) {
        return NULL;
    }
    memcpy(url, start, len);
    url[len] = '\0';
    return url;
}

static char *dup_image_url_with_size(const char *candidate, int width, int height)
{
    const char *https_prefix = "https://";
    const char *http_prefix = "http://";
    const char *source = candidate;
    if (strncmp(candidate, https_prefix, strlen(https_prefix)) == 0) {
        source = candidate + strlen(https_prefix);
    } else if (strncmp(candidate, http_prefix, strlen(http_prefix)) == 0) {
        source = candidate + strlen(http_prefix);
    }

    size_t encoded_cap = strlen(source) * 3 + 1;
    char *encoded_source = heap_caps_calloc(1, encoded_cap, MALLOC_CAP_SPIRAM);
    if (!encoded_source) {
        return NULL;
    }
    url_encode(source, encoded_source, encoded_cap);

    size_t len = strlen(MIMI_IMAGE_TRANSCODE_API_URL) + strlen(encoded_source) + 128;
    char *final_url = heap_caps_calloc(1, len, MALLOC_CAP_SPIRAM);
    if (!final_url) {
        free(encoded_source);
        return NULL;
    }

    snprintf(final_url, len,
             MIMI_IMAGE_TRANSCODE_API_URL
             "?url=%s&output=jpg&w=%d&h=%d&fit=cover&we&filename=web_image.jpg",
             encoded_source, width, height);
    free(encoded_source);
    return final_url;
}


static char *extract_best_image_url(const char *json, int width, int height)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return NULL;
    }

    cJSON *photos = cJSON_GetObjectItem(root, "photos");
    if (!photos || !cJSON_IsArray(photos) || cJSON_GetArraySize(photos) == 0) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *photo = cJSON_GetArrayItem(photos, 0);
    if (!photo) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON *src = cJSON_GetObjectItem(photo, "src");
    if (!src) {
        cJSON_Delete(root);
        return NULL;
    }

    static const char *fields[] = {
        "portrait", "large2x", "large", "medium", "original"
    };
    const char *candidate = get_first_string_field(src, fields, sizeof(fields) / sizeof(fields[0]));
    if (!candidate || strncmp(candidate, "https://", 8) != 0) {
        cJSON_Delete(root);
        return NULL;
    }

    char *final_url = dup_image_url_with_size(candidate, width, height);
    cJSON_Delete(root);
    return final_url;
}

static esp_err_t pexels_search_direct(const char *query, int width, int height, response_buf_t *buf)
{
    char *url = build_pexels_search_url(query, width, height);
    if (!url) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = response_event_handler,
        .user_data = buf,
        .timeout_ms = MIMI_WEB_IMAGE_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    free(url);
    if (!client) {
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Authorization", s_image_search_key);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        return err;
    }
    return status == 200 ? ESP_OK : ESP_FAIL;
}

static esp_err_t pexels_search_via_proxy(const char *query, int width, int height, response_buf_t *buf)
{
    char encoded_query[256];
    url_encode(query, encoded_query, sizeof(encoded_query));

    proxy_conn_t *conn = proxy_conn_open("api.pexels.com", 443, MIMI_WEB_IMAGE_HTTP_TIMEOUT_MS);
    if (!conn) {
        return ESP_ERR_HTTP_CONNECT;
    }

    char path[512];
    snprintf(path, sizeof(path),
        "/v1/search?query=%s&per_page=1&orientation=portrait&size=medium&locale=zh-CN",
        encoded_query);

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: api.pexels.com\r\n"
        "Accept: application/json\r\n"
        "Authorization: %s\r\n"
        "Connection: close\r\n\r\n",
        path, s_image_search_key);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), MIMI_WEB_IMAGE_HTTP_TIMEOUT_MS);
        if (n <= 0) {
            break;
        }
        size_t copy = (total + n < buf->cap - 1) ? (size_t)n : (buf->cap - 1 - total);
        if (copy > 0) {
            memcpy(buf->data + total, tmp, copy);
            total += copy;
        }
    }
    proxy_conn_close(conn);

    buf->len = total;
    buf->data[buf->len] = '\0';

    int status = 0;
    if (total > 5 && strncmp(buf->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(buf->data, ' ');
        if (sp) {
            status = atoi(sp + 1);
        }
    }

    char *body = strstr(buf->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t body_len = total - (size_t)(body - buf->data);
        memmove(buf->data, body, body_len);
        buf->len = body_len;
        buf->data[buf->len] = '\0';
    }

    return status == 200 ? ESP_OK : ESP_FAIL;
}

static esp_err_t search_web_image_url(const char *query, int width, int height, char **image_url_out)
{
    load_image_search_key();
    if (s_image_search_key[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    response_buf_t buf = {0};
    buf.cap = 16 * 1024;
    buf.data = heap_caps_calloc(1, buf.cap, MALLOC_CAP_SPIRAM);
    if (!buf.data) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = http_proxy_is_enabled()
        ? pexels_search_via_proxy(query, width, height, &buf)
        : pexels_search_direct(query, width, height, &buf);
    if (err != ESP_OK) {
        free(buf.data);
        return err;
    }

    *image_url_out = extract_best_image_url(buf.data, width, height);
    free(buf.data);
    if (!*image_url_out) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}

static bool content_type_is_jpeg(const char *content_type)
{
    if (!content_type) {
        return false;
    }
    return strstr(content_type, "image/jpeg") != NULL || strstr(content_type, "image/jpg") != NULL;
}

static bool file_has_jpeg_magic(const char *path)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return false;
    }

    unsigned char header[3] = {0};
    size_t n = fread(header, 1, sizeof(header), fp);
    fclose(fp);
    return n >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF;
}

static bool parse_https_url(const char *url, char *host, size_t host_size, char *path_out, size_t path_size)
{
    const char *prefix = "https://";
    size_t prefix_len = strlen(prefix);
    if (strncmp(url, prefix, prefix_len) != 0) {
        return false;
    }

    const char *host_start = url + prefix_len;
    const char *path_start = strchr(host_start, '/');
    size_t host_len = path_start ? (size_t)(path_start - host_start) : strlen(host_start);
    if (host_len == 0 || host_len >= host_size) {
        return false;
    }

    memcpy(host, host_start, host_len);
    host[host_len] = '\0';
    snprintf(path_out, path_size, "%s", path_start ? path_start : "/");
    return true;
}

static esp_err_t download_image_via_proxy(const char *url, const char *path)
{
    char host[128];
    char request_path[768];
    if (!parse_https_url(url, host, sizeof(host), request_path, sizeof(request_path))) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ensure_parent_dir(path);
    if (err != ESP_OK) {
        return err;
    }

    remove(path);
    FILE *file = fopen(path, "wb");
    if (!file) {
        return ESP_FAIL;
    }

    proxy_conn_t *conn = proxy_conn_open(host, 443, MIMI_WEB_IMAGE_HTTP_TIMEOUT_MS);
    if (!conn) {
        fclose(file);
        remove(path);
        return ESP_ERR_HTTP_CONNECT;
    }

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: image/jpeg,image/*;q=0.9,*/*;q=0.8\r\n"
        "Connection: close\r\n\r\n",
        request_path, host);

    if (proxy_conn_write(conn, header, hlen) < 0) {
        proxy_conn_close(conn);
        fclose(file);
        remove(path);
        return ESP_ERR_HTTP_WRITE_DATA;
    }

    char buf[4096];
    size_t total_written = 0;
    bool header_done = false;
    int status = 0;
    char content_type[64] = {0};
    char header_buf[4096] = {0};
    size_t header_len = 0;

    while (1) {
        int n = proxy_conn_read(conn, buf, sizeof(buf), MIMI_WEB_IMAGE_HTTP_TIMEOUT_MS);
        if (n <= 0) {
            break;
        }

        const char *data = buf;
        size_t data_len = (size_t)n;

        if (!header_done) {
            size_t copy = data_len;
            if (header_len + copy >= sizeof(header_buf)) {
                copy = sizeof(header_buf) - 1 - header_len;
            }
            memcpy(header_buf + header_len, data, copy);
            header_len += copy;
            header_buf[header_len] = '\0';

            char *header_end = strstr(header_buf, "\r\n\r\n");
            if (!header_end) {
                continue;
            }

            header_done = true;
            char *status_pos = strchr(header_buf, ' ');
            if (status_pos) {
                status = atoi(status_pos + 1);
            }
            char *ct = strcasestr(header_buf, "\r\nContent-Type: ");
            if (ct) {
                ct += 16;
                char *ct_end = strstr(ct, "\r\n");
                if (ct_end) {
                    size_t ct_len = (size_t)(ct_end - ct);
                    if (ct_len >= sizeof(content_type)) {
                        ct_len = sizeof(content_type) - 1;
                    }
                    memcpy(content_type, ct, ct_len);
                    content_type[ct_len] = '\0';
                }
            }

            const char *body = header_end + 4;
            data_len = header_len - (size_t)(body - header_buf);
            data = body;
        }

        if (data_len == 0) {
            continue;
        }
        if (total_written + data_len > MIMI_WEB_IMAGE_MAX_DOWNLOAD_BYTES) {
            proxy_conn_close(conn);
            fclose(file);
            remove(path);
            return ESP_ERR_NO_MEM;
        }
        if (fwrite(data, 1, data_len, file) != data_len) {
            proxy_conn_close(conn);
            fclose(file);
            remove(path);
            return ESP_FAIL;
        }
        total_written += data_len;
    }

    proxy_conn_close(conn);
    fclose(file);

    if (status != 200 || total_written == 0) {
        remove(path);
        return ESP_FAIL;
    }
    if (!content_type_is_jpeg(content_type) && !strstr(url, ".jpg") && !strstr(url, ".jpeg")) {
        remove(path);
        return ESP_FAIL;
    }
    if (!file_has_jpeg_magic(path)) {
        ESP_LOGE(TAG, "Downloaded file is not a baseline JPEG: %s", path);
        remove(path);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t download_image_direct(const char *url, const char *path)
{
    esp_err_t err = ensure_parent_dir(path);
    if (err != ESP_OK) {
        return err;
    }

    remove(path);

    FILE *file = fopen(path, "wb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open temp image path: %s", path);
        return ESP_FAIL;
    }

    download_ctx_t ctx = {
        .file = file,
        .written = 0,
        .max_bytes = MIMI_WEB_IMAGE_MAX_DOWNLOAD_BYTES,
        .overflow = false,
    };

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = download_event_handler,
        .user_data = &ctx,
        .timeout_ms = MIMI_WEB_IMAGE_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        fclose(file);
        remove(path);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Accept", "image/jpeg,image/*;q=0.9,*/*;q=0.8");

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    fclose(file);

    if (err != ESP_OK || status != 200 || ctx.overflow) {
        remove(path);
        return err != ESP_OK ? err : ESP_FAIL;
    }

    if (!content_type_is_jpeg(ctx.content_type) && !strstr(url, ".jpg") && !strstr(url, ".jpeg")) {
        remove(path);
        return ESP_FAIL;
    }

    if (ctx.written == 0) {
        remove(path);
        return ESP_FAIL;
    }

    if (!file_has_jpeg_magic(path)) {
        ESP_LOGE(TAG, "Downloaded file is not a baseline JPEG: %s", path);
        remove(path);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t download_image_to_spiffs(const char *url, const char *path)
{
    return http_proxy_is_enabled()
        ? download_image_via_proxy(url, path)
        : download_image_direct(url, path);
}

static esp_err_t download_and_display_image(const char *image_url, const char *path)
{
    esp_err_t err = download_image_to_spiffs(image_url, path);
    if (err != ESP_OK) {
        return err;
    }

    return bsp_display_show_image_file(path);
}
//生成图像
//把你输入的提示词、模型、尺寸等，打包成一段标准 JSON 格式数据，发给 AI 接口生成图片。
static char *build_image_generation_payload(const char *prompt, int width, int height)
{
    (void)width;
    (void)height;

    char *escaped_prompt = json_escape_psram(prompt);
    if (!escaped_prompt) {
        return NULL;
    }

    size_t len = strlen(s_image_generation_model) + strlen(escaped_prompt) + strlen(MIMI_IMAGE_GENERATION_DEFAULT_SIZE) + 96;
    char *payload = heap_caps_calloc(1, len, MALLOC_CAP_SPIRAM);
    if (!payload) {
        free(escaped_prompt);
        return NULL;
    }

    snprintf(payload, len,
             "{\"model\":\"%s\",\"prompt\":\"%s\",\"response_format\":\"url\",\"size\":\"%s\",\"stream\":false}",
             s_image_generation_model,
             escaped_prompt,
             MIMI_IMAGE_GENERATION_DEFAULT_SIZE);
    free(escaped_prompt);
    return payload;
}



//生成图像
//提取生成的图片 URL
static char *extract_generated_image_url(const char *json)
{
    return extract_json_https_url_psram(json);
}

//生成图像
//它拿着你之前打包好的画图请求，通过网络发给 AI 服务器，等待服务器画图，并把结果拿回来。
static esp_err_t generate_image_direct(const char *prompt, int width, int height, response_buf_t *buf)
{
    char *payload = build_image_generation_payload(prompt, width, height);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t config = {
        .url = MIMI_IMAGE_GENERATION_API_URL,
        .event_handler = response_event_handler,
        .user_data = buf,
        .timeout_ms = MIMI_IMAGE_GENERATION_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        free(payload);
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Content-Type", "application/json");
    if (s_image_generation_key[0] != '\0') {
        char auth[160];
        snprintf(auth, sizeof(auth), "Bearer %s", s_image_generation_key);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(payload);

    if (err != ESP_OK) {
        return err;
    }
    return status == 200 ? ESP_OK : ESP_FAIL;
}


//生成图像
static esp_err_t generate_image_via_proxy(const char *prompt, int width, int height, response_buf_t *buf)
{
    char *payload = build_image_generation_payload(prompt, width, height);
    if (!payload) {
        return ESP_ERR_NO_MEM;
    }

    char host[128];
    char path[256];
    if (!parse_https_url(MIMI_IMAGE_GENERATION_API_URL, host, sizeof(host), path, sizeof(path))) {
        free(payload);
        return ESP_ERR_INVALID_ARG;
    }

    proxy_conn_t *conn = proxy_conn_open(host, 443, MIMI_IMAGE_GENERATION_HTTP_TIMEOUT_MS);
    if (!conn) {
        free(payload);
        return ESP_ERR_HTTP_CONNECT;
    }

    char header[1024];
    int hlen;
    if (s_image_generation_key[0] != '\0') {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Accept: application/json\r\n"
            "Content-Type: application/json\r\n"
            "Authorization: Bearer %s\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            path, host, s_image_generation_key, (int)strlen(payload));
    } else {
        hlen = snprintf(header, sizeof(header),
            "POST %s HTTP/1.1\r\n"
            "Host: %s\r\n"
            "Accept: application/json\r\n"
            "Content-Type: application/json\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n\r\n",
            path, host, (int)strlen(payload));
    }

    if (proxy_conn_write(conn, header, hlen) < 0 ||
        proxy_conn_write(conn, payload, strlen(payload)) < 0) {
        free(payload);
        proxy_conn_close(conn);
        return ESP_ERR_HTTP_WRITE_DATA;
    }
    free(payload);

    char tmp[4096];
    size_t total = 0;
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), MIMI_IMAGE_GENERATION_HTTP_TIMEOUT_MS);
        if (n <= 0) {
            break;
        }
        size_t copy = (total + n < buf->cap - 1) ? (size_t)n : (buf->cap - 1 - total);
        if (copy > 0) {
            memcpy(buf->data + total, tmp, copy);
            total += copy;
        }
    }
    proxy_conn_close(conn);

    buf->len = total;
    buf->data[buf->len] = '\0';

    int status = 0;
    if (total > 5 && strncmp(buf->data, "HTTP/", 5) == 0) {
        const char *sp = strchr(buf->data, ' ');
        if (sp) {
            status = atoi(sp + 1);
        }
    }

    char *body = strstr(buf->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t body_len = total - (size_t)(body - buf->data);
        memmove(buf->data, body, body_len);
        buf->len = body_len;
        buf->data[buf->len] = '\0';
    }

    return status == 200 ? ESP_OK : ESP_FAIL;
}


//生成图像
//加载密钥 → 加载模型 → 发请求 → 拿图片链接，最终给你返回一个可用的图片 URL
static esp_err_t generate_image_url(const char *prompt, int width, int height, char **image_url_out)
{
    load_image_generation_key();
    load_image_generation_model();
    if (s_image_generation_model[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    response_buf_t buf = {0};
    buf.cap = 16 * 1024;
    buf.data = heap_caps_calloc(1, buf.cap, MALLOC_CAP_SPIRAM);
    if (!buf.data) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = http_proxy_is_enabled()
        ? generate_image_via_proxy(prompt, width, height, &buf)
        : generate_image_direct(prompt, width, height, &buf);
    if (err != ESP_OK) {
        free(buf.data);
        return err;
    }

    *image_url_out = extract_generated_image_url(buf.data);
    free(buf.data);
    if (!*image_url_out) {
        return ESP_ERR_NOT_FOUND;
    }

    return ESP_OK;
}



// esp_err_t tool_display_show_peach_execute(const char *input_json, char *output, size_t output_size)
// {
//     (void)input_json;

//     esp_err_t err = bsp_display_show_peach();
//     if (err != ESP_OK) {
//         snprintf(output, output_size, "Error: failed to show peach image (%s)", esp_err_to_name(err));
//         return err;
//     }

//     snprintf(output, output_size, "OK: peach image displayed");
//     return ESP_OK;
// }

//调用展示图片的工具
esp_err_t tool_display_show_image_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (!is_supported_image_path(path)) {
        snprintf(output, output_size,
                 "Error: path must start with %s/ and end with .png/.jpg/.jpeg",
                 MIMI_SPIFFS_BASE);
        cJSON_Delete(root);
        return ESP_ERR_INVALID_ARG;
    }

    //展示图片
    esp_err_t err = bsp_display_show_image_file(path);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to display image %s (%s)", path, esp_err_to_name(err));
        cJSON_Delete(root);
        return err;
    }

    ESP_LOGI(TAG, "Displayed image: %s", path);
    snprintf(output, output_size, "OK: displayed image %s", path);
    cJSON_Delete(root);
    return ESP_OK;
}

//展示网站下载的图片
esp_err_t tool_display_show_web_image_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *query_item = cJSON_GetObjectItem(root, "query");
    const char *query_value = cJSON_GetStringValue(query_item);
    if (!query_value || query_value[0] == '\0') {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing query");
        return ESP_ERR_INVALID_ARG;
    }

    char query[256];
    snprintf(query, sizeof(query), "%s", query_value);

    int width = MIMI_WEB_IMAGE_DEFAULT_WIDTH;
    int height = MIMI_WEB_IMAGE_DEFAULT_HEIGHT;
    cJSON *width_item = cJSON_GetObjectItem(root, "width");
    cJSON *height_item = cJSON_GetObjectItem(root, "height");
    if (cJSON_IsNumber(width_item) && width_item->valueint > 0) {
        width = width_item->valueint;
    }
    if (cJSON_IsNumber(height_item) && height_item->valueint > 0) {
        height = height_item->valueint;
    }
    cJSON_Delete(root);

    if (width > 1024 || height > 1024) {
        snprintf(output, output_size, "Error: width/height too large");
        return ESP_ERR_INVALID_ARG;
    }

    char *image_url = NULL;
    esp_err_t err = search_web_image_url(query, width, height, &image_url);
    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(output, output_size, "Error: no image search API key configured. Set MIMI_SECRET_IMAGE_SEARCH_KEY or MIMI_SECRET_SEARCH_KEY in mimi_secrets.h");
        return err;
    }
    if (err != ESP_OK || !image_url) {
        snprintf(output, output_size, "Error: failed to find a downloadable web image for '%s' (%s)", query, esp_err_to_name(err == ESP_OK ? ESP_FAIL : err));
        return err == ESP_OK ? ESP_FAIL : err;
    }

    err = download_and_display_image(image_url, MIMI_WEB_IMAGE_TEMP_PATH);
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: failed to download or display image for '%s' (%s)", query, esp_err_to_name(err));
        free(image_url);
        return err;
    }

    ESP_LOGI(TAG, "Displayed web image for query '%s': %s", query, image_url);
    snprintf(output, output_size, "OK: displayed web image for '%s' from %s", query, image_url);
    free(image_url);
    return ESP_OK;
}


//生成图像
esp_err_t tool_display_generate_image_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ESP_ERR_INVALID_ARG;
    }

    cJSON *prompt_item = cJSON_GetObjectItem(root, "prompt");
    const char *prompt_value = cJSON_GetStringValue(prompt_item);
    if (!prompt_value || prompt_value[0] == '\0') {
        cJSON_Delete(root);
        snprintf(output, output_size, "Error: missing prompt");
        return ESP_ERR_INVALID_ARG;
    }

    char prompt[256];
    snprintf(prompt, sizeof(prompt), "%s", prompt_value);

    int width = MIMI_IMAGE_GENERATION_DEFAULT_WIDTH;
    int height = MIMI_IMAGE_GENERATION_DEFAULT_HEIGHT;
    cJSON *width_item = cJSON_GetObjectItem(root, "width");
    cJSON *height_item = cJSON_GetObjectItem(root, "height");
    if (cJSON_IsNumber(width_item) && width_item->valueint > 0) {
        width = width_item->valueint;
    }
    if (cJSON_IsNumber(height_item) && height_item->valueint > 0) {
        height = height_item->valueint;
    }
    cJSON_Delete(root);

    if (width > 1024 || height > 1024) {
        snprintf(output, output_size, "Error: width/height too large");
        return ESP_ERR_INVALID_ARG;
    }

    log_heap_state("generate_image: before generate_image_url");

    char *image_url = NULL;
    esp_err_t err = generate_image_url(prompt, width, height, &image_url);
    log_heap_state("generate_image: after generate_image_url");
    if (err == ESP_ERR_INVALID_STATE) {
        snprintf(output, output_size, "Error: no image generation model configured. Set MIMI_SECRET_IMAGE_GENERATION_MODEL in mimi_secrets.h");
        return err;
    }
    if (err == ESP_ERR_NOT_FOUND) {
        snprintf(output, output_size, "Error: image generation API returned no image URL for '%s'", prompt);
        return err;
    }
    if (err != ESP_OK || !image_url) {
        snprintf(output, output_size, "Error: failed to generate image for '%s' (%s)", prompt, esp_err_to_name(err == ESP_OK ? ESP_FAIL : err));
        return err == ESP_OK ? ESP_FAIL : err;
    }

    char *display_url = dup_image_url_with_size(image_url, width, height);
    if (!display_url) {
        snprintf(output, output_size, "Error: failed to prepare generated image for display");
        free(image_url);
        return ESP_ERR_NO_MEM;
    }

    log_heap_state("generate_image: before download_and_display_image");
    err = download_and_display_image(display_url, MIMI_GENERATED_IMAGE_TEMP_PATH);
    log_heap_state("generate_image: after download_and_display_image");
    if (err != ESP_OK) {
        snprintf(output, output_size, "Error: generated image but failed to transcode, download, or display it (%s)", esp_err_to_name(err));
        free(display_url);
        free(image_url);
        return err;
    }

    ESP_LOGI(TAG, "Displayed generated image for prompt '%s': source=%s display=%s", prompt, image_url, display_url);
    snprintf(output, output_size, "OK: generated and displayed image for '%s'", prompt);
    free(display_url);
    free(image_url);
    return ESP_OK;
}
