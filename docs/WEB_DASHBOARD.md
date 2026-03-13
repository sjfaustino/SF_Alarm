# SF_Alarm — Web Dashboard & REST API Technical Guide

This document provides an exhaustive, deep-dive transition into the SF_Alarm Web Dashboard, its implementation details, security considerations, and extension points.

---

## 1. Overview & Vision

The SF_Alarm Web Dashboard is designed to provide a "single pane of glass" for the alarm system, accessible from any device on the local network. It prioritizes **speed**, **security**, and **responsive aesthetics**.

```text
  +-----------------------------------------------------------+
  |                   NETWORK ECOSYSTEM                       |
  +-----------------------------------------------------------+
  |                                                           |
  |  [ User Devices ] <--- HTTP/JSON ---> [ ESP32 Server ]    |
  |    (Phone/PC)                            (PsychicHttp)    |
  |                                               |           |
  |                                               v           |
  |  [ Dashboard SPA ] <--- I2C Bus ---> [ Alarm Hardware ]   |
  |    (In Flash)                         (16 Zones/Outputs)  |
  |                                                           |
  +-----------------------------------------------------------+
```

---

## 2. Frontend Architecture (web_ui.h)

The frontend is a self-contained **Single Page Application (SPA)** embedded within the firmware. By serving a single HTML file containing all CSS and JS, we eliminate multiple round-trips and ensure the UI is ready instantly.

### Component Breakdown

#### A. The Header
Contains the system title and a dynamic Wi-Fi signal strength indicator. 📶
- **Signal Logic**: RSSI is mapped to 0–4 bars.
- **Auto-Refresh Alert**: A subtle "Pulse" animation plays whenever data is successfully polled from the API.

#### B. The Status Card
The central focus of the UI.
- **Dynamic Classes**:
  - `status-disarmed`: Green theme.
  - `status-armed`: Blue theme.
  - `status-triggered`: Flashing Red theme.
- **Control Buttons**: ARM, HOME, and MUTE. These trigger a JavaScript-managed PIN modal before sending POST requests.

#### C. The Zone Grid
A 4x4 or responsive grid showing all 16 zones.
- **States**:
  - **Normal**: Muted gray background.
  - **Alert**: Glowing red/orange.
  - **Bypassed**: Diagonal stripe pattern or "Bypass" label.
- **Interactivity**: Clicking a zone allows the user to toggle its bypass status (requires PIN).

#### D. The Output List
A scrollable list or grid of the 16 MOSFET outputs.
- **Outputs Control**: 16 toggle switches to manually trigger connected relays or devices.
- **Alert Settings**: Configure how you receive notifications:
  - **Alert Channel**: Choose between SMS Only, WhatsApp Only, or Both.
  - **WhatsApp Credentials**: Set your phone number and CallMeBot API key directly from the dashboard.
- **System Metrics**: Real-time view of Uptime, Free Heap, RSSI, and IP address.
- **Sync**: Checkboxes are automatically synced with the hardware state every 2 seconds.

---

## 3. Communication Strategy (HTTP/REST)

The dashboard uses the `fetch()` API to communicate with the ESP32 backend.

### JSON Schema (Detailed)

Every 2 seconds, the JS client hits `/api/status`. The response contains the following detailed topology:

```json
{
  "alarm": {
    "state": "DISARMED",       // String for UI display
    "stateCode": 0,           // Integer for logical checks (0:Disarm, 1:Exit, 2:Away, 3:Home, 4:Entry, 5:Triggered)
    "delayRemaining": 0,      // Seconds left in entry/exit delay
    "sirenActive": false      // Boolean for siren status
  },
  "zones": [
    {
      "index": 0,             // 0-15
      "name": "Front Door",   // Custom name from NVS
      "state": "NORMAL",      // NORMAL, TRIGGERED, TAMPER, FAULT, BYPASSED
      "stateCode": 0,         // 0:Normal, 1:Trigger, 2:Tamper, 3:Fault, 4:Bypass
      "enabled": true,        // Zone configuration flag
      "type": "DELAYED"       // Zone type
    },
    // ... continues for 16 zones
  ],
  "outputs": 65535,           // 16-bit integer bitmask of all output states
  "network": {
    "ip": "192.168.10.105",
    "rssi": -42,              // dBm
    "ssid": "MyAlarmNet"
  },
  "system": {
    "uptime": 12345,          // Seconds
    "freeHeap": 182740,       // Bytes
    "temp": 42.5,             // Internal ESP32 temp (optional)
    "version": "0.1.0"
  }
}
```

---

## 4. Security & Authentication

### Local Access Only
The dashboard is served on port 80 and is intended for **Local Area Network (LAN)** use. It does not implement HTTPS or global basic auth by default to minimize memory overhead, relying instead on network-level security (WPA2/WPA3).

### Mandatory API Authentication (Remediation 9.0)
Endpoints that were previously unauthenticated for reconnaissance (`/api/status`, `/api/outputs`) now **require a valid PIN**. 
- **Status/Output Requests**: Must include a `pin` query parameter or be part of an authenticated session.
- **Fail-Safe**: Requests without a PIN will return a `403 Forbidden` response with a code suggesting the system is "LOCKED".

### Anti-Brute Force Rate Limiting
To prevent local network attackers from triggering a global alarm lockout, an IP-based tracking system is implemented:
1.  **Per-IP Tracking**: The system tracks the last 8 unique remote IPs.
2.  **Cooldown**: A 5-second cooldown is enforced between attempts from the same IP.
3.  **Local Lockout**: After 5 consecutive failures, a specific IP is blocked for 60 seconds. This prevents automated scripts from hammering the ESP32 while allowing the physical keypad and owner to remain operational.

### PIN Protection Layer
Sensitive operations (ARM, DISARM, BYPASS) are protected by a **client-side modal + server-side validation** pattern.

1.  **UI Trigger**: User clicks "ARM".
2.  **JS Modal**: A keypad modal appears asking for the Alarm PIN.
3.  **Encapsulation**: The PIN is sent in a JSON `POST` body.
4.  **Hardware Guard**: `web_server.cpp` receives the PIN, calls `alarmDisarm(pin)`, and returns a `401 Unauthorized` or `403 Forbidden` if the PIN is incorrect.
5.  **Feedback**: The UI shakes the modal on failure or closes it on success.

---

## 5. Backend Implementation (PsychicHttp)

`PsychicHttp` provides a significant upgrade over legacy libraries:
- **Asynchronous**: Does not block the main alarm loop during large transfers (like serving the 30KB HTML file).
- **Embedded Static Files**: Though we use header-based strings, PsychicHttp can also serve from LittleFS/SPIFFS.
- **Request Metadata**: Easily access headers, query params, and JSON bodies.

### Server Configuration
- **Port**: 80 (Standard HTTP).
- **Max Handlers**: Set to `20` to accommodate 16+ API endpoints.
- **Request Buffering**: Configured to handle JSON bodies up to 512 bytes.

---

## 6. Customization Guide

### Modifying the Aesthetics
The CSS is located within `src/web_ui.h` inside the `<style>` tags. To change colors:
1.  Locate the variables section:
    ```css
    :root {
      --primary-color: #007bff;
      --danger-color: #dc3545;
      --bg-dark: #121212;
    }
    ```
2.  Update the HEX codes to match your preferred theme.

### Adding New API Endpoints
1.  Open `src/web_server.cpp`.
2.  Add a new handler function:
    ```cpp
    esp_err_t handleMyNewFeature(PsychicRequest* request, PsychicResponse* response) {
        return response->send(200, "text/plain", "OK");
    }
    ```
3.  Register it in `webServerInit()`:
    ```cpp
    server.on("/api/my-feature", HTTP_GET, handleMyNewFeature);
    ```

---

*SF_Alarm v0.1.0 — Web Dashboard Deep Dive*
