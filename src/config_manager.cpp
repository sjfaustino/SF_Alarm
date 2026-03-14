#include <Arduino.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "logging.h"
#include "config_manager.h"
#include "alarm_controller.h"
#include "alarm_zones.h"
#include "sms_gateway.h"
#include "sms_commands.h"
#include "whatsapp_client.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include "telegram_client.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #include "secrets_template.h"
#endif
#include "network.h"
#include "string_utils.h"

static const char* TAG = "CFG";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static SemaphoreHandle_t configMutex = NULL;

// Dirty Flags for granular saving (Protected by configMutex)
static bool dirtyMain      = false;
static bool dirtyWifi      = false;
static bool dirtyRouter    = false;
static bool dirtyZones     = false;
static bool dirtyAlerts    = false;
static bool dirtyMqtt      = false;
static bool dirtyOnvif     = false;
static bool dirtySchedule  = false;
static bool dirtyHeartbeat = false;
static bool dirtyTimezone  = false;

// Helper to safely set a dirty flag
static void setDirty(bool &flag) {
    if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        flag = true;
        xSemaphoreGiveRecursive(configMutex);
    }
}

// Helper to scrub format string vulnerability symbols
static void scrubFmt(char* str) {
    if (!str) return;
    while (*str) {
        if (*str == '%') *str = '_';
        str++;
    }
}


// Keys for NVS storage
static const char* KEY_PIN           = "pin";
static const char* KEY_EXIT_DELAY    = "exitDelay";
static const char* KEY_ENTRY_DELAY   = "entryDelay";
static const char* KEY_SIREN_DUR     = "sirenDur";
static const char* KEY_SIREN_CH      = "sirenCh";
static const char* KEY_PHONE_COUNT   = "phoneCnt";
static const char* KEY_ROUTER_IP     = "routerIp";
static const char* KEY_ROUTER_USER   = "routerUser";
static const char* KEY_ROUTER_PASS   = "routerPass";
static const char* KEY_WIFI_SSID     = "wifiSsid";
static const char* KEY_WIFI_PASS     = "wifiPass";
static const char* KEY_RECOVERY_TXT  = "recStr";
static const char* KEY_ALARM_MODE    = "alarmMode";
static const char* KEY_REPORT_DUR    = "reportDur";
#define KEY_ALERT_CHANNELS "alertChans"
#define KEY_WA_PHONE       "waPhone"
#define KEY_WA_APIKEY      "waApiKey"
#define KEY_TG_TOKEN       "tgToken"
#define KEY_TG_CHATID      "tgChatId"
static const char* KEY_MQTT_SERVER   = "mqSrv";
static const char* KEY_MQTT_PORT     = "mqPort";
static const char* KEY_MQTT_USER     = "mqUser";
static const char* KEY_MQTT_PASS     = "mqPass";
static const char* KEY_MQTT_CLIENTID = "mqClid";
static const char* KEY_ONVIF_HOST    = "ovHost";
static const char* KEY_ONVIF_PORT    = "ovPort";
static const char* KEY_ONVIF_USER    = "ovUser";
static const char* KEY_ONVIF_PASS    = "ovPass";
static const char* KEY_ONVIF_ZONE    = "ovZone";
static const char* KEY_HEARTBEAT_EN  = "hbEn";
static const char* KEY_TZ            = "tz";
static const char* KEY_CONFIGURED    = "configured";

static bool    g_heartbeatEnabled = true;
static char    g_timezone[32]     = "GMT0";

// ---------------------------------------------------------------------------
// RTC Protected Storage (Flash Endurance Optimization)
// ---------------------------------------------------------------------------
// These survive soft-resets and watchdog triggers, preventing flash burnout.
// They are flushed to NVS only on disarm or graceful shutdown.
static RTC_NOINIT_ATTR uint32_t rtc_magic;
static RTC_NOINIT_ATTR uint8_t  rtc_failedAttempts;
static RTC_NOINIT_ATTR bool     rtc_lockedOut;
static RTC_NOINIT_ATTR uint32_t rtc_delayRemaining; // Seconds remaining in entry/exit delay

#define RTC_CONFIG_MAGIC 0xFEEDC0DE

static void rtcInit() {
    if (rtc_magic != RTC_CONFIG_MAGIC) {
        rtc_failedAttempts = 0;
        rtc_lockedOut = false;
        rtc_delayRemaining = 0;
        rtc_magic = RTC_CONFIG_MAGIC;
        LOG_INFO(TAG, "RTC Memory: Initialized (Cold Boot)");
    } else {
        LOG_INFO(TAG, "RTC Memory: Restored (Warm Boot)");
    }
}

uint32_t rtcGetDelayRemaining() { return rtc_delayRemaining; }
void rtcSetDelayRemaining(uint32_t s) { rtc_delayRemaining = s; }
bool rtcIsValid() { return rtc_magic == RTC_CONFIG_MAGIC; }

bool configGetHeartbeatEnabled() { 
    bool en = true;
    if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        en = g_heartbeatEnabled;
        xSemaphoreGiveRecursive(configMutex);
    }
    return en;
}

void configSetHeartbeatEnabled(bool en) { 
    if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (g_heartbeatEnabled != en) {
            g_heartbeatEnabled = en; 
            dirtyHeartbeat = true;
        }
        xSemaphoreGiveRecursive(configMutex);
    }
}

const char* configGetTimezone() { 
    static char buf[32]; // Return snapshot to prevent use-after-change
    if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(buf, g_timezone, sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';
        xSemaphoreGiveRecursive(configMutex);
    }
    return buf;
}

void configSetTimezone(const char* tz) { 
    if (tz == nullptr) return;
    if (configMutex && xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(g_timezone, tz) != 0) {
            strncpy(g_timezone, tz, sizeof(g_timezone)-1);
            g_timezone[sizeof(g_timezone)-1] = '\0';
            dirtyTimezone = true;
        }
        xSemaphoreGiveRecursive(configMutex);
    }
}

static int8_t g_schedArmHr[7] = {-1, -1, -1, -1, -1, -1, -1};
static int8_t g_schedArmMin[7] = {-1, -1, -1, -1, -1, -1, -1};
static int8_t g_schedDisarmHr[7] = {-1, -1, -1, -1, -1, -1, -1};
static int8_t g_schedDisarmMin[7] = {-1, -1, -1, -1, -1, -1, -1};
static uint8_t g_schedMode = 2; // ALARM_ARMED_AWAY (see alarm_controller.h)

void configGetSchedule(int dayOfWeek, int8_t &armHr, int8_t &armMin, int8_t &disarmHr, int8_t &disarmMin) {
    if (dayOfWeek < 0 || dayOfWeek > 6) return;
    armHr = g_schedArmHr[dayOfWeek];
    armMin = g_schedArmMin[dayOfWeek];
    disarmHr = g_schedDisarmHr[dayOfWeek];
    disarmMin = g_schedDisarmMin[dayOfWeek];
}

void configSetSchedule(int dayOfWeek, int8_t armHr, int8_t armMin, int8_t disarmHr, int8_t disarmMin) {
    if (dayOfWeek < 0 || dayOfWeek > 6) return;
    if (g_schedArmHr[dayOfWeek] != armHr || g_schedArmMin[dayOfWeek] != armMin ||
        g_schedDisarmHr[dayOfWeek] != disarmHr || g_schedDisarmMin[dayOfWeek] != disarmMin) {
        g_schedArmHr[dayOfWeek] = armHr;
        g_schedArmMin[dayOfWeek] = armMin;
        g_schedDisarmHr[dayOfWeek] = disarmHr;
        g_schedDisarmMin[dayOfWeek] = disarmMin;
        setDirty(dirtySchedule);
    }
}

void configMarkDirty(ConfigSection section) {
    if (!configMutex || xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    
    switch (section) {
        case CFG_MAIN:      dirtyMain = true; break;
        case CFG_WIFI:      dirtyWifi = true; break;
        case CFG_ROUTER:    dirtyRouter = true; break;
        case CFG_ZONES:     dirtyZones = true; break;
        case CFG_ALERTS:    dirtyAlerts = true; break;
        case CFG_MQTT:      dirtyMqtt = true; break;
        case CFG_ONVIF:     dirtyOnvif = true; break;
        case CFG_SCHEDULE:  dirtySchedule = true; break;
        case CFG_HEARTBEAT: dirtyHeartbeat = true; break;
        case CFG_TIMEZONE:  dirtyTimezone = true; break;
        case CFG_ALL:
            dirtyMain = dirtyWifi = dirtyRouter = dirtyZones = dirtyAlerts = 
            dirtyMqtt = dirtyOnvif = dirtySchedule = dirtyHeartbeat = dirtyTimezone = true;
            break;
    }
    xSemaphoreGiveRecursive(configMutex);
}

uint8_t configGetScheduleMode() { return g_schedMode; }
void configSetScheduleMode(uint8_t mode) { g_schedMode = mode; }

void configSaveHeartbeat() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        p.putBool(KEY_HEARTBEAT_EN, g_heartbeatEnabled);
        p.end();
    }
    dirtyHeartbeat = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSaveTimezone() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        p.putString(KEY_TZ, g_timezone);
        p.end();
    }
    dirtyTimezone = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSaveSchedule() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        uint8_t blob[29];
        blob[0] = g_schedMode;
        for (int i = 0; i < 7; i++) {
            blob[1 + i]      = (uint8_t)g_schedArmHr[i];
            blob[8 + i]      = (uint8_t)g_schedArmMin[i];
            blob[15 + i]     = (uint8_t)g_schedDisarmHr[i];
            blob[22 + i]     = (uint8_t)g_schedDisarmMin[i];
        }
        p.putBytes("sched", blob, sizeof(blob));
        p.end();
    }
    dirtySchedule = false;
    xSemaphoreGiveRecursive(configMutex);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void configInit()
{
    esp_err_t err = nvs_flash_init();
    
    // Only erase and retry if we explicitly have no free pages or a version mismatch
    // (which means the partition is functionally empty/invalid anyway).
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        LOG_WARN(TAG, "NVS Partition maintenance required (full or new). Repairing...");
        esp_err_t eraseErr = nvs_flash_erase();
        if (eraseErr == ESP_OK) {
            err = nvs_flash_init();
            if (err == ESP_OK) {
                LOG_INFO(TAG, "NVS successfully repaired.");
            }
        }
    }

    if (err != ESP_OK) {
        LOG_ERROR(TAG, "CRITICAL: NVS Flash Init FAILED (0x%X). System PANIC.", (uint32_t)err);
        // We PANIC here instead of factory resetting. 
        // We pet the watchdog manually for a short forensic window then allow reboot.
        for(int i=0; i<300; i++) {
            digitalWrite(HEARTBEAT_LED_PIN, HIGH);
            esp_task_wdt_reset();
            delay(50);
            digitalWrite(HEARTBEAT_LED_PIN, LOW);
            delay(50);
        }
        // After 30s of blinking, we stop petting and let WDT reboot us.
        while(true) {
            digitalWrite(HEARTBEAT_LED_PIN, HIGH);
            delay(50);
            digitalWrite(HEARTBEAT_LED_PIN, LOW);
            delay(50);
        }
    }

    configMutex = xSemaphoreCreateRecursiveMutex();
    rtcInit();
    LOG_INFO(TAG, "NVS manager initialized with RTC protection");
}

TaskHandle_t configGetLockOwner()
{
    if (configMutex == NULL) return NULL;
    return xSemaphoreGetMutexHolder(configMutex);
}

void configLoad()
{
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        LOG_ERROR(TAG, "Config load FAILED: Mutex timeout");
        return;
    }

    Preferences p;
    if (!p.begin(NVS_NAMESPACE, true)) {
        LOG_ERROR(TAG, "NVS Namespace open failed");
        xSemaphoreGiveRecursive(configMutex);
        return;
    }

    if (!p.getBool(KEY_CONFIGURED, false)) {
        LOG_INFO(TAG, "No saved configuration — using defaults");
        p.end();
        xSemaphoreGiveRecursive(configMutex);
        return;
    }

    LOG_INFO(TAG, "Loading configuration from NVS...");

    // --- Alarm PIN ---
    String pin = p.getString(KEY_PIN, "1234");
    String pinSh = p.getString("pin_sh", "");
    uint32_t pinChk = p.getUInt("pin_chk", 0);

    if (pinSh.length() > 0) {
        uint32_t calc = 0;
        for (int i=0; i<pinSh.length(); i++) calc += pinSh[i];
        if (calc == pinChk) {
            if (pin != pinSh) {
                LOG_WARN(TAG, "NVS: PIN recovered from shadow copy!");
                pin = pinSh;
            }
        }
    }
    alarmLoadPin(pin.c_str());

    // --- Alarm timing ---
    alarmSetExitDelay(p.getUShort(KEY_EXIT_DELAY, DEFAULT_EXIT_DELAY_S));
    alarmSetEntryDelay(p.getUShort(KEY_ENTRY_DELAY, DEFAULT_ENTRY_DELAY_S));
    alarmSetSirenDuration(p.getUShort(KEY_SIREN_DUR, DEFAULT_SIREN_DURATION_S));
    alarmSetSirenOutput(p.getUChar(KEY_SIREN_CH, 0));

    // --- Periodic Report ---
    smsCmdSetReportInterval(p.getUShort(KEY_REPORT_DUR, DEFAULT_REPORT_INTERVAL_MIN));

    // --- Recovery & Mode ---
    smsCmdSetRecoveryText(p.getString(KEY_RECOVERY_TXT, "SF_Alarm: All zones restored to normal.").c_str());
    smsCmdSetWorkingMode((WorkingMode)p.getUChar(KEY_ALARM_MODE, (uint8_t)MODE_SMS));

    // Zero-Heap NVS Loading: Direct buffer fulfillment (Senior Dev Recommended)
    char buf[128];
    size_t len;

    // --- WhatsApp ---
    len = p.getString(KEY_WA_PHONE, buf, sizeof(buf));
    if (len == 0) strncpy(buf, DEFAULT_WA_PHONE, sizeof(buf));
    
    char keyBuf[64];
    len = p.getString(KEY_WA_APIKEY, keyBuf, sizeof(keyBuf));
    if (len == 0) strncpy(keyBuf, DEFAULT_WA_APIKEY, sizeof(keyBuf));
    
    uint8_t channels = p.getUChar(KEY_ALERT_CHANNELS, (uint8_t)(CH_SMS | CH_TG));
    whatsappSetConfig(buf, keyBuf, (AlertChannel)channels);

    // --- Telegram ---
    char tgTok[80];
    len = p.getString(KEY_TG_TOKEN, tgTok, sizeof(tgTok));
    if (len == 0) strncpy(tgTok, DEFAULT_TG_TOKEN, sizeof(tgTok));

    char tgCid[32];
    len = p.getString(KEY_TG_CHATID, tgCid, sizeof(tgCid));
    if (len == 0) strncpy(tgCid, DEFAULT_TG_CHATID, sizeof(tgCid));

    telegramSetConfig(tgTok, tgCid, channels);

    // --- MQTT ---
    String mqServer = p.getString(KEY_MQTT_SERVER, "");
    uint16_t mqPort = p.getUShort(KEY_MQTT_PORT, 1883);
    String mqUser = p.getString(KEY_MQTT_USER, "");
    String mqPass = p.getString(KEY_MQTT_PASS, "");
    String mqClientId = p.getString(KEY_MQTT_CLIENTID, "SF_Alarm");
    mqttSetConfig(mqServer.c_str(), mqPort, mqUser.c_str(), mqPass.c_str(), mqClientId.c_str());

    // --- ONVIF ---
    String ovHost = p.getString(KEY_ONVIF_HOST, "");
    uint16_t ovPort = p.getUShort(KEY_ONVIF_PORT, 80);
    String ovUser = p.getString(KEY_ONVIF_USER, "");
    String ovPass = p.getString(KEY_ONVIF_PASS, "");
    uint8_t ovZone = p.getUChar(KEY_ONVIF_ZONE, 1);
    onvifSetServer(ovHost.c_str(), ovPort, ovUser.c_str(), ovPass.c_str(), ovZone);

    // --- System Extras ---
    g_heartbeatEnabled = p.getBool(KEY_HEARTBEAT_EN, true);
    String tz = p.getString(KEY_TZ, "GMT0");
    strncpy(g_timezone, tz.c_str(), sizeof(g_timezone)-1);
    g_timezone[sizeof(g_timezone)-1] = '\0';
    setenv("TZ", g_timezone, 1); // Set POSIX TZ environment var
    tzset();

    // --- Schedule ---
    uint8_t blob[29];
    if (p.getBytes("sched", blob, sizeof(blob)) == sizeof(blob)) {
        g_schedMode = blob[0];
        for (int i = 0; i < 7; i++) {
            g_schedArmHr[i]     = (int8_t)blob[1 + i];
            g_schedArmMin[i]    = (int8_t)blob[8 + i];
            g_schedDisarmHr[i]  = (int8_t)blob[15 + i];
            g_schedDisarmMin[i] = (int8_t)blob[22 + i];
        }
    }

    // --- Phone numbers ---
    smsCmdClearPhones();
    int phoneCnt = p.getInt(KEY_PHONE_COUNT, 0);
    for (int i = 0; i < phoneCnt && i < MAX_PHONE_NUMBERS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "phone%d", i);
        String phone = p.getString(key, "");
        if (phone.length() > 0) {
            smsCmdAddPhone(phone.c_str());
        }
    }

    // --- WiFi credentials ---
    String wifiSsid = p.getString(KEY_WIFI_SSID, "");
    String wifiPassStr = p.getString(KEY_WIFI_PASS, "");
    if (wifiSsid.length() > 0) {
        networkSetWifi(wifiSsid.c_str(), wifiPassStr.c_str());
    }

    // --- Router credentials ---
    String routerIp   = p.getString(KEY_ROUTER_IP, DEFAULT_ROUTER_IP);
    String routerUser = p.getString(KEY_ROUTER_USER, DEFAULT_ROUTER_USER);
    String routerPass = p.getString(KEY_ROUTER_PASS, DEFAULT_ROUTER_PASS);
    smsGatewaySetCredentials(routerIp.c_str(), routerUser.c_str(), routerPass.c_str());

    // --- Alarm Texts ---
    for (int i = 0; i < MAX_ZONES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "zTxt%d", i);
        String txt = p.getString(key, "");
        if (txt.length() > 0) {
            smsCmdSetAlarmText(i, txt.c_str());
        }
    }

    // Scrub all local string objects from heap/stack
    scrubString(pin);
    scrubString(mqServer);
    scrubString(mqUser);
    scrubString(mqPass);
    scrubString(mqClientId);
    scrubString(ovHost);
    scrubString(ovUser);
    scrubString(ovPass);
    scrubString(tz);
    scrubString(wifiSsid);
    scrubString(wifiPassStr);
    scrubString(routerIp);
    scrubString(routerUser);
    scrubString(routerPass);

    p.end();
    xSemaphoreGiveRecursive(configMutex);
    LOG_INFO(TAG, "Configuration loaded");
}

void configSavePin(const char* pin) {
    if (pin == nullptr || strlen(pin) == 0) return;
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        char safePin[MAX_PIN_LEN]; 
        strncpy(safePin, pin, sizeof(safePin) - 1);
        safePin[sizeof(safePin) - 1] = '\0';
        scrubFmt(safePin);

        // Shadow-Copy Protocol
        p.putString("pin_sh", safePin);
        
        uint32_t chk = 0;
        for (int i=0; i<strlen(safePin); i++) chk += safePin[i];
        p.putUInt("pin_chk", chk);

        p.putString(KEY_PIN, safePin);
        p.end();
    }
    dirtyMain = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSaveTiming() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        p.putUShort(KEY_EXIT_DELAY, alarmGetExitDelay());
        p.putUShort(KEY_ENTRY_DELAY, alarmGetEntryDelay());
        p.putUShort(KEY_SIREN_DUR, alarmGetSirenDuration());
        p.putUChar(KEY_SIREN_CH, alarmGetSirenOutput());
        p.end();
    }
    dirtyMain = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSaveWifi() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        char safeSsid[33];
        strncpy(safeSsid, networkGetSsid(), sizeof(safeSsid) - 1);
        safeSsid[sizeof(safeSsid) - 1] = '\0';
        scrubFmt(safeSsid);

        p.putString(KEY_WIFI_SSID, safeSsid);
        p.putString(KEY_WIFI_PASS, networkGetPass());
        memset(safeSsid, 0, sizeof(safeSsid));
        p.end();
    }
    dirtyWifi = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSavePhones() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        int phoneCnt = smsCmdGetPhoneCount();
        p.putInt(KEY_PHONE_COUNT, phoneCnt);
        for (int i = 0; i < phoneCnt; i++) {
            char key[16];
            snprintf(key, sizeof(key), "phone%d", i);
            p.putString(key, smsCmdGetPhone(i));
        }
        p.end();
    }
    dirtyAlerts = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSaveRouter() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        char ip[32], user[32], pass[64];
        strncpy(ip, smsGatewayGetRouterIp(), sizeof(ip)-1); ip[sizeof(ip)-1] = '\0';
        strncpy(user, smsGatewayGetRouterUser(), sizeof(user)-1); user[sizeof(user)-1] = '\0';
        strncpy(pass, smsGatewayGetRouterPass(), sizeof(pass)-1); pass[sizeof(pass)-1] = '\0';
        
        p.putString(KEY_ROUTER_IP, ip);
        p.putString(KEY_ROUTER_USER, user);
        p.putString(KEY_ROUTER_PASS, pass);
        
        memset(ip, 0, sizeof(ip));
        memset(user, 0, sizeof(user));
        memset(pass, 0, sizeof(pass));
        p.end();
    }
    dirtyRouter = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSaveZones() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(500)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        for (int i = 0; i < MAX_ZONES; i++) {
            const ZoneInfo* info = zonesGetInfo(i);
            if (!info) continue;

            char safeName[MAX_ZONE_NAME_LEN];
            strncpy(safeName, info->config.name, sizeof(safeName) - 1);
            safeName[sizeof(safeName) - 1] = '\0';
            scrubFmt(safeName);

            char key[16];
            snprintf(key, sizeof(key), "zName%d", i);
            p.putString(key, safeName);

            snprintf(key, sizeof(key), "zType%d", i);
            p.putUChar(key, (uint8_t)info->config.type);

            snprintf(key, sizeof(key), "zWire%d", i);
            p.putUChar(key, (uint8_t)info->config.wiring);

            snprintf(key, sizeof(key), "zEn%d", i);
            p.putBool(key, info->config.enabled);

            snprintf(key, sizeof(key), "zTxt%d", i);
            p.putString(key, smsCmdGetAlarmText(i));
        }
        p.end();
    }
    dirtyZones = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSavePeriodic() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        p.putUShort(KEY_REPORT_DUR, smsCmdGetReportInterval());
        p.putString(KEY_RECOVERY_TXT, smsCmdGetRecoveryText());
        p.putUChar(KEY_ALARM_MODE, (uint8_t)smsCmdGetWorkingMode());
        p.end();
    }
    dirtyAlerts = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSaveMqtt() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        char server[64], user[32], pass[64], client[32];
        strncpy(server, mqttGetServer(), sizeof(server)-1); server[sizeof(server)-1] = '\0';
        strncpy(user, mqttGetUser(), sizeof(user)-1); user[sizeof(user)-1] = '\0';
        strncpy(pass, mqttGetPass(), sizeof(pass)-1); pass[sizeof(pass)-1] = '\0';
        strncpy(client, mqttGetClientId(), sizeof(client)-1); client[sizeof(client)-1] = '\0';

        p.putString(KEY_MQTT_SERVER, server);
        p.putUShort(KEY_MQTT_PORT, mqttGetPort());
        p.putString(KEY_MQTT_USER, user);
        p.putString(KEY_MQTT_PASS, pass);
        p.putString(KEY_MQTT_CLIENTID, client);

        memset(server, 0, sizeof(server));
        memset(user, 0, sizeof(user));
        memset(pass, 0, sizeof(pass));
        memset(client, 0, sizeof(client));
        p.end();
    }
    dirtyMqtt = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSaveOnvif() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        p.putString(KEY_ONVIF_HOST, onvifGetHost());
        p.putUShort(KEY_ONVIF_PORT, onvifGetPort());
        p.putString(KEY_ONVIF_USER, onvifGetUser());
        p.putString(KEY_ONVIF_PASS, onvifGetPass());
        p.putUChar(KEY_ONVIF_ZONE, onvifGetTargetZone());
        p.end();
    }
    dirtyOnvif = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configSave()
{
    if (!configMutex || xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        LOG_ERROR(TAG, "Save failed: Mutex timeout");
        return;
    }

    LOG_INFO(TAG, "Executing granular save...");
    
    // Create local copies of flags to minimize mutex hold time
    bool sMain, sWifi, sAlerts, sRouter, sZones, sMqtt, sOnvif, sHeart, sTz, sSched;
    
    sMain = dirtyMain; sWifi = dirtyWifi; sAlerts = dirtyAlerts; sRouter = dirtyRouter;
    sZones = dirtyZones; sMqtt = dirtyMqtt; sOnvif = dirtyOnvif; sHeart = dirtyHeartbeat;
    sTz = dirtyTimezone; sSched = dirtySchedule;

    dirtyMain = dirtyWifi = dirtyAlerts = dirtyRouter = false;
    dirtyZones = dirtyMqtt = dirtyOnvif = dirtyHeartbeat = false;
    dirtyTimezone = dirtySchedule = false;

    if (sMain) {
        char pin[MAX_PIN_LEN];
        alarmCopyPin(pin, sizeof(pin));
        configSavePin(pin);
        scrubBuffer(pin, sizeof(pin));
        configSaveTiming();
    }
    if (sWifi)      configSaveWifi();
    if (sAlerts)    { configSavePhones(); configSavePeriodic(); configSaveWhatsapp(); configSaveTelegram(); }
    if (sRouter)    configSaveRouter();
    if (sZones)     configSaveZones();
    if (sMqtt)      configSaveMqtt();
    if (sOnvif)     configSaveOnvif();
    if (sHeart)     configSaveHeartbeat();
    if (sTz)        configSaveTimezone();
    if (sSched)     configSaveSchedule();

    // Persist "configured" bit ONLY if not already set (Flash Endurance)
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        if (!p.getBool(KEY_CONFIGURED, false)) {
            p.putBool(KEY_CONFIGURED, true);
            LOG_INFO(TAG, "NVS: First-time 'configured' bit set.");
        }
        p.end();
    }

    xSemaphoreGiveRecursive(configMutex);
    LOG_INFO(TAG, "Granular save complete");
}

void configSaveAlarmState(AlarmState state)
{
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        // Optimization: Check existing state before writing
        AlarmState current = ALARM_DISARMED;
        Preferences p;
        if (p.begin(NVS_NAMESPACE, true)) {
            current = (AlarmState)p.getUChar("state", (uint8_t)ALARM_DISARMED);
            p.end();
        }

        if (current == state) {
            xSemaphoreGiveRecursive(configMutex);
            return; 
        }

        if (p.begin(NVS_NAMESPACE, false)) {
            // Shadow-Copy Transactional Protocol: Write to shadow first
            p.putUChar("state_sh", (uint8_t)state);
            p.putUChar("state_chk", (uint8_t)(state ^ 0xFF));
            p.putUChar("state", (uint8_t)state);
            p.end();
            LOG_INFO(TAG, "NVS: Alarm state persisted: %d", (int)state);
        }

        // On Disarm, we flush security telemetry to NVS permanently
        if (state == ALARM_DISARMED) {
            configSaveSecurityState(rtc_failedAttempts, rtc_lockedOut);
        }

        xSemaphoreGiveRecursive(configMutex);
    }
}

AlarmState configLoadAlarmState()
{
    AlarmState state = ALARM_DISARMED;
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Preferences p;
        if (p.begin(NVS_NAMESPACE, true)) {
            uint8_t mainVal = p.getUChar("state", (uint8_t)ALARM_DISARMED);
            uint8_t shVal = p.getUChar("state_sh", 0xFF);
            uint8_t chkVal = p.getUChar("state_chk", 0);
            
            // Validate transaction integrity
            if (shVal == (uint8_t)(chkVal ^ 0xFF)) {
                state = (AlarmState)shVal;
                if (mainVal != shVal) {
                    LOG_WARN(TAG, "NVS: Recovered state from shadow copy!");
                }
            } else {
                state = (AlarmState)mainVal;
            }
            p.end();
        }
        xSemaphoreGiveRecursive(configMutex);
    }
    return state;
}

void configSaveSecurityState(uint8_t failedAttempts, bool lockedOut)
{
    // Update RTC immediately (Survives soft-reboot)
    rtc_failedAttempts = failedAttempts;
    rtc_lockedOut      = lockedOut;

    // Only commit to NVS if disarmed or locked out (to prevent brute-force reboot resets)
    if (lockedOut || failedAttempts == 0) {
        if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            Preferences p;
            if (p.begin(NVS_NAMESPACE, false)) {
                if (p.getUChar("failedAtts", 0) != failedAttempts || p.getBool("isLocked", false) != lockedOut) {
                    p.putUChar("failedAtts", failedAttempts);
                    p.putBool("isLocked", lockedOut);
                    LOG_INFO(TAG, "NVS: Security state committed.");
                }
                p.end();
            }
            xSemaphoreGiveRecursive(configMutex);
        }
    }
}

void configLoadSecurityState(uint8_t &failedAttempts, bool &lockedOut)
{
    // If RTC is valid, use it (Fastest, saves Flash)
    if (rtc_magic == RTC_CONFIG_MAGIC) {
        failedAttempts = rtc_failedAttempts;
        lockedOut      = rtc_lockedOut;
        return;
    }

    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Preferences p;
        if (p.begin(NVS_NAMESPACE, true)) {
            failedAttempts = p.getUChar("failedAtts", 0);
            lockedOut      = p.getBool("isLocked", false);
            p.end();
            // Populate RTC for next time
            rtc_failedAttempts = failedAttempts;
        }
        xSemaphoreGiveRecursive(configMutex);
    }
}
void configUpdateSirenTime(uint32_t seconds)
{
    // Throttled NVS writes to preserve flash longevity
    static uint32_t lastNvsCommit = 0;
    bool forceCommit = (seconds == 0);
    
    if (forceCommit || (millis() - lastNvsCommit >= 300000)) {
        if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            Preferences p;
            if (p.begin(NVS_NAMESPACE, false)) {
                if (p.getUInt("sirenTime", 0xFFFFFFFF) != seconds) {
                    p.putUInt("sirenTime", seconds);
                    LOG_INFO(TAG, "NVS: Siren active time updated (%u s)", seconds);
                }
                p.end();
            }
            lastNvsCommit = millis();
            xSemaphoreGiveRecursive(configMutex);
        }
    }
}


uint32_t configLoadSirenTime()
{
    uint32_t seconds = 0;
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Preferences p;
        if (p.begin(NVS_NAMESPACE, true)) {
            seconds = p.getUInt("sirenTime", 0);
            p.end();
        }
        xSemaphoreGiveRecursive(configMutex);
    }
    return seconds;
}

void configSaveWhatsapp() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        p.putString(KEY_WA_PHONE, whatsappGetPhone());
        p.putString(KEY_WA_APIKEY, whatsappGetApiKey());
        p.putUChar(KEY_ALERT_CHANNELS, (uint8_t)whatsappGetMode());
        p.end();
    }
    dirtyAlerts = false; 
    xSemaphoreGiveRecursive(configMutex);
}

void configSaveTelegram() {
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    Preferences p;
    if (p.begin(NVS_NAMESPACE, false)) {
        p.putString(KEY_TG_TOKEN, telegramGetToken());
        p.putString(KEY_TG_CHATID, telegramGetChatId());
        p.putUChar(KEY_ALERT_CHANNELS, (uint8_t)telegramGetChannels());
        p.end();
    }
    dirtyAlerts = false;
    xSemaphoreGiveRecursive(configMutex);
}

void configFactoryReset()
{
    LOG_INFO(TAG, "Factory reset — clearing NVS...");
    if (xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        Preferences p;
        p.begin(NVS_NAMESPACE, false);
        p.clear();
        p.end();
        xSemaphoreGiveRecursive(configMutex);
    }
    LOG_INFO(TAG, "NVS cleared. Restart to apply defaults.");
}

void configPrint()
{
    Serial.println("=== Configuration ===");

    Serial.println("Network:");
    Serial.printf("  WiFi SSID:     %s\n", networkGetSsid());
    Serial.printf("  WiFi Pass:     %s\n", strlen(networkGetPass()) ? "********" : "");
    Serial.printf("  Router IP:     %s\n", smsGatewayGetRouterIp());
    Serial.printf("  Router User:   %s\n", smsGatewayGetRouterUser());
    Serial.printf("  Router Pass:   %s\n", strlen(smsGatewayGetRouterPass()) ? "********" : "");
    Serial.printf("  Exit delay:  %d sec\n", alarmGetExitDelay());
    Serial.printf("  Entry delay: %d sec\n", alarmGetEntryDelay());
    Serial.printf("  Siren dur:   %d sec\n", alarmGetSirenDuration());
    Serial.printf("  Siren ch:    %d\n", alarmGetSirenOutput());
    Serial.printf("  Report int:  %d min\n", smsCmdGetReportInterval());
    Serial.printf("  Alarm mode:  %d (1:SMS, 2:Call, 3:Both)\n", (int)smsCmdGetWorkingMode());
    Serial.printf("  Recovery:    %s\n", smsCmdGetRecoveryText());
    Serial.printf("  WA Phone:    %s\n", whatsappGetPhone());
    Serial.printf("  Alert Chans: 0x%02X\n", (uint8_t)whatsappGetMode());
    Serial.printf("  TG ChatID:   %s\n", telegramGetChatId());
    Serial.printf("  MQTT Server: %s:%d\n", mqttGetServer(), mqttGetPort());
    Serial.printf("  MQTT User:   %s\n", mqttGetUser());
    Serial.printf("  ONVIF Cam:   %s:%d (Zone %d)\n", onvifGetHost(), onvifGetPort(), (int)onvifGetTargetZone());
    Serial.printf("  Heartbeat:   %s\n", g_heartbeatEnabled ? "ON" : "OFF");
    Serial.printf("  Timezone:    %s\n", g_timezone);
    Serial.printf("  Auto-Arm:    %s Mode\n", g_schedMode == 3 ? "HOME" : "AWAY");
    for (int i=0; i<7; i++) {
        Serial.printf("    Day %d: Arm %02d:%02d, Disarm %02d:%02d\n", 
            i, g_schedArmHr[i], g_schedArmMin[i], g_schedDisarmHr[i], g_schedDisarmMin[i]);
    }

    int phoneCnt = smsCmdGetPhoneCount();
    Serial.printf("  Phones (%d):\n", phoneCnt);
    for (int i = 0; i < phoneCnt; i++) {
        Serial.printf("    [%d] %s\n", i, smsCmdGetPhone(i));
    }

    Serial.println("=====================");
}
