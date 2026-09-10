/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ESP_VISION_BOARD_CONFIG_H
#define ESP_VISION_BOARD_CONFIG_H

#define ESP_VISION_BOARD_ARCH                  "ESP32S3"
#define ESP_VISION_BOARD_TYPE                  "ESP32_S3_MATOUCH_AI_28"
#define ESP_VISION_EV_MUX_DEFAULT_ENABLED           (0)
#define ESP_VISION_PORT_ESP32                       (1)

#define ESP_VISION_IMLIB_PROFILER_ENABLE            (0)
#define ESP_VISION_IMLIB_GPU_ENABLE                 (0)
#define ESP_VISION_IMLIB_JPEG_CODEC_ENABLE          (0)

#define ESP_VISION_CACHE_LINE_SIZE                  (32)
#define ESP_VISION_ALLOC_ALIGNMENT                  (ESP_VISION_CACHE_LINE_SIZE)
#define ESP_VISION_DMA_ALIGNMENT                    (ESP_VISION_CACHE_LINE_SIZE)

#define ESP_VISION_JPEG_QUALITY_LOW                 (60)
#define ESP_VISION_JPEG_QUALITY_HIGH                (60)
#define ESP_VISION_JPEG_QUALITY_THRESHOLD           (320 * 240 * 2)

/* Camera configuration. */
#define ESP_VISION_CAMERA_SENSOR_ID                 (0x3660)
#define ESP_VISION_CAMERA_RAW_INPUT_WIDTH           (320)
#define ESP_VISION_CAMERA_RAW_INPUT_HEIGHT          (240)
#define ESP_VISION_CAMERA_ACTIVE_INPUT_WIDTH        (320)
#define ESP_VISION_CAMERA_ACTIVE_INPUT_HEIGHT       (240)
#define ESP_VISION_CAMERA_OUTPUT_QQVGA_WIDTH        (160)
#define ESP_VISION_CAMERA_OUTPUT_QQVGA_HEIGHT       (120)
#define ESP_VISION_CAMERA_OUTPUT_QVGA_WIDTH         (320)
#define ESP_VISION_CAMERA_OUTPUT_QVGA_HEIGHT        (240)
#define ESP_VISION_CAMERA_BUFFER_COUNT              (2)
#define ESP_VISION_CAMERA_SCCB_I2C_PORT             (0)
#define ESP_VISION_CAMERA_SCCB_I2C_SCL_PIN          (38)
#define ESP_VISION_CAMERA_SCCB_I2C_SDA_PIN          (39)
#define ESP_VISION_CAMERA_SENSOR_RESET_PIN          (-1)
#define ESP_VISION_CAMERA_SENSOR_PWDN_PIN           (-1)
#define ESP_VISION_CAMERA_SCCB_I2C_FREQ             (400000)
#define ESP_VISION_CAMERA_XCLK_PIN                  (9)
#define ESP_VISION_CAMERA_XCLK_FREQ                 (20000000)
#define ESP_VISION_CAMERA_XCLK_LEDC_TIMER           (1)
#define ESP_VISION_CAMERA_XCLK_LEDC_CHANNEL         (2)
#define ESP_VISION_CAMERA_DVP_PCLK_PIN              (17)
#define ESP_VISION_CAMERA_DVP_VSYNC_PIN             (11)
#define ESP_VISION_CAMERA_DVP_HSYNC_PIN             (10)
#define ESP_VISION_CAMERA_DVP_D0_PIN                (7)
#define ESP_VISION_CAMERA_DVP_D1_PIN                (5)
#define ESP_VISION_CAMERA_DVP_D2_PIN                (4)
#define ESP_VISION_CAMERA_DVP_D3_PIN                (6)
#define ESP_VISION_CAMERA_DVP_D4_PIN                (16)
#define ESP_VISION_CAMERA_DVP_D5_PIN                (8)
#define ESP_VISION_CAMERA_DVP_D6_PIN                (3)
#define ESP_VISION_CAMERA_DVP_D7_PIN                (46)

/* LCD configuration. */
#define ESP_VISION_LCD_WIDTH                        (320)
#define ESP_VISION_LCD_HEIGHT                       (240)
#define ESP_VISION_LCD_SPI_HOST                     (SPI3_HOST)
#define ESP_VISION_LCD_PIXEL_CLOCK_HZ               (20 * 1000 * 1000)
#define ESP_VISION_LCD_CMD_BITS                     (8)
#define ESP_VISION_LCD_PARAM_BITS                   (8)
#define ESP_VISION_LCD_BPP                          (16)
#define ESP_VISION_LCD_PIN_MOSI                     (13)
#define ESP_VISION_LCD_PIN_CLK                      (48)
#define ESP_VISION_LCD_PIN_CS                       (40)
#define ESP_VISION_LCD_PIN_DC                       (21)
#define ESP_VISION_LCD_PIN_RST                      (-1)
#define ESP_VISION_LCD_PIN_BL                       (45)
#define ESP_VISION_LCD_SPI_MODE                     (3)
#define ESP_VISION_LCD_TRANS_QUEUE_DEPTH            (10)
#define ESP_VISION_LCD_BACKLIGHT_CH                 (1)
#define ESP_VISION_LCD_BACKLIGHT_TIMER              (0)
#define ESP_VISION_LCD_BACKLIGHT_PWM_HZ             (5000)
#define ESP_VISION_LCD_BACKLIGHT_OUTPUT_INVERT      (0)

// /* SD card configuration. */
// #define ESP_VISION_SDCARD_MOUNT_PATH                "/sdcard"
// #define ESP_VISION_SDCARD_SLOT                      (0)
// #define ESP_VISION_SDCARD_BUS_WIDTH                 (1)
// #define ESP_VISION_SDCARD_CLK_PIN                   (39)
// #define ESP_VISION_SDCARD_CMD_PIN                   (38)
// #define ESP_VISION_SDCARD_D0_PIN                    (40)

/* MicroSD configuration: SPI mode, shared with LCD SPI3 bus. */
#define ESP_VISION_SDCARD_USE_SPI              (1)
#define ESP_VISION_SDCARD_MOUNT_PATH           "/sdcard"
#define ESP_VISION_SDCARD_SPI_HOST             (SPI3_HOST)
#define ESP_VISION_SDCARD_SPI_MISO_PIN         (12)
#define ESP_VISION_SDCARD_SPI_MOSI_PIN         (13)
#define ESP_VISION_SDCARD_SPI_CLK_PIN          (48)
#define ESP_VISION_SDCARD_SPI_CS_PIN           (47)
#define ESP_VISION_SDCARD_SPI_FREQ_KHZ         (20000)

#endif /* ESP_VISION_BOARD_CONFIG_H */
