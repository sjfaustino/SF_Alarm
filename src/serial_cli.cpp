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

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static char lineBuffer[CLI_MAX_LINE_LEN];
static int  linePos = 0;

// Non-blocking state for test input monitor
static bool inputMonitorActive = false;
static uint16_t lastMonitorInputs = 0xFFFF;

// Non-blocking state for factory reset confirmation
static bool factoryPending = false;
static uint32_t factoryStartMs = 0;
static const uint32_t FACTORY_TIMEOUT_MS = 10000;
static char factoryConfirm[8] = "";
static int factoryConfirmPos = 0;

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
    Serial.println("  mode <1-3>             — Set alert mode (1:SMS, 2:Call, 3:Both)");
    Serial.println("  wa <phone> <apikey>    — Set WhatsApp credentials");
    Serial.println("  wa mode <1-3>          — Set WA mode (1:SMS, 2:WA, 3:Both)");
    Serial.println("  delay exit <sec>       — Set exit delay");
    Serial.println("  delay entry <sec>      — Set entry delay");
    Serial.println("  siren dur <sec>        — Set siren duration");
    Serial.println("  siren ch <0-15>        — Set siren output channel");
    Serial.println();
    Serial.println("  test sms <number> <msg> — Send test SMS");
    Serial.println("  test wa <msg>            — Send test WhatsApp");
    Serial.println("  test output <0-15>       — Toggle an output");
    Serial.println("  test input               — Live input monitor");
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
    else {
        // --- Critical Commands Require PIN Authentication ---
        // Any command not handled above requires an inline PIN check.
        // Format: <command> [args] pin <pin>
        char* pinMarker = strstr(start, " pin ");
        if (!pinMarker) {
            Serial.printf("Unknown command or missing PIN. Sensitive commands require 'pin <YOUR_PIN>' at the end.\nType 'help' for options.\n");
            printPrompt();
            return;
        }

        // Extract PIN
        char* providedPin = pinMarker + 5;
        while (*providedPin && isspace(*providedPin)) providedPin++;
        
        // Truncate the original command string at the " pin " marker
        *pinMarker = '\0';
        
        // Trim the command string again
        len = strlen(start);
        while (len > 0 && isspace(start[len - 1])) { start[len - 1] = '\0'; len--; }

        // Validate PIN
        if (!alarmValidatePin(providedPin)) {
            Serial.println("[CLI] ACCESS DENIED: Invalid PIN");
            printPrompt();
            return;
        }

        // --- Authenticated Command Dispatch ---
        bool configChanged = false;

        if (strcmp(start, "arm") == 0) {
            if (arg1 && strncmp(arg1, "home", 4) == 0) {
                alarmArmHome(providedPin); // Validated above, but pass for logging
            } else {
                alarmArmAway(providedPin);
            }
        }
        else if (strcmp(start, "disarm") == 0) {
            alarmDisarm(providedPin);
        }
        else if (strcmp(start, "mute") == 0) {
            alarmMuteSiren();
        }
        else if (strcmp(start, "zone") == 0 && arg1) {
            int zoneNum = atoi(arg1);
            if (zoneNum < 1 || zoneNum > MAX_ZONES) {
                Serial.println("Invalid zone number (1-16)");
            } else {
                char* subcmd = strchr(arg1, ' ');
                if (subcmd) {
                    subcmd++;
                    while (*subcmd && isspace(*subcmd)) subcmd++;
                    ZoneConfig* cfg = zonesGetConfig(zoneNum - 1);
                    if (cfg) {
                        if (strncmp(subcmd, "name ", 5) == 0) {
                            strncpy(cfg->name, subcmd + 5, MAX_ZONE_NAME_LEN - 1);
                            cfg->name[MAX_ZONE_NAME_LEN - 1] = '\0';
                            Serial.printf("Zone %d name: %s\n", zoneNum, cfg->name);
                            configChanged = true;
                        } else if (strncmp(subcmd, "type ", 5) == 0) {
                            char* typeStr = subcmd + 5;
                            char* endptr = strchr(typeStr, ' '); // Stop at " pin " which was cut
                            if (endptr) *endptr = '\0';
                            
                            if (strcmp(typeStr, "inst") == 0) { cfg->type = ZONE_INSTANT; configChanged = true; }
                            else if (strcmp(typeStr, "dly") == 0) { cfg->type = ZONE_DELAYED; configChanged = true; }
                            else if (strcmp(typeStr, "24h") == 0) { cfg->type = ZONE_24H; configChanged = true; }
                            else if (strcmp(typeStr, "flw") == 0) { cfg->type = ZONE_FOLLOWER; configChanged = true; }
                            else { Serial.println("Unknown type: inst|dly|24h|flw"); }
                            
                            if (configChanged) Serial.printf("Zone %d type updated\n", zoneNum);
                        } else if (strncmp(subcmd, "nc", 2) == 0) {
                            cfg->wiring = ZONE_NC;
                            Serial.printf("Zone %d set to NC\n", zoneNum);
                            configChanged = true;
                        } else if (strncmp(subcmd, "no", 2) == 0) {
                            cfg->wiring = ZONE_NO;
                            Serial.printf("Zone %d set to NO\n", zoneNum);
                            configChanged = true;
                        } else if (strncmp(subcmd, "enable", 6) == 0) {
                            cfg->enabled = true;
                            Serial.printf("Zone %d enabled\n", zoneNum);
                            configChanged = true;
                        } else if (strncmp(subcmd, "disable", 7) == 0) {
                            cfg->enabled = false;
                            Serial.printf("Zone %d disabled\n", zoneNum);
                            configChanged = true;
                        } else if (strncmp(subcmd, "bypass", 6) == 0) {
                            zonesSetBypassed(zoneNum - 1, true);
                        } else if (strncmp(subcmd, "unbypass", 8) == 0) {
                            zonesSetBypassed(zoneNum - 1, false);
                        } else if (strncmp(subcmd, "text ", 5) == 0) {
                            smsCmdSetAlarmText(zoneNum - 1, subcmd + 5);
                            configChanged = true;
                        } else {
                            Serial.println("Unknown zone subcommand");
                        }
                    }
                }
            }
        }
        else if (strcmp(start, "wifi") == 0 && arg1) {
            char* pass = strchr(arg1, ' ');
            if (pass) {
                *pass = '\0';
                pass++;
                networkSetWifi(arg1, pass);
                Serial.printf("Wi-Fi set: SSID=%s\n", arg1);
                configChanged = true;
            } else {
                Serial.println("Usage: wifi <ssid> <password> pin <pin>");
            }
        }
        else if (strcmp(start, "pin") == 0 && arg1) {
            alarmSetPin(arg1);
            configChanged = true;
        }
        else {
            Serial.printf("Unknown command: '%s'. Type 'help' for options.\n", start);
        }

        if (configChanged) {
            configSave();
            Serial.println("[CLI] Configuration saved to NVS");
        }
    }

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
    // --- Non-blocking input monitor ---
    if (inputMonitorActive) {
        if (Serial.available()) {
            while (Serial.available()) Serial.read(); // Flush
            inputMonitorActive = false;
            Serial.println("Monitor stopped");
            printPrompt();
            return;
        }
        // Sample inputs (called from main loop, so doesn't block)
        uint16_t inputs = ioExpanderReadInputs();
        if (inputs != lastMonitorInputs) {
            Serial.printf("  Inputs: 0x%04X |", inputs);
            for (int i = 0; i < 16; i++) {
                Serial.printf(" %d:%d", i + 1, (inputs >> i) & 1);
            }
            Serial.println();
            lastMonitorInputs = inputs;
        }
        return; // Don't process normal CLI while monitoring
    }

    // --- Non-blocking factory reset confirmation ---
    if (factoryPending) {
        if (millis() - factoryStartMs > FACTORY_TIMEOUT_MS) {
            factoryPending = false;
            Serial.println("\n[CLI] Factory reset timed out");
            printPrompt();
        } else if (Serial.available()) {
            char c = Serial.read();
            if (c == '\n' || c == '\r') {
                factoryPending = false;
                if (strcmp(factoryConfirm, "YES") == 0) {
                    configFactoryReset();
                } else {
                    Serial.println("\n[CLI] Factory reset cancelled");
                }
                printPrompt();
            } else if (factoryConfirmPos < 7) {
                factoryConfirm[factoryConfirmPos++] = c;
                factoryConfirm[factoryConfirmPos] = '\0';
                Serial.print(c);
            }
        }
        return; // Don't process normal CLI while confirming
    }

    // --- Normal CLI processing ---
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
