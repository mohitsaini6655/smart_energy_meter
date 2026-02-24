/*
  ESP32 Smart Energy Meter PRO
  Firebase + Telegram + LCD + 4-Channel Relay Control
  Author: Mohit Saini
  Version: 2.0
*/

#include "EmonLib.h"
#include <EEPROM.h>
#include <WiFi.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Firebase_ESP_Client.h>

// ---------------- LCD ----------------
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ---------------- WiFi ---------------
const char* ssid = "0000";
const char* pass = "00000000";

// ---------------- Firebase ------------
#define API_KEY " your firbase API KEY"
#define DATABASE_URL "your Database url"
#define USER_EMAIL "your email using firbase "
#define USER_PASSWORD "email password"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---------------- Telegram ------------
const char* telegramBotToken = "your telegrambot token";
const char* telegramChatID   = "your telegram chat id ";

// ---------------- Relay Pins ---------
#define RELAY1_PIN 26   // Main Relay
#define RELAY2_PIN 25   // Additional Relay 1
#define RELAY3_PIN 33   // Additional Relay 2
#define RELAY4_PIN 32   // Additional Relay 3

// ---------------- Calibration ---------
const float vCalibration = 167.4;
const float currCalibration = 3.0;

// ---------------- Energy Monitor ------
EnergyMonitor emon;

// ---------------- Energy Data ----------
float kWh = 0.0;
float cost = 0.0;
const float ratePerkWh = 6.5;

unsigned long lastMillis = 0;
unsigned long lastTelegram = 0;
unsigned long lastFirebaseCheck = 0;

// ---------------- EEPROM --------------
const int addrKWh  = 12;
const int addrCost = 16;

// ---------------- Smoothing ------------
const float SMOOTH_ALPHA = 0.2;
float smoothV = 0, smoothI = 0, smoothP = 0;
bool smoothInit = false;

// ---------------- Relay States ---------
bool relay1State = false;
bool relay2State = false;
bool relay3State = false;
bool relay4State = false;

// =====================================================
void setup() {

  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n=== SMART ENERGY METER PRO STARTING ===\n");

  // Initialize Relay Pins
  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  pinMode(RELAY3_PIN, OUTPUT);
  pinMode(RELAY4_PIN, OUTPUT);
  
  // Start all relays OFF
  digitalWrite(RELAY1_PIN, LOW);
  digitalWrite(RELAY2_PIN, LOW);
  digitalWrite(RELAY3_PIN, LOW);
  digitalWrite(RELAY4_PIN, LOW);

  // ADC Configuration
  analogReadResolution(12);
  analogSetPinAttenuation(34, ADC_11db);
  analogSetPinAttenuation(35, ADC_11db);

  // WiFi Connection
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, pass);
  int wifiAttempts = 0;
  while (WiFi.status() != WL_CONNECTED && wifiAttempts < 30) {
    delay(500);
    Serial.print(".");
    wifiAttempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✅ WiFi Connected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n❌ WiFi Failed!");
  }

  // Firebase Configuration
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
  Serial.println("✅ Firebase Initialized");

  // LCD Initialization
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Smart Energy");
  lcd.setCursor(0, 1);
  lcd.print("Meter PRO v2.0");
  delay(2000);
  lcd.clear();

  // EEPROM Initialization
  EEPROM.begin(32);
  EEPROM.get(addrKWh, kWh);
  EEPROM.get(addrCost, cost);
  
  // Energy Monitor Setup
  emon.voltage(35, vCalibration, 1.0);
  emon.current(34, currCalibration);

  lastMillis = millis();
  lastFirebaseCheck = millis();
  
  Serial.println("✅ System Ready!\n");
}

// =====================================================
void loop() {

  calculateEnergy();              // Calculate power, energy
  checkRelaysFromFirebase();      // Check relay commands
  updateFirebaseData();           // Send data to Firebase
  updateLCD();                    // Update LCD display
  sendTelegramAlert();            // Send Telegram alerts
}

// =====================================================
void calculateEnergy() {

  emon.calcVI(20, 2000);

  float Vrms = emon.Vrms;
  float realPower = emon.realPower;

  // Filter invalid readings
  if (Vrms < 10) Vrms = 0;
  if (realPower < 5) realPower = 0;

  float Ireal = (Vrms > 0) ? realPower / Vrms : 0;

  // Smooth readings
  if (!smoothInit) {
    smoothV = Vrms;
    smoothI = Ireal;
    smoothP = realPower;
    smoothInit = true;
  } else {
    smoothV = SMOOTH_ALPHA * Vrms + (1 - SMOOTH_ALPHA) * smoothV;
    smoothI = SMOOTH_ALPHA * Ireal + (1 - SMOOTH_ALPHA) * smoothI;
    smoothP = SMOOTH_ALPHA * realPower + (1 - SMOOTH_ALPHA) * smoothP;
  }

  // Calculate Energy (kWh)
  unsigned long now = millis();
  kWh += smoothP * (now - lastMillis) / 3600000000.0;
  lastMillis = now;

  // Calculate Cost
  cost = kWh * ratePerkWh;

  // Save to EEPROM every hour
  static unsigned long lastSave = 0;
  if (now - lastSave > 3600000) { // 1 hour
    EEPROM.put(addrKWh, kWh);
    EEPROM.put(addrCost, cost);
    EEPROM.commit();
    lastSave = now;
    Serial.println("💾 Data saved to EEPROM");
  }
}

// =====================================================
void checkRelaysFromFirebase() {

  if (!Firebase.ready()) return;

  // Check every 2 seconds
  if (millis() - lastFirebaseCheck < 2000) return;
  lastFirebaseCheck = millis();

  // Check Main Relay (Relay 1)
  if (Firebase.RTDB.getInt(&fbdo, "/RelayControl/main")) {
    int newState = fbdo.intData();
    if (newState != relay1State) {
      relay1State = newState;
      digitalWrite(RELAY1_PIN, relay1State ? HIGH : LOW);
      Serial.printf("🔌 Relay 1: %s\n", relay1State ? "ON" : "OFF");
      
      // Update status back to Firebase
      Firebase.RTDB.setBool(&fbdo, "/RelayStatus/main", relay1State);
    }
  }

  // Check Relay 2
  if (Firebase.RTDB.getInt(&fbdo, "/RelayControl/relay2")) {
    int newState = fbdo.intData();
    if (newState != relay2State) {
      relay2State = newState;
      digitalWrite(RELAY2_PIN, relay2State ? HIGH : LOW);
      Serial.printf("🔌 Relay 2: %s\n", relay2State ? "ON" : "OFF");
      Firebase.RTDB.setBool(&fbdo, "/RelayStatus/relay2", relay2State);
    }
  }

  // Check Relay 3
  if (Firebase.RTDB.getInt(&fbdo, "/RelayControl/relay3")) {
    int newState = fbdo.intData();
    if (newState != relay3State) {
      relay3State = newState;
      digitalWrite(RELAY3_PIN, relay3State ? HIGH : LOW);
      Serial.printf("🔌 Relay 3: %s\n", relay3State ? "ON" : "OFF");
      Firebase.RTDB.setBool(&fbdo, "/RelayStatus/relay3", relay3State);
    }
  }

  // Check Relay 4
  if (Firebase.RTDB.getInt(&fbdo, "/RelayControl/relay4")) {
    int newState = fbdo.intData();
    if (newState != relay4State) {
      relay4State = newState;
      digitalWrite(RELAY4_PIN, relay4State ? HIGH : LOW);
      Serial.printf("🔌 Relay 4: %s\n", relay4State ? "ON" : "OFF");
      Firebase.RTDB.setBool(&fbdo, "/RelayStatus/relay4", relay4State);
    }
  }
}

// =====================================================
void updateFirebaseData() {

  if (!Firebase.ready()) return;

  // Update every 5 seconds
  static unsigned long lastFirebaseUpdate = 0;
  if (millis() - lastFirebaseUpdate < 5000) return;
  lastFirebaseUpdate = millis();

  // Send energy data to Firebase
  Firebase.RTDB.setFloat(&fbdo, "/EnergyMeter/voltage", smoothV);
  Firebase.RTDB.setFloat(&fbdo, "/EnergyMeter/current", smoothI);
  Firebase.RTDB.setFloat(&fbdo, "/EnergyMeter/power", smoothP);
  Firebase.RTDB.setFloat(&fbdo, "/EnergyMeter/energy", kWh);
  Firebase.RTDB.setFloat(&fbdo, "/EnergyMeter/cost", cost);
  
  // Update timestamp
  Firebase.RTDB.setString(&fbdo, "/EnergyMeter/lastUpdate", String(millis()));
  
  Serial.printf("📊 Data Sent: %.1fV, %.2fA, %.0fW, %.2fkWh\n", 
                smoothV, smoothI, smoothP, kWh);
}

// =====================================================
void updateLCD() {

  static unsigned long lastLCDUpdate = 0;
  if (millis() - lastLCDUpdate < 1000) return;  // Update every 1 second
  lastLCDUpdate = millis();

  lcd.clear();
  
  // Line 1: Voltage and Current
  lcd.setCursor(0, 0);
  lcd.printf("V:%.1f I:%.2f", smoothV, smoothI);
  
  // Line 2: Energy and Relay Status
  lcd.setCursor(0, 1);
  lcd.printf("E:%.2f kWh", kWh);
  
  // Show relay status on right side
  lcd.setCursor(13, 1);
  if (relay1State || relay2State || relay3State || relay4State) {
    lcd.print("R*");
  } else {
    lcd.print("R-");
  }
}

// =====================================================
void sendTelegramAlert() {

  if (millis() - lastTelegram < 60000) return;  // Every 1 minute
  if (smoothP < 10) return;  // No load

  lastTelegram = millis();

  // Check if power exceeds threshold
  static float lastPower = 0;
  if (abs(smoothP - lastPower) < 50) return;  // No significant change
  
  lastPower = smoothP;

  String msg = "⚡ **Smart Energy Meter Update** ⚡\n\n";
  msg += "📊 **Current Readings:**\n";
  msg += "┌─────────────────────\n";
  msg += "│ Voltage  : " + String(smoothV, 1) + " V\n";
  msg += "│ Current  : " + String(smoothI, 2) + " A\n";
  msg += "│ Power    : " + String(smoothP, 0) + " W\n";
  msg += "│ Energy   : " + String(kWh, 2) + " kWh\n";
  msg += "│ Cost     : ₹ " + String(cost, 2) + "\n";
  msg += "└─────────────────────\n\n";
  
  msg += "🔌 **Relay Status:**\n";
  msg += "Relay 1: " + String(relay1State ? "✅ ON" : "⭕ OFF") + "\n";
  msg += "Relay 2: " + String(relay2State ? "✅ ON" : "⭕ OFF") + "\n";
  msg += "Relay 3: " + String(relay3State ? "✅ ON" : "⭕ OFF") + "\n";
  msg += "Relay 4: " + String(relay4State ? "✅ ON" : "⭕ OFF") + "\n\n";
  
  msg += "🕐 " + String(millis()/1000) + "s uptime";

  HTTPClient http;
  http.begin("https://api.telegram.org/bot" + String(telegramBotToken) + "/sendMessage");
  http.addHeader("Content-Type", "application/json");

  DynamicJsonDocument doc(512);
  doc["chat_id"] = telegramChatID;
  doc["text"] = msg;
  doc["parse_mode"] = "Markdown";

  String payload;
  serializeJson(doc, payload);
  
  int httpCode = http.POST(payload);
  if (httpCode > 0) {
    Serial.println("✅ Telegram message sent");
  } else {
    Serial.println("❌ Telegram failed");
  }
  http.end();
}

// =====================================================
// Manual relay control function (optional)
void setRelay(int relayNum, bool state) {
  
  switch(relayNum) {
    case 1:
      digitalWrite(RELAY1_PIN, state ? HIGH : LOW);
      relay1State = state;
      Firebase.RTDB.setBool(&fbdo, "/RelayStatus/main", state);
      break;
    case 2:
      digitalWrite(RELAY2_PIN, state ? HIGH : LOW);
      relay2State = state;
      Firebase.RTDB.setBool(&fbdo, "/RelayStatus/relay2", state);
      break;
    case 3:
      digitalWrite(RELAY3_PIN, state ? HIGH : LOW);
      relay3State = state;
      Firebase.RTDB.setBool(&fbdo, "/RelayStatus/relay3", state);
      break;
    case 4:
      digitalWrite(RELAY4_PIN, state ? HIGH : LOW);
      relay4State = state;
      Firebase.RTDB.setBool(&fbdo, "/RelayStatus/relay4", state);
      break;
  }
  
  Serial.printf("🔧 Manual: Relay %d set to %s\n", relayNum, state ? "ON" : "OFF");
}
