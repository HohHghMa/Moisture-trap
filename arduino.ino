#ifdef ESP32
 #include <WiFi.h>
 #include <HTTPClient.h>
 #include <WiFiClientSecure.h>
#elif defined(ESP8266)
 #include <ESP8266WiFi.h>
 #include <ESP8266HTTPClient.h>
 #include <WiFiClientSecure.h>
#endif

#include <WiFiManager.h>
#include <ArduinoJson.h>
#include "DHT.h"

// --- Configuration ---
#define RELAY_PIN 32
#define DHTPIN 14
#define DHTTYPE DHT11

// Discord Configuration
const char* discord_token = "วาง Token Discord bot";
const char* channel_id = "วาง Discord Channel ID";
const char* mentor_user_id = "วาง ID ของคนที่เป็นเจ้าของสำหรับแจ้งเตือน";

DHT dht(DHTPIN, DHTTYPE);

// --- State Variables ---
bool fanState = false;          // Desired Fan State (Manual or Auto decision)
bool autoMode = false;          // Auto Mode (Humidity Control)
bool overheatMode = false;      // Overheat Protection Mode
String lastMessageId = "";

// --- Timing Variables ---
unsigned long lastDiscordCheck = 0;
const long discordInterval = 3000;

// Overheat Protection Variables
unsigned long currentRunStartTime = 0;
bool isResting = false;
unsigned long restStartTime = 0;
const unsigned long MAX_RUN_TIME = 600000; // 10 minutes
const unsigned long REST_TIME = 60000;     // 1 minute

// Auto Mode Timer
unsigned long lastAutoCheck = 0;
const long autoInterval = 2000;

void sendDiscordMessage(String message) {
 if (WiFi.status() != WL_CONNECTED) return;
 HTTPClient http;
 WiFiClientSecure client;
 client.setInsecure();
 if (http.begin(client, String("https://discord.com/api/v10/channels/") + channel_id + "/messages")) {
   http.addHeader("Authorization", String("Bot ") + discord_token);
   http.addHeader("Content-Type", "application/json");
   StaticJsonDocument<512> doc;
   doc["content"] = message;
   String jsonBody;
   serializeJson(doc, jsonBody);
   int httpCode = http.POST(jsonBody);
   if (httpCode > 0) Serial.printf("[Discord] Sent: %d\n", httpCode);
   else Serial.printf("[Discord] Error: %s\n", http.errorToString(httpCode).c_str());
   http.end();
 }
}

void checkDiscordCommands() {
 if (WiFi.status() != WL_CONNECTED) return;
 HTTPClient http;
 WiFiClientSecure client;
 client.setInsecure();
  if (http.begin(client, String("https://discord.com/api/v10/channels/") + channel_id + "/messages?limit=1")) {
   http.addHeader("Authorization", String("Bot ") + discord_token);
   int httpCode = http.GET();
   if (httpCode == 200) {
     String payload = http.getString();
     DynamicJsonDocument doc(2048);
     DeserializationError error = deserializeJson(doc, payload);
     if (!error && doc.size() > 0) {
       String msgContent = doc[0]["content"].as<String>();
       String msgId = doc[0]["id"].as<String>();
      
       if (msgId != lastMessageId) {
         lastMessageId = msgId;
         msgContent.toLowerCase();
         Serial.println("[Discord] Cmd: " + msgContent);

         if (msgContent.indexOf("!status") >= 0) {
            float h = dht.readHumidity();
            String statusMsg = "📊 **System Status**\n";
            statusMsg += "🌡️ Humidity: **" + String(h, 1) + "%**\n";
            statusMsg += "🤖 Auto Mode: **" + String(autoMode ? "ON" : "OFF") + "**\n";
            statusMsg += "🛡️ Overheat Protection: **" + String(overheatMode ? "ON" : "OFF") + "**\n";
            statusMsg += "❄️ Fan State: **" + String(digitalRead(RELAY_PIN) == LOW ? "ON" : "OFF") + "**";
            if (overheatMode && isResting) statusMsg += " (recovering...)";
            sendDiscordMessage(statusMsg);
         }
         else if (msgContent.indexOf("!auto on") >= 0) {
           autoMode = true;
           sendDiscordMessage("🤖 Auto Mode: **ON** (Fan controlled by humidity > 60%)");
         }
         else if (msgContent.indexOf("!auto off") >= 0) {
           autoMode = false;
           sendDiscordMessage("🤖 Auto Mode: **OFF** (Manual control enabled)");
         }
         else if (msgContent.indexOf("!overheat on") >= 0) {
           overheatMode = true;
           currentRunStartTime = millis(); // Reset timer
           isResting = false;
           sendDiscordMessage("🛡️ Overheat Protection: **ON** (10m ON / 1m OFF cycle enabled)");
         }
         else if (msgContent.indexOf("!overheat off") >= 0) {
           overheatMode = false;
           isResting = false;
           sendDiscordMessage("'️ Overheat Protection: **OFF**");
         }
         else if (msgContent.indexOf("!fan on") >= 0) {
           if (autoMode) {
              sendDiscordMessage("⚠️ Cannot manually control fan while **Auto Mode** is ON. Disable auto mode first (!auto off).");
           } else {
              fanState = true;
              sendDiscordMessage("✅ Manual Fan: **ON**");
           }
         }
         else if (msgContent.indexOf("!fan off") >= 0) {
           if (autoMode) {
              sendDiscordMessage("⚠️ Cannot manually control fan while **Auto Mode** is ON. Disable auto mode first (!auto off).");
           } else {
              fanState = false;
              sendDiscordMessage("🛑 Manual Fan: **OFF**");
           }
         }
       }
     }
   }
   http.end();
 }
}

void runFanLogic() {
 // 1. Determine Desired State
 bool desiredState = fanState;

 // Auto Mode Logic
 if (autoMode) {
   if (millis() - lastAutoCheck > autoInterval) {
     lastAutoCheck = millis();
     float h = dht.readHumidity();
     if (!isnan(h)) {
       if (h > 60.0) {
          desiredState = true;
          // If we just switched to ON from OFF in auto mode
          if (!fanState) {
            fanState = true;
            // Optional: Notify? sendDiscordMessage("💧 Humidity > 60%. Auto Fast Start.");
          }
       } else {
          desiredState = false;
          if (fanState) { fanState = false; }
       }
     }
   }
   desiredState = fanState; // Sync local var
 }

 // 2. Apply Overheat Protection Logic (Duty Cycle)
 bool finalRelayState = desiredState; // True = ON (LOW)

 if (desiredState && overheatMode) {
   unsigned long now = millis();
   if (!isResting) {
      if (now - currentRunStartTime >= MAX_RUN_TIME) {
         isResting = true;
         restStartTime = now;
         finalRelayState = false; // Force OFF
         sendDiscordMessage("🛡️ Overheat Protection: Resting for 1 minute...");
      }
   } else {
      if (now - restStartTime >= REST_TIME) {
         isResting = false;
         currentRunStartTime = now;
         finalRelayState = true; // Resume ON
         sendDiscordMessage("🛡️ Overheat Protection: Resuming operation.");
      } else {
         finalRelayState = false; // Keep OFF
      }
   }
 } else {
   // If fan is OFF or Overheat protection disabled, reset protection state
   if (!desiredState) {
      currentRunStartTime = millis();
      isResting = false;
   }
 }

 // 3. Hardware Actuation
 // Relay is Active LOW (LOW = ON, HIGH = OFF)
 digitalWrite(RELAY_PIN, finalRelayState ? LOW : HIGH);
}

void setup() {
 Serial.begin(115200);
 pinMode(RELAY_PIN, OUTPUT);
 digitalWrite(RELAY_PIN, HIGH); // Default OFF
 dht.begin();
 WiFiManager wm;
 if (!wm.autoConnect("ESP32_Peltier_Control")) ESP.restart();
 Serial.println("Ready!");
 sendDiscordMessage("🤖 System Online. waiting for commands...");
}

void loop() {
 runFanLogic();
 if (millis() - lastDiscordCheck > discordInterval) {
   lastDiscordCheck = millis();
   checkDiscordCommands();
 }
}
