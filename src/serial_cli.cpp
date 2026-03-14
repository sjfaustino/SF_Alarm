#include "serial_cli.h"
#include "config.h"
#include "io_expander.h"
#include "alarm_zones.h"
#include "alarm_controller.h"
#include "sms_gateway.h"
#include "sms_commands.h"
#include "config_manager.h"
#include "network.h"
#include "whatsapp_client.h"
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <SimpleCLI.h>
#include "logging.h"

static const char* TAG = "CLI";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static SimpleCLI cli;

// Non-blocking state for test input monitor
static bool inputMonitorActive = false;
static uint16_t lastMonitorInputs = 0xFFFF;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void printPrompt() {
    Serial.print("sf_alarm> ");
}

// Global Auth Check Helper
static bool requirePin(cmd* c) {
    Command cmd(c);
    Argument argPin = cmd.getArgument("pin");
    if (!argPin.isSet()) {
        Serial.println("[CLI] ERROR: Sensitive command requires -pin <YOUR_PIN>");
        return false;
    }
    if (!alarmValidatePin(argPin.getValue().c_str())) {
        Serial.println("[CLI] ACCESS DENIED: Invalid PIN");
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// CLI Callbacks - Public Commands
// ---------------------------------------------------------------------------

static void cmdHelpCallback(cmd* c) {
    Serial.println();
    Serial.println("=== SF_Alarm CLI Commands ===");
    Serial.println(cli.toString());
    Serial.println("=============================");
}

static void cmdStatusCallback(cmd* c) {
    alarmPrintStatus();
    zonesPrintStatus();
    networkPrintStatus();
}

static void cmdZonesCallback(cmd* c) {
    zonesPrintStatus();
}

static void cmdInputsCallback(cmd* c) {
    uint16_t inputs = 0;
    ioExpanderReadInputs(&inputs);
    Serial.printf("Inputs: 0x%04X (", inputs);
    for (int i = 15; i >= 0; i--) {
        Serial.print((inputs >> i) & 1);
        if (i % 4 == 0 && i > 0) Serial.print(' ');
    }
    Serial.println(")");
}

static void cmdOutputsCallback(cmd* c) {
    uint16_t outputs = ioExpanderGetOutputs();
    Serial.printf("Outputs: 0x%04X\n", outputs);
}

static void cmdNetworkCallback(cmd* c) {
    networkPrintStatus();
}

static void cmdTimeCallback(cmd* c) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        Serial.println("Time not set! Check Network Connection and NTP.");
    } else {
        Serial.println(&timeinfo, "Current Time: %A, %B %d %Y %H:%M:%S");
    }
}

static void cmdConfigCallback(cmd* c) {
    configPrint();
}

// ---------------------------------------------------------------------------
// CLI Callbacks - Authenticated Commands
// ---------------------------------------------------------------------------

static void cmdArmCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    if (cmd.getArgument("home").isSet()) {
        alarmArmHome(cmd.getArgument("pin").getValue().c_str());
    } else {
        alarmArmAway(cmd.getArgument("pin").getValue().c_str());
    }
}

static void cmdDisarmCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    alarmDisarm(cmd.getArgument("pin").getValue().c_str());
}

static void cmdMuteCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    alarmMuteSiren(cmd.getArgument("pin").getValue().c_str());
}

static void cmdRebootCallback(cmd* c) {
    if (!requirePin(c)) return;
    Serial.println("[CLI] Rebooting...");
    delay(500);
    ESP.restart();
}

static void cmdFactoryCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    Argument confirmArg = cmd.getArgument("confirm");
    if (!confirmArg.isSet() || confirmArg.getValue() != "YES") {
        Serial.println("Error: Must provide -confirm YES. e.g.: factory -pin <PIN> -confirm YES");
        return;
    }

    Serial.println("\n!!! FACTORY RESET WARNING !!!");
    Serial.println("Erasing config...");
    configFactoryReset();
    Serial.println("DONE. Rebooting in 3s.");
    delay(3000);
    ESP.restart();
}

static void cmdZoneUpdateCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    
    int zoneNum = cmd.getArgument("id").getValue().toInt();
    if (zoneNum < 1 || zoneNum > MAX_ZONES) {
        Serial.println("Invalid zone number (1-16)");
        return;
    }
    ZoneConfig* cfg = zonesGetConfig(zoneNum - 1);
    if (!cfg) return;

    bool changed = false;

    if (cmd.getArgument("name").isSet()) {
        String name = cmd.getArgument("name").getValue();
        strncpy(cfg->name, name.c_str(), MAX_ZONE_NAME_LEN - 1);
        cfg->name[MAX_ZONE_NAME_LEN - 1] = '\0';
        Serial.printf("Zone %d name: %s\n", zoneNum, cfg->name);
        changed = true;
    }
    if (cmd.getArgument("type").isSet()) {
        String type = cmd.getArgument("type").getValue();
        if (type == "inst") { cfg->type = ZONE_INSTANT; changed = true; }
        else if (type == "dly") { cfg->type = ZONE_DELAYED; changed = true; }
        else if (type == "24h") { cfg->type = ZONE_24H; changed = true; }
        else if (type == "flw") { cfg->type = ZONE_FOLLOWER; changed = true; }
        else Serial.println("Unknown type: inst|dly|24h|flw");
    }
    if (cmd.getArgument("wiring").isSet()) {
        String w = cmd.getArgument("wiring").getValue();
        if (w == "nc") { cfg->wiring = ZONE_NC; changed = true; }
        else if (w == "no") { cfg->wiring = ZONE_NO; changed = true; }
    }
    if (cmd.getArgument("state").isSet()) {
        String s = cmd.getArgument("state").getValue();
        if (s == "enable") { cfg->enabled = true; changed = true; }
        else if (s == "disable") { cfg->enabled = false; changed = true; }
    }
    if (cmd.getArgument("mode").isSet()) {
        String m = cmd.getArgument("mode").getValue();
        if (m == "bypass") zonesSetBypassed(zoneNum - 1, true);
        else if (m == "unbypass") zonesSetBypassed(zoneNum - 1, false);
    }
    if (cmd.getArgument("text").isSet()) {
        smsCmdSetAlarmText(zoneNum - 1, cmd.getArgument("text").getValue().c_str());
        changed = true;
    }

    if (changed) {
        configSave();
        Serial.printf("Zone %d updated.\n", zoneNum);
    }
}

static void cmdSetPinCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    
    String currentPin = cmd.getArgument("pin").getValue();
    String newPin = cmd.getArgument("new").getValue();
    
    if (newPin.length() == 0) {
        Serial.println("ERROR: Must provide -new <NEW_PIN>");
        return;
    }

    if (alarmSetPin(currentPin.c_str(), newPin.c_str())) {
        Serial.println("PIN updated successfully.");
    } else {
        Serial.println("ERROR: Failed to update PIN.");
    }
}

static void cmdPhoneCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    
    String action = cmd.getArgument("action").getValue();
    if (action == "list") {
        int cnt = smsCmdGetPhoneCount();
        Serial.printf("Configured phones (%d):\n", cnt);
        for (int i = 0; i < cnt; i++) {
             Serial.printf("  [%d] %s\n", i, smsCmdGetPhone(i));
        }
    } else if (action == "clear") {
        smsCmdClearPhones();
        configSave();
        Serial.println("All phone numbers cleared");
    } else {
        String num = cmd.getArgument("number").getValue();
        if (action == "add" && num.length() > 0) {
            int idx = smsCmdAddPhone(num.c_str());
            if (idx >= 0) {
                 configSave();
                 Serial.printf("Phone %s added at index %d\n", num.c_str(), idx);
            }
            else Serial.println("Error: Phone list full or duplicate");
        } else if (action == "remove" && num.length() > 0) {
            if (smsCmdRemovePhone(num.c_str())) {
                 configSave();
                 Serial.printf("Phone %s removed\n", num.c_str());
            }
            else Serial.println("Error: Phone not found");
        } else {
            Serial.println("Usage: phone -action <add|remove|list|clear> [-number <num>] -pin <pin>");
        }
    }
}

static void cmdTestCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    String type = cmd.getArgument("type").getValue();
    
    if (type == "sms") {
        String num = cmd.getArgument("target").getValue();
        String msg = cmd.getArgument("msg").getValue();
        Serial.printf("Sending test SMS to %s...\n", num.c_str());
        if (smsGatewaySend(num.c_str(), msg.c_str())) Serial.println("Test SMS sent OK");
        else Serial.println("Test SMS FAILED");
    } else if (type == "wa") {
        String msg = cmd.getArgument("msg").getValue();
        Serial.println("Sending test WhatsApp...");
        if (whatsappSend(whatsappGetPhone(), whatsappGetApiKey(), msg.c_str())) Serial.println("Test WhatsApp sent OK");
        else Serial.println("Test WhatsApp FAILED");
    } else if (type == "output") {
        int ch = cmd.getArgument("target").getValue().toInt();
        if (ch >= 0 && ch <= 15) {
            uint16_t current = ioExpanderGetOutputs();
            bool isOn = (current >> ch) & 1;
            ioExpanderSetOutput(ch, !isOn);
            Serial.printf("Output %d toggled %s\n", ch, !isOn ? "ON" : "OFF");
        } else {
            Serial.println("Invalid output (0-15)");
        }
    } else if (type == "input") {
        inputMonitorActive = true;
        lastMonitorInputs = 0xFFFF;
        Serial.println("Live input monitor active. Press any key to stop.");
    } else {
        Serial.println("Unknown test type.");
    }
}

static void cmdSmsCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    String action = cmd.getArgument("box").getValue();
    
    if (action == "inbox") {
        Serial.println("Fetching router inbox...");
        SmsMessage msgs[10];
        int count = smsGatewayPollInbox(msgs, 10);
        if (count == 0) Serial.println("  (inbox empty or not reachable)");
        else {
             Serial.printf("=== Router Inbox (%d message%s) ===\n", count, count > 1 ? "s" : "");
             for (int i = 0; i < count; i++) {
                 Serial.printf("  [%d] %-16s  %s  \"%s\"\n", i + 1, msgs[i].sender, msgs[i].timestamp, msgs[i].body);
             }
             Serial.println("================================");
        }
    } else if (action == "outbox") {
        Serial.println("Fetching router outbox...");
        SmsMessage msgs[10];
        int count = smsGatewayPollOutbox(msgs, 10);
        if (count == 0) Serial.println("  (outbox empty or not reachable)");
        else {
             Serial.printf("=== Router Outbox (%d message%s) ===\n", count, count > 1 ? "s" : "");
             for (int i = 0; i < count; i++) {
                 Serial.printf("  [%d] %-16s  %s  \"%s\"\n", i + 1, msgs[i].sender, msgs[i].timestamp, msgs[i].body);
             }
             Serial.println("=================================");
        }
    } else {
        Serial.println("Unknown sms box. Use inbox or outbox.");
    }
}

static void cmdWifiCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    networkSetWifi(cmd.getArgument("ssid").getValue().c_str(), cmd.getArgument("pass").getValue().c_str());
    configSave();
    Serial.printf("Wi-Fi set: SSID=%s\n", cmd.getArgument("ssid").getValue().c_str());
}

static void cmdRouterCallback(cmd* c) {
    if (!requirePin(c)) return;
    Command cmd(c);
    smsGatewaySetCredentials(cmd.getArgument("ip").getValue().c_str(), cmd.getArgument("user").getValue().c_str(), cmd.getArgument("pass").getValue().c_str());
    configSave();
    Serial.printf("Router set: IP=%s User=%s\n", cmd.getArgument("ip").getValue().c_str(), cmd.getArgument("user").getValue().c_str());
}

// Global generic error callback for SimpleCLI
static void errorCallback(cmd_error* e) {
    CommandError cmdError(e);
    Serial.print("ERROR: ");
    Serial.println(cmdError.toString());
    if (cmdError.hasCommand()) {
        Serial.print("Did you mean \"");
        Serial.print(cmdError.getCommand().toString());
        Serial.println("\"?");
    }
}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

void cliInit() {
    cli.setOnError(errorCallback);

    // Public Commands
    cli.addCommand("help", cmdHelpCallback);
    cli.addCommand("status", cmdStatusCallback);
    cli.addCommand("zones", cmdZonesCallback);
    cli.addCommand("inputs", cmdInputsCallback);
    cli.addCommand("outputs", cmdOutputsCallback);
    cli.addCommand("network", cmdNetworkCallback);
    cli.addCommand("time", cmdTimeCallback);
    cli.addCommand("config", cmdConfigCallback);

    // Authenticated Commands (Require -pin)
    Command cmdArm = cli.addCommand("arm", cmdArmCallback);
    cmdArm.addArgument("pin", "");
    cmdArm.addFlagArgument("home");

    Command cmdDisarm = cli.addCommand("disarm", cmdDisarmCallback);
    cmdDisarm.addArgument("pin", "");

    Command cmdMute = cli.addCommand("mute", cmdMuteCallback);
    cmdMute.addArgument("pin", "");

    Command cmdReboot = cli.addCommand("reboot", cmdRebootCallback);
    cmdReboot.addArgument("pin", "");

    Command cmdFactory = cli.addCommand("factory", cmdFactoryCallback);
    cmdFactory.addArgument("pin", "");
    cmdFactory.addArgument("confirm", "");

    Command cmdZone = cli.addCommand("zone", cmdZoneUpdateCallback);
    cmdZone.addArgument("id", "");
    cmdZone.addArgument("name", "");
    cmdZone.addArgument("type", "");
    cmdZone.addArgument("wiring", "");
    cmdZone.addArgument("state", "");
    cmdZone.addArgument("mode", "");
    cmdZone.addArgument("text", "");
    cmdZone.addArgument("pin", "");

    Command cmdPhone = cli.addCommand("phone", cmdPhoneCallback);
    cmdPhone.addArgument("action", "");
    cmdPhone.addArgument("number", "");
    cmdPhone.addArgument("pin", "");

    Command cmdTest = cli.addCommand("test", cmdTestCallback);
    cmdTest.addArgument("type", "");   // sms, wa, output, input
    cmdTest.addArgument("target", ""); // number or ch
    cmdTest.addArgument("msg", "");
    cmdTest.addArgument("pin", "");

    Command cmdSms = cli.addCommand("sms", cmdSmsCallback);
    cmdSms.addArgument("box", "");
    cmdSms.addArgument("pin", "");
    
    Command cmdWifi = cli.addCommand("wifi", cmdWifiCallback);
    cmdWifi.addArgument("ssid", "");
    cmdWifi.addArgument("pass", "");
    cmdWifi.addArgument("pin", "");

    Command cmdRouter = cli.addCommand("router", cmdRouterCallback);
    cmdRouter.addArgument("ip", "");
    cmdRouter.addArgument("user", "");
    cmdRouter.addArgument("pass", "");
    cmdRouter.addArgument("pin", "");

    Command cmdSetPin = cli.addCommand("setpin", cmdSetPinCallback);
    cmdSetPin.addArgument("pin", "");
    cmdSetPin.addArgument("new", "");

    LOG_INFO(TAG, "Serial Interface Initialized. Type 'help' for command list.");
    printPrompt();
}

// ---------------------------------------------------------------------------
// Task Loop
// ---------------------------------------------------------------------------

void cliUpdate() {
    // --- Non-blocking Live Input Monitor ---
    if (inputMonitorActive) {
        if (Serial.available()) {
            Serial.read(); // Consume keypress
            while(Serial.available()) Serial.read(); // clear buffer
            inputMonitorActive = false;
            Serial.println("\nLive input monitor stopped.");
            printPrompt();
            return;
        }
        uint16_t inputs = 0;
        ioExpanderReadInputs(&inputs);
        if (inputs != lastMonitorInputs) {
            Serial.printf("  Inputs: 0x%04X\n", inputs);
            lastMonitorInputs = inputs;
        }
        return; 
    }

    // --- Standard CLI Input Loop ---

    // --- Normal CLI processing ---
    static String inputStr = "";
    inputStr.reserve(256); // Pre-allocate

    while (Serial.available()) {
        char c = (char)Serial.read();
        
        if (c == '\r') continue;

        if (c == '\n') {
            Serial.println();
            if (inputStr.length() > 0) {
                cli.parse(inputStr); // Pass to SimpleCLI
                // Deep Memory Scrubbing: Exorcise the static buffer immediately (Obsidian Aegis)
                for (size_t i = 0; i < inputStr.length(); i++) {
                    ((volatile char*)inputStr.c_str())[i] = 0;
                }
                inputStr = "";
            }
            printPrompt();
        } else if (c == '\b' || c == 127) {
             if (inputStr.length() > 0) {
                  inputStr.remove(inputStr.length() - 1);
                  Serial.print("\b \b");
             }
        } else if (isPrintable(c)) {
             // HARDENING: Strictly cap input length to prevent heap DoS
             if (inputStr.length() < 256) {
                 inputStr += c;
                 Serial.print(c);
             }
        }
    }
}
