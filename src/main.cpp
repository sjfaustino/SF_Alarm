#include <Arduino.h>
#include "config.h"
#include "io_expander.h"
#include "alarm_zones.h"
#include "alarm_controller.h"
#include "sms_gateway.h"
#include "sms_commands.h"
#include "config_manager.h"
#include "network.h"
#include "serial_cli.h"
#include "web_server.h"

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
static uint32_t lastInputScan   = 0;
static uint32_t lastSmsPoll     = 0;
static uint32_t lastReportMs    = 0;
static bool     lastAllClear    = true;

// ---------------------------------------------------------------------------
// Alarm Event Handler — sends SMS alerts
// ---------------------------------------------------------------------------

static void onAlarmEvent(AlarmEvent event, const char* details)
{
    char msg[160];

    switch (event) {
        case EVT_ALARM_TRIGGERED:
            // Use custom alarm text for the triggered zone if available
            snprintf(msg, sizeof(msg), "SF_Alarm ALERT: %s", details);
            
            // Check working mode (M1=SMS, M2=Call, M3=Both)
            if (smsCmdGetWorkingMode() != MODE_CALL) {
                smsCmdSendAlert(msg);
            } else {
                // Call mode simulation (we can't call, so we send a voice-prefixed SMS)
                char voiceMsg[180];
                snprintf(voiceMsg, sizeof(voiceMsg), "[VOICE CALL] %s", msg);
                smsCmdSendAlert(voiceMsg);
            }
            break;

        case EVT_ARMED_AWAY:
            smsCmdSendAlert("SF_Alarm: System ARMED (Away)");
            break;

        case EVT_ARMED_HOME:
            smsCmdSendAlert("SF_Alarm: System ARMED (Home)");
            break;

        case EVT_DISARMED:
            smsCmdSendAlert("SF_Alarm: System DISARMED");
            break;

        case EVT_TAMPER:
            snprintf(msg, sizeof(msg), "SF_Alarm TAMPER: %s", details);
            smsCmdSendAlert(msg);
            break;

        case EVT_ENTRY_DELAY:
            // Notify about entry delay (optional — can be noisy)
            Serial.printf("[MAIN] Entry delay: %s\n", details);
            break;

        case EVT_EXIT_DELAY:
            Serial.printf("[MAIN] Exit delay: %s\n", details);
            break;

        case EVT_ZONE_TRIGGERED:
            Serial.printf("[MAIN] Zone triggered: %s\n", details);
            break;

        case EVT_ZONE_RESTORED:
            Serial.printf("[MAIN] Zone restored: %s\n", details);
            break;

        case EVT_SIREN_ON:
        case EVT_SIREN_OFF:
            Serial.printf("[MAIN] Siren: %s\n", details);
            break;
    }
}

// ---------------------------------------------------------------------------
// SMS Inbox Processing
// ---------------------------------------------------------------------------

static void pollSmsInbox()
{
    if (!networkIsConnected()) return;
    if (!smsGatewayIsLoggedIn()) {
        smsGatewayLogin();
        return;  // Wait for next cycle
    }

    SmsMessage msgs[5];
    int count = smsGatewayPollInbox(msgs, 5);

    for (int i = 0; i < count; i++) {
        // Process the SMS command
        smsCmdProcess(msgs[i].sender, msgs[i].body);

        // Delete from router inbox after processing
        smsGatewayDeleteMessage(msgs[i].id);
    }
}

// ---------------------------------------------------------------------------
// Arduino Setup & Loop
// ---------------------------------------------------------------------------

void setup()
{
    // --- Serial ---
    Serial.begin(CLI_BAUD_RATE);
    delay(1000);  // Wait for serial monitor
    Serial.println();
    Serial.println("========================================");
    Serial.printf("  SF_Alarm v%s — Starting up...\n", FW_VERSION_STR);
    Serial.println("  KC868-A16 v1.6 Alarm System Controller");
    Serial.println("========================================");

    // --- Configuration ---
    configInit();

    // --- I/O Expander ---
    Serial.println("[INIT] I/O Expander...");
    if (!ioExpanderInit()) {
        Serial.println("[INIT] WARNING: Not all I2C chips responded!");
    }

    // --- Alarm Zones ---
    Serial.println("[INIT] Alarm Zones...");
    zonesInit();

    // --- Alarm Controller ---
    Serial.println("[INIT] Alarm Controller...");
    alarmInit();
    alarmSetCallback(onAlarmEvent);

    // --- SMS ---
    Serial.println("[INIT] SMS Gateway...");
    smsGatewayInit(DEFAULT_ROUTER_IP, DEFAULT_ROUTER_USER, DEFAULT_ROUTER_PASS);
    smsCmdInit();

    // --- Network ---
    Serial.println("[INIT] Network...");
    networkInit();

    // --- Load saved config (overrides defaults) ---
    configLoad();

    // --- Web Dashboard ---
    Serial.println("[INIT] Web Dashboard...");
    webServerInit();

    // --- Watchdog ---
    // esp_task_wdt_init(WATCHDOG_TIMEOUT_S, true);
    // esp_task_wdt_add(NULL);

    // --- CLI ---
    cliInit();

    Serial.println("[INIT] Startup complete!");
    Serial.println();
}

void loop()
{
    uint32_t now = millis();

    // --- 1. Scan Inputs (50 Hz) ---
    if (now - lastInputScan >= INPUT_SCAN_INTERVAL_MS) {
        lastInputScan = now;

        uint16_t inputs = ioExpanderReadInputs();
        zonesUpdate(inputs);

        // --- Recovery alert (GA09: #0#) ---
        bool currentAllClear = zonesAllClear();
        if (currentAllClear && !lastAllClear) {
            // All zones just returned to normal
            smsCmdSendAlert(smsCmdGetRecoveryText());
        }
        lastAllClear = currentAllClear;
    }

    // --- 2. Alarm State Machine ---
    alarmUpdate();

    // --- 3. Network ---
    networkUpdate();

    // --- 4. Poll SMS Inbox ---
    if (now - lastSmsPoll >= SMS_POLL_INTERVAL_MS) {
        lastSmsPoll = now;
        pollSmsInbox();
    }

    // --- 5. Periodic Status Report (GA09) ---
    uint16_t reportInt = smsCmdGetReportInterval();
    if (reportInt > 0) {
        if (lastReportMs == 0) lastReportMs = now; // Initialize on first enable

        if (now - lastReportMs >= (uint32_t)reportInt * 60 * 1000) {
            lastReportMs = now;
            
            char buf[160];
            uint16_t triggered = zonesGetTriggeredMask();
            int trigCount = 0;
            for (int i = 0; i < 16; i++) {
                if (triggered & (1 << i)) trigCount++;
            }

            snprintf(buf, sizeof(buf),
                     "SF_Alarm PERIODIC: [%s] Zones:%d triggered | Clear:%s",
                     alarmGetStateStr(),
                     trigCount,
                     zonesAllClear() ? "YES" : "NO");

            smsCmdSendAlert(buf);
        }
    } else {
        lastReportMs = 0; // Reset if disabled
    }

    // --- 6. Serial CLI ---
    cliUpdate();

    // --- 6. Watchdog ---
    // esp_task_wdt_reset();

    // Small yield for WiFi/system tasks
    yield();
}
