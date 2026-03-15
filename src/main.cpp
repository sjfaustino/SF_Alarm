#if defined(ESP32)
#include <Arduino.h>
#endif
#include "config.h"
#include "io_service.h"
#include "zone_manager.h"
#include "alarm_controller.h"
#include "sms_gateway.h"
#include "sms_command_processor.h"
#include "logging.h"
#include "whatsapp_client.h"
#include "config_manager.h"
#include "network.h"
#include "serial_cli.h"
#include "web_server.h"
#include "telegram_client.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include <esp_task_wdt.h>
#include "system_health.h"
#include "notification_manager.h"
#include "phone_authenticator.h"
#include "sms_command_processor.h"

static const char* TAG = "MAIN";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static IoService           ioSvc;
static ZoneManager         zoneMgr;
static AlarmController     almCtrl;
static NotificationManager notifMgr;
static SmsService          smsSvc;
static PhoneAuthenticator   phoneAuth;
static SmsCommandProcessor  smsProc;
static MqttService         mqttSvc;
static TelegramService     telegramSvc;
static WhatsappService     whatsappSvc;
static OnvifService        onvifSvc;
static SerialCLI           serialCli;

static uint32_t lastI2cPoll  = 0;
static uint32_t lastMqttStateSync = 0;
static bool     lastAllClear    = true;

// Task Heartbeat Registry (Task Integrity Monitor)
static volatile uint16_t taskHeartbeatBits = 0;
static portMUX_TYPE heartbeatMux = portMUX_INITIALIZER_UNLOCKED;

void sysHealthReport(uint16_t bit) {
    portENTER_CRITICAL(&heartbeatMux);
    taskHeartbeatBits |= bit;
    portEXIT_CRITICAL(&heartbeatMux);
}

static uint32_t lastGlobalHeartbeatCheck = 0;
static const uint32_t WATCHDOG_INTEGRITY_WINDOW_MS = 15000; // 15 seconds

static TaskHandle_t taskHandles[7] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL };
static uint8_t restartCount[7] = { 0, 0, 0, 0, 0, 0, 0 };

// Boot Loop Protection (RTC memory persists through soft/WDT reset)
RTC_NOINIT_ATTR uint32_t bootCount;
RTC_NOINIT_ATTR uint32_t lastKnownSystemTime; // Persistent system time (seconds since 1970 if synced, else uptime)
static bool recoveryMode = false;
static SemaphoreHandle_t bootLock = NULL;

static void configWorkerTask(void* pvParameters) {
    while(true) {
        configTick();
        sysHealthReport(HB_BIT_CONFIG);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void restartTask(int index);


// Alerts are now managed by NotificationManager

static void onAlarmEvent(const AlarmEventInfo& info)
{
    notifMgr.dispatch(info);
}

void alarmSendAlert(const char* message)
{
    // Bridge to unified notification distribution
    notifMgr.broadcast(message);
}

// ---------------------------------------------------------------------------
// Non-Blocking Alert Dispatcher
// ---------------------------------------------------------------------------
void alarmQueueReply(const char* phone, const char* message)
{
    notifMgr.queueReply(phone, message);
}

// ---------------------------------------------------------------------------
// Worker Tasks
// ---------------------------------------------------------------------------

static void zoneTask(void* pvParameters)
{
    TickType_t lastWakeTime = xTaskGetTickCount();
    const TickType_t period = pdMS_TO_TICKS(INPUT_SCAN_INTERVAL_MS);
    
    while(true) {
        // Wait for Boot Lock
        if (bootLock) xSemaphoreTake(bootLock, portMAX_DELAY);
        if (bootLock) xSemaphoreGive(bootLock);

        sysHealthReport(HB_BIT_ZONE); 

        uint16_t inputs = 0;
        if (!ioSvc.readInputs(&inputs)) {
            vTaskDelayUntil(&lastWakeTime, period);
            continue;
        }
        zoneMgr.update(inputs);

        bool currentAllClear = zoneMgr.areAllClear();
        AlarmState st = almCtrl.getState();
        bool isArmedOrActive = (st == ALARM_ARMED_AWAY || st == ALARM_ARMED_HOME ||
                                st == ALARM_TRIGGERED  || st == ALARM_ENTRY_DELAY);
        if (currentAllClear && !lastAllClear && isArmedOrActive) {
            almCtrl.broadcast(smsProc.getRecoveryText());
        }
        lastAllClear = currentAllClear;

        vTaskDelayUntil(&lastWakeTime, period); 
    }
}

static void netWorkerTask(void* pvParameters)
{
    while (true) {
        // Wait for Boot Lock
        if (bootLock) xSemaphoreTake(bootLock, portMAX_DELAY);
        if (bootLock) xSemaphoreGive(bootLock);

        sysHealthReport(HB_BIT_NET); 

        smsSvc.update(); 
        smsProc.update();
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

        mqttSvc.update();

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

        serialCli.update();

        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

static void alertWorkerTask(void* pvParameters)
{
    while (true) {
        // Wait for Boot Lock
        if (bootLock) xSemaphoreTake(bootLock, portMAX_DELAY);
        if (bootLock) xSemaphoreGive(bootLock);

        sysHealthReport(HB_BIT_ALERT); 
        notifMgr.update(); 
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void maintTask(void* pvParameters)
{
    int lastFiredMin = -1;
    uint32_t lastHbRecord = 0;
    uint32_t lowHeapStartMs = 0;
    const uint32_t HEAP_CRITICAL_THRESHOLD = 32768; // 32KB
    const uint32_t HEAP_SURVIVAL_WINDOW_MS = 10000; // 10 seconds

    while (true) {
        sysHealthReport(HB_BIT_MAINT);
        uint32_t now = millis();

        // 1. Visual/Audible Heartbeat (every 2s)
        if (now - lastHbRecord >= 2000) {
            lastHbRecord = now;
            if (configGetHeartbeatEnabled()) {
                AlarmState st = almCtrl.getState();
                if (st == ALARM_ARMED_AWAY || st == ALARM_ARMED_HOME) {
                    digitalWrite(HEARTBEAT_LED_PIN, HIGH);
                    digitalWrite(HEARTBEAT_BUZZER_PIN, HIGH);
                    vTaskDelay(pdMS_TO_TICKS(50));
                    digitalWrite(HEARTBEAT_LED_PIN, LOW);
                    digitalWrite(HEARTBEAT_BUZZER_PIN, LOW);
                }
            }
        }

        // 2. Resource Watchdog (Heap Sentinel)
        uint32_t freeHeap = esp_get_free_heap_size();
        if (freeHeap < HEAP_CRITICAL_THRESHOLD) {
            if (lowHeapStartMs == 0) lowHeapStartMs = now;
            else if (now - lowHeapStartMs > HEAP_SURVIVAL_WINDOW_MS) {
                LOG_ERROR(TAG, "RESOURCE EXHAUSTED: Heap at %u bytes. Forced reboot for stability.", freeHeap);
                delay(500);
                ESP.restart();
            }
        } else {
            lowHeapStartMs = 0;
        }

        // 3. Scheduler Check (every minute)
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10)) {
            int currentMin = timeinfo.tm_min;
            if (currentMin != lastFiredMin) {
                int8_t aHr, aMin, dHr, dMin;
                configGetSchedule(timeinfo.tm_wday, aHr, aMin, dHr, dMin);

                if (aHr != -1 && aMin != -1 && timeinfo.tm_hour == aHr && currentMin == aMin) {
                    AlarmState st = almCtrl.getState();
                    if (st == ALARM_DISARMED) {
                        LOG_INFO(TAG, "Auto-Arming (%02d:%02d)", aHr, aMin);
                        if (configGetScheduleMode() == ALARM_ARMED_HOME) almCtrl.armHomeInternal();
                        else almCtrl.armAwayInternal();
                        lastFiredMin = currentMin;
                    }
                }
                else if (dHr != -1 && dMin != -1 && timeinfo.tm_hour == dHr && currentMin == dMin) {
                    AlarmState st = almCtrl.getState();
                    if (st != ALARM_DISARMED) {
                        LOG_INFO(TAG, "Auto-Disarming (%02d:%02d)", dHr, dMin);
                        almCtrl.disarmInternal();
                        lastFiredMin = currentMin;
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
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
        ioSvc.init(NULL);
        configInit(&almCtrl, &zoneMgr, &ioSvc, &notifMgr, &mqttSvc, &onvifSvc, &phoneAuth, &smsProc, &smsSvc, &whatsappSvc, &telegramSvc);
        serialCli.init(&almCtrl, &zoneMgr, &ioSvc, &smsSvc, &whatsappSvc, &smsProc, &phoneAuth);
        while(true) {
            serialCli.update();
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

    // Initialize System Context - REMOVED (PURGED)
    SemaphoreHandle_t i2cBusMutex = xSemaphoreCreateMutex();

    configInit(&almCtrl, &zoneMgr, &ioSvc, &notifMgr, &mqttSvc, &onvifSvc, &phoneAuth, &smsProc, &smsSvc, &whatsappSvc, &telegramSvc);
    notifMgr.init();

    pinMode(HEARTBEAT_LED_PIN, OUTPUT);
    digitalWrite(HEARTBEAT_LED_PIN, LOW);
    pinMode(HEARTBEAT_BUZZER_PIN, OUTPUT);
    digitalWrite(HEARTBEAT_BUZZER_PIN, LOW);

    LOG_INFO(TAG, "Initializing Hardware...");
    if (!ioSvc.init(i2cBusMutex)) {
        LOG_ERROR(TAG, "FATAL: I/O Expander offline. Entering PANIC mode.");
        while (true) {
            digitalWrite(HEARTBEAT_LED_PIN, HIGH);
            esp_task_wdt_reset();
            delay(100);
            digitalWrite(HEARTBEAT_LED_PIN, LOW);
            delay(100);
        }
    }
    zoneMgr.init(&ioSvc);

    // CRITICAL: Load config BEFORE initializing alarm logic
    configLoad(&almCtrl, &zoneMgr, &ioSvc, &notifMgr, &mqttSvc, &onvifSvc, &phoneAuth, &smsProc, &smsSvc, &whatsappSvc, &telegramSvc);
    
    // Initialize Alarm Logic
    almCtrl.init(&zoneMgr, &notifMgr, &ioSvc);
    almCtrl.setCallback(onAlarmEvent);

    smsSvc.init(&notifMgr, &smsProc);
    phoneAuth.init();
    smsProc.init(&almCtrl, &zoneMgr, &ioSvc, &notifMgr, &mqttSvc, &onvifSvc, &whatsappSvc, &telegramSvc, &phoneAuth);
    
    smsSvc.setCredentials(DEFAULT_ROUTER_IP, DEFAULT_ROUTER_USER, DEFAULT_ROUTER_PASS);
    
    networkInit();
    whatsappSvc.init(&notifMgr);
    webServerInit(&almCtrl, &zoneMgr, &ioSvc, &notifMgr, &mqttSvc, &onvifSvc, &phoneAuth, &smsProc, &whatsappSvc, &telegramSvc);
    serialCli.init(&almCtrl, &zoneMgr, &ioSvc, &smsSvc, &whatsappSvc, &smsProc, &phoneAuth);
    mqttSvc.init(&almCtrl, &zoneMgr, &ioSvc, &notifMgr);
    telegramSvc.init(&notifMgr);
    onvifSvc.init(&zoneMgr);

    xTaskCreatePinnedToCore(zoneTask, "ZoneTask", 3072, NULL, 5, &taskHandles[0], 1);
    xTaskCreatePinnedToCore(netWorkerTask, "NetWorker", 4096, NULL, 1, &taskHandles[1], 0);
    xTaskCreatePinnedToCore(mqttWorkerTask, "MQTTWorker", 4096, NULL, 1, &taskHandles[2], 0);
    xTaskCreatePinnedToCore(maintTask, "MaintTask", 3072, NULL, 1, &taskHandles[3], 1);
    xTaskCreatePinnedToCore(cliWorkerTask, "CLITask", 4096, NULL, 1, &taskHandles[4], 0);
    xTaskCreatePinnedToCore(alertWorkerTask, "AlertWorker", 4096, NULL, 1, &taskHandles[5], 0);

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
    if (index < 0 || index >= 7) return;
    
    const char* names[] = {"ZoneTask", "NetWorker", "MQTTWorker", "MaintTask", "CLITask", "AlertWorker", "ConfigTask"};
    
    if (restartCount[index] >= 3) {
        LOG_ERROR(TAG, "Task %s failed too many times. Manual intervention or Global Reboot required.", names[index]);
        return;
    }

    // DEADLOCK PROTECTION: If one task holds a critical mutex and stops heartbeating, 
    // we have a "Mutex Graveyard" scenario. Individual restarts won't work.
    // We MUST perform a full hardware reboot in this case.
    TaskHandle_t h = taskHandles[index];
    if (h != NULL) {
        if (configGetLockOwner() == h || ioSvc.getLockOwner() == h) {
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
        case 3: xTaskCreatePinnedToCore(maintTask, names[index], 3072, NULL, 1, &taskHandles[index], 1); break;
        case 4: xTaskCreatePinnedToCore(cliWorkerTask, names[index], 4096, NULL, 1, &taskHandles[index], 0); break;
        case 5: xTaskCreatePinnedToCore(alertWorkerTask, names[index], 4096, NULL, 1, &taskHandles[index], 0); break;
        case 6: xTaskCreatePinnedToCore(configWorkerTask, names[index], 3072, NULL, 1, &taskHandles[index], 1); break;
    }
}

void loop()
{
    uint32_t now = millis();
    almCtrl.update();
    serialCli.update();
    networkUpdate();

    if (now - lastGlobalHeartbeatCheck >= 3000) {
        lastGlobalHeartbeatCheck = now;
        uint16_t currentBits = 0;
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
            for (int i = 0; i < 7; i++) {
                if (!(currentBits & (1 << i))) {
                    restartTask(i);
                }
            }
        }
    }
    yield();
}
