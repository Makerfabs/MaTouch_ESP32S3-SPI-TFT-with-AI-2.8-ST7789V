/*
Author: Yuki
Date:2025.5.27
Code version: V1.0.0
Note: 

Library version:
Arduino IDE 2.3.4
esp32 V2.0.16
GFX Library for Arduino v1.5.6
lvgl v8.3.11
bb_captouch v1.3.1
ESP32-audioI2S-master v2.0.0

Tools:
Flash size: 16MB(128Mb)
Partition Schrme: 16M Flash(3MB APP/9.9MB FATFS)
PSRAM: OPI PSRAM
*/
#include <lvgl.h>
#include <Arduino_GFX_Library.h>
#include <bb_captouch.h>
#include <ui.h>
#include <driver/i2s.h>
#include "FS.h"
#include "SD.h"
#include "SPI.h"
#include "Audio.h"

#define TFT_BLK 45
#define TFT_RES -1

#define SCLK  48
#define MISO 12
#define MOSI 13
#define SD_CS   47
#define TFT_CS 40
#define TFT_DC 21

#define TOUCH_INT 14
#define TOUCH_SDA 39
#define TOUCH_SCL 38
#define TOUCH_RST 18

#define SCREEN_W 320
#define SCREEN_H 240

//MIC
#define I2S_IN_PORT I2S_NUM_0
#define I2S_IN_BCLK 42
#define I2S_IN_LRC 2
#define I2S_IN_DIN 41

//Speaker
#define I2S_OUT_PORT I2S_NUM_1
#define I2S_OUT_BCLK 20
#define I2S_OUT_LRC  1
#define I2S_OUT_DOUT 19

// make changes as needed
#define RECORD_TIME   10  // seconds, The maximum value is 240
#define WAV_FILE_NAME "data"

// do not change for best
#define SAMPLE_RATE 16000U
#define SAMPLE_BITS 16
#define WAV_HEADER_SIZE 44
#define VOLUME_GAIN 2

int fileNumber = 1;
String baseFileName = "Record";
bool isRecording = false;

int record_flag = 0;
int play_flag = 0;

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, SCLK, MOSI, MISO, HSPI, true); // Constructor
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RES, 1 /* rotation */, true /* IPS */);
BBCapTouch bbct;
//Audio audio;

/* Change to your screen resolution */
static uint32_t screenWidth;
static uint32_t screenHeight;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *disp_draw_buf;
lv_state_t play_btn;
lv_state_t record_btn;

String fileName;
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

  SPI.begin(SCLK, MISO, MOSI);
  digitalWrite(SD_CS, LOW);

  if (!SD.begin(SD_CS, SPI, 80000000))
  {
    Serial.println(F("ERROR: File System Mount Failed!"));
  }

  listDir(SD, "/", 0); // Read SD card files

  digitalWrite(SD_CS, HIGH);
  digitalWrite(TFT_CS, LOW);

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, HIGH);

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

    //audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    //audio.setVolume(10); // 0...21
    I2S_Mic_Init();
    I2S_Speaker_Init();
    Serial.println("Setup done");

    xTaskCreatePinnedToCore(Task_TFT, "Task_TFT", 40960, NULL, 2, NULL, 0);
    xTaskCreatePinnedToCore(Task_main, "Task_main", 40960, NULL, 3, NULL, 1);
  }
}

void loop()
{
}

void Task_TFT(void *pvParameters)
{
  while (1)
  {
    lv_task_handler();
    vTaskDelay(20);
  }
}

void Task_main(void *pvParameters)
{
  while (1)
  {
    if (record_flag == 1)
    {
      lv_label_set_text(ui_Label1, "Recording...");
      lv_task_handler();
      vTaskDelay(30);
      digitalWrite(TFT_CS, HIGH);
      SPI.end();
      SPI.begin(SCLK, MISO, MOSI);
      digitalWrite(SD_CS, LOW);  
      if (!SD.begin(SD_CS, SPI, 80000000))
      {
        Serial.println(F("ERROR: File System Mount Failed!"));
      }
      listDir(SD, "/", 0); // Read SD card files

      while (SD.exists("/" + baseFileName + "." + String(fileNumber) + ".wav"))
      {
        fileNumber++;
      }
      fileName = "/" + baseFileName + "." + String(fileNumber) + ".wav";
      
      fileNumber++;
      record_wav(fileName);

      digitalWrite(SD_CS, HIGH);
      digitalWrite(TFT_CS, LOW);
      gfx->begin();
      
      lv_label_set_text(ui_Label1, "Recorded");
      lv_obj_invalidate(ui_Button1);
      lv_task_handler();
      vTaskDelay(30);
      record_flag = 0;
    }

    if(play_flag==1)
    {
      lv_label_set_text(ui_Label2, "Playing...");
      lv_task_handler();
      vTaskDelay(30);
      digitalWrite(TFT_CS, HIGH);
      SPI.end();
      SPI.begin(SCLK, MISO, MOSI);
      digitalWrite(SD_CS, LOW);  
      if (!SD.begin(SD_CS, SPI, 80000000))
      {
        Serial.println(F("ERROR: File System Mount Failed!"));
      }
      listDir(SD, "/", 0); // Read SD card files

      playWavFromSD(fileName.c_str());

      digitalWrite(SD_CS, HIGH);
      digitalWrite(TFT_CS, LOW);
      gfx->begin();
      
      lv_label_set_text(ui_Label2, "Played");
      lv_obj_invalidate(ui_Button2);
      lv_task_handler();
      vTaskDelay(30);
      play_flag = 0;
    }
    vTaskDelay(50);
  }
}

void playWavFromSD(const char *filename) {
  File audioFile = SD.open(filename);
  if (!audioFile) {
    Serial.println("Failed to open audio file!");
    return;
  }

  // 跳过 WAV 头（44字节）
  if (audioFile.size() <= 44) {
    Serial.println("File too short to be a valid WAV.");
    audioFile.close();
    return;
  }

  audioFile.seek(44);

  const size_t bufferSize = 1024;
  uint8_t buffer[bufferSize];

  size_t bytesRead, bytesWritten;
  while ((bytesRead = audioFile.read(buffer, bufferSize)) > 0)
  {
    // 提高音量：每个 16bit 样本乘以 2^gain
    for (int i = 0; i < bytesRead; i += 2)
    {
      int16_t *sample = (int16_t *)&buffer[i];
      *sample = (*sample) << 2;
    }
    i2s_write(I2S_OUT_PORT, buffer, bytesRead, &bytesWritten, portMAX_DELAY);
  }

  audioFile.close();
  Serial.println("Playback finished.");
}

void I2S_Mic_Init()
{
  // Initialize I2S for audio input
  i2s_config_t i2s_config_in = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,  // 注意：INMP441 输出 32 位数据
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = 0,
    .tx_desc_auto_clear = false,
    .fixed_mclk = I2S_PIN_NO_CHANGE
  };
  i2s_pin_config_t pin_config_in = {
    .bck_io_num = I2S_IN_BCLK,
    .ws_io_num = I2S_IN_LRC,
    .data_out_num = -1,
    .data_in_num = I2S_IN_DIN
  };
  i2s_driver_uninstall(I2S_IN_PORT); // 确保干净初始化
  i2s_driver_install(I2S_IN_PORT, &i2s_config_in, 0, NULL);
  i2s_set_pin(I2S_IN_PORT, &pin_config_in);
  i2s_set_clk(I2S_IN_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

void I2S_Speaker_Init()
{
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = 0,
    .tx_desc_auto_clear = true,
    .fixed_mclk = I2S_PIN_NO_CHANGE
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_OUT_BCLK,
    .ws_io_num = I2S_OUT_LRC,
    .data_out_num = I2S_OUT_DOUT,
    .data_in_num = -1 // 不用 RX
  };

  i2s_driver_uninstall(I2S_OUT_PORT);
  i2s_driver_install(I2S_OUT_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_OUT_PORT, &pin_config);
  i2s_set_clk(I2S_OUT_PORT, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}


void record_wav(String fileName)
{
  uint32_t sample_size = 0;
  uint32_t record_size = (SAMPLE_RATE * SAMPLE_BITS / 8) * RECORD_TIME;
  uint8_t *rec_buffer = NULL;
  Serial.printf("Start recording ...\n");
   
  File file = SD.open(fileName.c_str(), FILE_WRITE);
  // Write the header to the WAV file
  uint8_t wav_header[WAV_HEADER_SIZE];
  generate_wav_header(wav_header, record_size, SAMPLE_RATE);
  file.write(wav_header, WAV_HEADER_SIZE);

  // PSRAM malloc for recording
  rec_buffer = (uint8_t *)ps_malloc(record_size);
  if (rec_buffer == NULL) {
    Serial.printf("malloc failed!\n");
    while(1) ;
  }
  Serial.printf("Buffer: %d bytes\n", ESP.getPsramSize() - ESP.getFreePsram());

  // Start recording
  i2s_read(I2S_IN_PORT, rec_buffer, record_size, &sample_size, portMAX_DELAY);
  if (sample_size == 0) {
    Serial.printf("Record Failed!\n");
  } else {
    Serial.printf("Record %d bytes\n", sample_size);
  }

  // Increase volume
  for (uint32_t i = 0; i < sample_size; i += SAMPLE_BITS/8) {
    (*(uint16_t *)(rec_buffer+i)) <<= VOLUME_GAIN;
  }

  // Write data to the WAV file
  Serial.printf("Writing to the file ...\n");
  if (file.write(rec_buffer, record_size) != record_size)
    Serial.printf("Write file Failed!\n");

  free(rec_buffer);
  file.close();
  Serial.printf("Recording complete: \n");
  //Serial.printf("Send rec for a new sample or enter a new label\n\n");
}
void generate_wav_header(uint8_t *wav_header, uint32_t wav_size, uint32_t sample_rate)
{
  // See this for reference: http://soundfile.sapp.org/doc/WaveFormat/
  uint32_t file_size = wav_size + WAV_HEADER_SIZE - 8;
  uint32_t byte_rate = SAMPLE_RATE * SAMPLE_BITS / 8;
  const uint8_t set_wav_header[] = {
    'R', 'I', 'F', 'F', // ChunkID
    file_size, file_size >> 8, file_size >> 16, file_size >> 24, // ChunkSize
    'W', 'A', 'V', 'E', // Format
    'f', 'm', 't', ' ', // Subchunk1ID
    0x10, 0x00, 0x00, 0x00, // Subchunk1Size (16 for PCM)
    0x01, 0x00, // AudioFormat (1 for PCM)
    0x01, 0x00, // NumChannels (1 channel)
    sample_rate, sample_rate >> 8, sample_rate >> 16, sample_rate >> 24, // SampleRate
    byte_rate, byte_rate >> 8, byte_rate >> 16, byte_rate >> 24, // ByteRate
    0x02, 0x00, // BlockAlign
    0x10, 0x00, // BitsPerSample (16 bits)
    'd', 'a', 't', 'a', // Subchunk2ID
    wav_size, wav_size >> 8, wav_size >> 16, wav_size >> 24, // Subchunk2Size
  };
  memcpy(wav_header, set_wav_header, sizeof(set_wav_header));
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