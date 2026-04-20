#include "lvgl_lcd.h"
#include "peach_img.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "esp_heap_caps.h"

#include "../../managed_components/lvgl__lvgl/src/libs/lodepng/lodepng.h"
#include "../../managed_components/lvgl__lvgl/src/libs/tjpgd/tjpgd.h"

// ---- ST7789V SPI 显示屏 ----
#define LCD_SPI_HOST SPI2_HOST
#define LCD_PIXEL_CLOCK_HZ (40 * 1000 * 1000)

#define LCD_PIN_MOSI 13
#define LCD_PIN_SCLK 48
#define LCD_PIN_CS 40
#define LCD_PIN_DC 21
#define LCD_PIN_RST 18
#define LCD_PIN_BL 45

#define LCD_H_RES 240
#define LCD_V_RES 320
#define LCD_BIT_PER_PIXEL 16

// ---- LVGL 缓冲区 ----
#define LCD_BUF_LINES 10
#define LCD_BUF_SIZE (LCD_H_RES * LCD_BUF_LINES)
#define TJPGD_WORKBUFF_SIZE 4096

static const char *TAG = "LCD_INIT";

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

static lv_disp_t *s_disp = NULL;
static lv_obj_t *s_root_container = NULL;
static lv_image_dsc_t s_runtime_img = {0};
static uint8_t *s_runtime_img_buf = NULL;

typedef struct
{
    FILE *fp;
    uint16_t *rgb565;
    uint16_t width;
    uint16_t height;
} jpg_decode_ctx_t;

// rgb888_to_rgb565 颜色格式转换函数
static uint16_t rgb888_to_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// 释放动态加载的图片内存，防止内存泄漏、程序卡死。
static void release_runtime_image(void)
{
    if (s_runtime_img_buf)
    {
        free(s_runtime_img_buf);
        s_runtime_img_buf = NULL;
    }
    memset(&s_runtime_img, 0, sizeof(s_runtime_img));
}

// 删除 UI 界面的最底层容器（root container），清空整个屏幕界面。
static void display_clear_root(void)
{
    if (s_root_container)
    {
        lv_obj_del(s_root_container);
        s_root_container = NULL;
    }
}

// static void create_peach_scene(void)
// {
//     lv_obj_t *scr = lv_disp_get_scr_act(bsp_get_disp());
//     display_clear_root();

//     s_root_container = lv_obj_create(scr);
//     lv_obj_remove_style_all(s_root_container);
//     lv_obj_set_size(s_root_container, LCD_H_RES, LCD_V_RES);
//     lv_obj_set_style_bg_color(s_root_container, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
//     lv_obj_set_style_bg_opa(s_root_container, LV_OPA_COVER, LV_PART_MAIN);
//     lv_obj_center(s_root_container);

//     lv_obj_t *img = lv_image_create(s_root_container);
//     lv_image_set_src(img, &peach_img_240x320);
//     lv_obj_center(img);
// }

static void create_runtime_image_scene(const lv_image_dsc_t *img_dsc)
{
    // 获取当前 LCD 正在显示的“屏幕对象”
    lv_obj_t *scr = lv_disp_get_scr_act(bsp_get_disp());
    // 清空初始化界面的红绿蓝
    display_clear_root();

    // 在当前屏幕上创建一个容器
    s_root_container = lv_obj_create(scr);
    // 清掉 LVGL 默认样式
    lv_obj_remove_style_all(s_root_container);
    // 设置容器的大小为240*320
    lv_obj_set_size(s_root_container, LCD_H_RES, LCD_V_RES);
    // 背景色为黑色
    lv_obj_set_style_bg_color(s_root_container, lv_color_black(), LV_PART_MAIN);
    // 设置为完全不透明
    lv_obj_set_style_bg_opa(s_root_container, LV_OPA_COVER, LV_PART_MAIN);
    // 容器放中心
    lv_obj_center(s_root_container);

    // 在s_root_container容器里创建一个“图片控件”
    lv_obj_t *img = lv_image_create(s_root_container);
    // 让图片控件尺寸与屏幕一致
    lv_obj_set_size(img, LCD_H_RES, LCD_V_RES);
    // 设置图片的来源
    lv_image_set_src(img, img_dsc);
    // 拉伸铺满整个容器，确保显示为 240x320
    lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);
    // 图片放中间
    lv_obj_center(img);
}

static esp_err_t load_png_to_rgb565(const char *path, uint8_t **out_buf, lv_image_dsc_t *out_dsc)
{
    // 以二进制的方式打开文件
    FILE *fp = fopen(path, "rb");
    // 打开失败
    if (!fp)
    {
        ESP_LOGE(TAG, "PNG open failed for %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    // 文件指针指向末尾
    if (fseek(fp, 0, SEEK_END) != 0)
    {
        fclose(fp);
        return ESP_FAIL;
    }
    // 获取文件大小
    long file_size = ftell(fp);
    // 空文件
    if (file_size <= 0)
    {
        fclose(fp);
        return ESP_FAIL;
    }
    // 指针回到文件开头
    rewind(fp);

    // 分配内存
    //  unsigned char *png_data = malloc((size_t)file_size);
    unsigned char *png_data = heap_caps_malloc((size_t)file_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // 内存分配失败
    if (!png_data)
    {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    // 读取文件
    size_t read_size = fread(png_data, 1, (size_t)file_size, fp);
    // 关闭文件
    fclose(fp);
    // 读取失败，释放内存
    if (read_size != (size_t)file_size)
    {
        free(png_data);
        return ESP_FAIL;
    }

    // PNG转RGBA
    // 用来接收解码结果
    unsigned char *rgba = NULL;
    unsigned width = 0;
    unsigned height = 0;
    // 解码
    unsigned err = lodepng_decode32(&rgba, &width, &height, png_data, read_size);
    // 原始数据释放
    free(png_data);
    // 解码失败
    if (err != 0)
    {
        ESP_LOGE(TAG, "PNG decode failed for %s: %u (%s)", path, err, lodepng_error_text(err));
        return ESP_FAIL;
    }

    // 计算RGB565缓存区大小
    // 总像素数
    size_t pixel_count = (size_t)width * (size_t)height;
    // 每个像素字节
    size_t buf_size = pixel_count * sizeof(uint16_t);
    // uint16_t *rgb565 = malloc(buf_size);
    // 再开一块 PSRAM 存转换后的图像
    uint16_t *rgb565 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // 分配失败
    if (!rgb565)
    {
        free(rgba);
        return ESP_ERR_NO_MEM;
    }

    // RGBA → RGB565 转换（核心）
    // 遍历每个像素
    for (size_t i = 0; i < pixel_count; i++)
    {
        uint8_t r = rgba[i * 4 + 0];
        uint8_t g = rgba[i * 4 + 1];
        uint8_t b = rgba[i * 4 + 2];
        rgb565[i] = rgb888_to_rgb565(r, g, b);
    }
    // RGBA 用完就释放
    free(rgba);

    // 清零结构体（防止脏数据）
    memset(out_dsc, 0, sizeof(*out_dsc));
    // 标识这是合法 LVGL 图片
    out_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    // 图片格式：RGB565
    out_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    // 图片宽度
    out_dsc->header.w = width;
    // 图片高度
    out_dsc->header.h = height;
    // 每一行占多少字节（很关键）
    out_dsc->header.stride = width * sizeof(uint16_t);
    // 图片数据大小
    out_dsc->data_size = buf_size;
    // 指向图片数据
    out_dsc->data = (const uint8_t *)rgb565;
    // 把 RGB565 内存地址返回给调用者
    *out_buf = (uint8_t *)rgb565;
    return ESP_OK;
}

// JPEG 解码器提供“怎么从文件读取数据”
// 参数1：解码器对象
// 参数2：数据缓存区
// 参数3：读取多少哥字节
static size_t jpg_input_func(JDEC *jd, uint8_t *buff, size_t nbyte)
{
    // 获取上下文（文件指针 fp，其他解码信息）
    jpg_decode_ctx_t *ctx = (jpg_decode_ctx_t *)jd->device;
    // 上下文不存在，文件没打开
    if (!ctx || !ctx->fp)
    {
        return 0;
    }
    // 有数据，帮我读 nbyte 数据到 buff 里
    if (buff)
    {
        return fread(buff, 1, nbyte, ctx->fp);
    }
    // 没数据，我不要数据，你帮我跳过 nbyte 字节
    if (fseek(ctx->fp, (long)nbyte, SEEK_CUR) != 0)
    {
        return 0;
    }
    return nbyte;
}

// 把 JPEG 解码出来的像素数据（RGB888）→ 转成 RGB565 → 写入你的图像缓冲区
// 参数1：解码器对象
// 参数2：当前解码出来一小块像素数据
// 参数3：这块数据在整张图的位置
static int jpg_output_func(JDEC *jd, void *bitmap, JRECT *rect)
{
    // 获取上下文
    jpg_decode_ctx_t *ctx = (jpg_decode_ctx_t *)jd->device;
    // 一个无效，就解码失败
    if (!ctx || !ctx->rgb565 || !bitmap || !rect)
    {
        return 0;
    }

    // 是解码出来的数据
    uint8_t *src = (uint8_t *)bitmap;
    // 解码器是“分块输出”的
    // 宽度
    uint32_t rect_w = rect->right - rect->left + 1;
    // 高度
    uint32_t rect_h = rect->bottom - rect->top + 1;

    // 遍历每一行
    for (uint32_t y = 0; y < rect_h; y++)
    {
        // 找到这块在“整张图”中的正确位置
        // 第几列 第几行
        uint16_t *dst = ctx->rgb565 + ((rect->top + y) * ctx->width) + rect->left;
        // 遍历每个像素
        for (uint32_t x = 0; x < rect_w; x++)
        {
            // 从 bitmap 里读一个像素，BGR顺序
            uint8_t b = *src++;
            uint8_t g = *src++;
            uint8_t r = *src++;
            dst[x] = rgb888_to_rgb565(r, g, b);
        }
    }

    return 1;
}

static esp_err_t load_jpg_to_rgb565(const char *path, uint8_t **out_buf, lv_image_dsc_t *out_dsc)
{
    // 打开文件
    FILE *fp = fopen(path, "rb");
    if (!fp)
    {
        return ESP_ERR_NOT_FOUND;
    }

    // uint8_t *workbuf = malloc(TJPGD_WORKBUFF_SIZE);
    // 分配动态内存
    uint8_t *workbuf = heap_caps_malloc(TJPGD_WORKBUFF_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // 无内存
    if (!workbuf)
    {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    // 初始化上下文
    jpg_decode_ctx_t ctx = {
        .fp = fp,
        .rgb565 = NULL,
        .width = 0,
        .height = 0,
    };

    // 初始化解码器
    JDEC jd;
    JRESULT res = jd_prepare(&jd, jpg_input_func, workbuf, TJPGD_WORKBUFF_SIZE, &ctx);
    if (res != JDR_OK)
    {
        free(workbuf);
        fclose(fp);
        ESP_LOGE(TAG, "JPG prepare failed for %s: %d", path, res);
        return ESP_FAIL;
    }

    // 获取图片尺寸
    ctx.width = jd.width;
    ctx.height = jd.height;
    // 分配内存大小
    size_t buf_size = (size_t)ctx.width * (size_t)ctx.height * sizeof(uint16_t);
    // ctx.rgb565 = malloc(buf_size);
    ctx.rgb565 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // 内存不够
    if (!ctx.rgb565)
    {
        free(workbuf);
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }

    // 开始解码
    /*循环：
   调用 jpg_input_func 读数据
   解码一小块
   调用 jpg_output_func 写像素*/
    res = jd_decomp(&jd, jpg_output_func, 0);
    // 释放资源
    free(workbuf);
    fclose(fp);
    // 解码失败
    if (res != JDR_OK)
    {
        free(ctx.rgb565);
        ESP_LOGE(TAG, "JPG decode failed for %s: %d", path, res);
        return ESP_FAIL;
    }

    // 清空结构体
    memset(out_dsc, 0, sizeof(*out_dsc));
    // 创建图片
    out_dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    out_dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    out_dsc->header.w = ctx.width;
    out_dsc->header.h = ctx.height;
    out_dsc->header.stride = ctx.width * sizeof(uint16_t);
    out_dsc->data_size = buf_size;
    out_dsc->data = (const uint8_t *)ctx.rgb565;
    *out_buf = (uint8_t *)ctx.rgb565;
    return ESP_OK;
}

// 根据文件类型（PNG / JPG）自动选择对应的解码方式
static esp_err_t load_image_file_to_rgb565(const char *path, uint8_t **out_buf, lv_image_dsc_t *out_dsc)
{
    const char *ext = strrchr(path, '.');
    if (!ext)
    {
        return ESP_ERR_INVALID_ARG;
    }

    // 处理PNG图片
    if (strcasecmp(ext, ".png") == 0)
    {
        return load_png_to_rgb565(path, out_buf, out_dsc);
    }
    // 处理jpg图片
    if (strcasecmp(ext, ".jpg") == 0 || strcasecmp(ext, ".jpeg") == 0)
    {
        return load_jpg_to_rgb565(path, out_buf, out_dsc);
    }

    return ESP_ERR_NOT_SUPPORTED;
}

static void lcd_backlight_init(void)
{
#if LCD_PIN_BL >= 0
    gpio_config_t bk_gpio_config = {
        .pin_bit_mask = BIT64(LCD_PIN_BL),
        .mode = GPIO_MODE_OUTPUT,
    };
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));
    gpio_set_level(LCD_PIN_BL, 1);
    ESP_LOGI(TAG, "Backlight ON");
#endif
}

static void lcd_panel_init(esp_lcd_panel_io_handle_t *out_io,
                           esp_lcd_panel_handle_t *out_panel)
{
    ESP_LOGI(TAG, "Init SPI bus");
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = LCD_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_H_RES * LCD_BUF_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Init LCD panel IO (SPI)");
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .spi_mode = 0,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(
        esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                 &io_cfg, &io_handle));

    ESP_LOGI(TAG, "Init ST7789V panel");
    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = LCD_BIT_PER_PIXEL,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, true));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    *out_io = io_handle;
    *out_panel = panel_handle;
    ESP_LOGI(TAG, "ST7789V init done");
}

static void lvgl_port_setup(esp_lcd_panel_io_handle_t io_handle,
                            esp_lcd_panel_handle_t panel_handle)
{
    ESP_LOGI(TAG, "Init LVGL port");
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = LCD_BUF_SIZE,
        .double_buffer = false,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        },
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = true,
        },
    };
    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == NULL) {
        ESP_LOGE(TAG, "LVGL display add failed");
        return;
    }
    ESP_LOGI(TAG, "Display added to LVGL");
}

void bsp_display_init(void)
{
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_handle_t panel_handle = NULL;

    lcd_panel_init(&io_handle, &panel_handle);
    lvgl_port_setup(io_handle, panel_handle);
    if (s_disp == NULL) {
        return;
    }

    if (lvgl_port_lock(0))
    {
        lv_obj_t *root = lv_obj_create(lv_screen_active());
        lv_obj_remove_style_all(root);
        lv_obj_set_size(root, LCD_H_RES, LCD_V_RES);
        lv_obj_set_style_bg_color(root, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_center(root);

        int block_width = LCD_H_RES / 3;

        lv_obj_t *red_block = lv_obj_create(root);
        lv_obj_remove_style_all(red_block);
        lv_obj_set_size(red_block, block_width, LCD_V_RES);
        lv_obj_set_style_bg_color(red_block, lv_color_make(255, 0, 0), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(red_block, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(red_block, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *green_block = lv_obj_create(root);
        lv_obj_remove_style_all(green_block);
        lv_obj_set_size(green_block, block_width, LCD_V_RES);
        lv_obj_set_style_bg_color(green_block, lv_color_make(0, 255, 0), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(green_block, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(green_block, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *blue_block = lv_obj_create(root);
        lv_obj_remove_style_all(blue_block);
        lv_obj_set_size(blue_block, block_width, LCD_V_RES);
        lv_obj_set_style_bg_color(blue_block, lv_color_make(0, 0, 255), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(blue_block, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_align(blue_block, LV_ALIGN_RIGHT_MID, 0, 0);

        lv_obj_t *red_label = lv_label_create(red_block);
        lv_label_set_text(red_label, "RED");
        lv_obj_set_style_text_color(red_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(red_label);

        lv_obj_t *green_label = lv_label_create(green_block);
        lv_label_set_text(green_label, "GREEN");
        lv_obj_set_style_text_color(green_label, lv_color_black(), LV_PART_MAIN);
        lv_obj_center(green_label);

        lv_obj_t *blue_label = lv_label_create(blue_block);
        lv_label_set_text(blue_label, "BLUE");
        lv_obj_set_style_text_color(blue_label, lv_color_white(), LV_PART_MAIN);
        lv_obj_center(blue_label);

        lvgl_port_unlock();
        ESP_LOGI(TAG, "Color test: Red=Left, Green=Center, Blue=Right");
    }
}

// esp_err_t bsp_display_show_peach(void)
// {
//     if (!s_disp)
//     {
//         return ESP_ERR_INVALID_STATE;
//     }

//     if (!lvgl_port_lock(pdMS_TO_TICKS(1000)))
//     {
//         return ESP_ERR_TIMEOUT;
//     }

//     display_clear_root();
//     release_runtime_image();
//     create_peach_scene();
//     lvgl_port_unlock();
//     ESP_LOGI(TAG, "Peach image rendered on screen");
//     return ESP_OK;
// }

esp_err_t bsp_display_release_runtime_image(void)
{
    if (!s_disp)
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (!lvgl_port_lock(pdMS_TO_TICKS(1000)))
    {
        return ESP_ERR_TIMEOUT;
    }

    display_clear_root();
    release_runtime_image();
    log_heap_state("display_image: after explicit runtime release");
    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t bsp_display_show_image_file(const char *path)
{
    // 检查显示设备有没有初始化
    if (!s_disp)
    {
        return ESP_ERR_INVALID_STATE;
    }

    // 检查路径是否正常
    if (!path || path[0] == '\0')
    {
        return ESP_ERR_INVALID_ARG;
    }

    /*读取文件（PNG/JPG）
        解码
        转成 RGB565*/
    log_heap_state("display_image: before load_image_file_to_rgb565");

    uint8_t *new_buf = NULL;
    lv_image_dsc_t new_img = {0};
    esp_err_t err = load_image_file_to_rgb565(path, &new_buf, &new_img);
    log_heap_state("display_image: after load_image_file_to_rgb565");
    //解码失败
    if (err != ESP_OK)
    {
        return err;
    }

    //LVGL 不是线程安全的！！必须加锁再操作 UI
    if (!lvgl_port_lock(pdMS_TO_TICKS(1000)))
    {
        free(new_buf);
        return ESP_ERR_TIMEOUT;
    }
    //删除旧UI
    display_clear_root();
    //释放上一次的图片内存
    release_runtime_image();
    log_heap_state("display_image: after release_runtime_image");
    //保存新图片
    s_runtime_img_buf = new_buf;
    s_runtime_img = new_img;
    log_heap_state("display_image: after persist runtime image");
    //创建显示界面
    create_runtime_image_scene(&s_runtime_img);
    //放锁
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Image rendered on screen: %s (%ux%u RGB565)", path, s_runtime_img.header.w, s_runtime_img.header.h);
    return ESP_OK;
}

lv_disp_t *bsp_get_disp(void)
{
    return s_disp;
}
