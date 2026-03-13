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
#include "network.h"

static const char* TAG = "CFG";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static Preferences prefs;
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
    if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        flag = true;
        xSemaphoreGive(configMutex);
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
static const char* KEY_WA_PHONE      = "waPhone";
static const char* KEY_WA_APIKEY     = "waApiKey";
static const char* KEY_WA_MODE       = "waMode";
static const char* KEY_MQTT_SERVER   = "mqServer";
static const char* KEY_MQTT_PORT     = "mqPort";
static const char* KEY_MQTT_USER     = "mqUser";
static const char* KEY_MQTT_PASS     = "mqPass";
static const char* KEY_MQTT_CLIENTID = "mqClientId";
static const char* KEY_ONVIF_HOST     = "ovHost";
static const char* KEY_ONVIF_PORT     = "ovPort";
static const char* KEY_ONVIF_USER     = "ovUser";
static const char* KEY_ONVIF_PASS     = "ovPass";
static const char* KEY_ONVIF_ZONE     = "ovZone";
static const char* KEY_HEARTBEAT_EN   = "hbEn";
static const char* KEY_TZ            = "timezone";
static const char* KEY_CONFIGURED    = "configured";

static bool    g_heartbeatEnabled = true;
static char    g_timezone[32]     = "GMT0";

bool configGetHeartbeatEnabled() { 
    bool en = true;
    if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        en = g_heartbeatEnabled;
        xSemaphoreGive(configMutex);
    }
    return en;
}

void configSetHeartbeatEnabled(bool en) { 
    if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (g_heartbeatEnabled != en) {
            g_heartbeatEnabled = en; 
            dirtyHeartbeat = true;
        }
        xSemaphoreGive(configMutex);
    }
}

const char* configGetTimezone() { 
    static char buf[32]; // Return snapshot to prevent use-after-change
    if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(buf, g_timezone, sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';
        xSemaphoreGive(configMutex);
    }
    return buf;
}

void configSetTimezone(const char* tz) { 
    if (tz == nullptr) return;
    if (configMutex && xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (strcmp(g_timezone, tz) != 0) {
            strncpy(g_timezone, tz, sizeof(g_timezone)-1);
            g_timezone[sizeof(g_timezone)-1] = '\0';
            dirtyTimezone = true;
        }
        xSemaphoreGive(configMutex);
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
    if (!configMutex || xSemaphoreTake(configMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    
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
    xSemaphoreGive(configMutex);
}

uint8_t configGetScheduleMode() { return g_schedMode; }
void configSetScheduleMode(uint8_t mode) { g_schedMode = mode; }

void configSaveHeartbeat() {
    prefs.putBool(KEY_HEARTBEAT_EN, g_heartbeatEnabled);
    dirtyHeartbeat = false;
}

void configSaveTimezone() {
    prefs.putString(KEY_TZ, g_timezone);
    dirtyTimezone = false;
}

void configSaveSchedule() {
    // Pack: [mode, armHr0..6, armMin0..6, disarmHr0..6, disarmMin0..6] = 29 bytes
    uint8_t blob[29];
    blob[0] = g_schedMode;
    for (int i = 0; i < 7; i++) {
        blob[1 + i]      = (uint8_t)g_schedArmHr[i];
        blob[8 + i]      = (uint8_t)g_schedArmMin[i];
        blob[15 + i]     = (uint8_t)g_schedDisarmHr[i];
        blob[22 + i]     = (uint8_t)g_schedDisarmMin[i];
    }
    prefs.putBytes("sched", blob, sizeof(blob));
    dirtySchedule = false;
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

    configMutex = xSemaphoreCreateMutex();
    prefs.begin(NVS_NAMESPACE, false);
    LOG_INFO(TAG, "NVS namespace opened with Mutex protection");
}

void configLoad()
{
    if (!prefs.getBool(KEY_CONFIGURED, false)) {
        LOG_INFO(TAG, "No saved configuration — using defaults");
        return;
    }

    LOG_INFO(TAG, "Loading configuration from NVS...");

    // --- Alarm PIN ---
    String pin = prefs.getString(KEY_PIN, "1234");
    alarmSetPin(pin.c_str());

    // --- Alarm timing ---
    alarmSetExitDelay(prefs.getUShort(KEY_EXIT_DELAY, DEFAULT_EXIT_DELAY_S));
    alarmSetEntryDelay(prefs.getUShort(KEY_ENTRY_DELAY, DEFAULT_ENTRY_DELAY_S));
    alarmSetSirenDuration(prefs.getUShort(KEY_SIREN_DUR, DEFAULT_SIREN_DURATION_S));
    alarmSetSirenOutput(prefs.getUChar(KEY_SIREN_CH, 0));

    // --- Periodic Report ---
    smsCmdSetReportInterval(prefs.getUShort(KEY_REPORT_DUR, DEFAULT_REPORT_INTERVAL_MIN));

    // --- Recovery & Mode ---
    smsCmdSetRecoveryText(prefs.getString(KEY_RECOVERY_TXT, "SF_Alarm: All zones restored to normal.").c_str());
    smsCmdSetWorkingMode((WorkingMode)prefs.getUChar(KEY_ALARM_MODE, (uint8_t)MODE_SMS));

    // --- WhatsApp ---
    String waPh = prefs.getString(KEY_WA_PHONE, "");
    String waKey = prefs.getString(KEY_WA_APIKEY, "");
    WhatsAppMode waM = (WhatsAppMode)prefs.getUChar(KEY_WA_MODE, (uint8_t)WA_MODE_SMS);
    whatsappSetConfig(waPh.c_str(), waKey.c_str(), waM);

    // --- MQTT ---
    String mqServer = prefs.getString(KEY_MQTT_SERVER, "");
    uint16_t mqPort = prefs.getUShort(KEY_MQTT_PORT, 1883);
    String mqUser = prefs.getString(KEY_MQTT_USER, "");
    String mqPass = prefs.getString(KEY_MQTT_PASS, "");
    String mqClientId = prefs.getString(KEY_MQTT_CLIENTID, "SF_Alarm");
    mqttSetConfig(mqServer.c_str(), mqPort, mqUser.c_str(), mqPass.c_str(), mqClientId.c_str());

    // --- ONVIF ---
    String ovHost = prefs.getString(KEY_ONVIF_HOST, "");
    uint16_t ovPort = prefs.getUShort(KEY_ONVIF_PORT, 80);
    String ovUser = prefs.getString(KEY_ONVIF_USER, "");
    String ovPass = prefs.getString(KEY_ONVIF_PASS, "");
    uint8_t ovZone = prefs.getUChar(KEY_ONVIF_ZONE, 1);
    onvifSetServer(ovHost.c_str(), ovPort, ovUser.c_str(), ovPass.c_str(), ovZone);

    // --- System Extras ---
    g_heartbeatEnabled = prefs.getBool(KEY_HEARTBEAT_EN, true);
    String tz = prefs.getString(KEY_TZ, "GMT0");
    strncpy(g_timezone, tz.c_str(), sizeof(g_timezone)-1);
    g_timezone[sizeof(g_timezone)-1] = '\0';
    setenv("TZ", g_timezone, 1); // Set POSIX TZ environment var
    tzset();

    // --- Schedule ---
    uint8_t blob[29];
    if (prefs.getBytes("sched", blob, sizeof(blob)) == sizeof(blob)) {
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
    int phoneCnt = prefs.getInt(KEY_PHONE_COUNT, 0);
    for (int i = 0; i < phoneCnt && i < MAX_PHONE_NUMBERS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "phone%d", i);
        String phone = prefs.getString(key, "");
        if (phone.length() > 0) {
            smsCmdAddPhone(phone.c_str());
        }
    }

    // --- WiFi credentials ---
    String wifiSsid = prefs.getString(KEY_WIFI_SSID, "");
    String wifiPassStr = prefs.getString(KEY_WIFI_PASS, "");
    if (wifiSsid.length() > 0) {
        networkSetWifi(wifiSsid.c_str(), wifiPassStr.c_str());
    }

    // --- Router credentials ---
    String routerIp   = prefs.getString(KEY_ROUTER_IP, DEFAULT_ROUTER_IP);
    String routerUser = prefs.getString(KEY_ROUTER_USER, DEFAULT_ROUTER_USER);
    String routerPass = prefs.getString(KEY_ROUTER_PASS, DEFAULT_ROUTER_PASS);
    smsGatewaySetCredentials(routerIp.c_str(), routerUser.c_str(), routerPass.c_str());

    // --- Zone configurations ---
    for (int i = 0; i < MAX_ZONES; i++) {
        char key[16];

        snprintf(key, sizeof(key), "zName%d", i);
        String name = prefs.getString(key, "");
        if (name.length() > 0) {
            ZoneConfig* cfg = zonesGetConfig(i);
            if (cfg) {
                strncpy(cfg->name, name.c_str(), MAX_ZONE_NAME_LEN - 1);
                cfg->name[MAX_ZONE_NAME_LEN - 1] = '\0';

                snprintf(key, sizeof(key), "zType%d", i);
                cfg->type = (ZoneType)prefs.getUChar(key, ZONE_INSTANT);

                snprintf(key, sizeof(key), "zWire%d", i);
                cfg->wiring = (ZoneWiring)prefs.getUChar(key, ZONE_NO);

                snprintf(key, sizeof(key), "zEn%d", i);
                cfg->enabled = prefs.getBool(key, true);
            }
        }

        // Load alarm text
        snprintf(key, sizeof(key), "zTxt%d", i);
        String txt = prefs.getString(key, "");
        if (txt.length() > 0) {
            smsCmdSetAlarmText(i, txt.c_str());
        }
    }

    LOG_INFO(TAG, "Configuration loaded");
}

void configSavePin() {
    prefs.putString(KEY_PIN, alarmGetPin());
    dirtyMain = false;
}

void configSaveTiming() {
    prefs.putUShort(KEY_EXIT_DELAY, alarmGetExitDelay());
    prefs.putUShort(KEY_ENTRY_DELAY, alarmGetEntryDelay());
    prefs.putUShort(KEY_SIREN_DUR, alarmGetSirenDuration());
    prefs.putUChar(KEY_SIREN_CH, alarmGetSirenOutput());
    dirtyMain = false;
}

void configSaveWifi() {
    prefs.putString(KEY_WIFI_SSID, networkGetSsid());
    prefs.putString(KEY_WIFI_PASS, networkGetPass());
    dirtyWifi = false;
}

void configSavePhones() {
    int phoneCnt = smsCmdGetPhoneCount();
    prefs.putInt(KEY_PHONE_COUNT, phoneCnt);
    for (int i = 0; i < phoneCnt; i++) {
        char key[16];
        snprintf(key, sizeof(key), "phone%d", i);
        prefs.putString(key, smsCmdGetPhone(i));
    }
    dirtyAlerts = false;
}

void configSaveRouter() {
    prefs.putString(KEY_ROUTER_IP, smsGatewayGetRouterIp());
    prefs.putString(KEY_ROUTER_USER, smsGatewayGetRouterUser());
    prefs.putString(KEY_ROUTER_PASS, smsGatewayGetRouterPass());
    dirtyRouter = false;
}

void configSaveZones() {
    for (int i = 0; i < MAX_ZONES; i++) {
        const ZoneInfo* info = zonesGetInfo(i);
        if (!info) continue;

        char key[16];
        snprintf(key, sizeof(key), "zName%d", i);
        prefs.putString(key, info->config.name);

        snprintf(key, sizeof(key), "zType%d", i);
        prefs.putUChar(key, (uint8_t)info->config.type);

        snprintf(key, sizeof(key), "zWire%d", i);
        prefs.putUChar(key, (uint8_t)info->config.wiring);

        snprintf(key, sizeof(key), "zEn%d", i);
        prefs.putBool(key, info->config.enabled);

        snprintf(key, sizeof(key), "zTxt%d", i);
        prefs.putString(key, smsCmdGetAlarmText(i));
    }
    dirtyZones = false;
}

void configSavePeriodic() {
    prefs.putUShort(KEY_REPORT_DUR, smsCmdGetReportInterval());
    prefs.putString(KEY_RECOVERY_TXT, smsCmdGetRecoveryText());
    prefs.putUChar(KEY_ALARM_MODE, (uint8_t)smsCmdGetWorkingMode());
    dirtyAlerts = false;
}

void configSaveWhatsapp() {
    prefs.putString(KEY_WA_PHONE, whatsappGetPhone());
    prefs.putString(KEY_WA_APIKEY, whatsappGetApiKey());
    prefs.putUChar(KEY_WA_MODE, (uint8_t)whatsappGetMode());
    dirtyAlerts = false;
}

void configSaveMqtt() {
    prefs.putString(KEY_MQTT_SERVER, mqttGetServer());
    prefs.putUShort(KEY_MQTT_PORT, mqttGetPort());
    prefs.putString(KEY_MQTT_USER, mqttGetUser());
    prefs.putString(KEY_MQTT_PASS, mqttGetPass());
    prefs.putString(KEY_MQTT_CLIENTID, mqttGetClientId());
    dirtyMqtt = false;
}

void configSaveOnvif() {
    prefs.putString(KEY_ONVIF_HOST, onvifGetHost());
    prefs.putUShort(KEY_ONVIF_PORT, onvifGetPort());
    prefs.putString(KEY_ONVIF_USER, onvifGetUser());
    prefs.putString(KEY_ONVIF_PASS, onvifGetPass());
    prefs.putUChar(KEY_ONVIF_ZONE, onvifGetTargetZone());
    dirtyOnvif = false;
}

void configSave()
{
    if (!configMutex || xSemaphoreTake(configMutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        LOG_ERROR(TAG, "Save failed: Mutex timeout");
        return;
    }

    LOG_INFO(TAG, "Executing granular save...");
    
    // Create local copies of flags to minimize mutex hold time
    bool sMain = dirtyMain, sWifi = dirtyWifi, sAlerts = dirtyAlerts, sRouter = dirtyRouter;
    bool sZones = dirtyZones, sMqtt = dirtyMqtt, sOnvif = dirtyOnvif, sHeart = dirtyHeartbeat;
    bool sTz = dirtyTimezone, sSched = dirtySchedule;
    
    // Clear flags while inside mutex
    dirtyMain = dirtyWifi = dirtyAlerts = dirtyRouter = false;
    dirtyZones = dirtyMqtt = dirtyOnvif = dirtyHeartbeat = false;
    dirtyTimezone = dirtySchedule = false;

    xSemaphoreGive(configMutex);

    if (sMain)      { configSavePin(); configSaveTiming(); }
    if (sWifi)      configSaveWifi();
    if (sAlerts)    { configSavePhones(); configSavePeriodic(); configSaveWhatsapp(); }
    if (sRouter)    configSaveRouter();
    if (sZones)     configSaveZones();
    if (sMqtt)      configSaveMqtt();
    if (sOnvif)     configSaveOnvif();
    if (sHeart)     configSaveHeartbeat();
    if (sTz)        configSaveTimezone();
    if (sSched)     configSaveSchedule();

    // Persist "configured" bit LAST to ensure atomic boot integrity
    prefs.putBool(KEY_CONFIGURED, true);

    LOG_INFO(TAG, "Granular save complete");
}

void configFactoryReset()
{
    LOG_INFO(TAG, "Factory reset — clearing NVS...");
    prefs.clear();
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
    Serial.printf("  WA Mode:     %d (1:SMS, 2:WA, 3:Both)\n", (int)whatsappGetMode());
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
