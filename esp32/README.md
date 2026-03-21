# ESP32 firmware — OLED + keypad WiFi setup

## Arduino libraries

Install via **Sketch → Include Library → Manage Libraries**:

| Library            | Author / note        |
|--------------------|----------------------|
| **ArduinoJson**    | Benoit Blanchon      |
| **Adafruit SSD1306** | Adafruit           |
| **Adafruit GFX Library** | Adafruit       |
| **Keypad**         | Mark Stanley         |

Board support: **ESP32 by Espressif Systems**.

## Wiring (defaults in `esp32.ino`)

| Function | GPIO |
|----------|------|
| OLED SDA (I²C) | 21 |
| OLED SCL (I²C) | 22 |
| Keypad rows | 14, 15, 16, 17 |
| Keypad cols | 18, 19, 23, 4 |
| Relays D0–D2 | 25, 26, 27 |
| ADC A0–A2 | 36, 39, 34 |

If your OLED uses address **0x3D**, change `OLED_I2C_ADDR` in the sketch.

## WiFi entry (keypad)

1. **Saved network** (after first successful connect): screen shows `*=use #=new` for 15 s; timeout uses saved credentials.
2. **SSID**: keys **4** / **6** change character; **5** appends; **\*** deletes; **#** done.
3. **Password**: same keys; empty password is allowed (**#** immediately); shown as `*`.
4. On failure, press **\*** or **#** to try again.

## Headless / no OLED

- Set `USE_KEYPAD_FOR_WIFI` to `0` and fill `WIFI_SSID` / `WIFI_PASSWORD`, **or**
- If the OLED fails to init, the sketch falls back to `WIFI_SSID` / `WIFI_PASSWORD` (must be non-empty).

## Runtime OLED status (when connected)

Roughly every second: WiFi + RSSI, IP, API OK/ERR, last HTTP code / short detail.
