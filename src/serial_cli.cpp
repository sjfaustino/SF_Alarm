#include "serial_cli.h"
#include "config.h"
#include "io_expander.h"
#include "alarm_zones.h"
#include "alarm_controller.h"
#include "sms_gateway.h"
#include "sms_commands.h"
#include "config_manager.h"
#include "network.h"
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static char lineBuffer[CLI_MAX_LINE_LEN];
static int  linePos = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void printPrompt()
{
    Serial.print("sf_alarm> ");
}

static void printHelp()
{
    Serial.println();
    Serial.println("=== SF_Alarm CLI Commands ===");
    Serial.println("  status           — System status overview");
    Serial.println("  zones            — Show all zone states");
    Serial.println("  inputs           — Show raw input bitmask");
    Serial.println("  outputs          — Show output bitmask");
    Serial.println("  arm <pin>        — Arm system (away)");
    Serial.println("  arm home <pin>   — Arm system (home)");
    Serial.println("  disarm <pin>     — Disarm system");
    Serial.println("  mute             — Mute siren");
    Serial.println();
    Serial.println("  zone <n> name <text>   — Set zone name");
    Serial.println("  zone <n> type <inst|dly|24h|flw>  — Set zone type");
    Serial.println("  zone <n> nc|no         — Set zone wiring");
    Serial.println("  zone <n> enable|disable — Enable/disable zone");
    Serial.println("  zone <n> bypass|unbypass — Bypass/restore zone");
    Serial.println("  zone <n> text <msg>    — Set alarm SMS text");
    Serial.println();
    Serial.println("  phone add <number>     — Add alert phone number");
    Serial.println("  phone remove <number>  — Remove phone number");
    Serial.println("  phone list             — List phone numbers");
    Serial.println("  phone clear            — Clear all phone numbers");
    Serial.println();
    Serial.println("  wifi <ssid> <pass>     — Set Wi-Fi credentials");
    Serial.println("  router <ip> <user> <pass> — Set Cudy LT500D creds");
    Serial.println("  network                — Show network status");
    Serial.println();
    Serial.println("  pin <newpin>           — Set alarm PIN");
    Serial.println("  delay exit <sec>       — Set exit delay");
    Serial.println("  delay entry <sec>      — Set entry delay");
    Serial.println("  siren dur <sec>        — Set siren duration");
    Serial.println("  siren ch <0-15>        — Set siren output channel");
    Serial.println();
    Serial.println("  test sms <number> <msg> — Send test SMS");
    Serial.println("  test output <0-15>     — Toggle an output");
    Serial.println("  test input             — Live input monitor");
    Serial.println();
    Serial.println("  save                   — Save config to NVS");
    Serial.println("  load                   — Load config from NVS");
    Serial.println("  factory                — Factory reset");
    Serial.println("  config                 — Show configuration");
    Serial.println("  reboot                 — Restart ESP32");
    Serial.println("  help                   — This help text");
    Serial.println("=============================");
}

// ---------------------------------------------------------------------------
// Command Dispatcher
// ---------------------------------------------------------------------------

static void processLine(const char* line)
{
    // Skip empty lines
    if (strlen(line) == 0) {
        printPrompt();
        return;
    }

    // Make a mutable copy
    char cmd[CLI_MAX_LINE_LEN];
    strncpy(cmd, line, CLI_MAX_LINE_LEN - 1);
    cmd[CLI_MAX_LINE_LEN - 1] = '\0';

    // Trim
    char* start = cmd;
    while (*start && isspace(*start)) start++;
    int len = strlen(start);
    while (len > 0 && isspace(start[len - 1])) { start[len - 1] = '\0'; len--; }

    // Tokenize first word
    char* arg1 = strchr(start, ' ');
    if (arg1) {
        *arg1 = '\0';
        arg1++;
        while (*arg1 && isspace(*arg1)) arg1++;
    }

    // Convert command to lowercase
    for (int i = 0; start[i]; i++) start[i] = tolower(start[i]);

    // --- Dispatch ---

    if (strcmp(start, "help") == 0 || strcmp(start, "?") == 0) {
        printHelp();
    }
    else if (strcmp(start, "status") == 0) {
        alarmPrintStatus();
        zonesPrintStatus();
        networkPrintStatus();
    }
    else if (strcmp(start, "zones") == 0) {
        zonesPrintStatus();
    }
    else if (strcmp(start, "inputs") == 0) {
        uint16_t inputs = ioExpanderReadInputs();
        Serial.printf("Inputs: 0x%04X (", inputs);
        for (int i = 15; i >= 0; i--) {
            Serial.print((inputs >> i) & 1);
            if (i % 4 == 0 && i > 0) Serial.print(' ');
        }
        Serial.println(")");
    }
    else if (strcmp(start, "outputs") == 0) {
        uint16_t outputs = ioExpanderGetOutputs();
        Serial.printf("Outputs: 0x%04X\n", outputs);
    }
    else if (strcmp(start, "arm") == 0) {
        if (arg1 && strncmp(arg1, "home", 4) == 0) {
            char* pin = arg1 + 4;
            while (*pin && isspace(*pin)) pin++;
            alarmArmHome(pin);
        } else {
            alarmArmAway(arg1 ? arg1 : "");
        }
    }
    else if (strcmp(start, "disarm") == 0) {
        alarmDisarm(arg1 ? arg1 : "");
    }
    else if (strcmp(start, "mute") == 0) {
        alarmMuteSiren();
    }
    else if (strcmp(start, "zone") == 0 && arg1) {
        // Parse zone number
        int zoneNum = atoi(arg1);
        if (zoneNum < 1 || zoneNum > MAX_ZONES) {
            Serial.println("Invalid zone number (1-16)");
        } else {
            char* subcmd = strchr(arg1, ' ');
            if (subcmd) {
                subcmd++;
                while (*subcmd && isspace(*subcmd)) subcmd++;

                ZoneConfig* cfg = zonesGetConfig(zoneNum - 1);
                if (!cfg) {
                    Serial.println("Error getting zone config");
                } else if (strncmp(subcmd, "name ", 5) == 0) {
                    strncpy(cfg->name, subcmd + 5, MAX_ZONE_NAME_LEN - 1);
                    Serial.printf("Zone %d name: %s\n", zoneNum, cfg->name);
                } else if (strncmp(subcmd, "type ", 5) == 0) {
                    char* typeStr = subcmd + 5;
                    if (strcmp(typeStr, "inst") == 0) cfg->type = ZONE_INSTANT;
                    else if (strcmp(typeStr, "dly") == 0) cfg->type = ZONE_DELAYED;
                    else if (strcmp(typeStr, "24h") == 0) cfg->type = ZONE_24H;
                    else if (strcmp(typeStr, "flw") == 0) cfg->type = ZONE_FOLLOWER;
                    else { Serial.println("Unknown type: inst|dly|24h|flw"); goto done; }
                    Serial.printf("Zone %d type updated\n", zoneNum);
                } else if (strcmp(subcmd, "nc") == 0) {
                    cfg->wiring = ZONE_NC;
                    Serial.printf("Zone %d set to NC\n", zoneNum);
                } else if (strcmp(subcmd, "no") == 0) {
                    cfg->wiring = ZONE_NO;
                    Serial.printf("Zone %d set to NO\n", zoneNum);
                } else if (strcmp(subcmd, "enable") == 0) {
                    cfg->enabled = true;
                    Serial.printf("Zone %d enabled\n", zoneNum);
                } else if (strcmp(subcmd, "disable") == 0) {
                    cfg->enabled = false;
                    Serial.printf("Zone %d disabled\n", zoneNum);
                } else if (strcmp(subcmd, "bypass") == 0) {
                    zonesSetBypassed(zoneNum - 1, true);
                } else if (strcmp(subcmd, "unbypass") == 0) {
                    zonesSetBypassed(zoneNum - 1, false);
                } else if (strncmp(subcmd, "text ", 5) == 0) {
                    smsCmdSetAlarmText(zoneNum - 1, subcmd + 5);
                } else {
                    Serial.println("Unknown zone subcommand");
                }
            } else {
                // Just print zone info
                const ZoneInfo* info = zonesGetInfo(zoneNum - 1);
                if (info) {
                    Serial.printf("Zone %d: %s [%s] type=%d wiring=%s %s\n",
                                  zoneNum, info->config.name,
                                  info->state == ZONE_NORMAL ? "OK" :
                                  info->state == ZONE_TRIGGERED ? "TRIG" :
                                  info->state == ZONE_BYPASSED ? "BYP" : "?",
                                  info->config.type,
                                  info->config.wiring == ZONE_NC ? "NC" : "NO",
                                  info->config.enabled ? "" : "(disabled)");
                }
            }
        }
    }
    else if (strcmp(start, "phone") == 0 && arg1) {
        if (strncmp(arg1, "add ", 4) == 0) {
            smsCmdAddPhone(arg1 + 4);
        } else if (strncmp(arg1, "remove ", 7) == 0) {
            smsCmdRemovePhone(arg1 + 7);
        } else if (strcmp(arg1, "list") == 0) {
            int cnt = smsCmdGetPhoneCount();
            Serial.printf("Phone numbers (%d):\n", cnt);
            for (int i = 0; i < cnt; i++) {
                Serial.printf("  [%02d] %s\n", i + 1, smsCmdGetPhone(i));
            }
        } else if (strcmp(arg1, "clear") == 0) {
            smsCmdClearPhones();
        } else {
            Serial.println("phone add|remove|list|clear");
        }
    }
    else if (strcmp(start, "wifi") == 0 && arg1) {
        char* pass = strchr(arg1, ' ');
        if (pass) {
            *pass = '\0';
            pass++;
            networkSetWifi(arg1, pass);
            Serial.printf("Wi-Fi set: SSID=%s\n", arg1);
        } else {
            Serial.println("Usage: wifi <ssid> <password>");
        }
    }
    else if (strcmp(start, "router") == 0 && arg1) {
        // Parse: router <ip> <user> <pass>
        char* user = strchr(arg1, ' ');
        if (user) {
            *user = '\0';
            user++;
            char* pass = strchr(user, ' ');
            if (pass) {
                *pass = '\0';
                pass++;
                smsGatewaySetCredentials(arg1, user, pass);
                Serial.printf("Router set: IP=%s User=%s\n", arg1, user);
            } else {
                Serial.println("Usage: router <ip> <user> <password>");
            }
        } else {
            Serial.println("Usage: router <ip> <user> <password>");
        }
    }
    else if (strcmp(start, "network") == 0) {
        networkPrintStatus();
    }
    else if (strcmp(start, "pin") == 0 && arg1) {
        alarmSetPin(arg1);
    }
    else if (strcmp(start, "delay") == 0 && arg1) {
        if (strncmp(arg1, "exit ", 5) == 0) {
            alarmSetExitDelay(atoi(arg1 + 5));
        } else if (strncmp(arg1, "entry ", 6) == 0) {
            alarmSetEntryDelay(atoi(arg1 + 6));
        } else {
            Serial.println("delay exit|entry <seconds>");
        }
    }
    else if (strcmp(start, "siren") == 0 && arg1) {
        if (strncmp(arg1, "dur ", 4) == 0) {
            alarmSetSirenDuration(atoi(arg1 + 4));
        } else if (strncmp(arg1, "ch ", 3) == 0) {
            alarmSetSirenOutput(atoi(arg1 + 3));
        } else {
            Serial.println("siren dur|ch <value>");
        }
    }
    else if (strcmp(start, "test") == 0 && arg1) {
        if (strncmp(arg1, "sms ", 4) == 0) {
            char* msg = strchr(arg1 + 4, ' ');
            if (msg) {
                *msg = '\0';
                msg++;
                Serial.printf("Sending test SMS to %s...\n", arg1 + 4);
                if (smsGatewaySend(arg1 + 4, msg)) {
                    Serial.println("SMS sent OK");
                } else {
                    Serial.printf("SMS failed: %s\n", smsGatewayGetLastError());
                }
            } else {
                Serial.println("Usage: test sms <number> <message>");
            }
        } else if (strncmp(arg1, "output ", 7) == 0) {
            int ch = atoi(arg1 + 7);
            if (ch >= 0 && ch < 16) {
                bool current = (ioExpanderGetOutputs() >> ch) & 1;
                ioExpanderSetOutput(ch, !current);
                Serial.printf("Output %d toggled to %s\n", ch, !current ? "ON" : "OFF");
            } else {
                Serial.println("Output channel 0-15");
            }
        } else if (strcmp(arg1, "input") == 0) {
            Serial.println("Live input monitor (press any key to stop)...");
            uint16_t last = 0xFFFF;
            while (!Serial.available()) {
                uint16_t inputs = ioExpanderReadInputs();
                if (inputs != last) {
                    Serial.printf("  Inputs: 0x%04X |", inputs);
                    for (int i = 0; i < 16; i++) {
                        Serial.printf(" %d:%d", i + 1, (inputs >> i) & 1);
                    }
                    Serial.println();
                    last = inputs;
                }
                delay(100);
            }
            while (Serial.available()) Serial.read();  // Flush
            Serial.println("Monitor stopped");
        } else {
            Serial.println("test sms|output|input");
        }
    }
    else if (strcmp(start, "save") == 0) {
        configSave();
    }
    else if (strcmp(start, "load") == 0) {
        configLoad();
    }
    else if (strcmp(start, "factory") == 0) {
        Serial.println("Factory reset? Type 'YES' to confirm:");
        // Simple blocking confirmation
        delay(100);
        char confirm[8] = "";
        int ci = 0;
        uint32_t timeout = millis() + 10000;
        while (millis() < timeout) {
            if (Serial.available()) {
                char c = Serial.read();
                if (c == '\n' || c == '\r') break;
                if (ci < 7) { confirm[ci++] = c; confirm[ci] = '\0'; }
            }
        }
        if (strcmp(confirm, "YES") == 0) {
            configFactoryReset();
        } else {
            Serial.println("Factory reset cancelled");
        }
    }
    else if (strcmp(start, "config") == 0) {
        configPrint();
    }
    else if (strcmp(start, "reboot") == 0) {
        Serial.println("Rebooting...");
        delay(500);
        ESP.restart();
    }
    else {
        Serial.printf("Unknown command: '%s'. Type 'help' for options.\n", start);
    }

done:
    printPrompt();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void cliInit()
{
    linePos = 0;
    memset(lineBuffer, 0, sizeof(lineBuffer));
    Serial.println();
    Serial.println("================================");
    Serial.printf("  SF_Alarm v%s\n", FW_VERSION_STR);
    Serial.println("  KC868-A16 v1.6 Alarm Controller");
    Serial.println("  Type 'help' for commands");
    Serial.println("================================");
    printPrompt();
}

void cliUpdate()
{
    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\r') continue;  // Ignore CR

        if (c == '\n') {
            Serial.println();  // Echo newline
            lineBuffer[linePos] = '\0';
            processLine(lineBuffer);
            linePos = 0;
            memset(lineBuffer, 0, sizeof(lineBuffer));
            continue;
        }

        // Handle backspace
        if (c == '\b' || c == 127) {
            if (linePos > 0) {
                linePos--;
                lineBuffer[linePos] = '\0';
                Serial.print("\b \b");
            }
            continue;
        }

        // Echo and buffer printable characters
        if (linePos < CLI_MAX_LINE_LEN - 1 && isPrintable(c)) {
            lineBuffer[linePos++] = c;
            Serial.print(c);
        }
    }
}
