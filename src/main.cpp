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
#include "logging.h"

static const char* TAG = "MAIN";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static uint32_t lastI2cPoll  = 0;
static uint32_t lastMqttStateSync = 0;
static bool     lastAllClear    = true;

// Task Heartbeat Registry (Tungsten Aegis)
static volatile uint8_t taskHeartbeatBits = 0;
static portMUX_TYPE heartbeatMux = portMUX_INITIALIZER_UNLOCKED;

static const uint8_t TASK_HB_ZONE = (1 << 0);
static const uint8_t TASK_HB_NET  = (1 << 1);
static const uint8_t TASK_HB_MQTT = (1 << 2);
static const uint8_t TASK_HB_VIBE = (1 << 3);
static const uint8_t TASK_HB_CLI  = (1 << 4);
static const uint8_t TASK_HB_ALERT = (1 << 5);
static const uint8_t ALL_TASKS_HEALTHY = (TASK_HB_ZONE | TASK_HB_NET | TASK_HB_MQTT | TASK_HB_VIBE | TASK_HB_CLI | TASK_HB_ALERT);

static uint32_t lastGlobalHeartbeatCheck = 0;
static const uint32_t WATCHDOG_INTEGRITY_WINDOW_MS = 15000; // 15 seconds

static TaskHandle_t taskHandles[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
static uint8_t restartCount[6] = { 0, 0, 0, 0, 0, 0 };

// Boot Loop Protection (RTC memory persists through soft/WDT reset)
RTC_NOINIT_ATTR uint32_t bootCount;
RTC_NOINIT_ATTR uint32_t lastKnownSystemTime; // Persistent system time (seconds since 1970 if synced, else uptime)
static bool recoveryMode = false;

static void restartTask(int index);

// Alert Queue for non-blocking broadcasts
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct PendingAlert {
    char message[128];
    char targetPhone[24]; // Zero for broadcast
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
            if (info.zoneId >= 0 && info.zoneId < 16) {
                if (millis() - lastZoneAlertMs[info.zoneId] < 60000) {
                    LOG_INFO(TAG, "Throttling redundant alert for Zone %d", info.zoneId + 1);
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
            LOG_INFO(TAG, "Entry delay: %s", details);
            break;

        case EVT_EXIT_DELAY:
            LOG_INFO(TAG, "Exit delay: %s", details);
            break;

        case EVT_ZONE_TRIGGERED:
            LOG_INFO(TAG, "Zone triggered: %s", details);
            break;

        case EVT_ZONE_RESTORED:
            LOG_INFO(TAG, "Zone restored: %s", details);
            break;

        case EVT_SIREN_ON:
        case EVT_SIREN_OFF:
            LOG_INFO(TAG, "Siren: %s", details);
            break;
        
        default: break;
    }

    // High-latency MQTT events handled asynchronosly via worker thread
    switch (info.event) {
        case EVT_ALARM_TRIGGERED:
        case EVT_TAMPER:
        case EVT_ARMED_AWAY:
        case EVT_ARMED_HOME:
        case EVT_DISARMED: {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "EVENT:%d Z:%d | %s", (int)info.event, info.zoneId, details);
            mqttPublish("SF_Alarm/events", logMsg); 
            break;
        }
        default: break;
    }
}

// ---------------------------------------------------------------------------
// Non-Blocking Alert Dispatcher
// ---------------------------------------------------------------------------

void alarmBroadcast(const char* message)
{
    if (!rtosAlertQueue) return;
    PendingAlert alert;
    memset(&alert, 0, sizeof(PendingAlert)); 
    
    strncpy(alert.message, message, sizeof(alert.message) - 1);
    alert.message[sizeof(alert.message) - 1] = '\0';
    alert.targetPhone[0] = '\0'; 
    
    if (xQueueSend(rtosAlertQueue, &alert, 0) == pdTRUE) {
        LOG_INFO(TAG, "Alert queued");
    } else {
        LOG_ERROR(TAG, "Alert queue full!");
    }
}

void alarmQueueReply(const char* phone, const char* message)
{
    if (!rtosAlertQueue) return;
    PendingAlert alert;
    memset(&alert, 0, sizeof(PendingAlert)); 
    
    strncpy(alert.message, message, sizeof(alert.message) - 1);
    alert.message[sizeof(alert.message) - 1] = '\0';
    strncpy(alert.targetPhone, phone, sizeof(alert.targetPhone) - 1);
    alert.targetPhone[sizeof(alert.targetPhone) - 1] = '\0';

    if (xQueueSend(rtosAlertQueue, &alert, 0) == pdTRUE) {
        LOG_INFO(TAG, "Targeted reply queued");
    } else {
        LOG_ERROR(TAG, "Alert queue full!");
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
            LOG_INFO(TAG, "Targeted reply to %s: %s", alert.targetPhone, alert.message);
            smsGatewaySend(alert.targetPhone, alert.message);
        } else {
            WhatsAppMode waM = whatsappGetMode();
            if (waM == WA_MODE_WHATSAPP || waM == WA_MODE_BOTH) {
                whatsappSend(whatsappGetPhone(), whatsappGetApiKey(), alert.message);
            }

            if (waM == WA_MODE_SMS || waM == WA_MODE_BOTH) {
                if (smsCmdGetWorkingMode() == MODE_CALL) {
                    char voiceMsg[180];
                    snprintf(voiceMsg, sizeof(voiceMsg), "[VOICE CALL] %s", alert.message);
                    smsCmdSendAlert(voiceMsg);
                } else {
                    smsCmdSendAlert(alert.message);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Worker Tasks
// ---------------------------------------------------------------------------

static void zoneTask(void* pvParameters)
{
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(INPUT_SCAN_INTERVAL_MS);

    while (true) {
        portENTER_CRITICAL(&heartbeatMux);
        taskHeartbeatBits |= TASK_HB_ZONE; 
        portEXIT_CRITICAL(&heartbeatMux);

        uint16_t inputs = ioExpanderReadInputs();
        zonesUpdate(inputs);

        bool currentAllClear = zonesAllClear();
        AlarmState st = alarmGetState();
        bool isArmedOrActive = (st == ALARM_ARMED_AWAY || st == ALARM_ARMED_HOME ||
                                st == ALARM_TRIGGERED  || st == ALARM_ENTRY_DELAY);
        if (currentAllClear && !lastAllClear && isArmedOrActive) {
            alarmBroadcast(smsCmdGetRecoveryText());
        }
        lastAllClear = currentAllClear;

        vTaskDelayUntil(&lastWakeTime, period); 
    }
}

static void netWorkerTask(void* pvParameters)
{
    while (true) {
        portENTER_CRITICAL(&heartbeatMux);
        taskHeartbeatBits |= TASK_HB_NET; 
        portEXIT_CRITICAL(&heartbeatMux);

        smsGatewayUpdate(); 
        smsCmdUpdate();
        // Safe reset: if we've been stable for 10 minutes, clear boot counter
        if (millis() > 600000 && bootCount > 0) {
            bootCount = 0;
            LOG_INFO(TAG, "System stable. Boot counter reset.");
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void mqttWorkerTask(void* pvParameters)
{
    while (true) {
        portENTER_CRITICAL(&heartbeatMux);
        taskHeartbeatBits |= TASK_HB_MQTT; 
        portEXIT_CRITICAL(&heartbeatMux);

        uint32_t now = millis();
        mqttUpdate();

        if (now - lastMqttStateSync >= 5000) {
            lastMqttStateSync = now;
            mqttSyncState();
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void cliWorkerTask(void* pvParameters)
{
    while (true) {
        portENTER_CRITICAL(&heartbeatMux);
        taskHeartbeatBits |= TASK_HB_CLI; 
        portEXIT_CRITICAL(&heartbeatMux);

        cliUpdate();
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

static void alertWorkerTask(void* pvParameters)
{
    while (true) {
        portENTER_CRITICAL(&heartbeatMux);
        taskHeartbeatBits |= TASK_HB_ALERT;
        portEXIT_CRITICAL(&heartbeatMux);

        processAlertQueue(); 
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void heartbeatTask(void* pvParameters)
{
    while (true) {
        portENTER_CRITICAL(&heartbeatMux);
        taskHeartbeatBits |= TASK_HB_VIBE; 
        portEXIT_CRITICAL(&heartbeatMux);

        if (configGetHeartbeatEnabled()) {
            AlarmState st = alarmGetState();
            if (st == ALARM_ARMED_AWAY || st == ALARM_ARMED_HOME) {
                digitalWrite(HEARTBEAT_LED_PIN, HIGH);
                digitalWrite(HEARTBEAT_BUZZER_PIN, HIGH);
                vTaskDelay(pdMS_TO_TICKS(50)); 
                digitalWrite(HEARTBEAT_LED_PIN, LOW);
                digitalWrite(HEARTBEAT_BUZZER_PIN, LOW);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1950));
    }
}

static void schedulerTask(void* pvParameters)
{
    int lastFiredMin = -1;
    while (true) {
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10)) {
            int currentMin = timeinfo.tm_min;
            if (currentMin != lastFiredMin) {
                int8_t aHr, aMin, dHr, dMin;
                configGetSchedule(timeinfo.tm_wday, aHr, aMin, dHr, dMin);

                if (aHr != -1 && aMin != -1 && timeinfo.tm_hour == aHr && currentMin == aMin) {
                    AlarmState st = alarmGetState();
                    if (st == ALARM_DISARMED) {
                        LOG_INFO(TAG, "Auto-Arming (%02d:%02d)", aHr, aMin);
                        if (configGetScheduleMode() == ALARM_ARMED_HOME) alarmArmHome("AUTO");
                        else alarmArmAway("AUTO");
                        lastFiredMin = currentMin;
                    }
                }
                else if (dHr != -1 && dMin != -1 && timeinfo.tm_hour == dHr && currentMin == dMin) {
                    AlarmState st = alarmGetState();
                    if (st != ALARM_DISARMED) {
                        LOG_INFO(TAG, "Auto-Disarming (%02d:%02d)", dHr, dMin);
                        alarmDisarm("AUTO");
                        lastFiredMin = currentMin;
                    }
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

// ---------------------------------------------------------------------------
// Arduino Setup & Loop
// ---------------------------------------------------------------------------

void setup()
{
    esp_task_wdt_init(15, true); 
    esp_task_wdt_add(NULL);      

    logInit();

    Serial.begin(CLI_BAUD_RATE);
    LOG_INFO(TAG, "SF_Alarm Booting (v%s)...", FW_VERSION_STR);

    // Boot Loop Protection Logic
    // millis() resets to 0 on every boot. 
    // We only reset bootCount if the system has been stable for > 10 minutes.
    // If we're here, and bootCount hasn't been reset by the loop yet, we increment.
    bootCount++;

    if (bootCount > 5) {
        LOG_ERROR(TAG, "FATAL: Recursive boot loop detected! Entering RECOVERY MODE.");
        recoveryMode = true;
        // In recovery mode, we MUST still init I/O for CLI to work safely
        ioExpanderInit();
        configInit();
        cliInit();
        while(true) {
            cliUpdate();
            // Slow "Distress" blink
            digitalWrite(HEARTBEAT_LED_PIN, HIGH);
            delay(100);
            digitalWrite(HEARTBEAT_LED_PIN, LOW);
            delay(100);
            digitalWrite(HEARTBEAT_LED_PIN, HIGH);
            delay(100);
            digitalWrite(HEARTBEAT_LED_PIN, LOW);
            delay(2000);
            esp_task_wdt_reset();
        }
    }
    Serial.println("========================================");
    Serial.printf("  SF_Alarm v%s — Obsidian Mantle\n", FW_VERSION_STR);
    Serial.println("  Industrial Security Controller (ESP32)");
    Serial.println("========================================");

    configInit();
    rtosAlertQueue = xQueueCreate(ALERT_QUEUE_SIZE, sizeof(PendingAlert));

    pinMode(HEARTBEAT_LED_PIN, OUTPUT);
    digitalWrite(HEARTBEAT_LED_PIN, LOW);
    pinMode(HEARTBEAT_BUZZER_PIN, OUTPUT);
    digitalWrite(HEARTBEAT_BUZZER_PIN, LOW);

    LOG_INFO(TAG, "Initializing Hardware...");
    if (!ioExpanderInit()) {
        LOG_ERROR(TAG, "FATAL: I/O Expander offline. Entering PANIC mode.");
        while (true) {
            digitalWrite(HEARTBEAT_LED_PIN, HIGH);
            esp_task_wdt_reset();
            delay(100);
            digitalWrite(HEARTBEAT_LED_PIN, LOW);
            delay(100);
        }
    }

    // CRITICAL: Load config BEFORE initializing alarm logic
    configLoad();
    
    // Initialize Alarm Logic (Restores ARM state from NVS correctly now)
    alarmInit();
    alarmSetCallback(onAlarmEvent);

    smsGatewayInit(DEFAULT_ROUTER_IP, DEFAULT_ROUTER_USER, DEFAULT_ROUTER_PASS);
    smsCmdInit();
    networkInit();
    webServerInit();
    cliInit();
    mqttInit();
    onvifInit();

    LOG_INFO(TAG, "Startup complete!");

    // Final Aegis: Print dropped logs if any during startup
    uint32_t dropped = logGetDroppedCount();
    if (dropped > 0) {
        LOG_WARN(TAG, "Watchdog: %u logs dropped during burst startup.", dropped);
    }
}

static void restartTask(int index)
{
    if (index < 0 || index >= 6) return;
    
    const char* names[] = { "ZoneTask", "NetWorker", "MQTTWorker", "Heartbeat", "CLITask", "AlertWorker" };
    
    if (restartCount[index] >= 3) {
        LOG_ERROR(TAG, "Task %s failed too many times. Manual intervention or Global Reboot required.", names[index]);
        return;
    }

    // DEADLOCK PREVENTION: If this task holds a system-critical mutex, 
    // a partial restart will cause a permanent lockup (The Mutex Graveyard).
    // We MUST perform a full hardware reboot in this case.
    TaskHandle_t h = taskHandles[index];
    if (h != NULL) {
        if (configGetLockOwner() == h || ioExpanderGetLockOwner() == h) {
            LOG_ERROR(TAG, "WATCHDOG: Task %s holds CRITICAL MUTEX. Forced hardware reboot to clear lock.", names[index]);
            delay(500); // Allow logs to flush
            ESP.restart();
        }
        
        vTaskDelete(h);
        taskHandles[index] = NULL;
    }

    switch(index) {
        case 0: xTaskCreatePinnedToCore(zoneTask, names[index], 4096, NULL, 5, &taskHandles[index], 1); break;
        case 1: xTaskCreatePinnedToCore(netWorkerTask, names[index], 8192, NULL, 1, &taskHandles[index], 0); break;
        case 2: xTaskCreatePinnedToCore(mqttWorkerTask, names[index], 4096, NULL, 1, &taskHandles[index], 0); break;
        case 3: xTaskCreatePinnedToCore(heartbeatTask, names[index], 2048, NULL, 1, &taskHandles[index], 1); break;
        case 4: xTaskCreatePinnedToCore(cliWorkerTask, names[index], 4096, NULL, 1, &taskHandles[index], 1); break;
        case 5: xTaskCreatePinnedToCore(alertWorkerTask, names[index], 8192, NULL, 1, &taskHandles[index], 1); break;
    }
}

void loop()
{
    uint32_t now = millis();
    alarmUpdate();
    networkUpdate();

    if (now - lastGlobalHeartbeatCheck >= 3000) {
        lastGlobalHeartbeatCheck = now;
        uint8_t currentBits = 0;
        portENTER_CRITICAL(&heartbeatMux);
        currentBits = taskHeartbeatBits;
        taskHeartbeatBits = 0; 
        portEXIT_CRITICAL(&heartbeatMux);

        if (currentBits == ALL_TASKS_HEALTHY) {
            esp_task_wdt_reset();
            // Reset failure counters on healthy cycle
            memset(restartCount, 0, sizeof(restartCount));
        } else {
            LOG_ERROR(TAG, "WATCHDOG: HANG DETECTED! Bits missing: 0x%02X", (uint8_t)(ALL_TASKS_HEALTHY ^ currentBits));
            
            // Attempt task recovery before giving up
            for (int i = 0; i < 6; i++) {
                if (!(currentBits & (1 << i))) {
                    restartTask(i);
                }
            }
        }
    }
    yield();
}
