#include <Arduino.h>
#include "config.h"
#include "io_expander.h"
#include "alarm_zones.h"
#include "alarm_controller.h"
#include "sms_gateway.h"
#include "sms_commands.h"
#include "whatsapp_client.h"
#include "config_manager.h"
#include "network.h"
#include "serial_cli.h"
#include "web_server.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include <esp_task_wdt.h>

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
static uint32_t lastInputScan   = 0;
static uint32_t lastSmsPoll     = 0;
static uint32_t lastReportMs    = 0;
static uint32_t lastMqttSync    = 0;
static bool     lastAllClear    = true;

// Alert Queue for non-blocking broadcasts
struct PendingAlert {
    char message[160];
    bool active;
};
static PendingAlert alertQueue[ALERT_QUEUE_SIZE]; 
static uint32_t lastAlertProcessedMs = 0;
static const uint32_t ALERT_PROCESS_INTERVAL_MS = 1000; // Small gap between alerts

// ---------------------------------------------------------------------------
// Alarm Event Handler — sends SMS alerts
// ---------------------------------------------------------------------------

static void onAlarmEvent(AlarmEvent event, const char* details)
{
    char msg[160];

    switch (event) {
        case EVT_ALARM_TRIGGERED:
            snprintf(msg, sizeof(msg), "SF_Alarm ALERT: %s", details);
            alarmBroadcast(msg);
            break;

        case EVT_ARMED_AWAY:
            alarmBroadcast("SF_Alarm: System ARMED (Away)");
            break;

        case EVT_ARMED_HOME:
            alarmBroadcast("SF_Alarm: System ARMED (Home)");
            break;

        case EVT_DISARMED:
            alarmBroadcast("SF_Alarm: System DISARMED");
            break;

        case EVT_TAMPER:
            snprintf(msg, sizeof(msg), "SF_Alarm TAMPER: %s", details);
            alarmBroadcast(msg);
            break;

        case EVT_ENTRY_DELAY:
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

    // Sync state to MQTT for any alarm event
    mqttSyncState();
    
    // Publish specific event to a human-readable topic
    char eventMsg[128];
    snprintf(eventMsg, sizeof(eventMsg), "EVENT: %d | %s", (int)event, details ? details : "");
    mqttPublish("SF_Alarm/events", eventMsg);
}

// ---------------------------------------------------------------------------
// Non-Blocking Alert Dispatcher
// ---------------------------------------------------------------------------

void alarmBroadcast(const char* message)
{
    // Push into queue instead of sending immediately (avoids 25s hang)
    for (int i = 0; i < ALERT_QUEUE_SIZE; i++) {
        if (!alertQueue[i].active) {
            strncpy(alertQueue[i].message, message, sizeof(alertQueue[i].message) - 1);
            alertQueue[i].message[sizeof(alertQueue[i].message) - 1] = '\0';
            alertQueue[i].active = true;
            Serial.printf("[MAIN] Alert queued at slot %d\n", i);
            return;
        }
    }
    Serial.println("[MAIN] ERROR: Alert queue full!");
}

static void processAlertQueue()
{
    uint32_t now = millis();
    if (now - lastAlertProcessedMs < ALERT_PROCESS_INTERVAL_MS) return;

    for (int i = 0; i < ALERT_QUEUE_SIZE; i++) {
        if (alertQueue[i].active) {
            lastAlertProcessedMs = now;
            
            Serial.printf("[MAIN] Processing queued alert: %s\n", alertQueue[i].message);
            
            // 1. WhatsApp Delivery
            WhatsAppMode waM = whatsappGetMode();
            if (waM == WA_MODE_WHATSAPP || waM == WA_MODE_BOTH) {
                whatsappSend(whatsappGetPhone(), whatsappGetApiKey(), alertQueue[i].message);
            }

            // 2. SMS/Call Delivery
            if (waM == WA_MODE_SMS || waM == WA_MODE_BOTH) {
                if (smsCmdGetWorkingMode() == MODE_CALL) {
                    char voiceMsg[180];
                    snprintf(voiceMsg, sizeof(voiceMsg), "[VOICE CALL] %s", alertQueue[i].message);
                    smsCmdSendAlert(voiceMsg);
                } else {
                    smsCmdSendAlert(alertQueue[i].message);
                }
            }

            alertQueue[i].active = false;
            return; // Process only one per cycle to maintain responsiveness
        }
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

    // --- Alert Queue ---
    memset(alertQueue, 0, sizeof(alertQueue));

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
    esp_task_wdt_init(30, true); // 30s timeout
    esp_task_wdt_add(NULL);      // Add current thread (loop)

    // --- CLI ---
    cliInit();

    // --- MQTT ---
    Serial.println("[INIT] MQTT...");
    mqttInit();

    // --- ONVIF ---
    Serial.println("[INIT] ONVIF...");
    onvifInit();

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
            alarmBroadcast(smsCmdGetRecoveryText());
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

            alarmBroadcast(buf);
        }
    } else {
        lastReportMs = 0; // Reset if disabled
    }

    // --- 6. Serial CLI ---
    cliUpdate();

    // --- 7. MQTT Loop ---
    mqttUpdate();
    if (now - lastMqttSync >= 5000) {
        lastMqttSync = now;
        mqttSyncState();
    }

    // --- 8. ONVIF ---
    onvifUpdate();

    // --- 9. Alert Queue ---
    processAlertQueue();
    
    // --- 10. Watchdog ---
    esp_task_wdt_reset();

    // Small yield for WiFi/system tasks
    yield();
}
