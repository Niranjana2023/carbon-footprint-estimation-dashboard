/*
 * Carbon Footprint Dashboard - ESP32 Controller (30-pin board, 3 sockets)
 *
 * Connects to WiFi, reads ADC pins (current sensors), sends data to /process-data
 * every 200ms, and sets relay outputs from API control_states (D0-D2).
 *
 * Arduino IDE: Install "ESP32 by Espressif Systems" board support.
 *
 * Libraries (Sketch -> Include Library -> Manage Libraries):
 *   - ArduinoJson
 *   - Adafruit SSD1306
 *   - Adafruit GFX Library
 *   - Keypad (by Mark Stanley / Alexander Brevig)
 *
 * ESP32 30-pin board wiring:
 *   ADC:   A0-A2 -> GPIO 36 (VP), 39 (VN), 34
 *   Relay: D0-D2 -> GPIO 25, 26, 27
 *   OLED (SSD1306 I2C): SDA -> GPIO 21, SCL -> GPIO 22, VCC 3.3V, GND
 *   4x4 Keypad: rows -> GPIO 14, 15, 16, 17 | cols -> GPIO 18, 19, 23, 4
 *     (Change KEYPAD_* pins below if they clash with your board.)
 */

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>
#include <Preferences.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <string.h>

// ----- Feature: WiFi via OLED + keypad (set 0 to use WIFI_SSID / WIFI_PASSWORD only) -----
#define USE_KEYPAD_FOR_WIFI 1

// ----- WiFi fallback when USE_KEYPAD_FOR_WIFI is 0 -----
#define WIFI_SSID     ""
#define WIFI_PASSWORD ""

// ----- API -----
#define API_BASE_URL "https://39fe-2403-a080-1c-384c-b536-6fa3-c6ef-968c.ngrok-free.app"
#define PROCESS_DATA_PATH "/process-data"
#define REQUEST_INTERVAL_MS 200

// ----- OLED (SSD1306 I2C) -----
#define OLED_SCREEN_WIDTH   128
#define OLED_SCREEN_HEIGHT  64
#define OLED_I2C_ADDR       0x3C
#define OLED_RESET          -1
Adafruit_SSD1306 display(OLED_SCREEN_WIDTH, OLED_SCREEN_HEIGHT, &Wire, OLED_RESET);

// ----- 4x4 Keypad (must not use GPIO 25,26,27 relays or 34,36,39 ADC) -----
const byte KEYPAD_ROWS = 4;
const byte KEYPAD_COLS = 4;
char KEYPAD_KEYS[KEYPAD_ROWS][KEYPAD_COLS] = {
  {'1', '2', '3', 'A'},
  {'4', '5', '6', 'B'},
  {'7', '8', '9', 'C'},
  {'*', '0', '#', 'D'}
};
byte KEYPAD_ROW_PINS[KEYPAD_ROWS] = { 14, 15, 16, 17 };
byte KEYPAD_COL_PINS[KEYPAD_COLS] = { 18, 19, 23, 4 };
Keypad keypad = Keypad(makeKeymap(KEYPAD_KEYS), KEYPAD_ROW_PINS, KEYPAD_COL_PINS, KEYPAD_ROWS, KEYPAD_COLS);

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

#define WIFI_CREDS_MAX  63

Preferences prefs;
bool g_oledOk = false;
char g_wifiSsid[WIFI_CREDS_MAX + 1];
char g_wifiPass[WIFI_CREDS_MAX + 1];

unsigned long lastRequestMs = 0;
unsigned long lastOledStatusMs = 0;
bool wifiConnected = false;

int g_lastHttpCode = -1;
bool g_lastApiOk = false;
char g_apiDetail[24] = "---";

// ----- Forward declarations -----
#if USE_KEYPAD_FOR_WIFI
void initUiHardware();
bool offerSavedNetwork(char* ssid, char* pass, size_t ssidSz, size_t passSz);
bool enterCredential(const char* title, char* buf, size_t bufSize, bool masked);
void runWifiSetupWizard();
void saveWifiCredentials(const char* ssid, const char* pass);
bool attemptConnect(const char* ssid, const char* pass);
void oledShow4(const char* l0, const char* l1, const char* l2, const char* l3);
void refreshStatusDashboard();
#endif

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

#if USE_KEYPAD_FOR_WIFI
  initUiHardware();
  prefs.begin("wifi", false);

  if (!g_oledOk) {
    // No display: use compile-time credentials (fill WIFI_SSID / WIFI_PASSWORD)
    strncpy(g_wifiSsid, WIFI_SSID, WIFI_CREDS_MAX);
    g_wifiSsid[WIFI_CREDS_MAX] = '\0';
    strncpy(g_wifiPass, WIFI_PASSWORD, WIFI_CREDS_MAX);
    g_wifiPass[WIFI_CREDS_MAX] = '\0';
    if (strlen(g_wifiSsid) == 0) {
      Serial.println(F("[UI] OLED init failed. Set WIFI_SSID/WIFI_PASSWORD or fix I2C."));
      while (true) {
        delay(1000);
      }
    }
    if (!attemptConnect(g_wifiSsid, g_wifiPass)) {
      Serial.println(F("[WiFi] Connect failed (no OLED). Check credentials."));
      while (true) {
        delay(2000);
      }
    }
  } else {
    bool haveSaved = false;
    if (offerSavedNetwork(g_wifiSsid, g_wifiPass, sizeof(g_wifiSsid), sizeof(g_wifiPass))) {
      haveSaved = true;
      oledShow4("Using saved", g_wifiSsid, "Connecting...", "");
      if (attemptConnect(g_wifiSsid, g_wifiPass)) {
        oledShow4("WiFi OK", WiFi.localIP().toString().c_str(), "Starting...", "");
        delay(1500);
      } else {
        haveSaved = false;
        oledShow4("Saved WiFi", "failed", "# OK -> setup", "");
        while (keypad.getKey() != '#') {
          delay(50);
        }
      }
    }

    if (!haveSaved || !wifiConnected) {
      runWifiSetupWizard();
    }
  }
#else
  strncpy(g_wifiSsid, WIFI_SSID, WIFI_CREDS_MAX);
  g_wifiSsid[WIFI_CREDS_MAX] = '\0';
  strncpy(g_wifiPass, WIFI_PASSWORD, WIFI_CREDS_MAX);
  g_wifiPass[WIFI_CREDS_MAX] = '\0';
  connectWiFi();
#endif
}

void loop() {
#if USE_KEYPAD_FOR_WIFI
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    static unsigned long lastReconnectMs = 0;
    unsigned long t = millis();
    if (t - lastReconnectMs >= 8000UL) {
      lastReconnectMs = t;
      if (g_oledOk) {
        char ssidLine[22];
        strncpy(ssidLine, g_wifiSsid, sizeof(ssidLine) - 1);
        ssidLine[sizeof(ssidLine) - 1] = '\0';
        oledShow4("WiFi lost", "Reconnecting...", ssidLine, "");
      } else {
        Serial.println(F("[WiFi] Reconnecting..."));
      }
      WiFi.mode(WIFI_STA);
      WiFi.begin(g_wifiSsid, g_wifiPass);
    }
    delay(200);
    return;
  }

  if (!wifiConnected) {
    wifiConnected = true;
    if (g_oledOk) {
      oledShow4("WiFi restored", WiFi.localIP().toString().c_str(), "", "");
      delay(900);
    }
  }
#else
  if (!wifiConnected) {
    if (WiFi.status() != WL_CONNECTED) {
      connectWiFi();
    }
    delay(500);
    return;
  }
#endif

  unsigned long now = millis();
  if (now - lastRequestMs >= REQUEST_INTERVAL_MS) {
    lastRequestMs = now;
    sendProcessDataAndUpdateRelays();
  }

#if USE_KEYPAD_FOR_WIFI
  if (g_oledOk && (now - lastOledStatusMs >= 1000)) {
    lastOledStatusMs = now;
    refreshStatusDashboard();
  }
#endif
}

#if !USE_KEYPAD_FOR_WIFI
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
#endif

#if USE_KEYPAD_FOR_WIFI
void initUiHardware() {
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    g_oledOk = false;
    Serial.println(F("[OLED] SSD1306 init failed — check I2C wiring / address (try 0x3D)"));
    return;
  }
  g_oledOk = true;
  display.clearDisplay();
  display.setRotation(0);
  display.display();
  keypad.setDebounceTime(30);
}

void oledShow4(const char* l0, const char* l1, const char* l2, const char* l3) {
  if (!g_oledOk) {
    if (l0) Serial.println(l0);
    if (l1) Serial.println(l1);
    if (l2) Serial.println(l2);
    if (l3) Serial.println(l3);
    return;
  }
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(l0 ? l0 : "");
  display.println(l1 ? l1 : "");
  display.println(l2 ? l2 : "");
  display.println(l3 ? l3 : "");
  display.display();
}

static void truncateForOled(char* dst, size_t dstSz, const char* src) {
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t len = strlen(src);
  const size_t maxChars = 21;
  if (len <= maxChars) {
    strncpy(dst, src, dstSz - 1);
    dst[dstSz - 1] = '\0';
    return;
  }
  strncpy(dst, src + (len - maxChars), dstSz - 1);
  dst[dstSz - 1] = '\0';
}

bool enterCredential(const char* title, char* buf, size_t bufSize, bool masked) {
  buf[0] = '\0';
  size_t len = 0;
  static const char CHARSET[] =
    " abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "0123456789._-+!@#$%^&*()[]";
  const int csLen = (int)(sizeof(CHARSET) - 1);
  int charsetIndex = 0;

  char lineDisp[22];
  char pickLine[22];

  for (;;) {
    if (masked) {
      memset(lineDisp, '*', len);
      lineDisp[len] = '\0';
    } else {
      truncateForOled(lineDisp, sizeof(lineDisp), buf);
    }
    snprintf(pickLine, sizeof(pickLine), "[%c] 4/6 5=add", CHARSET[charsetIndex]);
    oledShow4(title, lineDisp, pickLine, "*=del #=done");

    char key = keypad.getKey();
    if (!key) {
      delay(40);
      continue;
    }
    if (key == '4') {
      charsetIndex = (charsetIndex - 1 + csLen) % csLen;
    } else if (key == '6') {
      charsetIndex = (charsetIndex + 1) % csLen;
    } else if (key == '5') {
      if (len + 1 < bufSize) {
        buf[len++] = CHARSET[charsetIndex];
        buf[len] = '\0';
      }
    } else if (key == '*') {
      if (len > 0) {
        buf[--len] = '\0';
      }
    } else if (key == '#') {
      if (len < 1 && !masked) {
        oledShow4("SSID empty", "Type SSID", "then #", "");
        delay(800);
        continue;
      }
      return true;
    }
  }
}

bool offerSavedNetwork(char* ssid, char* pass, size_t ssidSz, size_t passSz) {
  (void)passSz;
  String s = prefs.getString("ssid", "");
  String p = prefs.getString("pass", "");
  if (s.length() == 0) {
    return false;
  }

  char show[22];
  truncateForOled(show, sizeof(show), s.c_str());
  unsigned long start = millis();
  while (millis() - start < 15000UL) {
    oledShow4("Saved WiFi:", show, "*=use #=new", "");
    char key = keypad.getKey();
    if (key == '*') {
      s.toCharArray(ssid, ssidSz);
      p.toCharArray(pass, passSz);
      return true;
    }
    if (key == '#') {
      return false;
    }
    delay(40);
  }
  s.toCharArray(ssid, ssidSz);
  p.toCharArray(pass, passSz);
  return true;
}

void saveWifiCredentials(const char* ssid, const char* pass) {
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
}

void runWifiSetupWizard() {
  for (;;) {
    oledShow4("WiFi setup", "Enter SSID", "", "");
    delay(400);
    enterCredential("WiFi SSID", g_wifiSsid, sizeof(g_wifiSsid), false);

    oledShow4("WiFi setup", "Enter password", "(empty OK)", "");
    delay(400);
    enterCredential("Password", g_wifiPass, sizeof(g_wifiPass), true);

    oledShow4("Connecting...", g_wifiSsid, "Please wait", "");
    if (attemptConnect(g_wifiSsid, g_wifiPass)) {
      saveWifiCredentials(g_wifiSsid, g_wifiPass);
      oledShow4("Connected!", WiFi.localIP().toString().c_str(), "Saved to flash", "");
      delay(2000);
      return;
    }
    oledShow4("WiFi failed", "Check AP/router", "*=retry #=again", "");
    for (;;) {
      char k = keypad.getKey();
      if (k == '*' || k == '#') {
        break;
      }
      delay(40);
    }
  }
}

bool attemptConnect(const char* ssid, const char* pass) {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, false);
  delay(150);
  WiFi.begin(ssid, pass);

  int attempt = 0;
  while (WiFi.status() != WL_CONNECTED && attempt < 50) {
    char dots[8];
    int n = (attempt % 3) + 1;
    memset(dots, '.', (size_t)n);
    dots[n] = '\0';
    oledShow4("WiFi connect", ssid, dots, "");
    delay(400);
    attempt++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n[WiFi] Connected");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  wifiConnected = false;
  Serial.println("\n[WiFi] Failed");
  return false;
}

void refreshStatusDashboard() {
  if (!g_oledOk) {
    return;
  }
  char l0[22], l1[22], l2[22], l3[22];
  snprintf(l0, sizeof(l0), "WiFi:%s %ddBm",
           (WiFi.status() == WL_CONNECTED) ? "OK" : "NO",
           (WiFi.status() == WL_CONNECTED) ? WiFi.RSSI() : 0);
  IPAddress ip = WiFi.localIP();
  snprintf(l1, sizeof(l1), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  snprintf(l2, sizeof(l2), "API:%s", g_lastApiOk ? "OK" : "ERR");
  snprintf(l3, sizeof(l3), "HTTP:%d %s", g_lastHttpCode, g_apiDetail);
  oledShow4(l0, l1, l2, l3);
}
#endif

#if !USE_KEYPAD_FOR_WIFI
bool attemptConnect(const char* ssid, const char* pass) {
  (void)ssid;
  (void)pass;
  return false;
}
void oledShow4(const char* l0, const char* l1, const char* l2, const char* l3) {
  (void)l0;
  (void)l1;
  (void)l2;
  (void)l3;
}
#endif

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
  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  String url = String(API_BASE_URL) + PROCESS_DATA_PATH;
  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("ngrok-skip-browser-warning", "true");

  int code = http.POST(body);

  g_lastHttpCode = code;
  if (code != 200) {
    g_lastApiOk = false;
    snprintf(g_apiDetail, sizeof(g_apiDetail), "POST %d", code);
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

  StaticJsonDocument<1024> resDoc;
  DeserializationError err = deserializeJson(resDoc, response);
  if (err) {
    g_lastApiOk = false;
    snprintf(g_apiDetail, sizeof(g_apiDetail), "JSON");
    Serial.print("[API] JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  JsonObject control = resDoc["control_states"];
  if (control.isNull()) {
    g_lastApiOk = false;
    snprintf(g_apiDetail, sizeof(g_apiDetail), "no ctrl");
    Serial.println("[API] No control_states in response");
    return;
  }

  g_lastApiOk = true;
  snprintf(g_apiDetail, sizeof(g_apiDetail), "ok");

  for (int i = 0; i < NUM_RELAYS; i++) {
    bool on = control[RELAY_KEYS[i]] | false;
    pinMode(RELAY_PINS[i], OUTPUT);
    digitalWrite(RELAY_PINS[i], on ? LOW : HIGH);
  }
}
