/*
 * Carbon Footprint Dashboard - ESP32 Controller (30-pin board, 3 sockets)
 *
 * Connects to WiFi, reads ADC pins (current sensors), sends data to /process-data
 * every 200ms, and sets relay outputs from API control_states (D0-D2).
 *
 * Arduino IDE: Install "ESP32 by Espressif Systems" board support.
 * Libraries: ArduinoJson (install via Sketch -> Include Library -> Manage Libraries)
 *
 * ESP32 30-pin board wiring:
 *   ADC:   A0-A2 -> GPIO 36 (VP), 39 (VN), 34
 *   Relay: D0-D2 -> GPIO 25, 26, 27
 *
 * ZMCT103C sensor mapping:
 *   Sensor Vcc = 5V, max analog output = Vcc/2 = 2.5V (safe for 3.3V ESP32 pins).
 *   Zero-current DC offset = 2.5V -> ADC ~3102 on a 0-3.3V / 12-bit scale.
 *   Raw ADC values are sent to the server; voltage/current mapping happens there.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>

// ----- WiFi (edit to match your network) -----
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// ----- API -----
#define API_BASE_URL "https://39fe-2403-a080-1c-384c-b536-6fa3-c6ef-968c.ngrok-free.app"
#define PROCESS_DATA_PATH "/process-data"
#define REQUEST_INTERVAL_MS 200

// ----- ADC pins (3 sockets: A0-A2 -> GPIO on 30-pin ESP32) -----
const int ADC_PINS[] = { 36, 39, 34 };
const char* ADC_KEYS[] = { "A0", "A1", "A2" };
#define NUM_ADC 3

// ----- Relay output pins (3 sockets: D0-D2 -> GPIO on 30-pin ESP32) -----
const int RELAY_PINS[] = { 25, 26, 27 };
const char* RELAY_KEYS[] = { "D0", "D1", "D2" };
#define NUM_RELAYS 3

// ----- ADC config (match app.py) -----
#define ADC_ATTEN     ADC_11db
#define ADC_WIDTH     ADC_WIDTH_12Bit
#define ADC_SAMPLES   4

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

  // Use a fresh WiFiClientSecure per request to avoid ESP32 TLS/reuse bugs
  // (second request often fails with a single reused client)
  WiFiClientSecure client;
  client.setInsecure();

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
  StaticJsonDocument<1024> resDoc;
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
  // Re-assert OUTPUT and write so pins stay driven (avoids 2.5V float from high-Z)
  for (int i = 0; i < NUM_RELAYS; i++) {
    bool on = control[RELAY_KEYS[i]] | false;
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], on ? LOW : HIGH);
  }
}
