# SF_Alarm

## ESP32 Alarm System Controller for KC868-A16 v1.6

```
  ____  _____      _    _                      
 / ___||  ___|    / \  | | __ _ _ __ _ __ ___  
 \___ \| |_      / _ \ | |/ _` | '__| '_ ` _ \ 
  ___) |  _|    / ___ \| | (_| | |  | | | | | |
 |____/|_|     /_/   \_\_|\__,_|_|  |_| |_| |_|
                                                 
    KC868-A16 v1.6 Alarm System Controller
    Firmware v0.1.0
```

A full-featured **16-zone alarm system controller** firmware built for the
[KinCony KC868-A16 v1.6](https://www.kincony.com/esp32-16-channel-relay-module-kc868-a16.html)
ESP32-based I/O board. The system monitors up to **16 opto-isolated digital
inputs** via I2C (PCF8574) for alarm zone triggers and communicates with a
**Cudy LT500D** 4G router over HTTP to send and receive **SMS alerts** — using
a command set compatible with the popular **GA09 8-channel SMS alarm module**,
extended to support all 16 zones.

---

## Table of Contents

- [Features](#features)
- [Hardware Requirements](#hardware-requirements)
- [System Architecture](#system-architecture)
- [Wiring Guide](#wiring-guide)
- [Building & Flashing](#building--flashing)
- [First-Time Setup](#first-time-setup)
- [SMS Command Reference (GA09 Compatible)](#sms-command-reference-ga09-compatible)
- [Serial CLI Reference](#serial-cli-reference)
- [Web Dashboard](#web-dashboard)
- [Configuration & Persistence](#configuration--persistence)
- [Alarm System Operation](#alarm-system-operation)
- [SMS Gateway (Cudy LT500D)](#sms-gateway-cudy-lt500d)
- [Project Structure](#project-structure)
- [Module Documentation](#module-documentation)
- [Troubleshooting](#troubleshooting)
- [License](#license)

---

## Features

```
  +------------------------------------------------------------------+
  |                        SF_Alarm Features                         |
  +------------------------------------------------------------------+
  |                                                                  |
  |  [x] 16 opto-isolated alarm zone inputs (dry contact)           |
  |  [x] 16 MOSFET outputs (siren, strobe, relays)                  |
  |  [x] GA09-compatible SMS command set (extended to 16 channels)   |
  |  [x] SMS alerts via Cudy LT500D 4G router (HTTP/LuCI)           |
  |  [x] Receive & process SMS commands remotely                     |
  |  [x] Up to 5 alert phone numbers                                |
  |  [x] Customizable alarm text per zone                            |
  |  [x] NO/NC (normally open/closed) wiring per zone               |
  |  [x] 4 zone types: Instant, Delayed, 24-Hour, Follower          |
  |  [x] ARM AWAY / ARM HOME modes                                  |
  |  [x] Configurable entry/exit delays                              |
  |  [x] Siren auto-timeout (configurable duration)                  |
  |  [x] PIN-code protection for arm/disarm                          |
  |  [x] Zone bypass/unbypass                                        |
  |  [x] Input debouncing (50ms default, configurable)               |
  |  [x] Full serial CLI for local configuration & diagnostics       |
  |  [x] Web Dashboard for real-time monitoring & control           |
  |  [x] NVS-based persistent configuration (survives power cycles)  |
  |  [x] Wi-Fi connectivity with auto-reconnect                     |
  |  [x] Event-driven SMS notifications                              |
  |  [x] Session management with auto-relogin to router              |
  |  [x] Live input monitoring mode                                  |
  |                                                                  |
  +------------------------------------------------------------------+
```

---

## Hardware Requirements

### KC868-A16 v1.6 Board

| Specification         | Detail                                       |
|----------------------|-----------------------------------------------|
| **MCU**              | ESP-WROOM-32 (240 MHz dual-core, 320 KB RAM) |
| **Digital Inputs**   | 16x opto-isolated dry contact (EL357)        |
| **Digital Outputs**  | 16x MOSFET (12V/24V, NCE60P10K via TLP181)   |
| **Analog Inputs**    | 4x (0–5 V, GPIOs 36/39/34/35)               |
| **I2C Expanders**    | 4x PCF8574P (2 for inputs, 2 for outputs)    |
| **Connectivity**     | Wi-Fi, Ethernet (LAN8720A), RS485            |
| **RF**               | 433 MHz transmit & receive modules            |
| **USB**              | Type-C for programming                        |
| **Power**            | 12V DC                                        |

### I2C Address Map

```
  I2C Bus: SDA = GPIO4, SCL = GPIO5, 100 kHz
  
  ┌────────────────────────────────────────────────────┐
  │              I2C Device Address Map                │
  ├──────────────────┬─────────────┬───────────────────┤
  │ PCF8574 Chip     │ I2C Address │ Function          │
  ├──────────────────┼─────────────┼───────────────────┤
  │ Input  Chip 1    │   0x22      │ Inputs  1 – 8     │
  │ Input  Chip 2    │   0x21      │ Inputs  9 – 16    │
  │ Output Chip 1    │   0x24      │ Outputs 1 – 8     │
  │ Output Chip 2    │   0x25      │ Outputs 9 – 16    │
  └──────────────────┴─────────────┴───────────────────┘
```

### Cudy LT500D 4G Router

| Specification         | Detail                                   |
|----------------------|-------------------------------------------|
| **Model**            | Cudy LT500D                              |
| **Connectivity**     | 4G LTE Cat 6                             |
| **Firmware**         | OpenWrt / LuCI                           |
| **SMS Interface**    | Via LuCI web UI (HTTP)                   |
| **SIM Card**         | Standard SIM with SMS-capable plan       |
| **Default IP**       | 192.168.10.1                             |
| **Default Login**    | admin / admin                            |

> **Important:** The SIM card in the Cudy LT500D **must** have an active SMS
> plan and no PIN lock. Disable SIM PIN before inserting the card.

### Additional Components

| Component         | Purpose                              | Connection          |
|-------------------|--------------------------------------|---------------------|
| Alarm sensors     | PIR, reed switch, glass break, etc.  | Inputs S1–S16 → GND |
| Siren / strobe    | Audible/visual alarm                 | Output 1 (default)   |
| 12V DC PSU        | Power supply for KC868-A16           | DC input terminals   |
| Ethernet cable    | KC868-A16 ↔ Cudy LT500D (optional)  | LAN port            |

---

## System Architecture

```
  ┌──────────────────────────────────────────────────────────────────┐
  │                      PHYSICAL LAYER                             │
  │                                                                  │
  │   ┌─────────────┐    dry contact    ┌──────────────────────┐    │
  │   │ PIR Sensor  │───────────────────│                      │    │
  │   └─────────────┘                   │                      │    │
  │   ┌─────────────┐    dry contact    │   KC868-A16 v1.6     │    │
  │   │ Reed Switch │───────────────────│                      │    │
  │   └─────────────┘                   │   ┌──────────────┐   │    │
  │   ┌─────────────┐    dry contact    │   │ ESP-WROOM-32 │   │    │
  │   │ Glass Break │───────────────────│   │              │   │    │
  │   └─────────────┘                   │   │  GPIO4 SDA ──┼───┼──┐ │
  │        ...                          │   │  GPIO5 SCL ──┼───┼──┤ │
  │   ┌─────────────┐    dry contact    │   └──────────────┘   │  │ │
  │   │ Sensor 16   │───────────────────│                      │  │ │
  │   └─────────────┘                   │   ┌──────────┐       │  │ │
  │                                     │   │ PCF8574  │ ◄─────┼──┘ │
  │                                     │   │  0x22    │ IN1-8 │    │
  │                                     │   ├──────────┤       │    │
  │   ┌─────────────┐   12V MOSFET      │   │ PCF8574  │       │    │
  │   │   SIREN     │◄──────────────────│   │  0x21    │ IN9-16│    │
  │   └─────────────┘                   │   ├──────────┤       │    │
  │   ┌─────────────┐   12V MOSFET      │   │ PCF8574  │       │    │
  │   │   STROBE    │◄──────────────────│   │  0x24    │ OUT1-8│    │
  │   └─────────────┘                   │   ├──────────┤       │    │
  │                                     │   │ PCF8574  │       │    │
  │                                     │   │  0x25    │OUT9-16│    │
  │                                     │   └──────────┘       │    │
  │                                     └──────────┬───────────┘    │
  │                                                │ Wi-Fi/ETH      │
  │                                     ┌──────────▼───────────┐    │
  │                                     │  Local Network (LAN) │    │
  │                                     │  ┌────────────────┐  │    │
  │                                     │  │ Web Browser    │  │    │
  │                                     │  │ (Dashboard)    │  │    │
  │                                     │  └───────┬────────┘  │    │
  │                                     └──────────┼───────────┘    │
  │                                                │                │
  │                                     ┌──────────▼───────────┐    │
  │                                     │  Cudy LT500D Router  │    │
  │                                     │  ┌────────────────┐  │    │
  │                                     │  │  LuCI Web UI   │  │    │
  │                                     │  │  SMS Send/Recv │  │    │
  │                                     │  └────────────────┘  │    │
  │                                     └──────────┬───────────┘    │
  │                                                │ 4G LTE         │
  │                                     ┌──────────▼───────────┐    │
  │                                     │    CELL NETWORK      │    │
  │                                     │  ┌────────────────┐  │    │
  │                                     │  │  SMS to/from   │  │    │
  │                                     │  │  User Phone(s) │  │    │
  │                                     │  └────────────────┘  │    │
  │                                     └──────────────────────┘    │
  └──────────────────────────────────────────────────────────────────┘
```

### Software Module Architecture

```
  ┌─────────────────────────────────────────────────────────────┐
  │                        main.cpp                             │
  │                                                             │
  │   setup():  init all modules                                │
  │   loop():   scan → zones → alarm → SMS → CLI → yield       │
  │                                                             │
  │   ┌────────────────────┐    ┌────────────────────────┐     │
  │   │    io_expander      │    │      network           │     │
  │   │  ┌──────────────┐  │    │  ┌──────────────────┐  │     │
  │   │  │ PCF8574 x4   │  │    │  │ Wi-Fi STA        │  │     │
  │   │  │ read inputs  │  │    │  │ auto-reconnect   │  │     │
  │   │  │ write outputs│  │    │  └──────────────────┘  │     │
  │   │  └──────────────┘  │    └────────────────────────┘     │
  │   └────────┬───────────┘                 │                  │
  │            │ uint16_t bitmask            │ HTTP              │
  │            ▼                             ▼                  │
  │   ┌────────────────────┐    ┌────────────────────────┐     │
  │   │    alarm_zones      │    │     sms_gateway        │     │
  │   │  ┌──────────────┐  │    │  ┌──────────────────┐  │     │
  │   │  │ 16 zones     │  │    │  │ LuCI HTTP client │  │     │
  │   │  │ debounce     │  │    │  │ login / send     │  │     │
  │   │  │ NO/NC logic  │  │    │  │ poll inbox       │  │     │
  │   │  │ state track  │  │    │  │ delete message   │  │     │
  │   │  └──────────────┘  │    │  └──────────────────┘  │     │
  │   └────────┬───────────┘    └────────────┬───────────┘     │
  │            │ callbacks                   │ SmsMessage       │
  │            ▼                             ▼                  │
  │   ┌────────────────────┐    ┌────────────────────────┐     │
  │   │  alarm_controller   │◄──│     sms_commands       │     │
  │   │  ┌──────────────┐  │    │  ┌──────────────────┐  │     │
  │   │  │ state machine│  │    │  │ GA09 parser      │  │     │
  │   │  │ arm/disarm   │  │    │  │ phone management │  │     │
  │   │  │ delays       │  │    │  │ alarm text       │  │     │
  │   │  │ siren control│  │    │  │ NC/NO config     │  │     │
  │   │  └──────────────┘  │    │  │ auth whitelist   │  │     │
  │   └────────────────────┘    │  └──────────────────┘  │     │
  │            │                └────────────────────────┘     │
  │            │ events (EVT_ALARM, etc.)                       │
  │            ▼                                                │
  │   ┌────────────────────────────────────────────────────┐   │
  │   │        Notification System (in main.cpp)            │   │
  │   │  onAlarmEvent() → smsCmdSendAlert() to all phones   │   │
  │   └────────────────────────────────────────────────────┘   │
  │                                                             │
  │   ┌────────────────────┐    ┌────────────────────────┐     │
  │   │    serial_cli       │    │      web_server        │     │
  │   │  ┌──────────────┐  │    │  ┌──────────────────┐  │     │
  │   │  │ 30+ commands │  │    │  │ PsychicHttp      │  │     │
  │   │  │ line editor  │  │    │  │ REST API         │  │     │
  │   │  │ backspace    │  │    │  │ Embedded Dashboard│  │     │
  │   │  └──────────────┘  │    │  └──────────────────┘  │     │
  │   └────────────────────┘    └────────────┬───────────┘     │
  │                                          │                  │
  │   ┌────────────────────┐    ┌────────────▼───────────┐     │
  │   │   config_manager   │    │    web_dashboard       │     │
  │   │  ┌──────────────┐  │    │  ┌──────────────────┐  │     │
  │   │  │ ESP32 NVS        │  │    │  │ Real-time status │  │     │
  │   │  │ save/load all    │  │    │  │ Control panel    │  │     │
  │   │  │ factory reset    │  │    │  │ Configuration    │  │     │
  │   │  └──────────────┘  │    │  └──────────────────┘  │     │
  │   └────────────────────┘    └────────────────────────┘     │
  └─────────────────────────────────────────────────────────────┘
```

### Alarm State Machine

```
                        ARM (pin)
                           │
                           ▼
  ┌──────────┐    ┌────────────────┐    timeout    ┌──────────────┐
  │          │    │                │──────────────►│              │
  │ DISARMED │───►│  EXIT_DELAY    │               │  ARMED_AWAY  │
  │          │    │  (30 sec)      │    ┌──────────│  ARMED_HOME  │
  └──────────┘    └────────────────┘    │          └──────┬───────┘
       ▲                                │                 │
       │                                │  delayed zone   │ instant/24H
       │              DISARM (pin)      │  triggered      │ zone triggered
       │◄───────────────────────────────┤                 │
       │                                ▼                 │
       │                    ┌────────────────┐            │
       │       DISARM (pin) │                │            │
       │◄───────────────────│  ENTRY_DELAY   │            │
       │                    │  (15 sec)      │            │
       │                    └───────┬────────┘            │
       │                            │ timeout             │
       │                            ▼                     │
       │                    ┌────────────────┐            │
       │       DISARM (pin) │                │◄───────────┘
       │◄───────────────────│  TRIGGERED     │
       │                    │  (siren ON)    │
       │                    │  (SMS sent)    │
       │                    └────────────────┘
       │                            │
       │                            │ siren auto-timeout
       │                            │ (180 sec default)
       │                            ▼
       │                    Siren OFF, alarm state
       │                    remains TRIGGERED until
       │                    DISARM command received
       │◄───────────────────────────┘
```

### Zone Trigger Logic by Type

```
  ┌─────────────────────────────────────────────────────────────┐
  │                    Zone Trigger Matrix                       │
  ├──────────┬──────────┬───────────┬────────────┬─────────────┤
  │ Zone     │ DISARMED │ ARMED     │ ENTRY      │ Description │
  │ Type     │          │ (away/    │ DELAY      │             │
  │          │          │  home)    │ running    │             │
  ├──────────┼──────────┼───────────┼────────────┼─────────────┤
  │ INSTANT  │ ignored  │ immediate │ immediate  │ Interior    │
  │          │          │ ALARM     │ ALARM      │ motion      │
  ├──────────┼──────────┼───────────┼────────────┼─────────────┤
  │ DELAYED  │ ignored  │ starts    │ (already   │ Entry/exit  │
  │          │          │ entry     │  running)  │ doors       │
  │          │          │ delay     │            │             │
  ├──────────┼──────────┼───────────┼────────────┼─────────────┤
  │ 24-HOUR  │ instant  │ immediate │ immediate  │ Fire, panic │
  │          │ ALARM    │ ALARM     │ ALARM      │ tamper      │
  ├──────────┼──────────┼───────────┼────────────┼─────────────┤
  │ FOLLOWER │ ignored  │ immediate │ (no extra  │ Interior    │
  │          │          │ ALARM     │  action)   │ path        │
  └──────────┴──────────┴───────────┴────────────┴─────────────┘
```

---

## Wiring Guide

### Alarm Sensor Connections

The KC868-A16 v1.6 inputs are **opto-isolated dry contact** inputs.
To trigger an input, short the input pin to **GND**. The inputs have
internal 12V pull-ups via the opto-isolator circuit.

```
  SENSOR WIRING (Normally Open — default)
  ========================================
  
  Sensor (N.O.)         KC868-A16 Input Terminal
  ─────────────         ──────────────────────────
  
       ┌──────┐
  ─────┤ Reed │──────── S1  (Zone 1)
       │Switch│
  ─────┤ N.O. │──────── GND (Common ground)
       └──────┘
  
  
  When the reed switch CLOSES (magnet removed = door open),
  S1 is shorted to GND → input goes active → zone TRIGGERED.
  
  
  SENSOR WIRING (Normally Closed)
  ===============================
  
       ┌──────┐
  ─────┤ PIR  │──────── S2  (Zone 2, configured as NC)
       │Sensor│
  ─────┤ N.C. │──────── GND (Common ground)
       └──────┘
  
  When NC loop OPENS (wire cut or sensor triggered),
  S2 is disconnected from GND → input goes inactive → zone TRIGGERED.
```

### Siren / Strobe Connection

```
  SIREN WIRING
  ============
  
  KC868-A16 Output 1 ──────┐
  (MOSFET, 12V)            │
                            ▼
                     ┌──────────────┐
                     │              │
                     │    SIREN     │ (12V DC siren)
                     │              │
                     └──────┬───────┘
                            │
  12V Power Supply ─────────┘
  (shared GND with KC868-A16)
  
  Note: The MOSFET outputs switch the LOW side (ground) of the load.
  The siren's positive terminal connects to +12V, and the other
  terminal connects to the KC868-A16 output. When output 1 is ON,
  the MOSFET conducts to GND, completing the circuit.
```

### Network Connection

```
  NETWORK TOPOLOGY
  ================
  
  ┌───────────────┐            ┌──────────────────┐
  │  KC868-A16    │            │  Cudy LT500D     │
  │               │  Wi-Fi     │                  │
  │  ESP32 ───────┼────────────┤  LAN / Wi-Fi AP  │
  │               │            │                  │
  │  OR           │  Ethernet  │                  │
  │               │            │                  │
  │  LAN8720A ────┼────────────┤  LAN port        │
  │               │            │                  │
  └───────────────┘            │  4G LTE Modem ───┼──── Cell Network
                               │                  │
                               │  SMS Inbox       │
                               └──────────────────┘
  
  The ESP32 connects to the Cudy LT500D's local network
  (Wi-Fi or Ethernet) and communicates with the router's
  LuCI web interface via HTTP to send/receive SMS.
```

---

## Building & Flashing

### Prerequisites

| Tool           | Version   | Purpose                                  |
|---------------|-----------|------------------------------------------|
| PlatformIO    | 6.x+      | Build system & library management        |
| VS Code       | Latest    | IDE (with PlatformIO extension)          |
| USB-C cable   | —         | Connect KC868-A16 to computer            |
| CP2102 driver | Latest    | USB-to-serial driver (if not auto)       |

### Build

```bash
# Clone the repository
git clone https://github.com/sjfaustino/SF_Alarm.git
cd SF_Alarm

# Build the firmware
pio run

# Expected output:
# RAM:   [==        ]  15.3% (used 50,036 bytes from 327,680 bytes)
# Flash: [========  ]  75.8% (used 993,005 bytes from 1,310,720 bytes)
# =========== [SUCCESS] ===========
```

### Flash

```bash
# Flash to the KC868-A16 via USB-C
pio run --target upload

# If the port is not auto-detected, specify it:
pio run --target upload --upload-port COM3    # Windows
pio run --target upload --upload-port /dev/ttyUSB0  # Linux
```

### Serial Monitor

```bash
# Open serial monitor at 115200 baud
pio device monitor --baud 115200

# You should see:
# ========================================
#   SF_Alarm v0.1.0 — Starting up...
#   KC868-A16 v1.6 Alarm System Controller
# ========================================
# [INIT] I/O Expander...
# [IO] Init OK — IN1:OK IN2:OK OUT1:OK OUT2:OK
# ...
# sf_alarm>
```

---

## First-Time Setup

After flashing, connect via serial (115200 baud) and follow these steps:

### Step 1: Configure Wi-Fi

Connect the ESP32 to the same network as the Cudy LT500D router.

```
sf_alarm> wifi YourSSID YourPassword
[NET] Wi-Fi credentials updated: YourSSID
[NET] Connected! IP: 192.168.10.105  RSSI: -42 dBm
```

### Step 2: Configure Router Credentials

Set the Cudy LT500D's IP address and login credentials.

```
sf_alarm> router 192.168.10.1 admin admin
[SMS] Credentials updated — router: 192.168.10.1
```

### Step 3: Add Alert Phone Numbers

Add phone numbers that will receive SMS alerts and can send commands.

```
sf_alarm> phone add +1234567890
[CMD] Added phone [1]: +1234567890

sf_alarm> phone add +0987654321
[CMD] Added phone [2]: +0987654321

sf_alarm> phone list
Phone numbers (2):
  [01] +1234567890
  [02] +0987654321
```

### Step 4: Set Alarm PIN

```
sf_alarm> pin 5678
[ALARM] PIN updated
```

### Step 5: Name Your Zones

```
sf_alarm> zone 1 name Front Door
Zone 1 name: Front Door

sf_alarm> zone 2 name Back Door
Zone 2 name: Back Door

sf_alarm> zone 3 name Living Room PIR
Zone 3 name: Living Room PIR

sf_alarm> zone 4 name Smoke Detector
Zone 4 name: Smoke Detector
```

### Step 6: Configure Zone Types

```
sf_alarm> zone 1 type dly
Zone 1 type updated          (Delayed — entry/exit door)

sf_alarm> zone 2 type inst
Zone 2 type updated          (Instant — back door, no delay)

sf_alarm> zone 3 type flw
Zone 3 type updated          (Follower — interior path)

sf_alarm> zone 4 type 24h
Zone 4 type updated          (24-Hour — always active, even disarmed)
```

### Step 7: Configure NC/NO Wiring

```
sf_alarm> zone 4 nc
Zone 4 set to NC             (NC sensor like smoke detector)
```

### Step 8: Set Custom Alarm Texts

```
sf_alarm> zone 1 text Front door opened!
[CMD] Zone 1 alarm text: "Front door opened!"

sf_alarm> zone 4 text FIRE! Smoke detector activated!
[CMD] Zone 4 alarm text: "FIRE! Smoke detector activated!"
```

### Step 9: Configure Timing

```
sf_alarm> delay exit 30
[ALARM] Exit delay set to 30 seconds

sf_alarm> delay entry 15
[ALARM] Entry delay set to 15 seconds

sf_alarm> siren dur 180
[ALARM] Siren duration set to 180 seconds
```

### Step 10: Test SMS

```
sf_alarm> test sms +1234567890 Hello from SF_Alarm!
Sending test SMS to +1234567890...
[SMS] Logged in to LuCI (token: a3f2b1c8...)
[SMS] Sent to +1234567890: "Hello from SF_Alarm!"
SMS sent OK
```

### Step 11: Save Configuration

**CRITICAL:** Save to NVS so settings persist across power cycles.

```
sf_alarm> save
[CFG] Saving configuration to NVS...
[CFG] Configuration saved
```

### Step 12: Verify

```
sf_alarm> status
=== Alarm System Status ===
  State:          DISARMED
  Siren:          OFF
  Siren channel:  0
  Exit delay:     30 sec
  Entry delay:    15 sec
  Siren duration: 180 sec
===========================
--- Zone Status ---
  Z01 Front Door           [NORMAL] DLY NO
  Z02 Back Door            [NORMAL] INST NO
  Z03 Living Room PIR      [NORMAL] FLW NO
  Z04 Smoke Detector       [NORMAL] 24H NC
  Z05 Zone 5               [NORMAL] INST NO
  ...
-------------------
--- Network Status ---
  SSID:       YourSSID
  Status:     CONNECTED
  IP:         192.168.10.105
  Gateway:    192.168.10.1
  RSSI:       -42 dBm
  MAC:        AA:BB:CC:DD:EE:FF
----------------------
```

---

## SMS Command Reference (GA09 Compatible)

The SMS command set is compatible with the popular **GA09 8-channel SMS alarm
module**, extended to support all **16 channels** of the KC868-A16.

> **Important:** SMS commands are case-insensitive. No spaces before or after
> commands (except within alarm text content). Only phone numbers registered in
> the alert list can send commands.

### Phone Number Management

| SMS Command              | Description                                | Example                    | Reply                          |
|-------------------------|--------------------------------------------|----------------------------|-------------------------------|
| `#01#number#`           | Set alert phone slot 1                     | `#01#+1234567890#`        | `SF_Alarm: Phone 01 set to +1234567890` |
| `#02#number#`           | Set alert phone slot 2                     | `#02#+0987654321#`        | `SF_Alarm: Phone 02 set to +0987654321` |
| `#03#number#` – `#05#`  | Set alert phone slots 3–5                  | `#05#+1112223333#`        | `SF_Alarm: Phone 05 set to +1112223333` |
| `@#num1#num2#...`       | Set multiple phones at once                | `@#+111222333#+444555666#` | `SF_Alarm: 2 phone number(s) configured` |

> **Note on first registration:** When no phone numbers are configured yet,
> the first `#01#number#` SMS from any sender will be accepted. After that,
> only authorized numbers can send commands.

### Alarm Text Configuration

| SMS Command              | Description                                | Example                    | Reply                          |
|-------------------------|--------------------------------------------|----------------------------|-------------------------------|
| `#1#text`               | Set alarm text for zone 1                  | `#1#Front door opened!`   | `SF_Alarm: Zone 1 alarm text updated` |
| `#2#text`               | Set alarm text for zone 2                  | `#2#Back door breach!`    | `SF_Alarm: Zone 2 alarm text updated` |
| `#16#text`              | Set alarm text for zone 16                 | `#16#Garage sensor`       | `SF_Alarm: Zone 16 alarm text updated` |

> **Differentiation from phone commands:** Phone commands use a **leading zero**
> (`#01#` to `#05#`), while zone text commands use **no leading zero** (`#1#`
> to `#16#`).

### NC/NO Configuration

| SMS Command    | Description                                          | Example     |
|---------------|------------------------------------------------------|-------------|
| `*NC0`        | Set **all** zones to Normally Open (NO)              | `*NC0`      |
| `*NCALL`      | Set **all** zones to Normally Closed (NC)            | `*NCALL`    |
| `*NC246`      | Set zones 2, 4, 6 as NC; others remain NO            | `*NC246`    |
| `*NC1,10,12`  | Set zones 1, 10, 12 as NC (comma-separated for >9)   | `*NC1,10,12`|

```
  NC/NO Config Examples:
  
  *NC0          →  All 16 zones: NO NO NO NO NO NO NO NO NO NO NO NO NO NO NO NO
  *NCALL        →  All 16 zones: NC NC NC NC NC NC NC NC NC NC NC NC NC NC NC NC
  *NC246        →  Zone: 1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16
                         NO NC NO NC NO NC NO NO NO NO NO NO NO NO NO NO
  *NC1,10,12    →  Zone: 1  2  3  4  5  6  7  8  9  10 11 12 13 14 15 16
                         NC NO NO NO NO NO NO NO NO NC NO NC NO NO NO NO
```

### Arm / Disarm

| SMS Command       | Description                      | Reply                                 |
|-------------------|----------------------------------|---------------------------------------|
| `ARM <pin>`       | Arm system in AWAY mode          | `SF_Alarm: Arming AWAY. Exit delay started.` |
| `ARM HOME <pin>`  | Arm system in HOME mode          | `SF_Alarm: Arming HOME. Exit delay started.` |
| `DISARM <pin>`    | Disarm the system                | `SF_Alarm: System DISARMED.`         |

### Status & Control

| SMS Command    | Description                      | Reply                                          |
|---------------|----------------------------------|-------------------------------------------------|
| `@#STATUS?`   | Query full system status         | `SF_Alarm [ARMED_AWAY] Zones:0 triggered ...`  |
| `STATUS`      | Query system status (alias)      | (same as above)                                 |
| `MUTE`        | Silence active siren             | `SF_Alarm: Siren MUTED.`                       |
| `BYPASS 3`    | Bypass zone 3                    | `SF_Alarm: Zone 3 BYPASSED`                    |
| `UNBYPASS 3`  | Restore zone 3                   | `SF_Alarm: Zone 3 restored`                    |
| `%#T120`      | Set report interval (minutes)    | `SF_Alarm: Periodic status report set to 120 minutes` |
| `@#ARM11110000`| Binary zone enable (S1-S8)      | `SF_Alarm: Zone enable/disable configuration updated` |
| `&...`        | Call numbers (voice)             | `SF_Alarm: Voice call alerts not supported...` |
| `HELP`        | List available commands          | (command list summary)                          |

### SMS Alert Messages

When an alarm is triggered, **all configured phone numbers** receive an SMS:

```
  Alarm trigger (custom text):    "SF_Alarm ALERT: Front door opened!"
  System armed (away):            "SF_Alarm: System ARMED (Away)"
  System armed (home):            "SF_Alarm: System ARMED (Home)"
  System disarmed:                "SF_Alarm: System DISARMED"
  Tamper detected:                "SF_Alarm TAMPER: Zone 4 (Smoke Detector)"
```

---

## Serial CLI Reference

Connect via USB at **115200 baud**. The CLI provides a `sf_alarm>` prompt.

### Command Summary

```
  ┌──────────────────────────────────────────────────────────────┐
  │                    Serial CLI Commands                       │
  ├──────────────────────┬───────────────────────────────────────┤
  │ SYSTEM               │                                       │
  ├──────────────────────┼───────────────────────────────────────┤
  │ status               │ Full system status overview           │
  │ config               │ Show saved configuration              │
  │ save                 │ Save config to NVS (persist)          │
  │ load                 │ Load config from NVS                  │
  │ factory              │ Factory reset (requires YES confirm)  │
  │ reboot               │ Restart the ESP32                     │
  │ help                 │ Show all commands                     │
  ├──────────────────────┼───────────────────────────────────────┤
  │ ALARM CONTROL        │                                       │
  ├──────────────────────┼───────────────────────────────────────┤
  │ arm <pin>            │ Arm system (away mode)                │
  │ arm home <pin>       │ Arm system (home mode)                │
  │ disarm <pin>         │ Disarm system                         │
  │ mute                 │ Mute/silence siren                    │
  ├──────────────────────┼───────────────────────────────────────┤
  │ ZONE CONFIGURATION   │                                       │
  ├──────────────────────┼───────────────────────────────────────┤
  │ zones                │ Show all zone states                  │
  │ zone <n>             │ Show info for zone n                  │
  │ zone <n> name <text> │ Set zone name                         │
  │ zone <n> type <t>    │ Set type: inst|dly|24h|flw            │
  │ zone <n> nc          │ Set wiring to Normally Closed         │
  │ zone <n> no          │ Set wiring to Normally Open           │
  │ zone <n> enable      │ Enable zone                           │
  │ zone <n> disable     │ Disable zone                          │
  │ zone <n> bypass      │ Bypass zone                           │
  │ zone <n> unbypass    │ Restore zone                          │
  │ zone <n> text <msg>  │ Set custom alarm SMS text             │
  ├──────────────────────┼───────────────────────────────────────┤
  │ PHONE NUMBERS        │                                       │
  ├──────────────────────┼───────────────────────────────────────┤
  │ phone add <number>   │ Add alert phone number                │
  │ phone remove <number>│ Remove phone number                   │
  │ phone list           │ List all phone numbers                │
  │ phone clear          │ Clear all phone numbers               │
  ├──────────────────────┼───────────────────────────────────────┤
  │ NETWORK              │                                       │
  ├──────────────────────┼───────────────────────────────────────┤
  │ wifi <ssid> <pass>   │ Set Wi-Fi credentials & connect       │
  │ router <ip> <u> <p>  │ Set Cudy LT500D credentials           │
  │ network              │ Show network status                   │
  ├──────────────────────┼───────────────────────────────────────┤
  │ TIMING               │                                       │
  ├──────────────────────┼───────────────────────────────────────┤
  │ pin <newpin>         │ Set alarm PIN code                    │
  │ delay exit <sec>     │ Set exit delay (seconds)              │
  │ delay entry <sec>    │ Set entry delay (seconds)             │
  │ siren dur <sec>      │ Set siren auto-off duration           │
  │ siren ch <0-15>      │ Set siren output channel              │
  ├──────────────────────┼───────────────────────────────────────┤
  │ TESTING              │                                       │
  ├──────────────────────┼───────────────────────────────────────┤
  │ inputs               │ Show raw input bitmask                │
  │ outputs              │ Show output bitmask                   │
  │ test sms <num> <msg> │ Send a test SMS                       │
  │ test output <0-15>   │ Toggle an output channel              │
  │ test input           │ Live input monitor (key to stop)      │
  └──────────────────────┴───────────────────────────────────────┘
```

### Live Input Monitor

The `test input` command enters a real-time monitoring mode that displays
input changes as they happen:

```
sf_alarm> test input
Live input monitor (press any key to stop)...
  Inputs: 0x0000 | 1:0 2:0 3:0 4:0 5:0 6:0 7:0 8:0 9:0 10:0 11:0 12:0 13:0 14:0 15:0 16:0
  Inputs: 0x0001 | 1:1 2:0 3:0 4:0 5:0 6:0 7:0 8:0 9:0 10:0 11:0 12:0 13:0 14:0 15:0 16:0
  Inputs: 0x0005 | 1:1 2:0 3:1 4:0 5:0 6:0 7:0 8:0 9:0 10:0 11:0 12:0 13:0 14:0 15:0 16:0
  Inputs: 0x0000 | 1:0 2:0 3:0 4:0 5:0 6:0 7:0 8:0 9:0 10:0 11:0 12:0 13:0 14:0 15:0 16:0
Monitor stopped
```

---

## Web Dashboard

SF_Alarm includes a built-in, mobile-friendly web dashboard for real-time monitoring and control of your alarm system. It is served directly from the ESP32 using the high-performance **PsychicHttp** library.

### Accessing the Dashboard

1.  Ensure your ESP32 is connected to Wi-Fi.
2.  Find the ESP32's IP address via the serial CLI (`status` or `network` command).
3.  Open a web browser and navigate to `http://<ESP32-IP>/`.

### Dashboard Interface (Mockup)

```text
  +------------------------------------------------------------------+
  |  SF_Alarm Dashboard                                      [ 📶 ]  |
  +------------------------------------------------------------------+
  |                                                                  |
  |  +---------------------------+    +---------------------------+  |
  |  |      ALARM STATUS         |    |      SYSTEM INFO          |  |
  |  |  +---------------------+  |    |  IP: 192.168.10.105       |  |
  |  |  |       DISARMED      |  |    |  RSSI: -45 dBm            |  |
  |  |  +---------------------+  |    |  Uptime: 01:24:12         |  |
  |  |                           |    |  Free Heap: 182 KB        |  |
  |  |  [ ARM ] [ HOME ] [ MUTE ]|    |                           |  |
  |  +---------------------------+    +---------------------------+  |
  |                                                                  |
  |  ZONE STATUS                                                     |
  |  +----------+  +----------+  +----------+  +----------+          |
  |  |  Zone 1  |  |  Zone 2  |  |  Zone 3  |  |  Zone 4  |          |
  |  | [NORMAL] |  | [ALERT!] |  | [BYPASS] |  | [NORMAL] |          |
  |  +----------+  +----------+  +----------+  +----------+          |
  |                                                                  |
  |  OUTPUT CONTROL                                                  |
  |  [x] Out 1   [ ] Out 2   [ ] Out 3   [ ] Out 4                   |
  |  [ ] Out 5   [ ] Out 6   [ ] Out 7   [ ] Out 8                   |
  |                                                                  |
  +------------------------------------------------------------------+
```

### Key Features

- **Real-time Updates**: The dashboard automatically refreshes every 2 seconds to show the latest sensor states and alarm status.
- **Arm/Disarm Control**: Securely arm or disarm the system from your phone or PC. A PIN code modal will appear for verification.
- **Zone Management**: View the status of all 16 zones at a glance. Easily bypass problematic zones directly from the UI.
- **Output Toggles**: Manually control any of the 16 MOSFET outputs (relays, strobes, etc.) with simple checkboxes.
- **Responsive Design**: Dark-themed, modern interface that works perfectly on both desktop monitors and mobile devices.

### REST API Reference (Simple)

For integration with Home Assistant or other automation platforms, the following endpoints are available:

| Method | Endpoint        | Description                          |
|--------|-----------------|--------------------------------------|
| `GET`  | `/api/status`   | Get full system status as JSON       |
| `POST` | `/api/arm`      | Arm system (`{"pin":"...", "mode":"away|home"}`) |
| `POST` | `/api/disarm`   | Disarm system (`{"pin":"..."}`)      |
| `POST` | `/api/mute`     | Silence the siren                    |
| `POST` | `/api/bypass`   | Bypass zone (`{"zone":0..15, "bypass":true|false}`) |
| `POST` | `/api/output`   | Toggle output (`{"channel":0..15, "state":true|false}`) |

---

---

## Configuration & Persistence

All configuration is stored in the ESP32's **NVS (Non-Volatile Storage)** and
survives power cycles and reboots.

### What Is Saved

| Setting               | NVS Key(s)        | Default              |
|----------------------|-------------------|----------------------|
| Alarm PIN            | `pin`            | `1234`               |
| Exit delay           | `exitDelay`      | 30 seconds           |
| Entry delay          | `entryDelay`     | 15 seconds           |
| Siren duration       | `sirenDur`       | 180 seconds          |
| Siren output channel | `sirenCh`        | 0 (output 1)         |
| Phone count          | `phoneCnt`       | 0                    |
| Phone numbers        | `phone0`–`phone4`| (empty)              |
| Router IP            | `routerIp`       | `192.168.10.1`       |
| Router username      | `routerUser`     | `admin`              |
| Router password      | `routerPass`     | `admin`              |
| Wi-Fi SSID           | `wifiSsid`       | (empty)              |
| Wi-Fi password       | `wifiPass`       | (empty)              |
| Zone names           | `zName0`–`zName15` | `Zone 1`–`Zone 16` |
| Zone types           | `zType0`–`zType15` | INSTANT (0)         |
| Zone wiring          | `zWire0`–`zWire15` | NO (0)              |
| Zone enabled         | `zEn0`–`zEn15`   | true                 |
| Configuration flag   | `configured`     | false                |

### Save & Load Workflow

```
  1. Make changes via CLI or SMS
  2. Run: save          ← writes all settings to NVS
  3. Power cycle        ← board restarts
  4. Automatic: load    ← settings restored from NVS on boot
```

### Factory Reset

```
sf_alarm> factory
Factory reset? Type 'YES' to confirm:
YES
[CFG] Factory reset — clearing NVS...
[CFG] NVS cleared. Restart to apply defaults.
sf_alarm> reboot
```

---

## Alarm System Operation

### Arming the System

**Via Serial CLI:**
```
sf_alarm> arm 1234
[ALARM] Exit delay: 30 seconds
```

**Via SMS:**
```
Send to SIM number:  ARM 1234
Reply received:      SF_Alarm: Arming AWAY. Exit delay started.
```

### What Happens When an Alarm Triggers

```
  ALARM TRIGGER SEQUENCE
  ======================
  
  1. Sensor activates (e.g., door opened)
     │
  2. PCF8574 input changes state
     │ (scanned every 20ms)
     │
  3. Input debounced (50ms)
     │
  4. Zone state changes to TRIGGERED
     │ callback fired → alarm controller
     │
  5. Alarm controller evaluates zone type:
     │
     ├─ INSTANT/24H → immediate ALARM
     │
     ├─ DELAYED → start entry delay countdown
     │   │
     │   ├─ User disarms within delay → no alarm
     │   │
     │   └─ Delay expires → ALARM
     │
     └─ FOLLOWER → instant unless entry delay running
     
  6. ALARM state activated:
     │
     ├─ Siren output turned ON
     │
     ├─ SMS alert sent to ALL configured phones
     │   "SF_Alarm ALERT: Front door opened!"
     │
     └─ Serial log output
  
  7. Siren auto-silences after configured duration (default 180s)
     │
  8. Alarm remains in TRIGGERED state until DISARM command
```

### Disarming

**Via Serial CLI:**
```
sf_alarm> disarm 1234
[ALARM] Siren OFF
[ALARM] State: ... -> DISARMED
```

**Via SMS:**
```
Send:   DISARM 1234
Reply:  SF_Alarm: System DISARMED.
```

---

## SMS Gateway (Cudy LT500D)

### How It Works

The firmware communicates with the Cudy LT500D's **LuCI web interface** via
HTTP requests. There is no official API — the firmware mimics a web browser
session:

```
  SMS SEND FLOW
  =============
  
  ESP32                          Cudy LT500D
    │                                │
    │  POST /cgi-bin/luci            │
    │  body: username + password     │
    │ ──────────────────────────────►│
    │                                │
    │  302 + Set-Cookie: sysauth=... │
    │ ◄──────────────────────────────│
    │                                │
    │  POST /cgi-bin/luci/.../sms    │
    │  Cookie: sysauth=...           │
    │  body: action=send&phone=...   │
    │ ──────────────────────────────►│
    │                                │
    │  200 OK                        │
    │ ◄──────────────────────────────│
    │                                │
    │                                │── 4G ──► SMS to phone
    │                                │
  
  
  SMS RECEIVE FLOW
  ================
  
  Phone ── 4G ──► SMS to SIM        │
                                     │
  ESP32                          Cudy LT500D
    │                                │
    │  GET /cgi-bin/luci/.../sms     │
    │  Cookie: sysauth=...           │
    │  ?action=read                  │
    │ ──────────────────────────────►│
    │                                │
    │  200 OK (JSON or HTML body)    │
    │  [{sender, body, timestamp}]   │
    │ ◄──────────────────────────────│
    │                                │
    │  Parse messages                │
    │  Execute commands              │
    │  Send replies                  │
    │                                │
    │  POST action=delete&id=...     │
    │ ──────────────────────────────►│  (cleanup)
```

### Session Management

- The firmware logs in once and caches the `sysauth` session cookie.
- If a request returns HTTP 401 or 403, the firmware automatically re-logs in.
- There is a retry mechanism with exponential backoff (up to 3 attempts).

### Router Setup Checklist

1. Insert SIM card into Cudy LT500D (PIN disabled, SMS plan active).
2. Power on the router and wait for 4G connection.
3. Verify SMS works by browsing to:
   `http://192.168.10.1/cgi-bin/luci/admin/network/gcom/sms?iface=4g`
4. Send a test SMS from the web interface to confirm the SIM works.
5. Note the router's LAN IP, username, and password.
6. Configure the ESP32 via CLI: `router <ip> <user> <pass>`

### Adjusting the SMS Gateway

> **Important:** The exact HTTP endpoints and form parameters used by the
> firmware are based on typical OpenWrt/LuCI/gcom configurations. If your
> Cudy LT500D firmware differs, you may need to update `sms_gateway.cpp`.
> Use your browser's developer tools (F12 → Network tab) to inspect the
> actual requests made by the SMS page.

---

## Project Structure

```
  SF_Alarm/
  ├── platformio.ini              ← Build configuration
  ├── README.md                   ← This file
  ├── docs/
  │   └── MODULES.md              ← Detailed module documentation
  ├── include/
  │   └── config.h                ← Hardware pins, I2C addresses, defaults
  └── src/
      ├── main.cpp                ← Application entry point
      ├── io_expander.h           ← PCF8574 I/O driver (header)
      ├── io_expander.cpp         ← PCF8574 I/O driver (impl)
      ├── alarm_zones.h           ← Zone manager (header)
      ├── alarm_zones.cpp         ← Zone manager (impl)
      ├── alarm_controller.h      ← Alarm state machine (header)
      ├── alarm_controller.cpp    ← Alarm state machine (impl)
      ├── sms_gateway.h           ← SMS HTTP client (header)
      ├── sms_gateway.cpp         ← SMS HTTP client (impl)
      ├── sms_commands.h          ← GA09 SMS command parser (header)
      ├── sms_commands.cpp        ← GA09 SMS command parser (impl)
      ├── config_manager.h        ← NVS persistence (header)
      ├── config_manager.cpp      ← NVS persistence (impl)
      ├── network.h               ← Wi-Fi connectivity (header)
      ├── network.cpp             ← Wi-Fi connectivity (impl)
      ├── serial_cli.h            ← Serial CLI (header)
      └── serial_cli.cpp          ← Serial CLI (impl)
```

### Memory Usage

| Resource | Used       | Available | Utilization |
|----------|-----------|-----------|-------------|
| RAM      | 50,036 B  | 327,680 B | 15.3%      |
| Flash    | 993,005 B | 1,310,720 B | 75.8%    |

---

## Module Documentation

Detailed documentation for each module is available in
[docs/MODULES.md](docs/MODULES.md).

### Quick Module Summary

| Module             | Files                            | Responsibility                        |
|-------------------|----------------------------------|---------------------------------------|
| **io_expander**   | `io_expander.h/.cpp`             | I2C PCF8574 read/write (4 chips)     |
| **alarm_zones**   | `alarm_zones.h/.cpp`             | 16-zone manager, debounce, state     |
| **alarm_controller** | `alarm_controller.h/.cpp`     | State machine, siren, events         |
| **sms_gateway**   | `sms_gateway.h/.cpp`             | HTTP client for Cudy LT500D LuCI    |
| **sms_commands**  | `sms_commands.h/.cpp`            | GA09 SMS command parser              |
| **config_manager**| `config_manager.h/.cpp`          | NVS save/load/reset                  |
| **network**       | `network.h/.cpp`                 | Wi-Fi STA with auto-reconnect        |
| **serial_cli**    | `serial_cli.h/.cpp`              | Serial command-line interface        |
| **config**        | `include/config.h`               | All hardware & system constants      |
| **main**          | `main.cpp`                       | Setup, main loop, event handler      |

---

## Troubleshooting

### I2C Chips Not Detected

```
[IO] Init PARTIAL — IN1:FAIL IN2:OK OUT1:OK OUT2:OK
```

**Cause:** One or more PCF8574 chips not responding on the I2C bus.

**Solutions:**
- Check that the I2C SDA (GPIO4) and SCL (GPIO5) lines are not damaged.
- Verify the KC868-A16 board version is v1.6 (addresses may differ on older versions).
- Use `test input` to see if partial inputs are readable.
- Check for bus conflicts with other I2C devices connected to the header.

### Cannot Send SMS

```
[SMS] Error: Login HTTP error: -1
```

**Cause:** ESP32 cannot reach the Cudy LT500D router.

**Solutions:**
1. Verify Wi-Fi connection: run `network` in the CLI.
2. Ping the router IP: the ESP32 should be on the same subnet.
3. Check router credentials: `router <ip> <user> <pass>`.
4. Verify the SIM card has credit and no PIN lock.
5. Try sending an SMS manually via the router's web interface first.

### SMS Sent But Not Received

**Solutions:**
- Verify the phone number format (include country code, e.g., `+1234567890`).
- Check the SIM card has SMS capability and sufficient credit.
- Verify 4G signal on the Cudy LT500D (check router status page).

### Zones Not Triggering

**Solutions:**
- Use `test input` to verify the raw input state changes when the sensor activates.
- Check NO/NC configuration matches your sensor wiring: `zone <n> nc` or `zone <n> no`.
- Ensure the zone is enabled: `zone <n> enable`.
- Ensure the zone is not bypassed: `zone <n> unbypass`.
- If the system is disarmed, only `24H` zones will trigger an alarm.

### Configuration Lost After Reboot

**Cause:** Configuration was not saved to NVS.

**Solution:** Always run `save` after making changes via the CLI or SMS.

### Wi-Fi Keeps Disconnecting

```
[NET] Reconnecting to YourSSID...
```

**Solutions:**
- Check Wi-Fi signal strength: `network` command shows RSSI.
- Move the KC868-A16 closer to the Wi-Fi access point or router.
- Consider using the Ethernet connection instead (requires code to enable ETH).
- Check for 2.4 GHz vs 5 GHz — ESP32 only supports 2.4 GHz.

---

## License

This project is provided as-is for personal and non-commercial use. See
[LICENSE](LICENSE) for details.

---

```
  SF_Alarm v0.1.0
  KC868-A16 v1.6 Alarm System Controller
  
  ┌────────────────────────────────────────┐
  │  Protect what matters.                 │
  │  16 zones. SMS alerts. Zero cloud.     │
  └────────────────────────────────────────┘
```
