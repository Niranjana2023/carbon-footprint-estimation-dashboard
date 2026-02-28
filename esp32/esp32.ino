/*
 * Carbon Footprint Dashboard - ESP32 Controller
 *
 * Connects to WiFi, reads ADC pins (current sensors), sends data to /process-data
 * every 200ms, and sets relay outputs from API control_states (D0-D8).
 *
 * Arduino IDE: Install "ESP32 by Espressif Systems" board support.
 * Libraries: ArduinoJson (install via Sketch -> Include Library -> Manage Libraries)
 *
 * Wiring (edit ADC_PINS and RELAY_PINS below to match your board):
 *   ADC:  A0-A8 -> GPIO 36, 39, 34, 35, 32, 33, 25, 26, 27
 *   Relay: D0-D8 -> GPIO 2, 4, 5, 18, 19, 21, 22, 23, 13
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

// ----- WiFi (edit to match your network) -----
#define WIFI_SSID     "TECNO"
#define WIFI_PASSWORD "anju@2005"

// ----- API -----
#define API_BASE_URL "https://39fe-2403-a080-1c-384c-b536-6fa3-c6ef-968c.ngrok-free.app"
#define PROCESS_DATA_PATH "/process-data"
#define REQUEST_INTERVAL_MS 200

// ----- ADC pins (logical A0-A8 -> GPIO). ESP32: 36, 39, 34, 35, 32, 33, 25, 26, 27 -----
const int ADC_PINS[] = { 36, 39, 34, 35, 32, 33, 25, 26, 27 };
const char* ADC_KEYS[] = { "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7", "A8" };
#define NUM_ADC 9

// ----- Relay output pins (D0-D8 -> GPIO). Edit to match your relay board -----
const int RELAY_PINS[] = { 2, 4, 5, 18, 19, 21, 22, 23, 13 };
const char* RELAY_KEYS[] = { "D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7", "D8" };
#define NUM_RELAYS 9

// ----- ADC config (match app.py) -----
#define ADC_ATTEN     ADC_11db
#define ADC_WIDTH     ADC_WIDTH_12Bit
#define ADC_SAMPLES   4

WiFiClientSecure client;
unsigned long lastRequestMs = 0;
bool wifiConnected = false;

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n[Carbon Footprint] ESP32 controller starting");

  // Configure ADC pins as inputs
  analogReadResolution(12);
  analogSetAttenuation(ADC_ATTEN);
  for (int i = 0; i < NUM_ADC; i++) {
    pinMode(ADC_PINS[i], INPUT);
  }

  // Configure relay outputs (NO: HIGH = off at startup)
  for (int i = 0; i < NUM_RELAYS; i++) {
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], HIGH);
  }

  connectWiFi();
  // Accept ngrok HTTPS certificate (no CA pin for dynamic ngrok URLs)
  client.setInsecure();
}

void loop() {
  if (!wifiConnected) {
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
    }
    delay(500);
    return;
  }

  unsigned long now = millis();
  if (now - lastRequestMs >= REQUEST_INTERVAL_MS) {
    lastRequestMs = now;
    sendProcessDataAndUpdateRelays();
  }
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    return;
  }
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n[WiFi] Connected");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
  } else {
    wifiConnected = false;
    Serial.println("\n[WiFi] Failed");
  }
}

void sendProcessDataAndUpdateRelays() {
  // Build JSON body: { "A0": value, ..., "D0": 0/1, ... } (ADC + current digital output states)
  StaticJsonDocument<768> doc;
  for (int i = 0; i < NUM_ADC; i++) {
    long sum = 0;
    for (int s = 0; s < ADC_SAMPLES; s++) {
      sum += analogRead(ADC_PINS[i]);
      delay(1);
    }
    int avg = (int)(sum / ADC_SAMPLES);
    doc[ADC_KEYS[i]] = avg;
  }
  // Report state for NO relay: LOW = on (1), HIGH = off (0)
  for (int i = 0; i < NUM_RELAYS; i++) {
    doc[RELAY_KEYS[i]] = digitalRead(RELAY_PINS[i]) == LOW ? 1 : 0;
  }

  String body;
  serializeJson(doc, body);

  HTTPClient http;
  String url = String(API_BASE_URL) + PROCESS_DATA_PATH;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  // ngrok free tier often requires this header to avoid browser interstitial
  http.addHeader("ngrok-skip-browser-warning", "true");

  int code = http.POST(body);

  if (code != 200) {
    Serial.print("[API] POST /process-data failed: ");
    Serial.println(code);
    if (code > 0) {
      Serial.println(http.getString());
    }
    http.end();
    return;
  }

  String response = http.getString();
  http.end();

  // Parse response and update relays: control_states["D0"] = true/false, ...
  StaticJsonDocument<512> resDoc;
  DeserializationError err = deserializeJson(resDoc, response);
  if (err) {
    Serial.print("[API] JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  JsonObject control = resDoc["control_states"];
  if (control.isNull()) {
    Serial.println("[API] No control_states in response");
    return;
  }

  // NO relay: LOW = on (close contact), HIGH = off (open)
  for (int i = 0; i < NUM_RELAYS; i++) {
    bool on = control[RELAY_KEYS[i]] | false;
    digitalWrite(RELAY_PINS[i], on ? LOW : HIGH);
  }
}
