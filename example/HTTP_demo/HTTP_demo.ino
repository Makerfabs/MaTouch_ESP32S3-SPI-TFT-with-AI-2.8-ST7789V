/*
Author: Yuki
Date:2025.5.19
Code version: V1.0.0
Note: 

Library version:
Arduino IDE 2.3.4
esp32 V2.0.16
GFX Library for Arduino v1.5.6
DHT sensor library v1.4.6
Adafruit Unified Sensor v1.1.14
FastLED v3.7.7
*/

#include <Arduino_GFX_Library.h>
#include "DHT.h"
#include <WiFi.h>
#include <FastLED.h>

//WS2812
#define NUM_LEDS 1
#define LED_PIN 0
CRGB leds[NUM_LEDS];

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

#define DHTPIN 7
#define DHTTYPE DHT11
DHT dht(DHTPIN, DHTTYPE);

float temp, humi;

const char* ssid     = "YOUR-SSID"; // Change this to your WiFi SSID
const char* password = "YOUR-PASSWORD"; // Change this to your WiFi password

const char* host = "api.thingspeak.com"; // This should not be changed
const int httpPort = 80; // This should not be changed
const String writeApiKey = "YOUR-API-KEY"; // Change this to your Write API key

Arduino_ESP32SPI *bus = new Arduino_ESP32SPI(TFT_DC, TFT_CS, TFT_SCLK, TFT_MOSI, TFT_MISO, HSPI, true); // Constructor
Arduino_GFX *gfx = new Arduino_ST7789(bus, TFT_RES, 1 /* rotation */, true /* IPS */);

void setup() 
{
  Serial.begin(115200);
  Serial.println("HTTP demo!");

  dht.begin();
  FastLED.addLeds<WS2812, LED_PIN, RGB>(leds, NUM_LEDS);//ssd1306初始化
  leds[0] = CRGB::Green;
  FastLED.show();
  Serial.print("Connecting to ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  
  pinMode(TFT_BLK, OUTPUT);
  digitalWrite(TFT_BLK, 1);

  gfx->begin();
  gfx->fillScreen(BLACK);
  gfx->setTextSize(4);
  gfx->setCursor(10, 10);
  gfx->setTextColor(RED);
  gfx->println(F("HTTP DEMO"));

  while (WiFi.status() != WL_CONNECTED) 
  {
    delay(500);
    Serial.print(".");
  }

  leds[0] = CRGB::Red;
  FastLED.show();

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());
}


void loop() 
{
  WiFiClient client;
  String footer = String(" HTTP/1.1\r\n") + "Host: " + String(host) + "\r\n" + "Connection: close\r\n\r\n";
  
  temp = dht.readTemperature();
  humi = dht.readHumidity();
  if (!client.connect(host, httpPort)) {
    return;
  }

  client.print("GET /update?api_key=" + writeApiKey + "&field1=" + temp + "&field2=" + humi + footer);
  readResponse(&client);
  
  Serial.print("Temperature: "); Serial.print(temp); Serial.println(" degrees C");
  Serial.print("Humidity: "); Serial.print(humi); Serial.println("% rH");Serial.println();
  
  gfx->fillScreen(BLACK);
  gfx->setCursor(10, 10);
  gfx->setTextSize(4);
  gfx->println("HTTP DEMO");

  gfx->setCursor(0, 100);
  gfx->setTextSize(3);
  gfx->print("Temperature:");gfx->println(temp);
  gfx->print("Humidity:");gfx->println(humi);
  
  delay(10000);//每10s传一次
}

void readResponse(WiFiClient *client)
{
  unsigned long timeout = millis();
  while(client->available() == 0)
  {
    if(millis() - timeout > 5000)
    {
      Serial.println(">>> Client Timeout !");
      client->stop();
      return;
    }
  }

  // Read all the lines of the reply from server and print them to Serial
  while(client->available()) 
  {
    String line = client->readStringUntil('\r');
    Serial.print(line);
  }

  Serial.printf("\nClosing connection\n\n");
}
