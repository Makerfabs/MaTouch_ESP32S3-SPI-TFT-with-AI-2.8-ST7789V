/*
Author: Yuki
Date:2025.5.15
Code version: V1.0.0
Note: 

Library version:
Arduino IDE 2.3.4
esp32 V2.0.16
GFX Library for Arduino v1.5.6
lvgl v8.3.11
bb_captouch v1.3.1
ArduinoJson v7.2.0
UrlEncode v1.0.1
base64 v1.3.0

Tools:
Flash size: 16MB(128Mb)
Partition Schrme: 16M Flash(3MB APP/9.9MB FATFS)
PSRAM: OPI PSRAM
*/

#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <bb_captouch.h>
#include <ui.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <driver/i2s.h>
#include <UrlEncode.h>
#include <base64.hpp>

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

#define I2S_SD 41
#define I2S_SCK 42
#define I2S_WS 2
#define I2S_PORT I2S_NUM_0
#define SAMPLE_RATE 16000
#define BUFFER_LEN 64
#define RECORD_TIME 5

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO, HSPI, true); // Constructor
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RES, 1 /* rotation */, true /* IPS */);
BBCapTouch bbct;

const char* ssid = "YOUR-SSID";
const char* password = "YOUR-PIN";

// 百度智能云配置
const char* apiKey = "YOUR-APIKEY";
const char* secretKey = "YOUR-SECRETKEY";
const char* tokenUrl = "https://aip.baidubce.com/oauth/2.0/token";
const char* asrUrl = "https://vop.baidu.com/server_api";

String accessToken = "";

int16_t sBuffer[BUFFER_LEN]; // 音频数据缓冲区
uint8_t* audioBuffer = nullptr; // 存储录制的音频数据
size_t audioBufferSize = 0; // 音频数据总大小
bool isRecording = false; // 是否正在录音

lv_state_t speak_state;

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
    //Serial.print(x);
    //Serial.print(",");
    //Serial.println(y);
  }
  else
  {
    data->state = LV_INDEV_STATE_REL;
  }
}

void setup()
{
  Serial.begin(115200);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, 1);
  
  WiFi.begin(ssid, password);
  // Init touch device
  bbct.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

  // 初始化I2S
  i2s_install();
  i2s_setpin();
  i2s_start(I2S_PORT);

  // Init Display
  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);
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

    Serial.println("Connecting to WiFi...");

    while (WiFi.status() != WL_CONNECTED)
    {
      Serial.println("Connecting to WiFi...");
      lv_textarea_set_text(ui_TextArea1, "Connecting to WiFi...");
      delay(500);
    }
    
    getBaiduAccessToken();

    audioBufferSize = SAMPLE_RATE * RECORD_TIME * 2;
    audioBuffer = (uint8_t*)malloc(audioBufferSize);
    if (audioBuffer == nullptr)
    {
      Serial.println("音频缓冲区分配失败");
      lv_textarea_set_text(ui_TextArea1, "Audio buffer allocation failure");
      while (1);
    }

    Serial.println("Setup done");

    xTaskCreatePinnedToCore(Task_TFT, "Task_TFT", 40960, NULL, 3, NULL, 0); 
    xTaskCreatePinnedToCore(Task_main, "Task_main", 40960, NULL, 2, NULL, 1);
  }
}

void loop()
{
}

void Task_TFT(void *pvParameters)
{
    while (1)
    {
      lv_timer_handler();
      vTaskDelay(10);
    }
}

void Task_main(void *pvParameters)
{
  while (1)
  {
    speak_state = lv_obj_get_state(ui_Button1); 
    if((speak_state & LV_STATE_PRESSED))
    {
        Serial.println("开始录音...");
        lv_textarea_set_text(ui_TextArea1, "Start recording...");
        isRecording = true;
        recordAudio();
    }
    else if (speak_state & LV_STATE_FOCUSED)
    {
      if (isRecording)
      {
        Serial.println("录音完成");
        lv_textarea_set_text(ui_TextArea1, "Recording complete.");

        Serial.println("音频数据指针: " + String((uintptr_t)audioBuffer, HEX));
        Serial.println("音频数据长度: " + String(audioBufferSize));
        for (int i = 0; i < 10; i++) {
          Serial.println(((int16_t*)audioBuffer)[i]);
        }

        String recognizedText = baiduSTT_Send(accessToken, audioBuffer, audioBufferSize);
        Serial.println("Recognized text: " + recognizedText);
        lv_textarea_set_text(ui_TextArea1, recognizedText.c_str());
        isRecording = false;
      }
    }
    vTaskDelay(100);
  }
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

void getBaiduAccessToken()
{
  HTTPClient http;
  String url = String(tokenUrl) + "?grant_type=client_credentials&client_id=" + String(apiKey) + "&client_secret=" + String(secretKey);
  http.begin(url);

  int httpCode = http.GET();
  if (httpCode == HTTP_CODE_OK)
  {
    String payload = http.getString();
    DynamicJsonDocument doc(256);
    deserializeJson(doc, payload);
    accessToken = doc["access_token"].as<String>();
    Serial.println("获取Access Token成功: " + accessToken);
    lv_textarea_set_text(ui_TextArea1, "Get Access Token");
  }
  else
  {
    gfx->fillScreen(RGB565_BLACK);
    gfx->setCursor(0, 10);
    lv_textarea_set_text(ui_TextArea1, "Get Access Token Fail");
    Serial.print("获取Access Token失败");
  }

  http.end();
}

void recordAudio()
{
  size_t totalBytesRead = 0;
  size_t bytesRead;

  while (totalBytesRead < audioBufferSize)
  {
    esp_err_t result = i2s_read(I2S_PORT, sBuffer, BUFFER_LEN * 2, &bytesRead, portMAX_DELAY);
    if (result == ESP_OK)
    {
      memcpy(audioBuffer + totalBytesRead, sBuffer, bytesRead);
      totalBytesRead += bytesRead;
    }
    else
    {
      Serial.println("I2S read fail");
      break;
    }
  }
}

String baiduSTT_Send(String access_token, uint8_t* audioData, int audioDataSize)
{
  String recognizedText = "";

  if (access_token == "")
  {
    Serial.println("access_token is null");
    return recognizedText;
  }

  int audio_data_len = audioDataSize * sizeof(char) * 1.4;
  unsigned char* audioDataBase64 = (unsigned char*)ps_malloc(audio_data_len);
  if (!audioDataBase64)
  {
    Serial.println("Failed to allocate memory for audioDataBase64");
    return recognizedText;
  }

  int data_json_len = audioDataSize * sizeof(char) * 1.4;
  char* data_json = (char*)ps_malloc(data_json_len);
  if (!data_json)
  {
    Serial.println("Failed to allocate memory for data_json");
    return recognizedText;
  }

  // Base64 encode audio data
  encode_base64(audioData, audioDataSize, audioDataBase64);

  memset(data_json, '\0', data_json_len);
  strcat(data_json, "{");
  strcat(data_json, "\"format\":\"pcm\",");
  strcat(data_json, "\"rate\":16000,");
  strcat(data_json, "\"dev_pid\":1737,");
  strcat(data_json, "\"channel\":1,");
  strcat(data_json, "\"cuid\":\"57722200\",");
  strcat(data_json, "\"token\":\"");
  strcat(data_json, access_token.c_str());
  strcat(data_json, "\",");
  sprintf(data_json + strlen(data_json), "\"len\":%d,", audioDataSize);
  strcat(data_json, "\"speech\":\"");
  strcat(data_json, (const char*)audioDataBase64);
  strcat(data_json, "\"");
  strcat(data_json, "}");

  HTTPClient http_client;

  http_client.begin("http://vop.baidu.com/server_api");
  http_client.addHeader("Content-Type", "application/json");
  int httpCode = http_client.POST(data_json);

  if (httpCode > 0)
  {
    if (httpCode == HTTP_CODE_OK)
    {
      String response = http_client.getString();
      Serial.println(response);

      DynamicJsonDocument responseDoc(2048);
      deserializeJson(responseDoc, response);
      recognizedText = responseDoc["result"].as<String>();
    }
  }
  else
  {
    Serial.printf("[HTTP] POST failed, error: %s\n", http_client.errorToString(httpCode).c_str());
  }

  if (audioDataBase64)
  {
    free(audioDataBase64);
  }

  if (data_json)
  {
    free(data_json);
  }

  http_client.end();

  return recognizedText;
}

void i2s_install()
{
  const i2s_config_t i2s_config =
  {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = i2s_bits_per_sample_t(16),
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_LEN,
    .use_apll = false
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
}

void i2s_setpin()
{
  const i2s_pin_config_t pin_config =
  {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };

  i2s_set_pin(I2S_PORT, &pin_config);
}