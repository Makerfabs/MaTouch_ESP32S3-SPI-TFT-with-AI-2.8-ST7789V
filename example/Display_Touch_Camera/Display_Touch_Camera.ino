/*
Author: Yuki
Date:2025.6.9
Code version: V1.0.0
Note: Add flip (mirror) to camera display

Library version:
Arduino IDE 2.3.4
esp32 V2.0.16
GFX Library for Arduino v1.5.6
bb_captouch v1.3.1
JPEGDecoder v2.0.0

Tools:
Flash size: 16MB(128Mb)
Partition Schrme: 16M Flash(3MB APP/9.9MB FATFS)
PSRAM: OPI PSRAM
*/ 

#include <Arduino_GFX_Library.h>
#include <bb_captouch.h>
#include <SD.h>
#include <JPEGDecoder.h>
#include <FS.h>
#include "esp_camera.h"

#define CAMERA_MODEL_MAKERFABS
#define CAMERA_MODEL_AI_THINKER
#include "camera_pins.h"

#define TFT_BLK 45
#define TFT_RES -1

#define SD_CS 47
#define TFT_CS 40
#define TFT_DC 21
#define MOSI 13
#define MISO 12
#define SCLK 48

#define TOUCH_INT 14
#define TOUCH_SDA 39
#define TOUCH_SCL 38
#define TOUCH_RST 18

#define PIN_SD_CMD 2
#define PIN_SD_CLK 42
#define PIN_SD_D0 41

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, SCLK, MOSI, MISO, HSPI, true); // Constructor
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RES, 1 /* rotation */, true /* IPS */);
BBCapTouch bbct;

uint8_t* jpgArray = nullptr;
size_t jpgArraySize = 0;

void setup()
{
  // put your setup code here, to run once:
  Serial.begin(115200);

  SPI.begin(SCLK, MISO, MOSI);
  digitalWrite(SD_CS, LOW);

  if (!SD.begin(SD_CS, SPI, 80000000))
  {
    Serial.println(F("ERROR: File System Mount Failed!"));
  }

  listDir(SD, "/", 0); // Read SD card files
  processJPG("/logo_240240.jpg");

  digitalWrite(SD_CS, HIGH);
  digitalWrite(TFT_CS, LOW);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, 1);

  gfx->begin();
  gfx->fillScreen(WHITE);
  displayJPGFromArray(jpgArray, jpgArraySize);

  delay(3000);
  Serial.println("start");
  
  gfx->fillScreen(WHITE);
  gfx->setTextSize(4);
  gfx->setCursor(10, 10);
  gfx->setTextColor(RED);

  bbct.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

  /*Serial.println("Color");
  gfx->fillScreen(RED);
  delay(1000);
  gfx->fillScreen(GREEN);
  delay(1000);
  gfx->fillScreen(BLUE);
  delay(1000);
  gfx->fillScreen(BLACK);
  delay(1000);
  gfx->fillScreen(WHITE);
  Serial.println("Color Over");*/

  gfx->fillRect(280, 200, 40, 40, BLUE);
  gfx->setTextSize(1);
  gfx->setCursor(290, 210);
  gfx->setTextColor(YELLOW);
  gfx->println(F("CAM"));

  while (1)
  {
      uint16_t x, y;

      if (get_touch(&x, &y))
      {
          if (x > 280 && x < 320 && y > 200 && y < 240)
              break;
          Serial.print(x);
          Serial.print(",");
          Serial.println(y);

          gfx->fillCircle(x, y, 6, RED);
      }
      delay(10);
  }

  gfx->fillScreen(BLACK);
  camera_init_s3();

}

void loop()
{
    long runtime = millis();

    camera_fb_t *fb = NULL;
    fb = esp_camera_fb_get();
    // char s[20];
    // sprintf(s, "x %d y %d", fb->width, fb->height);
    // Serial.println(s);

    gfx->draw16bitBeRGBBitmap(0, 0, (uint16_t *)fb->buf, fb->width, fb->height);

    esp_camera_fb_return(fb);

    gfx->setCursor(250, 10);
    gfx->fillRect(250, 10, 50, 8, BLUE);

    runtime = millis() - runtime;
    char s[20];
    sprintf(s, "FPS:%.1f", 1000.0 / runtime);

    gfx->println(s);
}

int get_touch(uint16_t *x, uint16_t *y)
{
    TOUCHINFO ti;
    if (bbct.getSamples(&ti))
    {
        // *x = ti.y[0];
        // *y = map(ti.x[0], 240, 0, 0, 240);

        *x = ti.y[0];
        *y = 240-ti.x[0];

        return 1;
    }
    else
        return 0;
}

void listDir(fs::FS &fs, const char * dirname, uint8_t levels){
    Serial.printf("Listing directory: %s\n", dirname);

    File root = fs.open(dirname);
    if(!root){
        Serial.println("Failed to open directory");
        return;
    }
    if(!root.isDirectory()){
        Serial.println("Not a directory");
        return;
    }

    File file = root.openNextFile();
    while(file){
        if(file.isDirectory()){
            Serial.print("  DIR : ");
            Serial.println(file.name());
            if(levels){
                listDir(fs, file.path(), levels -1);
            }
        } else {
            Serial.print("  FILE: ");
            Serial.print(file.name());
            Serial.print("  SIZE: ");
            Serial.println(file.size());
        }
        file = root.openNextFile();
    }
}

// Check whether the file is a JPG by inspecting the file header
bool isJPGByHeader(fs::FS &fs, const char* filePath) 
{
  File file = fs.open(filePath, FILE_READ);
  if (!file) 
  {
    Serial.println("Failed to open file!");
    return false;
  }

  uint8_t header[3];
  file.read(header, 3);
  file.close();

  return (header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF);
}

// Read JPG file content
bool saveJPGToArray(fs::FS &fs, const char* jpgPath) 
{
  if (jpgArray != nullptr) 
  {
    free(jpgArray);
    jpgArray = nullptr;
    jpgArraySize = 0;
  }

  File jpgFile = fs.open(jpgPath, FILE_READ);
  if (!jpgFile) 
  {
    Serial.println("Failed to open JPG file!");
    return false;
  }

  jpgArraySize = jpgFile.size();
  Serial.printf("JPG file size: %d bytes\n", jpgArraySize);

  jpgArray = (uint8_t*)malloc(jpgArraySize);
  if (jpgArray == nullptr) 
  {
    Serial.println("Memory allocation failed!");
    jpgFile.close();
    return false;
  }

  jpgFile.read(jpgArray, jpgArraySize);
  jpgFile.close();

  Serial.println("JPG successfully saved to global array.");
  return true;
}

// Print JPG array content
void printJPGArray()
{
  if (jpgArray == nullptr || jpgArraySize == 0) 
  {
    Serial.println("JPG array is empty!");
    return;
  }

  Serial.println("const uint8_t jpgArray[] PROGMEM = {");
  for (size_t i = 0; i < jpgArraySize; i++) 
  {
    Serial.printf("0x%02X", jpgArray[i]);
    if (i < jpgArraySize - 1) 
      Serial.print(", ");
    if ((i + 1) % 16 == 0) 
      Serial.println();  // 每行16字节
  }
  Serial.println("};");
  Serial.printf("JPG array size: %d bytes\n", jpgArraySize);
}

// Check and save JPG
void processJPG(const char* jpgPath) 
{
  if (!isJPGByHeader(SD, jpgPath)) 
  {
    Serial.printf("File %s is not a valid JPG file.\n", jpgPath);
    return;
  }

  if (!saveJPGToArray(SD, jpgPath)) 
  {
    Serial.println("Error saving JPG to array!");
    return;
  }

  Serial.printf("JPG %s saved to global array successfully.\n", jpgPath);
}

// display JPG
void displayJPGFromArray(const uint8_t* jpgData, size_t jpgSize) 
{
  // decode JPG
  JpegDec.decodeArray(jpgData, jpgSize);

  uint16_t w = JpegDec.width;
  uint16_t h = JpegDec.height;
  Serial.printf("Image decoded: %dx%d\n", w, h);

  // Calculate center offset
  int xOffset = (gfx->width() - w) / 2;
  int yOffset = (gfx->height() - h) / 2;

  // Start pixel-by-pixel rendering
  while (JpegDec.read()) 
  {
    for (uint16_t y = 0; y < JpegDec.MCUHeight; y++) 
    {
      for (uint16_t x = 0; x < JpegDec.MCUWidth; x++) 
      {
        int16_t drawX = JpegDec.MCUx * JpegDec.MCUWidth + x;
        int16_t drawY = JpegDec.MCUy * JpegDec.MCUHeight + y;

        if (drawX < w && drawY < h) 
        {
          uint16_t color = JpegDec.pImage[y * JpegDec.MCUWidth + x];
          gfx->drawPixel(drawX + xOffset, drawY + yOffset, color);
        }
      }
    }
  }

  Serial.println("JPG image rendered to screen.");
}

// Camera setting
void camera_init_s3()
{
    camera_config_t config;
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.frame_size = FRAMESIZE_240X240;
    // config.pixel_format = PIXFORMAT_JPEG; // for streaming
    config.pixel_format = PIXFORMAT_RGB565; // for face detection/recognition
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.jpeg_quality = 12;
    config.fb_count = 2;

    // camera init
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        Serial.printf("Camera init failed with error 0x%x", err);
        return;
    }

    sensor_t *s = esp_camera_sensor_get();
    if (s->id.PID == OV3660_PID)
    {
        s->set_hmirror(s, 1);    //左右翻转
        s->set_vflip(s, 1);      // flip it back左右翻转
        s->set_brightness(s, 1); // up the brightness just a bit
        s->set_saturation(s, 0); // lower the saturation
    }
}
