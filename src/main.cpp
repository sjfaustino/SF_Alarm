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
#include "telegram_client.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include <esp_task_wdt.h>
#include "logging.h"
#include "system_health.h"
#include "notification_manager.h"
#include "system_context.h"

static const char* TAG = "MAIN";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static SystemContext sysCtx;
static NotificationManager notifMgr;
static AlarmController almCtrl;

static uint32_t lastI2cPoll  = 0;
static uint32_t lastMqttStateSync = 0;
static bool     lastAllClear    = true;

// Task Heartbeat Registry (Task Integrity Monitor)
static volatile uint8_t taskHeartbeatBits = 0;
static portMUX_TYPE heartbeatMux = portMUX_INITIALIZER_UNLOCKED;

void sysHealthReport(uint8_t bit) {
    portENTER_CRITICAL(&heartbeatMux);
    taskHeartbeatBits |= bit;
    portEXIT_CRITICAL(&heartbeatMux);
}

static uint32_t lastGlobalHeartbeatCheck = 0;
static const uint32_t WATCHDOG_INTEGRITY_WINDOW_MS = 15000; // 15 seconds

static TaskHandle_t taskHandles[6] = { NULL, NULL, NULL, NULL, NULL, NULL };
static uint8_t restartCount[6] = { 0, 0, 0, 0, 0, 0 };

// Boot Loop Protection (RTC memory persists through soft/WDT reset)
RTC_NOINIT_ATTR uint32_t bootCount;
RTC_NOINIT_ATTR uint32_t lastKnownSystemTime; // Persistent system time (seconds since 1970 if synced, else uptime)
static bool recoveryMode = false;
static SemaphoreHandle_t bootLock = NULL;

static void restartTask(int index);

// Alerts are now managed by NotificationManager

// ---------------------------------------------------------------------------
// Alarm Event Handler — sends SMS alerts
// ---------------------------------------------------------------------------
static void onAlarmEvent(const AlarmEventInfo& info)
{
    sysCtx.notificationManager->dispatch(info, &sysCtx);
}

// ---------------------------------------------------------------------------
// Non-Blocking Alert Dispatcher
// ---------------------------------------------------------------------------
void alarmQueueReply(const char* phone, const char* message)
{
    notificationQueueReply(phone, message);
}

// ---------------------------------------------------------------------------
// Worker Tasks
// ---------------------------------------------------------------------------

static void zoneTask(void* pvParameters)
{
    SystemContext* ctx = (SystemContext*)pvParameters;
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(INPUT_SCAN_INTERVAL_MS);

    while (true) {
        // Wait for Boot Lock (Core 0 must finish setup)
        if (bootLock) xSemaphoreTake(bootLock, portMAX_DELAY);
        if (bootLock) xSemaphoreGive(bootLock); // Release again for other tasks

        sysHealthReport(HB_BIT_ZONE); 

        uint16_t inputs = 0;
        if (!ioExpanderReadInputs(&inputs)) {
            // Bus busy or faulted: skip update to fail-secure
            vTaskDelayUntil(&lastWakeTime, period);
            continue;
        }
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
    SystemContext* ctx = (SystemContext*)pvParameters;
    while (true) {
        // Wait for Boot Lock
        if (bootLock) xSemaphoreTake(bootLock, portMAX_DELAY);
        if (bootLock) xSemaphoreGive(bootLock);

        sysHealthReport(HB_BIT_NET); 

        smsGatewayUpdate(); 
        smsCmdUpdate();
        // Safe reset: if we've been stable for 10 minutes, clear boot counter
        if (millis() > 600000 && bootCount > 0) {
            bootCount = 0;
            LOG_INFO(TAG, "=== SF_Alarm System Core Initialized ===");
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void mqttWorkerTask(void* pvParameters)
{
    while (true) {
        // Wait for Boot Lock
        if (bootLock) xSemaphoreTake(bootLock, portMAX_DELAY);
        if (bootLock) xSemaphoreGive(bootLock);

        sysHealthReport(HB_BIT_MQTT); 

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
        // Wait for Boot Lock
        if (bootLock) xSemaphoreTake(bootLock, portMAX_DELAY);
        if (bootLock) xSemaphoreGive(bootLock);

        sysHealthReport(HB_BIT_CLI); 

        cliUpdate();
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

static void alertWorkerTask(void* pvParameters)
{
    SystemContext* ctx = (SystemContext*)pvParameters;
    while (true) {
        // Wait for Boot Lock
        if (bootLock) xSemaphoreTake(bootLock, portMAX_DELAY);
        if (bootLock) xSemaphoreGive(bootLock);

        sysHealthReport(HB_BIT_ALERT); 
        ctx->notificationManager->update(); 
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void heartbeatTask(void* pvParameters)
{
    while (true) {
        sysHealthReport(HB_BIT_VIBE); 

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
                        if (configGetScheduleMode() == ALARM_ARMED_HOME) alarmArmHomeInternal();
                        else alarmArmAwayInternal();
                        lastFiredMin = currentMin;
                    }
                }
                else if (dHr != -1 && dMin != -1 && timeinfo.tm_hour == dHr && currentMin == dMin) {
                    AlarmState st = alarmGetState();
                    if (st != ALARM_DISARMED) {
                        LOG_INFO(TAG, "Auto-Disarming (%02d:%02d)", dHr, dMin);
                        alarmDisarmInternal();
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

    bootLock = xSemaphoreCreateBinary(); // Boot Lock (Atomic Startup Sync)

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
    Serial.printf("  SF_Alarm v%s — System Core\n", FW_VERSION_STR);
    Serial.println("  Industrial Security Controller (ESP32)");
    Serial.println("========================================");

    // Initialize System Context
    sysCtx.notificationManager = &notifMgr;
    sysCtx.alarmController = &almCtrl;
    sysCtx.taskHeartbeatBits = &taskHeartbeatBits;

    configInit();
    sysCtx.notificationManager->init();

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
    
    // Initialize Alarm Logic
    sysCtx.alarmController->init(&sysCtx);
    sysCtx.alarmController->setCallback(onAlarmEvent);

    smsGatewayInit(DEFAULT_ROUTER_IP, DEFAULT_ROUTER_USER, DEFAULT_ROUTER_PASS);
    // TODO: activeGateway is still a global in sms_gateway.cpp, but we can wrap it
    
    smsCmdInit();
    networkInit();
    webServerInit();
    cliInit();
    mqttInit();
    onvifInit();

    xTaskCreatePinnedToCore(zoneTask, "ZoneTask", 3072, &sysCtx, 5, &taskHandles[0], 1);
    xTaskCreatePinnedToCore(netWorkerTask, "NetWorker", 4096, &sysCtx, 1, &taskHandles[1], 0);
    xTaskCreatePinnedToCore(mqttWorkerTask, "MQTTWorker", 4096, &sysCtx, 1, &taskHandles[2], 0);
    xTaskCreatePinnedToCore(heartbeatTask, "Heartbeat", 2048, &sysCtx, 1, &taskHandles[3], 1);
    xTaskCreatePinnedToCore(cliWorkerTask, "CLITask", 4096, &sysCtx, 1, &taskHandles[4], 0);
    xTaskCreatePinnedToCore(alertWorkerTask, "AlertWorker", 4096, &sysCtx, 1, &taskHandles[5], 0);

    xSemaphoreGive(bootLock); // Release workers (Total Sync achieved)
    networkDiscoveryInit();   // Enable .local discovery
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

    // DEADLOCK PROTECTION: If one task holds a critical mutex and stops heartbeating, 
    // we have a "Mutex Graveyard" scenario. Individual restarts won't work.
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
        case 0: xTaskCreatePinnedToCore(zoneTask, names[index], 3072, NULL, 5, &taskHandles[index], 1); break;
        case 1: xTaskCreatePinnedToCore(netWorkerTask, names[index], 4096, NULL, 1, &taskHandles[index], 0); break;
        case 2: xTaskCreatePinnedToCore(mqttWorkerTask, names[index], 4096, NULL, 1, &taskHandles[index], 0); break;
        case 3: xTaskCreatePinnedToCore(heartbeatTask, names[index], 2048, NULL, 1, &taskHandles[index], 1); break;
        case 4: xTaskCreatePinnedToCore(cliWorkerTask, names[index], 4096, NULL, 1, &taskHandles[index], 0); break;
        case 5: xTaskCreatePinnedToCore(alertWorkerTask, names[index], 4096, NULL, 1, &taskHandles[index], 0); break;
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

        if (currentBits == HB_ALL_HEALTHY) {
            esp_task_wdt_reset();
            // Reset failure counters on healthy cycle
            memset(restartCount, 0, sizeof(restartCount));
        } else {
            LOG_ERROR(TAG, "WATCHDOG: HANG DETECTED! Bits missing: 0x%02X", (uint8_t)(HB_ALL_HEALTHY ^ currentBits));
            
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
