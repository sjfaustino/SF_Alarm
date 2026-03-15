#include "serial_cli.h"
#include "config.h"
#include "io_service.h"
#include "zone_manager.h"
#include "alarm_controller.h"
#include "sms_gateway.h"
#include "sms_command_processor.h"
#include "phone_authenticator.h"
#include "config_manager.h"
#include "network.h"
#include "whatsapp_client.h"
#include <string.h>
#include <ctype.h>
#include <time.h>
#include "logging.h"

static const char* TAG = "CLI";

// ---------------------------------------------------------------------------
// SerialCLI Implementation
// ---------------------------------------------------------------------------

SerialCLI::SerialCLI() 
    : _alarm(nullptr), _zones(nullptr), _io(nullptr), _sms(nullptr), _whatsapp(nullptr),
      _smsProc(nullptr), _auth(nullptr),
      _inputMonitorActive(false), _lastMonitorInputs(0xFFFF) {
}

SerialCLI::~SerialCLI() {
}

void SerialCLI::init(AlarmController* alarm, ZoneManager* zones, IoService* io, 
                     SmsService* sms, WhatsappService* whatsapp,
                     SmsCommandProcessor* smsProc, PhoneAuthenticator* auth) {
    _alarm = alarm;
    _zones = zones;
    _io = io;
    _sms = sms;
    _whatsapp = whatsapp;
    _smsProc = smsProc;
    _auth = auth;

    _cli.setOnError([](cmd_error* e) {
        CommandError cmdError(e);
        Serial.print("ERROR: ");
        Serial.println(cmdError.toString());
    });

    registerCommands();
    LOG_INFO(TAG, "Serial Interface Initialized. Type 'help' for command list.");
    printPrompt();
}

void SerialCLI::registerCommands() {
    // Note: No callbacks here! We poll for available commands in update().
    _cli.addCommand("help");
    _cli.addCommand("status");
    _cli.addCommand("zones");
    _cli.addCommand("inputs");
    _cli.addCommand("outputs");
    _cli.addCommand("network");
    _cli.addCommand("time");
    _cli.addCommand("config");

    Command cArm = _cli.addCommand("arm");
    cArm.addArgument("pin", "");
    cArm.addFlagArgument("home");

    Command cDisarm = _cli.addCommand("disarm");
    cDisarm.addArgument("pin", "");

    Command cMute = _cli.addCommand("mute");
    cMute.addArgument("pin", "");

    Command cReboot = _cli.addCommand("reboot");
    cReboot.addArgument("pin", "");

    Command cFact = _cli.addCommand("factory");
    cFact.addArgument("pin", "");
    cFact.addArgument("confirm", "");

    Command cZone = _cli.addCommand("zone");
    cZone.addArgument("id", "");
    cZone.addArgument("name", "");
    cZone.addArgument("type", "");
    cZone.addArgument("wiring", "");
    cZone.addArgument("mode", "");
    cZone.addArgument("text", "");
    cZone.addArgument("pin", "");

    Command cPhone = _cli.addCommand("phone");
    cPhone.addArgument("action", "");
    cPhone.addArgument("number", "");
    cPhone.addArgument("pin", "");

    Command cTest = _cli.addCommand("test");
    cTest.addArgument("type", "");
    cTest.addArgument("target", "");
    cTest.addArgument("msg", "");
    cTest.addArgument("pin", "");

    Command cGsm = _cli.addCommand("gsm");
    cGsm.addArgument("cmd", "");
    cGsm.addArgument("pin", "");

    Command cmdSms = _cli.addCommand("sms");
    cmdSms.addArgument("box", "");
    cmdSms.addArgument("pin", "");

    Command cmdWifi = _cli.addCommand("wifi");
    cmdWifi.addArgument("ssid", "");
    cmdWifi.addArgument("pass", "");
    cmdWifi.addArgument("pin", "");

    Command cmdRouter = _cli.addCommand("router");
    cmdRouter.addArgument("ip", "");
    cmdRouter.addArgument("user", "");
    cmdRouter.addArgument("pass", "");
    cmdRouter.addArgument("pin", "");

    Command cmdSetPin = _cli.addCommand("setpin");
    cmdSetPin.addArgument("pin", "");
    cmdSetPin.addArgument("new", "");
}

bool SerialCLI::requirePin(Command& command) {
    Argument argPin = command.getArgument("pin");
    if (!argPin.isSet()) {
        Serial.println("[CLI] ERROR: Sensitive command requires -pin <YOUR_PIN>");
        return false;
    }
    return _alarm->validatePin(argPin.getValue().c_str());
}

void SerialCLI::printPrompt() {
    Serial.print("sf_alarm> ");
}

void SerialCLI::handleCommand(Command command) {
    String name = command.getName();

    if (name == "help") {
        Serial.println("\n=== SF_Alarm CLI Commands ===");
        Serial.println(_cli.toString());
        Serial.println("=============================");
    } else if (name == "status") {
        _alarm->printStatus();
        _zones->printStatus();
        networkPrintStatus();
    } else if (name == "zones") {
        _zones->printStatus();
    } else if (name == "inputs") {
        uint16_t inputs = 0;
        _io->readInputs(&inputs);
        Serial.printf("Inputs: 0x%04X (", inputs);
        for (int i = 15; i >= 0; i--) {
            Serial.print((inputs >> i) & 1);
            if (i % 4 == 0 && i > 0) Serial.print(' ');
        }
        Serial.println(")");
    } else if (name == "outputs") {
        uint16_t outputs = _io->getOutputs();
        Serial.printf("Outputs: 0x%04X\n", outputs);
    } else if (name == "network") {
        networkPrintStatus();
    } else if (name == "time") {
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo)) {
            Serial.println("Time not set! Check Network Connection and NTP.");
        } else {
            Serial.println(&timeinfo, "Current Time: %A, %B %d %Y %H:%M:%S");
        }
    } else if (name == "config") {
        configPrint();
    } else if (name == "arm") {
        if (!requirePin(command)) return;
        bool home = command.getArgument("home").isSet();
        if (home) _alarm->armHomeInternal();
        else _alarm->armAwayInternal();
        Serial.println("System ARMING...");
    } else if (name == "disarm") {
        if (!requirePin(command)) return;
        _alarm->disarmInternal();
        Serial.println("System DISARMED.");
    } else if (name == "mute") {
        if (!requirePin(command)) return;
        _alarm->muteSiren("CLI");
        Serial.println("Siren muted.");
    } else if (name == "reboot") {
        if (!requirePin(command)) return;
        Serial.println("Rebooting system...");
        delay(500);
        ESP.restart();
    } else if (name == "factory") {
        if (!requirePin(command)) return;
        if (command.getArgument("confirm").getValue() == "YES") {
            Serial.println("FACTORY RESET: Clearing all configuration...");
            configFactoryReset();
            delay(1000);
            ESP.restart();
        } else {
            Serial.println("Aborted. Must use -confirm YES");
        }
    } else if (name == "zone") {
        if (!requirePin(command)) return;
        int zoneNum = command.getArgument("id").getValue().toInt();
        if (zoneNum < 1 || zoneNum > MAX_ZONES) {
            Serial.println("Invalid Zone ID (1-16)");
            return;
        }
        
        ZoneConfig* config = _zones->getConfig(zoneNum - 1);
        bool changed = false;

        if (command.getArgument("name").isSet()) {
            strncpy(config->name, command.getArgument("name").getValue().c_str(), sizeof(config->name)-1);
            changed = true;
        }
        if (command.getArgument("type").isSet()) {
            config->type = (ZoneType)command.getArgument("type").getValue().toInt();
            changed = true;
        }
        if (command.getArgument("wiring").isSet()) {
            config->wiring = (ZoneWiring)command.getArgument("wiring").getValue().toInt();
            changed = true;
        }
        if (command.getArgument("mode").isSet()) {
            String m = command.getArgument("mode").getValue();
            if (m == "bypass") _zones->setBypassed(zoneNum - 1, true);
            else if (m == "unbypass") _zones->setBypassed(zoneNum - 1, false);
        }
        if (command.getArgument("text").isSet()) {
            _smsProc->setAlarmText(zoneNum - 1, command.getArgument("text").getValue().c_str());
            changed = true;
        }

        if (changed) {
            configSave();
            Serial.printf("Zone %d updated.\n", zoneNum);
        }
    } else if (name == "gsm") {
        if (!requirePin(command)) return;
        String gsmParam = command.getArgument("cmd").getValue();
        if (gsmParam == "status") {
            Serial.printf("GSM Provider: %s\n", configGetSmsProvider() == SMS_GSM_A6 ? "A6 Module" : "LuCI Router");
            Serial.printf("Gateway Ready: %s\n", _sms->getGateway()->isReady() ? "YES" : "NO");
        } else if (gsmParam.length() > 0) {
            Serial.print("Executing AT: ");
            Serial.println(gsmParam);
            char resp[256];
            if (_sms->execCommand(gsmParam.c_str(), resp, sizeof(resp))) {
                 Serial.println("Response:");
                 Serial.println(resp);
            } else {
                 Serial.println("No response or error.");
            }
        } else {
            Serial.println("Usage: gsm -cmd <status|AT...> -pin <pin>");
        }
    } else if (name == "phone") {
        if (!requirePin(command)) return;
        String action = command.getArgument("action").getValue();
        if (action == "list") {
            int cnt = _auth->getPhoneCount();
            Serial.printf("Configured phones (%d):\n", cnt);
            for (int i = 0; i < cnt; i++) {
                 Serial.printf("  [%d] %s\n", i, _auth->getPhone(i));
            }
        } else if (action == "clear") {
            _auth->clearPhones();
            configSave();
            Serial.println("All phone numbers cleared");
        } else {
            String num = command.getArgument("number").getValue();
            if (action == "add" && num.length() > 0) {
                int idx = _auth->addPhone(num.c_str());
                if (idx >= 0) {
                     configSave();
                     Serial.printf("Phone %s added at index %d\n", num.c_str(), idx);
                }
                else Serial.println("Error: Phone list full or duplicate");
            } else if (action == "remove" && num.length() > 0) {
                if (_auth->removePhone(num.c_str())) {
                     configSave();
                     Serial.printf("Phone %s removed\n", num.c_str());
                }
                else Serial.println("Error: Phone not found");
            } else {
                Serial.println("Usage: phone -action <add|remove|list|clear> [-number <num>] -pin <pin>");
            }
        }
    } else if (name == "test") {
        if (!requirePin(command)) return;
        String type = command.getArgument("type").getValue();
        
        if (type == "sms") {
            String num = command.getArgument("target").getValue();
            String msg = command.getArgument("msg").getValue();
            Serial.printf("Sending test SMS to %s...\n", num.c_str());
            if (_sms->send(num.c_str(), msg.c_str())) Serial.println("Test SMS sent OK");
            else Serial.println("Test SMS FAILED");
        } else if (type == "wa") {
            String msg = command.getArgument("msg").getValue();
            Serial.println("Sending test WhatsApp...");
            if (_whatsapp->send(msg.c_str())) Serial.println("Test WhatsApp sent OK");
            else Serial.println("Test WhatsApp FAILED");
        } else if (type == "output") {
            int ch = command.getArgument("target").getValue().toInt();
            if (ch >= 0 && ch <= 15) {
                uint16_t current = _io->getOutputs();
                bool isOn = (current >> ch) & 1;
                _io->setOutput(ch, !isOn);
                Serial.printf("Output %d toggled %s\n", ch, !isOn ? "ON" : "OFF");
            } else {
                Serial.println("Invalid output (0-15)");
            }
        } else if (type == "input") {
            _inputMonitorActive = true;
            _lastMonitorInputs = 0xFFFF;
            Serial.println("Live input monitor active. Press any key to stop.");
        } else {
            Serial.println("Unknown test type.");
        }
    } else if (name == "sms") {
        if (!requirePin(command)) return;
        String action = command.getArgument("box").getValue();
        
        if (action == "inbox") {
            Serial.println("Fetching router inbox...");
            SmsMessage msgs[10];
            int count = _sms->pollInbox(msgs, 10);
            if (count == 0) Serial.println("  (inbox empty or not reachable)");
            else {
                 Serial.printf("=== Router Inbox (%d message) ===\n", count);
                 for (int i = 0; i < count; i++) {
                     Serial.printf("  [%d] %-16s  %s  \"%s\"\n", i + 1, msgs[i].sender, msgs[i].timestamp, msgs[i].body);
                 }
            }
        } else if (action == "sent") {
            Serial.println("Fetching router sent messages...");
            SmsMessage msgs[10];
            int count = _sms->pollSent(msgs, 10);
            if (count == 0) Serial.println("  (sent messages empty or not reachable)");
            else {
                 Serial.printf("=== Router Sent SMS (%d message) ===\n", count);
                 for (int i = 0; i < count; i++) {
                     Serial.printf("  [%d] %-16s  %s  \"%s\"\n", i + 1, msgs[i].sender, msgs[i].timestamp, msgs[i].body);
                 }
            }
        }
    } else if (name == "wifi") {
        if (!requirePin(command)) return;
        networkSetWifi(command.getArgument("ssid").getValue().c_str(), command.getArgument("pass").getValue().c_str());
        configSave();
        Serial.printf("Wi-Fi set: SSID=%s\n", command.getArgument("ssid").getValue().c_str());
    } else if (name == "router") {
        if (!requirePin(command)) return;
        _sms->setCredentials(command.getArgument("ip").getValue().c_str(), command.getArgument("user").getValue().c_str(), command.getArgument("pass").getValue().c_str());
        configSave();
        Serial.printf("Router set: IP=%s User=%s\n", command.getArgument("ip").getValue().c_str(), command.getArgument("user").getValue().c_str());
    } else if (name == "setpin") {
        if (!requirePin(command)) return;
        if (_alarm->setPin(command.getArgument("pin").getValue().c_str(), command.getArgument("new").getValue().c_str())) {
            Serial.println("PIN updated successfully.");
        } else {
            Serial.println("ERROR: Failed to update PIN.");
        }
    } else {
        Serial.println("Unknown command. Type 'help'.");
    }
}

void SerialCLI::update() {
    if (_inputMonitorActive) {
        if (Serial.available()) {
            Serial.read();
            while(Serial.available()) Serial.read();
            _inputMonitorActive = false;
            Serial.println("\nLive input monitor stopped.");
            printPrompt();
            return;
        }
        uint16_t inputs = 0;
        _io->readInputs(&inputs);
        if (inputs != _lastMonitorInputs) {
            Serial.printf("  Inputs: 0x%04X\n", inputs);
            _lastMonitorInputs = inputs;
        }
        return; 
    }

    static String inputStr = "";
    inputStr.reserve(256);

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\r') continue;
        if (c == '\n') {
            Serial.println();
            if (inputStr.length() > 0) {
                // Command parsing triggers the "available" queue in SimpleCLI
                _cli.parse(inputStr);
                
                // Clear sensitive data from stack
                for (size_t i = 0; i < inputStr.length(); i++) {
                    ((volatile char*)inputStr.c_str())[i] = 0;
                }
                inputStr = "";
            }

            // Process any commands found in the queue
            while (_cli.available()) {
                handleCommand(_cli.getCommand());
            }

            printPrompt();
        } else if (c == '\b' || c == 127) {
             if (inputStr.length() > 0) {
                  inputStr.remove(inputStr.length() - 1);
                  Serial.print("\b \b");
             }
        } else if (isPrintable(c)) {
             if (inputStr.length() < 256) {
                 inputStr += c;
                 Serial.print(c);
             }
        }
    }
}
