/*
Author: Yuki
Date:2025.5.23
Code version: V1.0.0
Note: 

Library version:
Arduino IDE 2.3.4
esp32 V2.0.16
GFX Library for Arduino v1.5.6
bb_captouch v1.3.1
lvgl v8.3.11
ArduinoMqttClient v0.1.8

Tools:
Flash size: 16MB(128Mb)
Partition Schrme: 16M Flash(3MB APP/9.9MB FATFS)
PSRAM: OPI PSRAM
*/

#include <Arduino_GFX_Library.h>
#include <bb_captouch.h>
#include <lvgl.h>
#include <ArduinoMqttClient.h>
#include <WiFi.h>

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

#define SCREEN_WIDTH 320
#define SCREEN_HEIGHT 240

char ssid[] = "Makerfabs";
char pass[] = "20160704";

WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

const char broker[]    = "broker.emqx.io"; //"test.mosquitto.org";
int        port        = 1883;

#if 1
const char inTopic[]   = "makerfabs/in";
const char outTopic[]  = "makerfabs/out";
#else
const char inTopic[]   = "makerfabs/out";
const char outTopic[]  = "makerfabs/in";
#endif

int btn_flag=0;
lv_state_t btn_state;

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO, HSPI, true);
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RES, 1 /* rotation */, true /* IPS */);
BBCapTouch bbct;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf = (lv_color_t *)malloc(sizeof(lv_color_t) * SCREEN_WIDTH * 10);
lv_obj_t *btn;
lv_obj_t *label;

void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
  gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full,
                          area->x2 - area->x1 + 1,
                          area->y2 - area->y1 + 1);
  lv_disp_flush_ready(disp);
}

void my_touch_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
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

void btn_event_cb(lv_event_t *e)
{
  btn_flag=1;
  Serial.println("Button clicked!");
}

void setup()
{
  Serial.begin(115200);
  delay(1000);
  Serial.println("LVGL Demo Start");

  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, 1);

  gfx->begin();
  gfx->fillScreen(BLACK);

  bbct.init(TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT);

  lv_init();

  lv_disp_draw_buf_init(&draw_buf, buf, NULL, SCREEN_WIDTH * 10);

  static lv_disp_drv_t disp_drv;
  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_WIDTH;
  disp_drv.ver_res = SCREEN_HEIGHT;
  disp_drv.flush_cb = my_disp_flush;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);

  static lv_indev_drv_t indev_drv;
  lv_indev_drv_init(&indev_drv);
  indev_drv.type = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = my_touch_read;
  lv_indev_drv_register(&indev_drv);

  btn = lv_btn_create(lv_scr_act());
  lv_obj_set_size(btn, 160, 100);
  lv_obj_center(btn);
  lv_obj_add_flag(btn, LV_OBJ_FLAG_CHECKABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS);
  lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, NULL);

  label = lv_label_create(btn);
  lv_label_set_text(label, "Click Me!");
  lv_obj_center(label);

  WiFi.begin(ssid, pass);

  while (WiFi.status()!= WL_CONNECTED)
  {
    Serial.print(".");
    delay(1000);
  }

  if (!mqttClient.connect(broker, port))
  {
    Serial.print("MQTT connection failed! Error code = ");
    Serial.println(mqttClient.connectError());

    while (1);
  }

  Serial.println("You're connected to the MQTT broker!");
  Serial.println();

  // set the message receive callback
  mqttClient.onMessage(onMqttMessage);

  Serial.print("Subscribing to topic: ");
  Serial.println(inTopic);
  Serial.println();

  // subscribe to a topic
  // the second parameter sets the QoS of the subscription,
  // the the library supports subscribing at QoS 0, 1, or 2
  int subscribeQos = 1;

  mqttClient.subscribe(inTopic, subscribeQos);

  // topics can be unsubscribed using:
  // mqttClient.unsubscribe(inTopic);

  Serial.print("Waiting for messages on topic: ");
  Serial.println(inTopic);
  Serial.println();

  Serial.println("Setup down!");

  xTaskCreatePinnedToCore(Task_TFT, "Task_TFT", 40960, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(Task_main, "Task_main", 40960, NULL, 1, NULL, 1);
}

void loop()
{
}

void Task_TFT(void *pvParameters)
{
    while (1)
    {
      lv_timer_handler();
      vTaskDelay(5);
    }
}

void Task_main(void *pvParameters)
{
  while (1)
  {
      mqttClient.poll();

      if(btn_flag==1)
      {
        bool retained = false;
        int qos = 1;
        bool dup = false;
        String payload;
        btn_state=lv_obj_get_state(btn);
        
        if(btn_state & LV_STATE_CHECKED)
        {
          payload="ON";
        }
        else
        {
          payload="OFF";
        }

        Serial.print("Sending message to topic: ");
        Serial.println(outTopic);
        Serial.println(payload);
        mqttClient.beginMessage(outTopic, payload.length(), retained, qos, dup);
        mqttClient.write((const uint8_t *)payload.c_str(), payload.length());
        mqttClient.endMessage();

        btn_flag=0;
      }
      
      vTaskDelay(50);
  }
}

void onMqttMessage(int messageSize)
{
  String receive;
  // we received a message, print out the topic and contents
  Serial.print("Received a message with topic '");
  Serial.print(mqttClient.messageTopic());
  Serial.print("', duplicate = ");
  Serial.print(mqttClient.messageDup() ? "true" : "false");
  Serial.print(", QoS = ");
  Serial.print(mqttClient.messageQoS());
  Serial.print(", retained = ");
  Serial.print(mqttClient.messageRetain() ? "true" : "false");
  Serial.print("', length ");
  Serial.print(messageSize);
  Serial.println(" bytes:");

  // use the Stream interface to print the contents
  while (mqttClient.available())
  {
    char c = (char)mqttClient.read();
    receive += c;
  }
  Serial.println("Received payload: " + receive);
  Serial.println();

  if(receive=="ON")
  {
    lv_obj_add_state(btn, LV_STATE_CHECKED);
  }
  else if(receive=="OFF")
  {
    lv_obj_clear_state(btn, LV_STATE_CHECKED);
  }
  Serial.println();
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