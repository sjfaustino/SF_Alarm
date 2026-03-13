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
// Module State
// ---------------------------------------------------------------------------
static uint32_t lastI2cPoll  = 0;
static uint32_t lastMqttStateSync = 0;
// lastSmsPoll and lastReportMs were moved to their respective modules
static bool     lastAllClear    = true;

// Alert Queue for non-blocking broadcasts
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct PendingAlert {
    char message[192];
    char targetPhone[32]; // Empty for broadcast, populated for targeted reply
};
static QueueHandle_t rtosAlertQueue = NULL; 
static uint32_t lastAlertProcessedMs = 0;
static const uint32_t ALERT_PROCESS_INTERVAL_MS = 1000; // Small gap between alerts

// ---------------------------------------------------------------------------
// Alarm Event Handler — sends SMS alerts
// ---------------------------------------------------------------------------
static uint32_t lastZoneAlertMs[16] = {0};
static void onAlarmEvent(const AlarmEventInfo& info)
{
    char msg[160];
    const char* details = info.details ? info.details : "";

    switch (info.event) {
        case EVT_ALARM_TRIGGERED:
            // STORM THROTTLING: Use structured Zone ID for robust rate-limiting
            if (info.zoneId >= 0 && info.zoneId < 16) {
                if (millis() - lastZoneAlertMs[info.zoneId] < 60000) {
                    Serial.printf("[MAIN] Suppressing redundant storm alert for Zone %d\n", info.zoneId + 1);
                    return;
                }
                lastZoneAlertMs[info.zoneId] = millis();
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
        
        default: break;
    }

    // Task-specific syncs
    mqttSyncState();
    
    char eventMsg[128];
    snprintf(eventMsg, sizeof(eventMsg), "EVENT:%d Z:%d | %s", (int)info.event, info.zoneId, details);
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

static void netWorkerTask(void* pvParameters)
{
    esp_task_wdt_add(NULL); // Register shadow core thread to hardware watchdog
    while (true) {
        // 1. Process Outbound Alerts (Highest Priority)
        processAlertQueue(); // Can block for HTTP POSTs
        esp_task_wdt_reset();

        // 2. Poll SMS Inbox (Secondary Priority)
        // SOS Mode: Skip polling entirely if there are pending alerts to send
        if (uxQueueMessagesWaiting(rtosAlertQueue) == 0) {
            smsGatewayUpdate(); // Inside: network checks, login, polling, cmd execution, deletion
            esp_task_wdt_reset();
        } else {
            Serial.println("[MAIN] SOS MODE: Skipping network poll to prioritize outgoing alerts");
        }

        // 3. Periodic Status Report (GA09)
        smsCmdUpdate();
        
        esp_task_wdt_reset(); // Final pet for the watchdog
        vTaskDelay(pdMS_TO_TICKS(NET_WORKER_YIELD_MS)); // Sleep to yield
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
// Background Heartbeat Task (Visual & Audio Status)
// ---------------------------------------------------------------------------

static void heartbeatTask(void* pvParameters)
{
    while (true) {
        if (configGetHeartbeatEnabled()) {
            AlarmState st = alarmGetState();
            // Only pulse if armed (Away or Home)
            if (st == ALARM_ARMED_AWAY || st == ALARM_ARMED_HOME) {
                digitalWrite(HEARTBEAT_LED_PIN, HIGH);
                digitalWrite(HEARTBEAT_BUZZER_PIN, HIGH);
                vTaskDelay(pdMS_TO_TICKS(50)); // Tiny 50ms blip
                digitalWrite(HEARTBEAT_LED_PIN, LOW);
                digitalWrite(HEARTBEAT_BUZZER_PIN, LOW);
            }
        }
        // Wait ~2 seconds before next tick
        vTaskDelay(pdMS_TO_TICKS(1950));
    }
}

// ---------------------------------------------------------------------------
// Background Scheduler Task (Auto-Arm/Disarm)
// ---------------------------------------------------------------------------

static void schedulerTask(void* pvParameters)
{
    int lastFiredMin = -1;

    while (true) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10)) { // 10ms timeout to read RTC
            int currentMin = timeinfo.tm_min;
            if (currentMin != lastFiredMin) { // Only evaluate once per minute
                int8_t aHr, aMin, dHr, dMin;
                configGetSchedule(timeinfo.tm_wday, aHr, aMin, dHr, dMin);

                // Auto Arm
                if (aHr != -1 && aMin != -1 && timeinfo.tm_hour == aHr && currentMin == aMin) {
                    AlarmState st = alarmGetState();
                    if (st == ALARM_DISARMED) {
                        Serial.printf("[SCHEDULER] Auto-Arming triggered at %02d:%02d\n", aHr, aMin);
                        uint8_t mode = configGetScheduleMode();
                        if (mode == ALARM_ARMED_HOME) {
                            alarmArmHome("AUTO");
                        } else {
                            alarmArmAway("AUTO");
                        }
                        lastFiredMin = currentMin;
                    }
                }
                
                // Auto Disarm
                else if (dHr != -1 && dMin != -1 && timeinfo.tm_hour == dHr && currentMin == dMin) {
                    AlarmState st = alarmGetState();
                    if (st != ALARM_DISARMED) {
                        Serial.printf("[SCHEDULER] Auto-Disarming triggered at %02d:%02d\n", dHr, dMin);
                        alarmDisarm("AUTO");
                        lastFiredMin = currentMin;
                    }
                }
            }
        }
        
        // Wait ~30 seconds before checking again
        vTaskDelay(pdMS_TO_TICKS(30000));
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

    // --- Heartbeat Pins ---
    pinMode(HEARTBEAT_LED_PIN, OUTPUT);
    digitalWrite(HEARTBEAT_LED_PIN, LOW);
    pinMode(HEARTBEAT_BUZZER_PIN, OUTPUT);
    digitalWrite(HEARTBEAT_BUZZER_PIN, LOW);

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

    // --- Start Heartbeat Task ---
    Serial.println("[INIT] Starting Heartbeat Task...");
    xTaskCreatePinnedToCore(heartbeatTask, "Heartbeat", 2048, NULL, 1, NULL, 1); // Pin to Core 1 (App logic)

    // --- Start Scheduler Task ---
    Serial.println("[INIT] Starting Scheduler Task...");
    xTaskCreatePinnedToCore(schedulerTask, "Scheduler", 4096, NULL, 1, NULL, 1); // Pin to Core 1

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
