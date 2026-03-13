# SF_Alarm — Module Documentation

Detailed technical documentation for every module in the SF_Alarm firmware.

---

## Table of Contents

- [include/config.h — Hardware & System Configuration](#includeconfigh--hardware--system-configuration)
- [src/io_expander — I2C PCF8574 I/O Driver](#srcio_expander--i2c-pcf8574-io-driver)
- [src/alarm_zones — Alarm Zone Manager](#srcalarm_zones--alarm-zone-manager)
- [src/alarm_controller — Alarm State Machine](#srcalarm_controller--alarm-state-machine)
- [src/sms_gateway — SMS HTTP Client](#srcsms_gateway--sms-http-client)
- [src/sms_commands — GA09 SMS Command Parser](#srcsms_commands--ga09-sms-command-parser)
- [src/config_manager — NVS Persistence](#srcconfig_manager--nvs-persistence)
- [src/network — Wi-Fi Connectivity](#srcnetwork--wifi-connectivity)
- [src/whatsapp_client — WhatsApp Notifications](#srcwhatsapp_client--whatsapp-notifications)
- [src/mqtt_client — IoT Integration](#srcmqtt_client--iot-integration)
- [src/serial_cli — Serial Command-Line Interface](#srcserial_cli--serial-command-line-interface)
- [src/web_server & web_ui — Web Dashboard & REST API](#srcweb_server--web_ui--web-dashboard--rest-api)
- [src/main.cpp — Application Entry Point](#srcmaincpp--application-entry-point)
- [Data Flow & Timing](#data-flow--timing)
- [Memory Map](#memory-map)

---

## include/config.h — Hardware & System Configuration

Central header containing every hardware constant and system default. This is the
**single source of truth** for all pin assignments, I2C addresses, timing
parameters, and compile-time limits.

### Hardware Pin Map

```
  ┌────────────────────────────────────────────────────────────┐
  │                 ESP32 GPIO Assignments                     │
  │                 KC868-A16 v1.6                             │
  ├─────────────┬──────────────────────────────────────────────┤
  │ GPIO 4      │ I2C SDA (to PCF8574 x4)                     │
  │ GPIO 5      │ I2C SCL (to PCF8574 x4)                     │
  │ GPIO 36     │ Analog Input 1 (ADC1_CH0, 0–5V)             │
  │ GPIO 39     │ Analog Input 2 (ADC1_CH3, 0–5V)             │
  │ GPIO 34     │ Analog Input 3 (ADC1_CH6, 0–5V)             │
  │ GPIO 35     │ Analog Input 4 (ADC1_CH7, 0–5V)             │
  │ GPIO 13     │ RS485 TX (UART2)                             │
  │ GPIO 16     │ RS485 RX (UART2)                             │
  │ GPIO 14     │ RS485 Direction Control                      │
  │ GPIO 23     │ Ethernet MDC  (LAN8720A)                     │
  │ GPIO 18     │ Ethernet MDIO (LAN8720A)                     │
  └─────────────┴──────────────────────────────────────────────┘
```

### Compile-Time Constants

| Constant                   | Value      | Description                          |
|---------------------------|-----------|---------------------------------------|
| `MAX_ZONES`               | 16        | Maximum alarm zones                   |
| `MAX_PHONE_NUMBERS`       | 5         | Maximum alert phone numbers           |
| `MAX_PHONE_LEN`           | 20        | Max characters per phone number       |
| `MAX_ZONE_NAME_LEN`       | 24        | Max characters per zone name          |
| `MAX_PIN_LEN`             | 8         | Max characters for alarm PIN          |
| `INPUT_DEBOUNCE_MS`       | 50        | Input debounce period (ms)            |
| `DEFAULT_EXIT_DELAY_S`    | 30        | Default exit delay (seconds)          |
| `DEFAULT_ENTRY_DELAY_S`   | 15        | Default entry delay (seconds)         |
| `DEFAULT_SIREN_DURATION_S`| 180       | Default siren auto-off (seconds)      |
| `SMS_POLL_INTERVAL_MS`    | 10000     | SMS inbox poll interval (ms)          |
| `MQTT_SYNC_INTERVAL_MS`   | 30000     | MQTT state sync frequency (ms)        |
| `INPUT_SCAN_INTERVAL_MS`  | 20        | Input scan rate (ms) = 50 Hz          |
| `NET_WORKER_YIELD_MS`     | 100       | Background task yield delay (ms)      |
| `CLI_BAUD_RATE`           | 115200    | Serial baud rate                      |
| `CLI_MAX_LINE_LEN`        | 128       | Max characters per CLI command        |
| `WATCHDOG_TIMEOUT_S`      | 30        | Watchdog timer (seconds)              |
| `NVS_NAMESPACE`           | `sf_alarm`| ESP32 NVS namespace                   |

---

## src/io_expander — I2C PCF8574 I/O Driver

Manages all I2C communication with the four PCF8574 chips on the KC868-A16
board. Provides a clean abstraction to read all 16 inputs as a single
`uint16_t` bitmask and control all 16 outputs independently.

### Theory of Operation

```
  I2C Bus (100 kHz)
  ─────────────────────────────────────────────────────────
    │         │         │         │
    │  ┌──────┴──────┐  │  ┌──────┴──────┐
    │  │ PCF8574     │  │  │ PCF8574     │
    │  │ Addr: 0x22  │  │  │ Addr: 0x21  │
    │  │ INPUTS 1-8  │  │  │ INPUTS 9-16 │
    │  │             │  │  │             │
    │  │ P0 = IN1    │  │  │ P0 = IN9    │
    │  │ P1 = IN2    │  │  │ P1 = IN10   │
    │  │ ...         │  │  │ ...         │
    │  │ P7 = IN8    │  │  │ P7 = IN16   │
    │  └─────────────┘  │  └─────────────┘
    │                   │
    │  ┌─────────────┐  │  ┌─────────────┐
    │  │ PCF8574     │  │  │ PCF8574     │
    │  │ Addr: 0x24  │  │  │ Addr: 0x25  │
    │  │ OUTPUTS 1-8 │  │  │ OUTPUTS 9-16│
    │  │             │  │  │             │
    │  │ P0 = OUT1   │  │  │ P0 = OUT9   │
    │  │ P1 = OUT2   │  │  │ P1 = OUT10  │
    │  │ ...         │  │  │ ...         │
    │  │ P7 = OUT8   │  │  │ P7 = OUT16  │
    │  └─────────────┘  │  └─────────────┘
```

### Input Logic

The KC868-A16's inputs use **EL357 opto-isolators** with 12V pull-ups.
When an input is shorted to GND (dry contact closure), the opto-isolator
conducts, pulling the PCF8574 pin LOW. Therefore:

- **PCF8574 reads 0** → input activated (sensor triggered)
- **PCF8574 reads 1** → input inactive (sensor normal)

The driver **inverts** the raw reading so that:
- **Bit = 1** → input triggered
- **Bit = 0** → input normal

### API Reference

| Function                    | Return     | Description                              |
|-----------------------------|-----------|------------------------------------------|
| `ioExpanderInit()`          | `bool`    | Init I2C + all 4 chips. Returns `true` if all OK. |
| `ioExpanderReadInputs()`    | `uint16_t`| Read all 16 inputs as bitmask (1=triggered). |
| `ioExpanderWriteOutputs(mask)` | `void` | Set all 16 outputs at once.              |
| `ioExpanderSetOutput(ch, state)` | `void` | Set single output (0–15).              |
| `ioExpanderGetOutputs()`    | `uint16_t`| Get the current output state bitmask.    |
| `ioExpanderChipOk(index)`   | `bool`    | Check if chip N is responding (0–3).     |

### Bitmask Layout

```
  Bit:  15  14  13  12  11  10   9   8   7   6   5   4   3   2   1   0
        ├───────────────────────┤   ├───────────────────────────────┤
        │    Chip 2 (0x21)      │   │       Chip 1 (0x22)          │
        │    Inputs 9-16        │   │       Inputs 1-8             │
        └───────────────────────┘   └───────────────────────────────┘
```

---

## src/alarm_zones — Alarm Zone Manager

Manages the state of all 16 alarm zones. Handles input debouncing, NO/NC
wiring logic, zone state tracking, and fires callbacks to the alarm
controller when zone states change.

### Zone Types

| Type       | Enum            | Value | Behavior                                      |
|-----------|-----------------|-------|-----------------------------------------------|
| Instant    | `ZONE_INSTANT`  | 0     | Triggers alarm immediately when system is armed |
| Delayed    | `ZONE_DELAYED`  | 1     | Starts entry/exit delay countdown              |
| 24-Hour    | `ZONE_24H`      | 2     | Always active, triggers even when disarmed     |
| Follower   | `ZONE_FOLLOWER` | 3     | Instant unless an entry delay is already running |

### Zone Wiring

| Wiring        | Enum       | Value | Input = 1 means    | Input = 0 means    |
|--------------|-----------|-------|---------------------|--------------------|
| Normally Open  | `ZONE_NO` | 0     | Triggered (closed)  | Normal (open)      |
| Normally Closed| `ZONE_NC` | 1     | Normal (closed)     | Triggered (open)   |

### Zone States

| State      | Enum             | Value | Description                             |
|-----------|------------------|-------|-----------------------------------------|
| Normal     | `ZONE_NORMAL`    | 0     | Zone is clear, sensor not activated     |
| Triggered  | `ZONE_TRIGGERED` | 1     | Zone sensor activated (debounced)       |
| Tamper     | `ZONE_TAMPER`    | 2     | Tamper condition detected               |
| Fault      | `ZONE_FAULT`     | 3     | Wiring fault or communication error     |
| Bypassed   | `ZONE_BYPASSED`  | 4     | Zone manually bypassed (ignored)        |

### Debounce Algorithm

```
  Input signal (with noise):
  
  HIGH ──┐   ┌─┐ ┌──────────────────────
         │   │ │ │
  LOW  ──┴───┘ └─┘
         ◄───► ◄─► noise
               ◄──────────────────────── stable HIGH
  
  Debounce logic (50ms window):
  
  1. Input changes from current level
  2. Start debounce timer (50ms)
  3. If input stays at new level for 50ms → accept
  4. If input changes back during 50ms → reset timer
  5. If input changes to yet another level → restart with new target
  
  Result:
  
  HIGH ────────────────────────────
                │
                └─ accepted after 50ms stable
  LOW  ────────┘
```

### Data Structures

```c
struct ZoneConfig {
    char       name[24];     // Human-readable zone name
    ZoneType   type;         // INSTANT, DELAYED, 24H, FOLLOWER
    ZoneWiring wiring;       // NO or NC
    bool       enabled;      // Whether zone is active
};

struct ZoneInfo {
    ZoneConfig config;
    ZoneState  state;          // Current zone state
    bool       rawInput;       // Debounced input level
    uint32_t   lastChangeMs;   // Timestamp of last state change
    uint32_t   debounceStartMs;// Debounce timer start
    bool       debouncing;     // Debounce in progress
    bool       pendingLevel;   // Level being debounced
};
```

### API Reference

| Function                        | Return         | Description                          |
|---------------------------------|---------------|--------------------------------------|
| `zonesInit()`                   | `void`        | Initialize all 16 zones with defaults |
| `zonesSetCallback(cb)`          | `void`        | Register zone state change callback   |
| `zonesUpdate(bitmask)`          | `void`        | Process raw input bitmask (call at 50Hz) |
| `zonesGetInfo(index)`           | `ZoneInfo*`   | Get read-only zone info (0–15)       |
| `zonesGetConfig(index)`         | `ZoneConfig*` | Get mutable zone config (0–15)       |
| `zonesSetBypassed(index, bypass)`| `void`       | Bypass or unbypass a zone            |
| `zonesAllClear()`               | `bool`        | Check if all enabled zones are NORMAL |
| `zonesGetTriggeredMask()`       | `uint16_t`    | Bitmask of all TRIGGERED zones       |
| `zonesPrintStatus()`            | `void`        | Print status table to Serial         |

---

## src/alarm_controller — Alarm State Machine

The core alarm logic. Manages the system state (disarmed, arming, armed,
entry delay, alarm), controls the siren output, validates PINs, manages
timing, and fires event callbacks for SMS notification.

### States

| State          | Enum                | Value | Description                            |
|---------------|---------------------|-------|----------------------------------------|
| Disarmed       | `ALARM_DISARMED`    | 0     | System off, only 24H zones active      |
| Exit Delay     | `ALARM_EXIT_DELAY`  | 1     | Countdown after ARM command            |
| Armed Away     | `ALARM_ARMED_AWAY`  | 2     | All zones monitored                     |
| Armed Home     | `ALARM_ARMED_HOME`  | 3     | Perimeter + 24H zones only             |
| Entry Delay    | `ALARM_ENTRY_DELAY` | 4     | Countdown after delayed zone triggered  |
| Triggered      | `ALARM_TRIGGERED`   | 5     | Alarm active, siren on, SMS sent        |

### Events & Callbacks

SF_Alarm uses a structured event system to notify external modules (SMS, MQTT)
about state changes. Unlike simple string-based systems, SF_Alarm passes
an `AlarmEventInfo` struct containing the Event Type, the triggering Zone ID,
and a details string.

```cpp
struct AlarmEventInfo {
    AlarmEvent  event;
    int8_t      zoneId;  // 0-15, or -1 if not applicable
    const char* details; // Context string (e.g. zone name)
};

typedef void (*AlarmEventCallback)(const AlarmEventInfo& info);
```

| Event                 | Enum                  | When Fired                              |
|----------------------|----------------------|-----------------------------------------|
| Armed Away            | `EVT_ARMED_AWAY`     | Exit delay expired, system now armed     |
| Armed Home            | `EVT_ARMED_HOME`     | Exit delay expired (home mode)           |
| Disarmed              | `EVT_DISARMED`       | System disarmed via valid PIN            |
| Alarm Triggered       | `EVT_ALARM_TRIGGERED`| Zone triggered alarm (siren activated)   |
| Alarm Restored        | `EVT_ALARM_RESTORED` | (reserved for future use)                |
| Entry Delay           | `EVT_ENTRY_DELAY`    | Delayed zone triggered, countdown starts |
| Exit Delay            | `EVT_EXIT_DELAY`     | ARM command received, countdown starts   |
| Zone Triggered        | `EVT_ZONE_TRIGGERED` | Any zone changes to TRIGGERED state      |
| Zone Restored         | `EVT_ZONE_RESTORED`  | Any zone returns to NORMAL state         |
| Tamper                | `EVT_TAMPER`         | Tamper condition detected on a zone      |
| Siren On              | `EVT_SIREN_ON`       | Siren output activated                   |
| Siren Off             | `EVT_SIREN_OFF`      | Siren output deactivated                 |

### PIN Validation

The alarm PIN is stored in RAM (loaded from NVS on boot). All arm/disarm
operations require a valid PIN match. The default PIN is `1234`.

```
  ARM 5678   ──── validate "5678" == stored PIN ──── match? ──── start exit delay
                                                      │
                                                   no match ──── return false
```

### Siren Control

```
  ALARM TRIGGERED
       │
       ├─── ioExpanderSetOutput(sirenChannel, HIGH) ──── siren ON
       │
       ├─── sirenStartMs = millis()
       │
       └─── in alarmUpdate():
              │
              └─── if (millis() - sirenStartMs >= sirenDurationSec * 1000)
                     │
                     └─── ioExpanderSetOutput(sirenChannel, LOW) ──── siren OFF
                                                                      (auto-timeout)
  
  MUTE command:
       │
       └─── ioExpanderSetOutput(sirenChannel, LOW) ──── siren OFF
            sirenMuted = true
            (alarm state remains TRIGGERED — must DISARM to clear)
```

### API Reference

| Function                        | Return       | Description                              |
|---------------------------------|-------------|------------------------------------------|
| `alarmInit()`                   | `void`      | Initialize controller, set DISARMED       |
| `alarmSetCallback(cb)`          | `void`      | Register event callback                   |
| `alarmUpdate()`                 | `void`      | Run state machine (call in loop)          |
| `alarmArmAway(pin)`             | `bool`      | Arm away mode (false if PIN wrong/zones not clear) |
| `alarmArmHome(pin)`             | `bool`      | Arm home mode                             |
| `alarmDisarm(pin)`              | `bool`      | Disarm (false if PIN wrong)               |
| `alarmMuteSiren()`              | `void`      | Silence siren (alarm stays active)        |
| `alarmGetState()`               | `AlarmState` | Get current state                        |
| `alarmGetStateStr()`            | `const char*`| Get state as string                      |
| `alarmGetDelayRemaining()`      | `uint16_t`  | Get remaining delay (seconds)             |
| `alarmSetPin(pin)`              | `void`      | Update alarm PIN                          |
| `alarmSetExitDelay(sec)`        | `void`      | Set exit delay duration                   |
| `alarmSetEntryDelay(sec)`       | `void`      | Set entry delay duration                  |
| `alarmSetSirenDuration(sec)`    | `void`      | Set siren auto-off duration               |
| `alarmSetSirenOutput(ch)`       | `void`      | Set siren output channel (0–15)           |
| `alarmPrintStatus()`            | `void`      | Print status to Serial                    |

---

## src/sms_gateway — SMS HTTP Client

HTTP client that interfaces with the Cudy LT500D router's LuCI web
interface to send and receive SMS messages. Since the Cudy LT500D has
no documented API, this module mimics a web browser session.

### Authentication Flow

```
  Step 1: Login to LuCI
  ─────────────────────────────────────────────────
  
  ESP32 ──── POST http://<router>/cgi-bin/luci
             Content-Type: application/x-www-form-urlencoded
             Body: luci_username=admin&luci_password=admin
             
  Router ─── HTTP 302 Found
             Set-Cookie: sysauth=a3f2b1c8d4e5f6a7; path=/
             Location: /cgi-bin/luci/admin/...
  
  ESP32 saves the sysauth cookie for all subsequent requests.
  
  
  Alternative authentication methods (auto-detected):
  ─────────────────────────────────────────────────
  
  A) Token in redirect URL:
     Location: /cgi-bin/luci/;stok=abc123def/admin/...
     → sessionToken = "abc123def"
  
  B) Token in response body:
     <script>... stok="abc123def" ...</script>
     → sessionToken = "abc123def"
```

### Send SMS

```
  POST http://<router>/cgi-bin/luci/admin/network/gcom/sms?iface=4g
  Cookie: sysauth=<token>
  Content-Type: application/x-www-form-urlencoded
  Body: action=send&phone=+1234567890&message=Hello&token=<token>
  
  Response: HTTP 200 OK
```

### Poll Inbox

```
  GET http://<router>/cgi-bin/luci/admin/network/gcom/sms?iface=4g&action=read
  Cookie: sysauth=<token>
  
  Response parsing priority:
  1. JSON array: [{id, sender, body, timestamp}, ...]
  2. JSON object: {messages: [...]} or {sms: [...]}
  3. HTML scraping: look for sms-message/message-item class elements
```

### Retry & Session Management

```
  smsGatewaySend()
       │
       ├── attempt 1 ──── POST ──── 200 OK? ──── done ✓
       │                             │
       │                         401/403? ──── re-login ──── retry
       │                             │
       │                         other? ──── wait 2000ms
       │
       ├── attempt 2 ──── POST ──── 200 OK? ──── done ✓
       │                             │
       │                         fail? ──── wait 4000ms
       │
       └── attempt 3 ──── POST ──── 200 OK? ──── done ✓
                                     │
                                  fail? ──── return false
```

### Data Structure

```c
struct SmsMessage {
    int   id;             // Message index in router inbox
    char  sender[24];     // Sender phone number
    char  body[160];      // Message body (ASCII, 160 char max)
    char  timestamp[24];  // Received timestamp
};
```

### API Reference

| Function                            | Return   | Description                              |
|-------------------------------------|---------|------------------------------------------|
| `smsGatewayInit(ip, user, pass)`    | `void`  | Store router credentials                  |
| `smsGatewaySetCredentials(ip,u,p)`  | `void`  | Update credentials, invalidate session    |
| `smsGatewayLogin()`                 | `bool`  | Authenticate with LuCI                    |
| `smsGatewaySend(phone, message)`    | `bool`  | Send SMS via HTTP POST (with retry)       |
| `smsGatewayPollInbox(msgs, max)`    | `int`   | Poll inbox, return message count          |
| `smsGatewayDeleteMessage(id)`       | `bool`  | Delete message from inbox                 |
| `smsGatewayUpdate()`                | `void`  | Handle background polling and auto-login  |
| `smsGatewayIsLoggedIn()`            | `bool`  | Check if session is active                |
| `smsGatewayGetLastError()`          | `const char*` | Get last error message for debugging |

---

## src/sms_commands — GA09 SMS Command Parser

Implements a command set compatible with the **GA09 8-channel SMS alarm**
module, extended to support 16 channels. Handles incoming SMS parsing,
phone number authorization, command dispatch, and alert broadcasting.

### Command Parsing Flow

```
  Incoming SMS
       │
       ▼
  smsCmdProcess(sender, body)
       │
       ├── isAuthorized(sender)? ──── NO ──► (ignored, return)
       │                              │
       │         YES (or first registration if no phones configured)
       │                              │
       ▼                              ▼
  Try each parser in order:
       │
       ├── parseSetPhone()           #01#phone#
       ├── parseSetMultiplePhones()  @#num1#num2#
       ├── parseSetAlarmText()       #N#text
       ├── parseSetNC()              *NCxyz
       ├── parseStatus()             @#STATUS? or STATUS
       ├── parseArmDisarm()          ARM/DISARM
       ├── parseMute()               MUTE
       ├── parseBypass()             BYPASS/UNBYPASS
       └── parseHelp()               HELP
       │
       └── No match? ──► "Unknown command" reply
```

### Phone Number Authorization

Authorization compares the **last 9 digits** of the sender's number
against stored numbers. This provides flexibility with country code
formatting (e.g., Portugal uses +351 followed by 9 digits):

```
  Stored:  +351912345678
  Sender:  00351912345678 ← matches (last 9: 912345678)
  Sender:  +351912345678  ← matches (exact)
  Sender:  912345678      ← matches (last 9)
  Sender:  +9876543210    ← no match
```

### GA09 vs SF_Alarm Command Differences

| Feature              | GA09 (Original)     | SF_Alarm (Extended)         |
|---------------------|---------------------|-----------------------------|
| Zone count          | 8 (S1–S8)          | 16 (S1–S16)                 |
| Phone slots         | 6                   | 5                           |
| NC config (zones>9) | N/A                 | Comma-separated: `*NC1,10,12` |
| ARM modes           | Basic               | AWAY + HOME                 |
| PIN protection      | No                  | Yes                         |
| Zone bypass         | No                  | BYPASS / UNBYPASS           |
| Status query        | Basic               | Detailed with zone count    |

### Custom Alarm Text Storage

```
  alarmTexts[16][80]
  
  Index 0:  "ALARM Zone 1 triggered!"    (default)
  Index 1:  "Front door opened!"          (custom, set via #1#Front door opened!)
  Index 2:  "ALARM Zone 3 triggered!"    (default)
  ...
  Index 15: "Garage sensor alert!"        (custom, set via #16#Garage sensor alert!)
```

### API Reference

| Function                   | Return       | Description                                |
|----------------------------|--------------|--------------------------------------------|
| `smsCmdInit()`             | `void`       | Initialize with default alarm texts         |
| `smsCmdProcess(sender,body)`| `void`      | Parse + execute an incoming SMS command     |
| `smsCmdAddPhone(phone)`    | `int`        | Add phone, return slot index (-1 if full)   |
| `smsCmdSetPhone(slot,phone)`| `bool`      | Set specific phone slot (0-based)           |
| `smsCmdRemovePhone(phone)` | `bool`       | Remove phone by number                      |
| `smsCmdGetPhoneCount()`    | `int`        | Number of configured phones                 |
| `smsCmdGetPhone(index)`    | `const char*`| Get phone by index                          |
| `smsCmdClearPhones()`      | `void`       | Clear all phone numbers                     |
| `smsCmdSendAlert(msg)`     | `void`       | Send SMS to all configured phones           |
| `smsCmdGetAlarmText(index)`| `const char*`| Get alarm text for zone (0-based)           |
| `smsCmdSetAlarmText(i,txt)`| `void`       | Set alarm text for zone (0-based)           |
| `smsCmdUpdate()`           | `void`       | Handle periodic status reports (GA09)       |

---

## src/config_manager — NVS Persistence

Manages persistent storage of all system configuration using the ESP32's
**NVS (Non-Volatile Storage)** flash partition.

### NVS Key Map

```
  Namespace: "sf_alarm"
  
  ┌──────────────┬──────────┬──────────────────────────────┐
  │ Key          │ Type     │ Description                  │
  ├──────────────┼──────────┼──────────────────────────────┤
  │ configured   │ bool     │ Whether NVS has valid config │
  │ pin          │ String   │ Alarm PIN code               │
  │ exitDelay    │ uint16   │ Exit delay (seconds)         │
  │ entryDelay   │ uint16   │ Entry delay (seconds)        │
  │ sirenDur     │ uint16   │ Siren duration (seconds)     │
  │ sirenCh      │ uint8    │ Siren output channel         │
  │ phoneCnt     │ int32    │ Number of phone numbers      │
  │ phone0..4    │ String   │ Alert phone numbers          │
  │ routerIp     │ String   │ Cudy LT500D IP address       │
  │ routerUser   │ String   │ Router username              │
  │ routerPass   │ String   │ Router password              │
  │ wifiSsid     │ String   │ Wi-Fi SSID                   │
  │ wifiPass     │ String   │ Wi-Fi password               │
  │ zName0..15   │ String   │ Zone names                   │
  │ zType0..15   │ uint8    │ Zone types                   │
  │ zWire0..15   │ uint8    │ Zone wiring (NO/NC)          │
  │ zEn0..15     │ bool     │ Zone enabled flags           │
  └──────────────┴──────────┴──────────────────────────────┘
```

### Load/Save Sequence

```
  BOOT
   │
   ├── configInit()        ← opens NVS namespace
   │
   ├── (all modules init with defaults)
   │
   └── configLoad()        ← reads NVS, overrides defaults
         │
         ├── if "configured" == false → skip (use defaults)
         │
         └── if "configured" == true:
               ├── alarmSetPin(nvs.pin)
               ├── alarmSetExitDelay(nvs.exitDelay)
               ├── alarmSetEntryDelay(nvs.entryDelay)
               ├── alarmSetSirenDuration(nvs.sirenDur)
               ├── alarmSetSirenOutput(nvs.sirenCh)
               ├── smsCmdSetPhone(0..N, nvs.phoneN)
               ├── smsGatewaySetCredentials(nvs.router*)
               └── zonesGetConfig(0..15) ← set name, type, wiring, enabled
  
  SAVE (user runs "save" command)
   │
   └── configSave()
         ├── configured = true
         ├── write phone numbers
         ├── write zone configs
         └── (PIN, delays, etc. stored when changed via CLI)
```

### API Reference

| Function            | Return | Description                                  |
|---------------------|--------|----------------------------------------------|
| `configInit()`      | `void` | Open NVS namespace                            |
| `configLoad()`      | `void` | Load all config from NVS into running modules |
| `configSave()`      | `void` | Save all current config to NVS                |
| `configFactoryReset()`| `void`| Clear all NVS data                           |
| `configPrint()`     | `void` | Print config summary to Serial                |

---

## src/network — Wi-Fi Connectivity

Manages Wi-Fi Station (STA) mode connectivity. Handles initial connection,
auto-reconnection, and status reporting.

### Connection State Machine

```
  networkInit()
       │
       ├── load wifiSsid/wifiPass from NVS
       │
       ├── ssid empty? ──── yes ──── idle (no connection)
       │                     │
       │                    no
       │                     │
       └── WiFi.begin(ssid, pass)
            │
            ├── WL_CONNECTED ──── log IP + RSSI
            │                      │
            │                      └── (normal operation)
            │
            └── not connected ──── wait 5 seconds
                                   │
                                   └── WiFi.begin() retry
                                        │
                                        └── (repeat until connected)
```

### API Reference

| Function                   | Return  | Description                           |
|----------------------------|---------|---------------------------------------|
| `networkInit()`            | `void`  | Initialize Wi-Fi STA, load credentials |
| `networkSetWifi(ssid,pass)`| `void`  | Set + save credentials, reconnect     |
| `networkUpdate()`          | `void`  | Handle reconnection (call in loop)    |
| `networkIsConnected()`     | `bool`  | Check Wi-Fi connection status         |
| `networkGetIP()`           | `String`| Get current IP address                |
| `networkGetRSSI()`         | `int`   | Get signal strength (dBm)             |
| `networkPrintStatus()`     | `void`  | Print status to Serial                |

---

## src/serial_cli — Serial Command-Line Interface

Full-featured command-line interface over the USB serial port at 115200
baud. Supports line editing with backspace, command parsing, and over
30 commands for system configuration and diagnostics.

### Line Editor

```
  Serial input processing:
  
  Character received:
    │
    ├── '\r' (CR)      → ignored
    ├── '\n' (LF)      → execute command, clear buffer
    ├── '\b' / DEL      → backspace (remove last char)
    └── printable       → echo + add to buffer
  
  Buffer: char[128], null-terminated
  Prompt: "sf_alarm> "
```

### Command Dispatch

```
  processLine(line)
       │
       ├── tokenize first word (command)
       ├── rest of line = arguments
       ├── command converted to lowercase
       │
       └── strcmp dispatch:
             status → alarmPrintStatus() + zonesPrintStatus() + networkPrintStatus()
             zones  → zonesPrintStatus()
             arm    → alarmArmAway() or alarmArmHome()
             disarm → alarmDisarm()
             zone   → parse zone number + subcommand
             phone  → add/remove/list/clear
             wifi   → networkSetWifi()
             router → smsGatewaySetCredentials()
             test   → sms/output/input
             save   → configSave()
             load   → configLoad()
             factory→ confirm + configFactoryReset()
             reboot → ESP.restart()
             help   → printHelp()
```

---

---

## src/web_server & web_ui — Web Dashboard & REST API

This module provides a local web-based interface for monitoring and controlling the alarm system. It is built using the **PsychicHttp** library, which leverages the ESP-IDF HTTP server for robust, asynchronous performance.

> [!NOTE]
> For a more detailed guide on the frontend architecture, polling logic, and customization, see the dedicated [Web Dashboard Guide](WEB_DASHBOARD.md).

### Architecture

```
  ┌─────────────────────────────────────────────────────────────┐
  │                        web_server                           │
  │                                                             │
  │   webServerInit():                                          │
  │     1. Set max_uri_handlers = 20                            │
  │     2. Register endpoints (GET / , GET/POST /api/...)       │
  │     3. server.begin()                                       │
  │                                                             │
  │   ┌────────────────────┐    ┌────────────────────────┐     │
  │   │      Endpoints     │    │      PsychicHttp       │     │
  │   │  ┌──────────────┐  │    │  ┌──────────────────┐  │     │
  │   │  │ handleRoot   │──┼────┼──► WEB_UI_HTML      │  │     │
  │   │  │ handleStatus │──┼────┼──► JSON Generator   │  │     │
  │   │  │ handleAction │──┼────┼──► Command Dispatch │  │     │
  │   │  └──────────────┘  │    │  └──────────────────┘  │     │
  │   └────────────────────┘    └────────────────────────┘     │
  └─────────────────────────────────────────────────────────────┘
```

### Dashboard Content (web_ui.h)

The dashboard is a **Single Page Application (SPA)** stored as a constant string literal (`WEB_UI_HTML`) in the ESP32's flash memory. It includes:
- **HTML5 Semantic Structure**: Header, status card, zone grid, output list, and modals.
- **CSS3 Styling**: A modern "Dark Mode" theme with glassmorphism effects and responsive flexbox/grid layouts.
- **Vanilla JavaScript**: 
  - **Polling**: Fetches `/api/status` every 2000ms.
  - **Dynamic Rendering**: Rebuilds the zone grid and output list without page reloads.
  - **Modals**: Handles PIN entry for sensitive actions.
  - **State Management**: Updates UI elements (colors, badges, text) based on the latest JSON data.

### REST API Specification

The server listens on **port 80** and provides the following endpoints:

#### 1. System Status
- **URL**: `/api/status`
- **Method**: `GET`
- **Response**: Full system state in JSON.
  ```json
  {
    "alarm": {
      "state": "DISARMED",
      "stateCode": 0,
      "delayRemaining": 0
    },
    "zones": [
      {
        "index": 0,
        "name": "Front Door",
        "state": "NORMAL",
        "stateCode": 0,
        "enabled": true
      }
    ],
    "outputs": 0,
    "network": { "ip": "192.168.10.105", "rssi": -45 },
    "system": { "uptime": 3600, "freeHeap": 182000, "version": "0.1.0" }
  }
  ```

#### 2. Arm System
- **URL**: `/api/arm`
- **Method**: `POST`
- **Body**: `{"pin": "1234", "mode": "away"|"home"}`
- **Behavior**: Calls `alarmArmAway()` or `alarmArmHome()`.

#### 3. Disarm System
- **URL**: `/api/disarm`
- **Method**: `POST`
- **Body**: `{"pin": "1234"}`
- **Behavior**: Calls `alarmDisarm()`.

#### 4. Zone Bypass
- **URL**: `/api/bypass`
- **Method**: `POST`
- **Body**: `{"zone": 0..15, "bypass": true|false}`
- **Behavior**: Calls `zonesSetBypassed()`.

#### 5. Output Control
- **URL**: `/api/output`
- **Method**: `POST`
- **Body**: `{"channel": 0..15, "state": true|false}`
- **Behavior**: Calls `ioExpanderSetOutput()`.

### API Reference

| Function          | Return | Description                                  |
|-------------------|--------|----------------------------------------------|
| `webServerInit()` | `void` | Initialize server, register routes, and start |

---

## src/main.cpp — Application Entry Point

The main Arduino sketch that ties all modules together.

### setup() Sequence

```
  setup()
    │
    ├── 1. Serial.begin(115200)
    ├── 2. configInit()                  ← open NVS
    ├── 3. ioExpanderInit()              ← I2C + PCF8574 x4
    ├── 4. zonesInit()                   ← 16 zones with defaults
    ├── 5. alarmInit()                   ← state machine, register zone callback
    │      alarmSetCallback(onAlarmEvent)
    ├── 6. smsGatewayInit()              ← HTTP client config
    │      smsCmdInit()                  ← command parser
    ├── 7. networkInit()                 ← Wi-Fi connect (from NVS)
    ├── 8. configLoad()                  ← override defaults from NVS
    └── 9. cliInit()                     ← print banner, show prompt
```

### loop() Cycle

```
  loop()  ← runs continuously
    │
    │  ┌─── every 20ms ───────────────────────────┐
    ├──│ 1. ioExpanderReadInputs()                │
    │  │ 2. zonesUpdate(inputs)                   │
    │  │    → debounce → callbacks → alarm events │
    │  └──────────────────────────────────────────┘
    │
    ├── 3. alarmUpdate()
    │      → check exit/entry delay timers
    │      → auto-silence siren on timeout
    │
    ├── 4. networkUpdate()
    │      → auto-reconnect Wi-Fi if disconnected
    │
    │  ┌─── every 5000ms ─────────────────────────┐
    ├──│ 5. pollSmsInbox()                        │
    │  │    → smsGatewayPollInbox()               │
    │  │    → smsCmdProcess() for each message    │
    │  │    → smsGatewayDeleteMessage() cleanup   │
    │  └──────────────────────────────────────────┘
    │
    ├── 6. cliUpdate()
    │      → process serial input characters
    │
    └── 7. yield()
           → allow WiFi/system background tasks
```

### Event Handler

The `onAlarmEvent()` function in `main.cpp` bridges alarm events to alert channels.
It now uses structured data for better rate-limiting ("Storm Throttling").

```cpp
void onAlarmEvent(const AlarmEventInfo& info) {
    // 1. Log to Serial
    // 2. Perform Storm Throttling (using info.zoneId)
    // 3. Broadcast to all channels (WhatsApp, SMS, Call)
    // 4. Send to MQTT
}
```

| Event Type (info.event) | Notification Action                        |
|-------------------------|--------------------------------------------|
| `EVT_ALARM_TRIGGERED`   | Broadcast ALERT: details (throttled)      |
| `EVT_ARMED_AWAY`        | Broadcast: System ARMED (Away)             |
| `EVT_ARMED_HOME`        | Broadcast: System ARMED (Home)             |
| `EVT_DISARMED`          | Broadcast: System DISARMED                |
| `EVT_TAMPER`            | Broadcast TAMPER: details                 |
| `EVT_SIREN_ON/OFF`      | Serial Log + MQTT Update                  |
| `EVT_ZONE_TRIGGERED`    | Serial Log + MQTT Update                  |

---

## Data Flow & Timing

### Main Loop Timing Budget

```
  ┌─────────────────────────────────────────────────────────────┐
  │              Main Loop Timing (per cycle)                   │
  ├──────────────────────┬──────────────────────────────────────┤
  │ Task                 │ Typical Duration                     │
  ├──────────────────────┼──────────────────────────────────────┤
  │ I2C read (2 chips)   │ ~1 ms (100 kHz bus, 2 bytes each)  │
  │ Zone debounce logic  │ ~0.01 ms (16 zones, simple math)   │
  │ Alarm state machine  │ ~0.01 ms (comparisons + timer)     │
  │ Network update       │ ~0.01 ms (status check only)       │
  │ SMS poll (if due)    │ ~100–2000 ms (HTTP round trip)     │
  │ CLI update           │ ~0.01 ms (if no input)             │
  │ yield()              │ ~0.01 ms                            │
  ├──────────────────────┼──────────────────────────────────────┤
  │ TOTAL (no SMS)       │ ~1–2 ms per cycle                   │
  │ TOTAL (with SMS)     │ ~100–2000 ms (blocks during HTTP)   │
  └──────────────────────┴──────────────────────────────────────┘
  
  Note: SMS HTTP operations are blocking. However, since they run inside the
`netWorkerTask` on a separate core/context, the main `loop()` remains
highly responsive (20ms) for critical sensor monitoring and local siren
control. The 50ms debounce ensures no false triggers are missed.
```

### Event Propagation

```
  SENSOR → HARDWARE → SOFTWARE → NOTIFICATION
  
  Time:     t=0ms         t=1ms          t=51ms            t=52ms          t=200ms+
            │              │               │                 │                │
  Physical  │  PCF8574     │  zonesUpdate() │                │                │
  contact   │  input LOW   │  detects       │ debounce       │ alarm state    │ SMS sent
  closes    │              │  change,       │ period         │ evaluates      │ to all
            │              │  starts        │ passes,        │ zone type,     │ configured
            │              │  debounce      │ zone →         │ triggers       │ phone
            │              │  timer         │ TRIGGERED      │ alarm,         │ numbers
            │              │               │ callback       │ siren ON       │
            │              │               │ fires          │                │
```

---

## Memory Map

### RAM Usage Breakdown (Estimated)

```
  ┌────────────────────────────────────────────────────────────┐
  │                    RAM Usage (~50 KB)                      │
  ├───────────────────────────┬────────────────────────────────┤
  │ Component                 │ Estimated Size                 │
  ├───────────────────────────┼────────────────────────────────┤
  │ Arduino framework         │ ~30 KB (WiFi, HTTPClient, etc) │
  │ ZoneInfo[16]              │ ~1.2 KB (16 × 76 bytes)        │
  │ alarmTexts[16][80]        │ ~1.3 KB                        │
  │ phoneNumbers[5][20]       │ ~100 bytes                     │
  │ CLI line buffer           │ 128 bytes                      │
  │ SMS message buffer        │ ~1 KB (5 × SmsMessage)         │
  │ ArduinoJson document      │ ~4 KB (allocated on demand)    │
  │ HTTP response strings     │ ~2 KB (allocated on demand)    │
  │ Stack (main task)         │ ~8 KB                          │
  │ Other (PCF8574 lib, etc.) │ ~2 KB                          │
  ├───────────────────────────┼────────────────────────────────┤
  │ TOTAL                     │ ~50 KB / 320 KB available      │
  └───────────────────────────┴────────────────────────────────┘
```

---

*SF_Alarm v0.1.0 — Module Documentation*
