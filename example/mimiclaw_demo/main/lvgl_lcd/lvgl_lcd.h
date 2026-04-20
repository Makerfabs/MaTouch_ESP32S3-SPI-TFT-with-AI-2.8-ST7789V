#pragma once


#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_err.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "driver/gpio.h"
#include "esp_log.h"

#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_gt911.h"

#include "esp_lvgl_port.h"
#include "lvgl.h"


/**
 * @brief 初始化 ST7789V 显示屏 + GT911 触摸 + LVGL 绑定
 */
void bsp_display_init(void);

/** @brief 获取 LVGL 显示句柄（init 之后调用）*/
lv_disp_t *bsp_get_disp(void);

/** @brief 在屏幕上显示桃子图案 */
esp_err_t bsp_display_show_peach(void);

/** @brief 从 SPIFFS 图片文件显示到屏幕，支持 png/jpg/jpeg */
esp_err_t bsp_display_show_image_file(const char *path);

/** @brief 释放运行时图片与其根容器 */
esp_err_t bsp_display_release_runtime_image(void);

/** @brief 获取 LVGL 触摸输入设备句柄（init 之后调用）*/
lv_indev_t *bsp_get_indev(void);
