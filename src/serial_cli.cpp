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
    Serial.println("  time                   — Show current NTP time");
    Serial.println("  schedule show          — Show weekly auto-arm schedule");
    Serial.println();
    Serial.println("  tz <posix_tz_string>   — Set local timezone");
    Serial.println("  schedule mode <away|home> — Set auto-arm mode");
    Serial.println("  schedule <arm|disarm> <day> <HH:MM|off> — Edit schedule");
    Serial.println("     <day> can be: 0=Sun..6=Sat, weekdays, weekends, all");
    Serial.println();
    Serial.println("  heartbeat <on|off>     — Toggle armed heartbeat LED/Buzzer");
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

static bool parseTimeStr(const char* str, int8_t &hr, int8_t &min) {
    if (strcmp(str, "off") == 0) {
        hr = -1; min = -1;
        return true;
    }
    if (sscanf(str, "%c%c:%c%c", &hr, &hr, &hr, &hr) != 4) { // Fast check format
        if (strlen(str) != 5 || str[2] != ':') return false;
    }
    int h = atoi(str);
    int m = atoi(str + 3);
    if (h >= 0 && h <= 23 && m >= 0 && m <= 59) {
        hr = h; min = m;
        return true;
    }
    return false;
}

static void applySchedule(bool isArm, int dayClass, int8_t hr, int8_t min) {
    auto applyFn = [&](int d) {
        int8_t aHr, aMin, dHr, dMin;
        configGetSchedule(d, aHr, aMin, dHr, dMin);
        if (isArm) {
            configSetSchedule(d, hr, min, dHr, dMin);
        } else {
            configSetSchedule(d, aHr, aMin, hr, min);
        }
    };
    
    if (dayClass >= 0 && dayClass <= 6) { applyFn(dayClass); }
    else if (dayClass == 10) { for(int i=1; i<=5; i++) applyFn(i); } // weekdays
    else if (dayClass == 11) { applyFn(0); applyFn(6); } // weekends
    else if (dayClass == 12) { for(int i=0; i<=6; i++) applyFn(i); } // all
}

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
    else if (strcmp(start, "time") == 0) {
        struct tm timeinfo;
        if (!getLocalTime(&timeinfo)) {
            Serial.println("Time not set! Check Network Connection and NTP.");
        } else {
            Serial.println(&timeinfo, "Current Time: %A, %B %d %Y %H:%M:%S");
        }
    }
    else if (strcmp(start, "schedule") == 0 && arg1 && strncmp(arg1, "show", 4) == 0) {
        Serial.println("=== Weekly Auto-Arm Schedule ===");
        Serial.printf("Target Mode: %s\n", configGetScheduleMode() == 3 ? "HOME" : "AWAY");
        const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        for (int i=0; i<7; i++) {
            int8_t aH, aM, dH, dM;
            configGetSchedule(i, aH, aM, dH, dM);
            Serial.printf("  %s: ", days[i]);
            if (aH == -1) Serial.print("Arm: OFF   |"); else Serial.printf("Arm: %02d:%02d |", aH, aM);
            if (dH == -1) Serial.println(" Disarm: OFF"); else Serial.printf(" Disarm: %02d:%02d\n", dH, dM);
        }
        Serial.println("================================");
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

        // Restore the null byte we placed when splitting arg1, so that
        // strstr can see the FULL original command string again.
        if (arg1 && arg1 > start) {
            *(arg1 - 1) = ' ';  // undo the '\0' placed at the first space
        }

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

        // Re-tokenize: split start into command + arg1 again
        arg1 = strchr(start, ' ');
        if (arg1) {
            *arg1 = '\0';
            arg1++;
            while (*arg1 && isspace(*arg1)) arg1++;
        }

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
        else if (strcmp(start, "router") == 0 && arg1) {
            // router <ip> <user> <pass>
            char* userStr = strchr(arg1, ' ');
            if (userStr) {
                *userStr = '\0';
                userStr++;
                char* passStr = strchr(userStr, ' ');
                if (passStr) {
                    *passStr = '\0';
                    passStr++;
                    smsGatewaySetCredentials(arg1, userStr, passStr);
                    Serial.printf("Router set: IP=%s User=%s\n", arg1, userStr);
                    configChanged = true;
                } else {
                    Serial.println("Usage: router <ip> <user> <pass> pin <pin>");
                }
            } else {
                Serial.println("Usage: router <ip> <user> <pass> pin <pin>");
            }
        }
        else if (strcmp(start, "pin") == 0 && arg1) {
            alarmSetPin(arg1);
            configChanged = true;
        }
        else if (strcmp(start, "heartbeat") == 0 && arg1) {
            if (strcmp(arg1, "on") == 0) {
                configSetHeartbeatEnabled(true);
                Serial.println("Heartbeat ENABLED. (Pulses only when ARMED)");
                configChanged = true;
            } else if (strcmp(arg1, "off") == 0) {
                configSetHeartbeatEnabled(false);
                Serial.println("Heartbeat DISABLED.");
                configChanged = true;
            } else {
                Serial.println("Usage: heartbeat <on|off> pin <pin>");
            }
        }
        else if (strcmp(start, "tz") == 0 && arg1) {
            char* tzStr = arg1;
            char* endptr = strchr(tzStr, ' '); // Stop at " pin "
            if (endptr) *endptr = '\0';
            configSetTimezone(tzStr);
            setenv("TZ", configGetTimezone(), 1);
            tzset();
            Serial.printf("Timezone updated to: %s\n", configGetTimezone());
            configChanged = true;
        }
        else if (strcmp(start, "schedule") == 0 && arg1) {
            char* subcmd = arg1;
            char* dayStr = strchr(subcmd, ' ');
            if (dayStr) {
                *dayStr = '\0';
                dayStr++;
                while (*dayStr && isspace(*dayStr)) dayStr++;
                
                if (strcmp(subcmd, "mode") == 0) {
                    char* endptr = strchr(dayStr, ' ');
                    if (endptr) *endptr = '\0';
                    if (strcmp(dayStr, "home") == 0) {
                        configSetScheduleMode(3); // ALARM_ARMED_HOME
                        Serial.println("Auto-Arm mode set to HOME");
                        configChanged = true;
                    } else if (strcmp(dayStr, "away") == 0) {
                        configSetScheduleMode(2); // ALARM_ARMED_AWAY
                        Serial.println("Auto-Arm mode set to AWAY");
                        configChanged = true;
                    } else {
                        Serial.println("Usage: schedule mode <home|away> pin <pin>");
                    }
                }
                else if (strcmp(subcmd, "arm") == 0 || strcmp(subcmd, "disarm") == 0) {
                    char* timeStr = strchr(dayStr, ' ');
                    if (timeStr) {
                        *timeStr = '\0';
                        timeStr++;
                        while (*timeStr && isspace(*timeStr)) timeStr++;
                        char* endptr = strchr(timeStr, ' ');
                        if (endptr) *endptr = '\0';

                        int dayClass = -1;
                        if (strcmp(dayStr, "weekdays") == 0) dayClass = 10;
                        else if (strcmp(dayStr, "weekends") == 0) dayClass = 11;
                        else if (strcmp(dayStr, "all") == 0) dayClass = 12;
                        else if (isdigit(dayStr[0])) {
                            dayClass = atoi(dayStr);
                            if (dayClass < 0 || dayClass > 6) dayClass = -1;
                        }

                        int8_t h, m;
                        if (dayClass != -1 && parseTimeStr(timeStr, h, m)) {
                            applySchedule(strcmp(subcmd, "arm") == 0, dayClass, h, m);
                            Serial.println("Schedule updated.");
                            configChanged = true;
                        } else {
                            Serial.println("Invalid day or time. Format: <0-6|weekdays|weekends|all> <HH:MM|off>");
                        }
                    } else {
                        Serial.println("Usage: schedule <arm|disarm> <day> <HH:MM|off> pin <pin>");
                    }
                } else {
                    Serial.println("Unknown schedule command");
                }
            } else {
                Serial.println("Usage: schedule <arm|disarm|mode> ...");
            }
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
