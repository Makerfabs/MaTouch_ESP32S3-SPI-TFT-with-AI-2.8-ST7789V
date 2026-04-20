#include "tool_registry.h"
#include "mimi_config.h"
#include "tools/tool_web_search.h"
#include "tools/tool_get_time.h"
#include "tools/tool_files.h"
#include "tools/tool_cron.h"
#include "tools/tool_gpio.h"
#include "tools/tool_ws2812.h"
#include "tools/tool_dht11.h"
#include "tools/tool_screen.h"
#include "tools/tool_display.h"

#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "tools";

#define MAX_TOOLS 20

static mimi_tool_t s_tools[MAX_TOOLS];
static int s_tool_count = 0;
static char *s_tools_json = NULL; /* cached JSON array string */

static void register_tool(const mimi_tool_t *tool)
{
    if (s_tool_count >= MAX_TOOLS)
    {
        ESP_LOGE(TAG, "Tool registry full");
        return;
    }
    s_tools[s_tool_count++] = *tool;
    ESP_LOGI(TAG, "Registered tool: %s", tool->name);
}

static void build_tools_json(void)
{
    cJSON *arr = cJSON_CreateArray();

    for (int i = 0; i < s_tool_count; i++)
    {
        cJSON *tool = cJSON_CreateObject();
        cJSON_AddStringToObject(tool, "name", s_tools[i].name);
        cJSON_AddStringToObject(tool, "description", s_tools[i].description);

        cJSON *schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema)
        {
            cJSON_AddItemToObject(tool, "input_schema", schema);
        }

        cJSON_AddItemToArray(arr, tool);
    }

    free(s_tools_json);
    s_tools_json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    ESP_LOGI(TAG, "Tools JSON built (%d tools)", s_tool_count);
}

esp_err_t tool_registry_init(void)
{
    s_tool_count = 0;

    /* Register web_search */
    tool_web_search_init();

    mimi_tool_t ws = {
        .name = "web_search",
        .description = "Search the web for current information via Tavily (preferred) or Brave when configured.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"query\":{\"type\":\"string\",\"description\":\"The search query\"}},"
            "\"required\":[\"query\"]}",
        .execute = tool_web_search_execute,
    };
    register_tool(&ws);

    /* Register get_current_time */
    mimi_tool_t gt = {
        .name = "get_current_time",
        .description = "Get the current date and time. Also sets the system clock. Call this when you need to know what time or date it is.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_get_time_execute,
    };
    register_tool(&gt);

    /* Register read_file */
    mimi_tool_t rf = {
        .name = "read_file",
        .description = "Read a file from SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"}},"
            "\"required\":[\"path\"]}",
        .execute = tool_read_file_execute,
    };
    register_tool(&rf);

    /* Register write_file */
    mimi_tool_t wf = {
        .name = "write_file",
        .description = "Write or overwrite a file on SPIFFS storage. Path must start with " MIMI_SPIFFS_BASE "/.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"content\":{\"type\":\"string\",\"description\":\"File content to write\"}},"
            "\"required\":[\"path\",\"content\"]}",
        .execute = tool_write_file_execute,
    };
    register_tool(&wf);

    /* Register edit_file */
    mimi_tool_t ef = {
        .name = "edit_file",
        .description = "Find and replace text in a file on SPIFFS. Replaces first occurrence of old_string with new_string.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"path\":{\"type\":\"string\",\"description\":\"Absolute path starting with " MIMI_SPIFFS_BASE "/\"},"
            "\"old_string\":{\"type\":\"string\",\"description\":\"Text to find\"},"
            "\"new_string\":{\"type\":\"string\",\"description\":\"Replacement text\"}},"
            "\"required\":[\"path\",\"old_string\",\"new_string\"]}",
        .execute = tool_edit_file_execute,
    };
    register_tool(&ef);

    /* Register list_dir */
    mimi_tool_t ld = {
        .name = "list_dir",
        .description = "List files on SPIFFS storage, optionally filtered by path prefix.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"prefix\":{\"type\":\"string\",\"description\":\"Optional path prefix filter, e.g. " MIMI_SPIFFS_BASE "/memory/\"}},"
            "\"required\":[]}",
        .execute = tool_list_dir_execute,
    };
    register_tool(&ld);

    /* Register cron_add */
    mimi_tool_t ca = {
        .name = "cron_add",
        .description = "Schedule a recurring or one-shot task. The message will trigger an agent turn when the job fires.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"name\":{\"type\":\"string\",\"description\":\"Short name for the job\"},"
            "\"schedule_type\":{\"type\":\"string\",\"description\":\"'every' for recurring interval or 'at' for one-shot at a unix timestamp\"},"
            "\"interval_s\":{\"type\":\"integer\",\"description\":\"Interval in seconds (required for 'every')\"},"
            "\"at_epoch\":{\"type\":\"integer\",\"description\":\"Unix timestamp to fire at (required for 'at')\"},"
            "\"message\":{\"type\":\"string\",\"description\":\"Message to inject when the job fires, triggering an agent turn\"},"
            "\"channel\":{\"type\":\"string\",\"description\":\"Optional reply channel (e.g. 'telegram'). If omitted, current turn channel is used when available\"},"
            "\"chat_id\":{\"type\":\"string\",\"description\":\"Optional reply chat_id. Required when channel='telegram'. If omitted during a Telegram turn, current chat_id is used\"}"
            "},"
            "\"required\":[\"name\",\"schedule_type\",\"message\"]}",
        .execute = tool_cron_add_execute,
    };
    register_tool(&ca);

    /* Register cron_list */
    mimi_tool_t cl = {
        .name = "cron_list",
        .description = "List all scheduled cron jobs with their status, schedule, and IDs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_cron_list_execute,
    };
    register_tool(&cl);

    /* Register cron_remove */
    mimi_tool_t cr = {
        .name = "cron_remove",
        .description = "Remove a scheduled cron job by its ID.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"job_id\":{\"type\":\"string\",\"description\":\"The 8-character job ID to remove\"}},"
            "\"required\":[\"job_id\"]}",
        .execute = tool_cron_remove_execute,
    };
    register_tool(&cr);

    /* Register GPIO tools */
    tool_gpio_init();

    mimi_tool_t gw = {
        .name = "gpio_write",
        .description = "Set a GPIO pin HIGH or LOW. Controls LEDs, relays, and other digital outputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"},"
            "\"state\":{\"type\":\"integer\",\"description\":\"1 for HIGH, 0 for LOW\"}},"
            "\"required\":[\"pin\",\"state\"]}",
        .execute = tool_gpio_write_execute,
    };
    register_tool(&gw);

    mimi_tool_t gr = {
        .name = "gpio_read",
        .description = "Read a GPIO pin state. Returns HIGH or LOW. Use for checking switches, sensors, and digital inputs.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{\"pin\":{\"type\":\"integer\",\"description\":\"GPIO pin number\"}},"
            "\"required\":[\"pin\"]}",
        .execute = tool_gpio_read_execute,
    };
    register_tool(&gr);

    mimi_tool_t ga = {
        .name = "gpio_read_all",
        .description = "Read all allowed GPIO pin states in a single call. Returns each pin's HIGH/LOW state.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_gpio_read_all_execute,
    };
    register_tool(&ga);

    /* Register WS2812 LED strip */
    tool_ws2812_init();

    mimi_tool_t ws2812 = {
        .name = "ws2812_set",
        .description = "Control WS2812B LED strip: turn on/off, set brightness (0-100), set RGB color (0-255).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"enable\":{\"type\":\"boolean\",\"description\":\"Turn LED on/off\"},"
            "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100,\"description\":\"Brightness 0-100%\"},"
            "\"r\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"Red 0-255\"},"
            "\"g\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"Green 0-255\"},"
            "\"b\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":255,\"description\":\"Blue 0-255\"}"
            "},"
            "\"required\":[\"enable\"]}",
        .execute = tool_ws2812_set_execute,
    };
    register_tool(&ws2812);

    mimi_tool_t ws2812_get = {
        .name = "ws2812_get_status",
        .description = "Get current WS2812 LED status: on/off state, brightness (0-100), RGB color (0-255).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_ws2812_get_status,
    };
    register_tool(&ws2812_get);

    /* Register DHT11 Temperature & Humidity Sensor */
    tool_dht11_init();

    mimi_tool_t dht11 = {
        .name = "dht11_read",
        .description = "Read temperature and humidity from DHT11 sensor. Returns temperature (C) and humidity (%RH).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_dht11_read_execute,
    };
    register_tool(&dht11);

    /* Register ST7789 Screen Backlight */
    tool_screen_bl_init();

    mimi_tool_t screen_bl_set = {
        .name = "screen_backlight_set",
        .description = "Control ST7789 display backlight: turn on/off, set brightness (0-100%).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"enable\":{\"type\":\"boolean\",\"description\":\"Turn backlight on/off\"},"
            "\"brightness\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":100,\"description\":\"Brightness 0-100%\"}"
            "},"
            "\"required\":[\"enable\"]}",
        .execute = tool_screen_bl_set_execute,
    };
    register_tool(&screen_bl_set);

    mimi_tool_t screen_bl_get = {
        .name = "screen_backlight_get_status",
        .description = "Get current ST7789 display backlight status: on/off state and brightness (0-100%).",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{},"
            "\"required\":[]}",
        .execute = tool_screen_bl_get_status,
    };
    register_tool(&screen_bl_get);

    // mimi_tool_t show_peach = {
    //     .name = "show_peach",
    //     .description = "Display the built-in peach image on the ST7789 screen.",
    //     .input_schema_json =
    //         "{\"type\":\"object\","
    //         "\"properties\":{},"
    //         "\"required\":[]}",
    //     .execute = tool_display_show_peach_execute,
    // };
    // register_tool(&show_peach);

    mimi_tool_t show_image = {
        .name = "show_image",
        .description = "Display an image from SPIFFS on the ST7789 screen. Supports PNG, JPG, and JPEG. The image is decoded and converted to RGB565 before rendering.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"path\":{\"type\":\"string\",\"description\":\"Absolute image path starting with " MIMI_SPIFFS_BASE "/ and ending with .png/.jpg/.jpeg\"}"
            "},"
            "\"required\":[\"path\"]}",
        .execute = tool_display_show_image_execute,
    };
    register_tool(&show_image);

    mimi_tool_t show_web_image = {
        .name = "show_web_image",
        .description = "Search the web for a matching image, download a 240x320 JPG to SPIFFS, and display it on the ST7789 screen. Use this when the user asks to find an image by meaning and show it on screen.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"query\":{\"type\":\"string\",\"description\":\"Semantic image query, for example: orange cat, sunset mountain, cyberpunk city\"},"
            "\"width\":{\"type\":\"integer\",\"description\":\"Target output width in pixels. Defaults to 240\"},"
            "\"height\":{\"type\":\"integer\",\"description\":\"Target output height in pixels. Defaults to 320\"}"
            "},"
            "\"required\":[\"query\"]}",
        .execute = tool_display_show_web_image_execute,
    };
    register_tool(&show_web_image);

    mimi_tool_t generate_image = {
        .name = "generate_image",
        .description = "Generate an image from text using the configured image API, download the final JPG to SPIFFS, and display it on the ST7789 screen. Use this when the user asks to create or draw a new image from a description.",
        .input_schema_json =
            "{\"type\":\"object\","
            "\"properties\":{"
            "\"prompt\":{\"type\":\"string\",\"description\":\"Image generation prompt, for example: a cat astronaut, cyberpunk city at night, watercolor peach blossom\"},"
            "\"width\":{\"type\":\"integer\",\"description\":\"Target output width in pixels. Defaults to 240\"},"
            "\"height\":{\"type\":\"integer\",\"description\":\"Target output height in pixels. Defaults to 320\"}"
            "},"
            "\"required\":[\"prompt\"]}",
        .execute = tool_display_generate_image_execute,
    };
    register_tool(&generate_image);

    build_tools_json();

    ESP_LOGI(TAG, "Tool registry initialized");
    return ESP_OK;
}

const char *tool_registry_get_tools_json(void)
{
    return s_tools_json;
}

esp_err_t tool_registry_execute(const char *name, const char *input_json,
                                char *output, size_t output_size)
{
    for (int i = 0; i < s_tool_count; i++)
    {
        if (strcmp(s_tools[i].name, name) == 0)
        {
            ESP_LOGI(TAG, "Executing tool: %s", name);
            return s_tools[i].execute(input_json, output, output_size);
        }
    }

    ESP_LOGW(TAG, "Unknown tool: %s", name);
    snprintf(output, output_size, "Error: unknown tool '%s'", name);
    return ESP_ERR_NOT_FOUND;
}
