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
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct PendingAlert {
    char message[160];
    char targetPhone[32]; // Empty for broadcast, populated for targeted reply
};
static QueueHandle_t rtosAlertQueue = NULL; 
static uint32_t lastAlertProcessedMs = 0;
static const uint32_t ALERT_PROCESS_INTERVAL_MS = 1000; // Small gap between alerts

// ---------------------------------------------------------------------------
// Alarm Event Handler — sends SMS alerts
// ---------------------------------------------------------------------------
static uint32_t lastZoneAlertMs[16] = {0};

static void onAlarmEvent(AlarmEvent event, const char* details)
{
    char msg[160];

    switch (event) {
        case EVT_ALARM_TRIGGERED:
            // STORM THROTTLING: Prevent flooding SMS for the same window kick
            if (strstr(details, "Zone ")) {
                int zId = atoi(details + 5) - 1;
                if (zId >= 0 && zId < 16) {
                    if (millis() - lastZoneAlertMs[zId] < 60000) {
                        Serial.printf("[MAIN] Suppressing redundant storm alert for Zone %d\n", zId + 1);
                        return;
                    }
                    lastZoneAlertMs[zId] = millis();
                }
            }
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
    if (!rtosAlertQueue) return;
    PendingAlert alert;
    memset(&alert, 0, sizeof(PendingAlert)); // Zero-out to prevent RTOS stack memory leak
    
    strncpy(alert.message, message, sizeof(alert.message) - 1);
    alert.message[sizeof(alert.message) - 1] = '\0';
    alert.targetPhone[0] = '\0'; // Empty for broadcast
    
    if (xQueueSend(rtosAlertQueue, &alert, 0) == pdTRUE) {
        Serial.println("[MAIN] Alert queued into RTOS IPC");
    } else {
        Serial.println("[MAIN] ERROR: RTOS Alert queue full!");
    }
}

void alarmQueueReply(const char* phone, const char* message)
{
    if (!rtosAlertQueue) return;
    PendingAlert alert;
    memset(&alert, 0, sizeof(PendingAlert)); // Zero-out to prevent RTOS stack memory leak
    
    strncpy(alert.message, message, sizeof(alert.message) - 1);
    alert.message[sizeof(alert.message) - 1] = '\0';
    strncpy(alert.targetPhone, phone, sizeof(alert.targetPhone) - 1);
    alert.targetPhone[sizeof(alert.targetPhone) - 1] = '\0';

    if (xQueueSend(rtosAlertQueue, &alert, 0) == pdTRUE) {
        Serial.println("[MAIN] Targeted reply queued into RTOS IPC");
    } else {
        Serial.println("[MAIN] ERROR: RTOS Alert queue full! (Reply dropped)");
    }
}

static void processAlertQueue()
{
    uint32_t now = millis();
    if (now - lastAlertProcessedMs < ALERT_PROCESS_INTERVAL_MS) return;

    if (!rtosAlertQueue) return;

    PendingAlert alert;
    if (xQueueReceive(rtosAlertQueue, &alert, 0) == pdTRUE) {
        lastAlertProcessedMs = now;
        
        if (strlen(alert.targetPhone) > 0) {
            // Targeted Reply Only
            Serial.printf("[MAIN] Processing queued targeted reply to %s: %s\n", alert.targetPhone, alert.message);
            smsGatewaySend(alert.targetPhone, alert.message);
            esp_task_wdt_reset(); // Yield to watchdog after potentially blocking SMS send
        } else {
            // Broadcast Delivery
            Serial.printf("[MAIN] Processing queued broadcast alert: %s\n", alert.message);
            
            // 1. WhatsApp Delivery
            WhatsAppMode waM = whatsappGetMode();
            if (waM == WA_MODE_WHATSAPP || waM == WA_MODE_BOTH) {
                whatsappSend(whatsappGetPhone(), whatsappGetApiKey(), alert.message);
                esp_task_wdt_reset(); // Yield to watchdog
            }

            // 2. SMS/Call Delivery
            if (waM == WA_MODE_SMS || waM == WA_MODE_BOTH) {
                if (smsCmdGetWorkingMode() == MODE_CALL) {
                    char voiceMsg[180];
                    snprintf(voiceMsg, sizeof(voiceMsg), "[VOICE CALL] %s", alert.message);
                    smsCmdSendAlert(voiceMsg);
                } else {
                    smsCmdSendAlert(alert.message);
                }
                esp_task_wdt_reset(); // Yield to watchdog
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Background Network Worker Task (SMS Polling & Sending)
// ---------------------------------------------------------------------------

// Forward declaration
static void pollSmsInbox();

static void netWorkerTask(void* pvParameters)
{
    esp_task_wdt_add(NULL); // Register shadow core thread to hardware watchdog
    while (true) {
        uint32_t now = millis();
        
        // 1. Process Outbound Alerts (Highest Priority)
        processAlertQueue(); // Can block for HTTP POSTs
        esp_task_wdt_reset();

        // 2. Poll SMS Inbox (Secondary Priority)
        // SOS Mode: Skip polling entirely if there are pending alerts to send
        if (now - lastSmsPoll >= SMS_POLL_INTERVAL_MS) {
            lastSmsPoll = now;
            if (uxQueueMessagesWaiting(rtosAlertQueue) == 0) {
                pollSmsInbox(); // Can block for 10s if router is down
                esp_task_wdt_reset();
            } else {
                Serial.println("[MAIN] SOS MODE: Skipping SMS poll to prioritize outgoing alerts");
            }
        }

        // 3. Periodic Status Report (GA09)
        uint16_t reportInt = smsCmdGetReportInterval();
        if (reportInt > 0) {
            if (lastReportMs == 0) lastReportMs = now;
            if (now - lastReportMs >= (uint32_t)reportInt * 60 * 1000) {
                lastReportMs = now;
                char buf[160];
                uint16_t triggered = zonesGetTriggeredMask();
                int trigCount = 0;
                for (int j = 0; j < 16; j++) {
                    if (triggered & (1 << j)) trigCount++;
                }

                snprintf(buf, sizeof(buf),
                         "SF_Alarm PERIODIC: [%s] Trig:%d Mask:%04X | Clear:%s",
                         alarmGetStateStr(),
                         trigCount,
                         alarmGetActiveAlarmMask(),
                         zonesAllClear() ? "YES" : "NO");
                alarmBroadcast(buf);
            }
        } else {
            lastReportMs = 0;
        }

        esp_task_wdt_reset(); // Final pet for the watchdog
        vTaskDelay(pdMS_TO_TICKS(100)); // Sleep to yield
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
// Background MQTT Worker Task
// ---------------------------------------------------------------------------

static void mqttWorkerTask(void* pvParameters)
{
    while (true) {
        uint32_t now = millis();

        mqttUpdate();

        if (now - lastMqttSync >= 5000) {
            lastMqttSync = now;
            mqttSyncState();
        }

        vTaskDelay(pdMS_TO_TICKS(50)); // Yield
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
    rtosAlertQueue = xQueueCreate(ALERT_QUEUE_SIZE, sizeof(PendingAlert));

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
    xTaskCreatePinnedToCore(mqttWorkerTask, "MQTTWorker", 4096, NULL, 1, NULL, 0); // Pin to Core 0 (Network)

    // --- ONVIF ---
    Serial.println("[INIT] ONVIF...");
    onvifInit();

    // --- Start Network Worker Task ---
    Serial.println("[INIT] Starting Network Worker Task...");
    xTaskCreatePinnedToCore(netWorkerTask, "NetWorker", 8192, NULL, 1, NULL, 0); // Pin to Core 0 (Network)

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

        // --- Recovery alert (GA09: #0#) — only when system was/is armed ---
        bool currentAllClear = zonesAllClear();
        AlarmState st = alarmGetState();
        bool isArmedOrActive = (st == ALARM_ARMED_AWAY || st == ALARM_ARMED_HOME ||
                                st == ALARM_TRIGGERED  || st == ALARM_ENTRY_DELAY);
        if (currentAllClear && !lastAllClear && isArmedOrActive) {
            alarmBroadcast(smsCmdGetRecoveryText());
        }
        lastAllClear = currentAllClear;
    }

    // --- 2. Alarm State Machine ---
    alarmUpdate();

    // --- 3. Network ---
    networkUpdate();

    // --- 4. Serial CLI ---
    cliUpdate();

    // --- 5. Watchdog ---
    esp_task_wdt_reset();

    // Small yield for WiFi/system tasks
    yield();
}
