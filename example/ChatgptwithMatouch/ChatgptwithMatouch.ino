/*
Author: Charlin
Date:2025.5.27
Code version: V1.0.0
Note: 

Library version:
Arduino IDE 2.3.4
esp32 V2.0.10
ArduinoJson v7.2.0
*/
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// Replace with your network credentials
const char* ssid     = "YOUR-SSID";
const char* password = "YOUR-PIN";

// Replace with your OpenAI API key
const char* apiKey = "YOUR-APIKEY";

String apiUrl = "https://api.openai.com/v1/chat/completions";
String finalPayload = "";

bool initialPrompt = true;
bool gettingResponse = true;

HTTPClient http;
  
void setup() 
{
  // Initialize Serial
  Serial.begin(115200);
  // Connect to Wi-Fi network
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ..");
  Serial.println(ssid);
  while (WiFi.status() != WL_CONNECTED) 
  {
      Serial.print('.');
      delay(1000);
  }
  Serial.println("Connected");
  Serial.println();
  Serial.println(WiFi.localIP());

  http.begin(apiUrl);
}

void loop() 
{
    if(Serial.available() > 0)
    {
      String prompt = Serial.readStringUntil('\n');
      prompt.trim();
      Serial.print("USER:");
      Serial.println(prompt);
      gettingResponse = true;
      chatGptCall(prompt);
    }
    delay(1);
} 
void chatGptCall(String payload)
{
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + String(apiKey));
  
  if(initialPrompt)
  {
    finalPayload = "{\"model\": \"gpt-4.1\",\"messages\": [{\"role\": \"user\", \"content\": \"" + payload + "\"}]}";
    //finalPayload = "{\"model\": \"gpt-3.5-turbo\",\"messages\": [{\"role\": \"user\", \"content\": \"" + inputText1 + "\"}]}";
    initialPrompt = false;
  }
  else
  {
    finalPayload = finalPayload + ",{\"role\": \"user\", \"content\": \"" + payload + "\"}]}";
  }
  //Serial.println(finalPayload);
  while(gettingResponse)
  {
  int httpResponseCode = http.POST(finalPayload);
  if (httpResponseCode == 200) {
    String response = http.getString(); 
    // Parse JSON response
    DynamicJsonDocument jsonDoc(1024);
    deserializeJson(jsonDoc, response);
    String outputText = jsonDoc["choices"][0]["message"]["content"];
    outputText.remove(outputText.indexOf('\n'));
    Serial.print("CHATGPT: ");
    Serial.println(outputText);
    String returnResponse = "{\"role\": \"assistant\", \"content\": \"" + outputText + "\"}";
    finalPayload = removeEndOfString(finalPayload);
    finalPayload = finalPayload + "," + returnResponse;
    gettingResponse = false;
  } 
  else 
  {
    //Serial.printf("Error %i \n", httpResponseCode);
   // Serial.println("Trying again");
  }
getDelay();
  }
}
String removeEndOfString(String originalString)
{
  int stringLength = originalString.length();
  String newString = originalString.substring(0, stringLength - 2);
  return(newString);
}
void getDelay()
{
  unsigned long initialMillis = millis();
  while ((initialMillis + 5000) >= millis()) 
  {    
  }
}
