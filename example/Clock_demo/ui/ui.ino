/*
Author: Yuki
Date:2025.5.22
Code version: V1.0.0
Note: 

Library version:
Arduino IDE 2.3.4
esp32 V2.0.16
GFX Library for Arduino v1.5.6
lvgl v8.3.11
bb_captouch v1.3.1
RTClib v2.1.4
Adafruit BusIO v1.16.2

Tools:
Flash size: 16MB(128Mb)
Partition Schrme: 16M Flash(3MB APP/9.9MB FATFS)
PSRAM: OPI PSRAM
*/
#include <RTClib.h>
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <bb_captouch.h>
#include <ui.h>

#define RTC_SCL 38
#define RTC_SDA 39
#define RTC_INT 15

#define TFT_BLK 45
#define TFT_RES -1

#define TFT_CS 40
#define TFT_MOSI 13
#define TFT_MISO 12
#define TFT_SCLK 48
#define TFT_DC 21

#define TOUCH_INT 14
#define TOUCH_SDA 39
#define TOUCH_SCL 38
#define TOUCH_RST 18

#define SCREEN_W 320
#define SCREEN_H 240

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO, HSPI, true); // Constructor
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RES, 1 /* rotation */, true /* IPS */);
BBCapTouch bbct;
RTC_PCF8563 rtc_pcf;

/* Change to your screen resolution */
static uint32_t screenWidth;
static uint32_t screenHeight;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf;

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  uint32_t w = (area->x2 - area->x1 + 1);
  uint32_t h = (area->y2 - area->y1 + 1);

#if (LV_COLOR_16_SWAP != 0)
  gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#else
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
#endif

  lv_disp_flush_ready(disp);
}

void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{

  uint16_t x = 0, y = 0;
  if (get_touch(&x, &y))
  {
    data->state = LV_INDEV_STATE_PR;

    /*Set the coordinates*/
    data->point.x = x;
    data->point.y = y;
  }
  else
  {
    data->state = LV_INDEV_STATE_REL;
  }
}

void setup()
{
  Serial.begin(115200);

  Wire.begin(RTC_SDA, RTC_SCL);
  rtc_pcf_init();

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, 1);

  // Init touch device
  bbct.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

  // Init Display
  gfx->begin();

  lv_init();

  screenWidth = SCREEN_W;
  screenHeight = SCREEN_H;
#ifdef ESP32
  disp_draw_buf = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * screenWidth * 10, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
  disp_draw_buf = (lv_color_t *)malloc(sizeof(lv_color_t) * screenWidth * 10);
#endif
  if (!disp_draw_buf)
  {
    Serial.println("LVGL disp_draw_buf allocate failed!");
  }
  else
  {
    lv_disp_draw_buf_init(&draw_buf, disp_draw_buf, NULL, screenWidth * 10);

    /* Initialize the display */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    /* Change the following line to your display resolution */
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* Initialize the (dummy) input device driver */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    ui_init();

    Serial.println("Setup done");
  }
}

void loop()
{
  lv_timer_handler();
  DateTime now = rtc_pcf.now();

  int hour=now.hour();
  int min=now.minute();
  int sec=now.second();

  Serial.print(hour);
  Serial.print(min);
  Serial.println(sec);

  lv_img_set_angle(ui_Image1, hour*300);
  lv_img_set_angle(ui_Image2, min*60);
  lv_img_set_angle(ui_Image3, sec*60);

  Serial.println("done");
  delay(50);
}

void rtc_pcf_init()
{
    if (!rtc_pcf.begin())
    {
        Serial.println("Couldn't find RTC");
        Serial.flush();
    }

    rtc_pcf.adjust(DateTime(F(__DATE__), F(__TIME__)));
}

int get_touch(uint16_t *x, uint16_t *y)
{
  TOUCHINFO ti;
  if (bbct.getSamples(&ti))
  {
    *x = map(ti.y[0], 0, 320, 0, 320);
    *y = map(ti.x[0], 240, 0, 0, 240);

    // *x = ti.x[0];
    // *y = ti.y[0];

    return 1;
  }
  else
    return 0;
}