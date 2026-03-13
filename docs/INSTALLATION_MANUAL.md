# SF_Alarm System: Complete Installation & Setup Guide

Welcome to the SF_Alarm system, a professional-grade, highly resilient open-source alarm controller built on the Kincony KC868-A16 v1.6 hardware platform. 

This manual is written for the **Installer**. It assumes you have the hardware but know absolutely nothing about the underlying code. By the end of the "**Quick Start**" section, you will have a fully functioning, armed, and communicating alarm system.

---

## Part 1: Hardware Preparation

### 1. What You Need
*   **The Controller:** Kincony KC868-A16 v1.6 board.
*   **Power Supply:** A reliable 12V DC power supply capable of at least 2 Amps (more if driving heavy sirens directly).
*   **Sensors:** Magnetic reed switches (for doors/windows), PIR motion sensors, beam sensors.
*   **Siren/Strobe:** A standard 12V active alarm siren.
*   **Computer:** A Windows/Mac/Linux PC with a USB-C cable for the initial setup.
*   **Terminal Software:** PuTTY (Windows) or any serial monitor application.
*   **Cudy Router (Optional but Recommended):** A Cudy LT500D LTE router with an active SIM card for SMS/Call alerts.

### 2. Physical Wiring
> [!CAUTION]
> **Ensure power is completely disconnected before wiring.**

1.  **Power:** Connect the 12V DC positive and negative wires to the main power terminal block on the KC868-A16.
2.  **Sensors (Zones 1-16):** 
    *   The board has 16 digital input terminals. 
    *   Wire one leg of your sensor to an `Input` terminal (e.g., `IN1`) and the other leg to a common `COM` or `GND` terminal. 
    *   By default, the system expects **Normally Closed (NC)** sensors (the circuit breaks when the door opens), which is standard for security.
3.  **Siren (Outputs):**
    *   The board has 16 MOSFET outputs capable of driving 12V loads directly.
    *   Wire the positive leg of your 12V siren to your main 12V power rail, and the negative ground leg to `OUT1` (Output Channel 0 internally). The board switches the ground to turn the siren on.
4.  **Heartbeat Indicator (Optional):**
    *   To see if the system is armed and running, you can use the "Free GPIO" header pins.
    *   **LED:** Connect a standard 3V LED (with a 220Ω resistor) to `GPIO33`.
    *   **Beeper:** Connect a small 3V active piezo buzzer to `GPIO14`.
    *   Tie both ground legs to a `GND` pin.

---

## Part 2: The Initial Boot (Serial Console)

Before installing the board in a wallbox, you must configure it via a USB cable.

1.  Plug the USB-C cable into the ESP32 module on the KC868-A16 and connect it to your computer.
2.  Open your Terminal software (e.g., PuTTY).
3.  Connect to the COM port assigned to the board at a baud rate of **`115200`**.
4.  Power on the 12V supply.

### Expected Result on Screen:
You should see a boot sequence looking something like this:
```text
========================================
  SF_Alarm v0.1.0 — Starting up...
  KC868-A16 v1.6 Alarm System Controller
========================================
[INIT] I/O Expander...
[INIT] Alarm Zones...
[INIT] Startup complete!

sf_alarm>
```

---

## Part 3: Quick Start Configuration (CLI)

The `sf_alarm>` prompt is your command-line interface (CLI). 

> [!IMPORTANT]
> **The PIN Security System**
> To prevent an intruder from plugging a laptop into the board to disable it, all configuration commands require you to append `-pin <your_pin>` to the end of the command. 
> The factory default PIN is **`1234`**.

Type the following commands into the terminal, pressing `Enter` after each one.

### Step 1: Secure the System
Change the default master PIN to your own 4-digit code (e.g., `9999`).
*   **Command:** `pin 9999 -pin 1234`
*   *(Expected: `[CLI] Configuration saved to NVS`)*
*   **Note:** Keep your new PIN safe. All future commands in this guide will use `9999` as the example PIN.

### Step 2: Connect to the Network
To send WhatsApp messages or MQTT data, connect the board to your local Wi-Fi.
*   **Command:** `wifi -ssid MyNetwork -pass MyPassword -pin 9999`
*   *(Expected: `Wi-Fi set: SSID=MyNetwork`)*
*   Wait 5-10 seconds. Type `status` to ensure you got an IP address.

### Step 3: Setup the SMS Cudy Router (Life-Safety Alerts)
If you have a Cudy Router on your network handling SMS, point the alarm board to it.
*   **Command:** `router -ip 192.168.10.1 -user admin -pass YourRouterPass -pin 9999`

### Step 4: Add Authorized Phone Numbers
Only numbers explicitly added to the system can text it commands. Add your mobile number (include country code, e.g., `+44...` or `+1...`).
*   **Command:** `phone -action add -number +15551234567 -pin 9999`
*   *(Expected: `Phone +15551234567 added at index 0`)*

### Step 5: Configure Your Zones
Let's assume `Zone 1` is your Front Door, and `Zone 2` is your Living Room Motion Sensor.

1.  **Name the Front Door:**
    *   `zone -id 1 -name Front_Door -pin 9999`
2.  **Make the Front Door a "Delayed" zone** (gives you time to enter the PIN when you get home):
    *   `zone -id 1 -type dly -pin 9999`
3.  **Name the Motion Sensor:**
    *   `zone -id 2 -name Living_Room_PIR -pin 9999`
4.  **Make the Motion Sensor a "Follower"** (it won't trigger immediately if you walked through the front door first to get to the keypad):
    *   `zone -id 2 -type flw -pin 9999`
5.  **Set custom SMS Alert Text:**
    *   `zone -id 1 -text "Front Door opened while armed!" -pin 9999`

*   *(To view your setup, simply type: `zones` and hit enter)*

### Step 6: Adjust Timers
By default, you have 30 seconds to leave the house, and 15 seconds to enter the PIN when returning. Sirens ring for 3 minutes (180s). To change this:
*   Set Exit delay to 60s: `delay -exit 60 -pin 9999`
*   Set Entry delay to 45s: `delay -entry 45 -pin 9999`

### Step 7: Turn on the Heartbeat
If you wired the LED and Buzzer in Part 1 to see system status.
*   **Command:** `heartbeat -on -pin 9999`
*   *(It will now tick/flash every 2 seconds ONLY when the system is actually Armed).*

### Step 8: Set Your Timezone
To allow the system to accurately sync its clock with the internet and handle Daylight Saving Time, tell it your local timezone using a POSIX string.
*   **Command:** `tz EST5EDT,M3.2.0,M11.1.0 -pin 9999` *(Example for New York)*
*   *(Expected: `Timezone updated to: EST5EDT...`)*
*   Verify it worked by typing `time`.

### Step 9: Save to Hardware
While some commands auto-save, it is best practice to force a memory write before disconnecting the USB cable:
*   **Command:** `save -pin 9999`
*   *(Expected: `[CFG] Full configuration saved`)*

---

## Part 4: Everyday Usage & Examples

Your system is now installed, wired, and configured. Disconnect the USB cable. The system will run independently off the 12V supply.

### How to Arm the System (Leaving the House)
You want to arm all zones, including interior motion sensors.
*   **Via SMS:** Send a text message to the Cudy Router SIM number: `ARM 9999`
*   **Expected Behavior:** 
    1. You will receive a reply: `SF_Alarm: System ARMED (Away)`.
    2. Your heartbeat LED/Buzzer will begin pulsing every 2 seconds.
    3. The 60-second exit delay starts silently. You must leave the house.

### How to Arm the System (Sleeping at Home)
You want to arm the perimeter doors/windows, but disable interior motion sensors so you can walk to the kitchen.
*   **Via SMS:** Text: `ARM HOME 9999`
*   **Expected Behavior:** Delay zones convert to instant, and interior zones are ignored.

### What Happens During a Break-In?
1. An intruder pries open the Back Door (configued as an INSTANT zone).
2. The alarm instantly registers `TRIGGERED`.
3. The 12V Siren output turns ON immediately.
4. The system sends an SMS to your phone: `SF_Alarm ALERT: Zone 3 (Back_Door)`
5. The siren will roar for exactly 3 minutes, then shut off (to comply with noise ordinances), but the system remains ARMED. 
6. If the intruder triggers another sensor, the cycle repeats.

### How to Disarm
*   **Via SMS:** Text: `DISARM 9999`
*   **Expected Behavior:** The siren (if running) stops instantly. Heartbeat LED turns off. You receive a text: `SF_Alarm: System DISARMED`.

---

## Part 5: Automating the Alarm (Auto-Arm)

You can tell the system to automatically arm or disarm itself on a 7-day schedule, meaning you never have to remember to secure the house at night.

### Step 1: Choose the Auto-Arm Mode
When the schedule triggers an "Arm" event, should it arm the entire house (`away`), or just the perimeter doors/windows while you are sleeping inside (`home`)?
*   **Command:** `schedule mode home pin 9999`

### Step 2: Set the Schedule
You explicitly set the time (in 24-hour HH:MM format) for the system to Arm and Disarm. You can target specific days (`0` = Sunday ... `6` = Saturday), or use bulk targets: `weekdays`, `weekends`, or `all`.

*   **Example 1 (Bedtime):** Arm the house every single day at 11:30 PM, and disarm at 6:30 AM:
    *   `schedule arm all 23:30 pin 9999`
    *   `schedule disarm all 06:30 pin 9999`

*   **Example 2 (Office/Shop):** Arm the shop on weekdays at 6:00 PM:
    *   `schedule arm weekdays 18:00 pin 9999`

### Step 3: View Your Schedule
*   **Command:** `schedule show` (No PIN required)
*   This will print a table showing exactly when the alarm is scheduled to turn on and off for all 7 days of the week.

---

## Cheat Sheet: Common SMS Commands

Send these from your registered mobile phone.

| Text Message | What It Does |
| :--- | :--- |
| `STATUS` | Replies with system health, armed state, and open doors. |
| `ARM 9999` | Arms the entire house (Away Mode). |
| `ARM HOME 9999` | Arms the perimeter, bypasses interior (Night Mode). |
| `DISARM 9999` | Disarms the system / silences alarms. |
| `MUTE 9999` | Silences the siren but leaves the police response active. |
| `%#M1` | Sets alert mode to SMS Text Messages only. |
| `%#M2` | Sets alert mode to Voice Phone Calls only. |
| `%#M3` | System will send an SMS, *and* call you during an alarm. |

## Advanced Reset
If you lose your PIN and cannot use the Serial CLI or SMS:
1. Connect via USB.
2. Type: `factory -pin <the_lost_pin>` (This will fail).
3. If you do not have the PIN, there is no backdoor. You must forcefully re-flash the ESP32 microchip via PlatformIO to clear the NVS partition. This ensures thieves cannot steal the unit and easily bypass the security.
