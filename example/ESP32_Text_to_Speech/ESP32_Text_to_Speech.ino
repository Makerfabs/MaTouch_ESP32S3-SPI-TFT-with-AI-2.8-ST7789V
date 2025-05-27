/*
Author: Charlin
Date:2025.5.27
Code version: V1.0.0
Note: 

Library version:
Arduino IDE 2.3.4
esp32 V2.0.10
ESP32-audioI2S-master v2.0.0

Tools:
Flash size: 16MB(128Mb)
Partition Schrme: 16M Flash(3MB APP/9.9MB FATFS)
PSRAM: OPI PSRAM
*/
#include "Arduino.h"
#include "WiFi.h"
#include "Audio.h"

#define I2S_DOUT      19
#define I2S_BCLK      20
#define I2S_LRC       1

Audio audio;

const char* ssid = "TP-LINK_401";
const char* password = "20160704";


void setup()
{

  Serial.begin(115200);
  //  Serial2.begin(115200, SERIAL_8N1, RXp2,TXp2);

  WiFi.disconnect();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  while (WiFi.status() != WL_CONNECTED)
    delay(1500);

  Serial.println("connected");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
  
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(100);
  audio.connecttospeech("Starting Text To Speech", "en"); // Google TTS

}

void loop()
{
if (Serial.available()){
    String Answer = Serial.readString();
    Serial.println(Answer);
    audio.connecttospeech(Answer.c_str(), "en");
  }
  audio.loop();

}

void audio_info(const char *info) {
  Serial.print("audio_info: "); Serial.println(info);}
