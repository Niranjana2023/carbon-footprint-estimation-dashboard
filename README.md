# Carbon Footprint Dashboard

A lightweight Flask application that monitors real-time power consumption and carbon footprint of various home appliances. It integrates with ESP32 microcontroller to collect ADC data and displays it through an interactive, modern web dashboard.

## Features

- **Real-time Monitoring**: Live visualization of voltage, current, and power consumption
- **Carbon Footprint Tracking**: Automatic calculation of carbon emissions based on power consumption
- **Multiple Appliances**: Pre-configured appliances (Lights, Fan, Washing Machine, Refrigerator, Microwave)
- **Interactive Charts**: Real-time line charts showing voltage, current, power, and carbon footprint trends
- **Modern UI**: Clean, responsive design with gradient backgrounds and smooth animations
- **Data Persistence**: In-memory storage of readings with automatic cleanup to manage memory
- **REST API**: Easy integration with ESP32 and other IoT devices

## Project Structure

```
carbon_footprint_dashboard/
├── app.py                          # Main Flask application
├── requirements.txt                # Python dependencies
├── README.md                       # This file
└── app/
    ├── templates/
    │   ├── dashboard.html         # Main dashboard with appliance cards
    │   └── appliance_detail.html  # Detailed view with charts
    └── static/
        ├── css/
        │   └── style.css          # Modern styling
        └── js/
            └── (Chart.js via CDN)
```

## Installation

### Prerequisites

- Python 3.7 or higher
- pip (Python package manager)

### Setup Steps

1. **Clone or navigate to the project directory**:
   ```bash
   cd /path/to/carbon_footprint_dashboard
   ```

2. **Create a virtual environment** (optional but recommended):
   ```bash
   python3 -m venv venv
   source venv/bin/activate  # On Windows: venv\Scripts\activate
   ```

3. **Install dependencies**:
   ```bash
   pip install -r requirements.txt
   ```

## Running the Application

1. **Start the Flask development server**:
   ```bash
   python app.py
   ```

2. **Open your browser and navigate to**:
   ```
   http://127.0.0.1:5000
   ```

The dashboard should load with all available appliances displayed as cards.

## Usage

### Dashboard (Root Route `/`)

- Displays all available appliances as clickable cards
- Each card shows the appliance icon and name
- Click any card to view detailed monitoring data

### Appliance Detail Page (`/appliance/<appliance_id>`)

- Shows real-time metrics: Voltage, Current, Power, Carbon Footprint
- Displays total energy consumption and cumulative carbon footprint
- Four interactive line charts showing:
  - **Voltage**: Real-time voltage in volts (V)
  - **Current**: Real-time current in amperes (A)
  - **Power**: Real-time power in watts (W)
  - **Carbon Footprint**: Cumulative carbon emissions in kg CO₂

**Controls**:
- **Refresh Data**: Manually update metrics and charts
- **Reset**: Clear all data for the selected appliance

## API Endpoints

### 1. GET `/` - Dashboard
Returns the main dashboard page with all appliances.

### 2. GET `/appliance/<appliance_id>` - Appliance Detail
Returns the detailed monitoring page for a specific appliance.

**Appliance IDs**: `lights`, `fan`, `washing_machine`, `refrigerator`, `microwave`

### 3. POST `/process-data` - Process ESP32 Data
Accepts ADC readings from ESP32 and calculates power/carbon metrics.

**Request Format**:
```json
{
  "A0": 2048,
  "A1": 1024,
  "A2": 3072,
  "A3": 512,
  "A4": 1536
}
```

Where keys are ADC pin names and values are 12-bit ADC readings (0-4095).

**Response Format**:
```json
{
  "status": "success",
  "data": {
    "lights": {
      "timestamp": "2024-02-20T10:30:45.123456",
      "adc_value": 2048,
      "voltage": 4.5,
      "current": 0.045,
      "power": 0.203,
      "energy": 0.000000566,
      "carbon": 0.000000481
    }
    // ... other appliances
  },
  "timestamp": "2024-02-20T10:30:45.123456"
}
```

### 4. GET `/api/appliance/<appliance_id>/data` - Get Appliance Data
Retrieves current and historical readings for an appliance.

**Response Format**:
```json
{
  "appliance_id": "lights",
  "current_reading": {
    "timestamp": "2024-02-20T10:30:45.123456",
    "adc_value": 2048,
    "voltage": 4.5,
    "current": 0.045,
    "power": 0.203,
    "energy": 0.000000566,
    "carbon": 0.000000481
  },
  "readings": [
    // Last 100 readings
  ],
  "total_energy": 0.000056,
  "total_carbon": 0.000048
}
```

### 5. POST `/api/appliance/<appliance_id>/reset` - Reset Data
Clears all data for a specific appliance.

**Response Format**:
```json
{
  "status": "success",
  "message": "Data reset for lights"
}
```

### 6. GET `/config` - Get Configuration
Returns system configuration values.

**Response Format**:
```json
{
  "VOLTAGE_CONSTANT": 9.0,
  "ADC_MAX": 4095,
  "CURRENT_SENSOR_RESOLUTION": 100,
  "CARBON_EMISSION_FACTOR": 0.85
}
```

## Configuration

Edit `app.py` to customize the following settings in the `CONFIG` dictionary:

```python
CONFIG = {
    'VOLTAGE_CONSTANT': 9.0,              # Voltage in volts (adjust to your setup)
    'ADC_MAX': 4095,                      # 12-bit ADC maximum value
    'CURRENT_SENSOR_RESOLUTION': 100,     # 100mV per 1A (adjust based on sensor)
    'CARBON_EMISSION_FACTOR': 0.85        # kg CO2 per kWh (region-specific)
}
```

## Appliance Configuration

To add or modify appliances, edit the `APPLIANCES` dictionary in `app.py`:

```python
APPLIANCES = {
    'appliance_id': {
        'name': 'Display Name',
        'icon': '🔌',
        'adc_pin': 'A0',
        'color': '#HEX_COLOR'
    }
}
```

## Data Calculation Details

### Voltage Calculation
```
Voltage (V) = (ADC_VALUE / ADC_MAX) × VOLTAGE_CONSTANT
```

### Current Calculation
```
Current (A) = (Voltage × 1000) / CURRENT_SENSOR_RESOLUTION
```
With default resolution of 100mV/A, a 0.9V reading = 9A

### Power Calculation
```
Power (W) = Voltage (V) × Current (A)
```

### Energy Calculation
```
Energy (kWh) = (Power (W) / 1000) / 3600  # Per second
```

### Carbon Footprint Calculation
```
Carbon (kg CO₂) = Energy (kWh) × CARBON_EMISSION_FACTOR
```

## ESP32 Integration Example

Here's a sample Arduino sketch to send ADC data to the dashboard:

```cpp
#include <WiFi.h>
#include <HTTPClient.h>

const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";
const char* serverURL = "http://YOUR_IP:5000/process-data";

void setup() {
    Serial.begin(115200);
    WiFi.begin(ssid, password);
}

void loop() {
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        
        // Read ADC values
        int adc0 = analogRead(A0);
        int adc1 = analogRead(A1);
        int adc2 = analogRead(A2);
        int adc3 = analogRead(A3);
        int adc4 = analogRead(A4);
        
        // Create JSON payload
        String payload = "{\"A0\":" + String(adc0) + 
                        ",\"A1\":" + String(adc1) + 
                        ",\"A2\":" + String(adc2) + 
                        ",\"A3\":" + String(adc3) + 
                        ",\"A4\":" + String(adc4) + "}";
        
        http.begin(serverURL);
        http.addHeader("Content-Type", "application/json");
        
        int httpResponseCode = http.POST(payload);
        Serial.println("Response code: " + String(httpResponseCode));
        
        http.end();
    }
    
    delay(1000);  // Send data every second
}
```

## Testing the API

You can test the `/process-data` endpoint using `curl`:

```bash
curl -X POST http://127.0.0.1:5000/process-data \
  -H "Content-Type: application/json" \
  -d '{"A0": 2048, "A1": 1024, "A2": 3072, "A3": 512, "A4": 1536}'
```

## UI Features

### Dashboard Cards
- Gradient background with smooth animations
- Hover effect that lifts cards
- Color-coded border for each appliance
- Icon and name display

### Detail Page Metrics
- Large, easy-to-read value displays
- Real-time updates every 2 seconds
- Special styling for carbon footprint (green accent)
- Responsive grid layout

### Charts
- Real-time line charts with smooth curves
- Automatic scaling
- Last 50 data points displayed
- Color-matched to appliance theme
- Hover tooltips for precise values

### Responsive Design
- Works on desktop, tablet, and mobile
- Adaptive grid layouts
- Touch-friendly buttons

## Performance Considerations

- **Memory Management**: Stores only the last 100 readings per appliance to prevent memory bloat
- **Efficient Updates**: Charts updated without animation for performance
- **Lightweight**: No heavy dependencies, just Flask and Chart.js
- **Data Refresh**: Default 2-second interval can be adjusted in the template

## Troubleshooting

### Port 5000 Already in Use
Change the port in `app.py`:
```python
app.run(debug=True, host='127.0.0.1', port=5001)
```

### Charts Not Displaying
Ensure internet connection for Chart.js CDN or download locally.

### No Data Appearing
1. Check if POST requests are being sent to `/process-data`
2. Verify ADC values are between 0-4095
3. Check browser console for JavaScript errors

## Browser Compatibility

- Chrome/Chromium 90+
- Firefox 88+
- Safari 14+
- Edge 90+

## License

This project is open source and available under the MIT License.

## Support

For issues or questions, please check the application console for error messages or enable debug mode:

```python
app.run(debug=True)  # Already enabled by default
```

---

**Last Updated**: February 2026
