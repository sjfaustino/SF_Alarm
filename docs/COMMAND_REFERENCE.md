# SF_Alarm: Comprehensive Command & Parameter Reference

This document provides a detailed technical guide for interacting with the SF_Alarm controller via the **Serial CLI**, **SMS Interface**, and **MQTT API**.

---

## 🔐 Authentication Principles

### Serial CLI Security
To prevent unauthorized access via physical console, all "destructive" or "config-altering" commands require a trailing PIN signature.
* **Syntax**: `<command> [args] -pin <YOUR_PIN>`
* **Default PIN**: `1234`
* **Lockout**: 3 failed attempts will lock the CLI for 5 minutes.

### SMS Security
* **Whitelisting**: Only phone numbers stored in slots 01–05 can issue commands.
* **Master Override**: If the phone list is empty, the first phone that sends a `#01#` registration command becomes the master admin.
* **PIN Requirement**: `ARM` and `DISARM` commands over SMS *must* include the PIN (e.g., `DISARM 1234`).

---

## 📟 Serial CLI Reference

### 1. System Information
| Command | Parameter | Description | Expected Result |
| :--- | :--- | :--- | :--- |
| `status` | - | Aggregated system health. | Displays Arm State, Zone Status, Network IP, and RSSI. |
| `zones` | - | Detailed zone audit. | Table showing Name, Type, State, and Wiring for 16 zones. |
| `network` | - | Link layer status. | Shows ETH/WiFi connection, MAC, and Gateway IP. |
| `inputs` | - | Hardware debug. | Prints a 16-bit binary representation of physical IO state. |
| `config` | - | Flash dump. | Lists all persistent variables stored in NVS. |
| `sms inbox` | - | SMS read-only diagnostic. | Shows last 10 received messages in the router inbox. |
| `sms outbox` | - | SMS read-only diagnostic. | Shows last 10 sent messages in the router outbox. |

### 2. Control Commands (Requires `pin <pin>`)
| Command | Syntax | Example | Result |
| :--- | :--- | :--- | :--- |
| **Arm Away** | `arm -pin <pin>` | `arm -pin 1234` | Transitions to `ARMED_AWAY`. 30s exit delay starts. |
| **Arm Home** | `arm -home -pin <pin>` | `arm -home -pin 1234` | Transitions to `ARMED_HOME`. Interior zones bypassed. |
| **Disarm** | `disarm -pin <pin>` | `disarm -pin 1234` | Resets system to `DISARMED`. Stops siren. |
| **Mute** | `mute -pin <pin>` | `mute -pin 1234` | Instantly silences siren. State remains `TRIGGERED`. |

### 3. Zone Configuration (Requires `pin <pin>`)
*Syntax: `zone -id <1-16> -<subcommand> <value> -pin <pin>`*

| Subcommand | Value | Effect |
| :--- | :--- | :--- |
| `name` | `<text>` | Changes zone label (Max 16 chars). |
| `type` | `inst` | **Instant**: Triggers immediately when armed. |
| | `dly` | **Delay**: Starts entry timer (e.g., for front door). |
| | `24h` | **Always On**: Triggers even if disarmed (e.g., fire/panic). |
| | `flw` | **Follower**: Only delays if a `dly` zone was hit first. |
| `nc`/`no` | - | Sets wiring logic to Normally Closed or Normally Open. |
| `enable`/`disable`| - | Logically removes the zone from the scan loop. |
| `text` | `<msg>` | Sets the custom SMS text sent when this zone triggers. |

### 4. System Settings (Requires `pin <pin>`)
| Command | Parameter | Description |
| :--- | :--- | :--- |
| `delay exit` | `<sec>` | Seconds before system is fully armed. Default: 30. |
| `delay entry` | `<sec>` | Seconds allowed to enter PIN after entry zone hit. Default: 30. |
| `siren dur` | `<sec>` | Siren timeout (Fixed max 180s for ordinance compliance). |
| `siren ch` | `<0-15>` | Relays 1-16. Usually `1` or `2` for local sirens. |
| `pin` | `<newpin>` | Sets a new 4-digit master PIN. |
| `heartbeat` | `<on/off>` | Enables/disables the armed status LED and buzzer strobe on GPIO33/14. |
| `time` | - | (No PIN) Prints system time via NTP. |
| `tz` | `<timezone_string>` | Sets POSIX timezone string (e.g., `MST7MDT,M3.2.0,M11.1.0`). |
| `schedule show`| - | (No PIN) Prints table of Auto-Arm schedule. |
| `schedule mode`| `<away/home>` | Set Auto-Arm target mode (Away or Home perimeter). |
| `schedule arm` | `<day> <HH:MM/off>` | Set time to Auto-Arm. Day can be `0-6` (Sun-Sat), `weekdays`, `weekends`, or `all`. |
| `schedule disarm`| `<day> <HH:MM/off>` | Set time to Auto-Disarm. Same format as schedule arm. |

---

## 📱 SMS Command Reference (GA09 Compatible)

SMS commands are case-insensitive. Parameters are separated by `#`.

### 1. Account & Phone Setup
| Command | Format | Example | Description |
| :--- | :--- | :--- | :--- |
| **Register** | `#NN#number#` | `#01#+447700123#` | Sets phone slot 01-05. |
| **Multi-Set** | `@#num1#num2#...`| `@#123#456#` | Fills multiple slots starting at 01. |
| **Mode** | `%#Mx` | `%#M1` | `1`: SMS only, `2`: Voice Call, `3`: Both. |

### 2. Alert Customization
| Command | Format | Example | Description |
| :--- | :--- | :--- | :--- |
| **Zone Text** | `#N#<text>` | `#3#Window Broken`| Sets SMS text for Zone N. |
| **Recovery** | `#0#<text>` | `#0#System Clear` | Text sent when zones return to normal. |
| **Interval** | `%#Txx` | `%#T60` | Periodic heartbeat interval in minutes. `0` = Off. |
| **Channel** | `%#Wx` | `%#W2` | `1`: SMS, `2`: WhatsApp, `3`: Both. |

### 3. Hardware Logic
| Command | Format | Description |
| :--- | :--- | :--- |
| **Wiring** | `*NC1110...` | 16 digits. `1`=Normally Closed, `0`=Normally Open. |
| **Enable** | `@#ARM1110...`| 16 digits. `1`=Enabled, `0`=Disabled. |

### 4. Integrations
| Command | Format | Example |
| :--- | :--- | :--- |
| **WhatsApp** | `#WA#ph#key#` | `#WA#1234#ABCXYZ#` | Set CallMeBot credentials. |
| **MQTT** | `#MQTT#host#port#u#p#` | `#MQTT#192.168.1.5#1883#usr#pw#` | Set Broker info. |

---

## 🤖 MQTT Topic Reference

The system exposes a rich MQTT API for Home Assistant / Node-RED.

| Topic | Access | Payload | Description |
| :--- | :--- | :--- | :--- |
| `SF_Alarm/state` | Read | `disarmed`, `armed_away`, `armed_home`, `triggered`, `pending` | Current system state. |
| `SF_Alarm/cmd` | Write | `DISARM:1234`, `ARM_HOME:1234`, `ARM_AWAY:1234`, `MUTE` | Command bus. |
| `SF_Alarm/zone/N` | Read | `ON`, `OFF` | Per-zone real-time status (N=1..16). |
| `SF_Alarm/events`| Read | String | Human readable event log. |
| `SF_Alarm/availability`| Read | `online`, `offline` | LWT connectivity status. |

---

## 🛠️ Diagnostics & Maintenance (CLI Only)

### Live Input Monitor
Run `test input` to enter real-time diagnostics mode. The CLI will print a bitmask every time a sensor status changes. 
* *Exit by pressing any key.*

### Factory Reset
Command: `factory -pin <pin>`
* The system will ask for confirmation. Type `YES` and press Enter within 10 seconds to wipe NVS memory.

### Reboot
Command: `reboot -pin <pin>`
* Performs a software restart of the ESP32. Useful after network configuration changes.
