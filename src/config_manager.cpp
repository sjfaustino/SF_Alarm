#include "config_manager.h"
#include "config.h"
#include "alarm_controller.h"
#include "alarm_zones.h"
#include "sms_gateway.h"
#include "sms_commands.h"
#include "whatsapp_client.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include "network.h"
#include <Preferences.h>
#include <nvs_flash.h>

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static Preferences prefs;

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

// ---------------------------------------------------------------------------
// Global config state mapping
// ---------------------------------------------------------------------------
static bool g_heartbeatEnabled = true;
static String g_timezone = "GMT0";

bool configGetHeartbeatEnabled() { return g_heartbeatEnabled; }
void configSetHeartbeatEnabled(bool en) { g_heartbeatEnabled = en; }

const char* configGetTimezone() { return g_timezone.c_str(); }
void configSetTimezone(const char* tz) { g_timezone = tz; }

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
    g_schedArmHr[dayOfWeek] = armHr;
    g_schedArmMin[dayOfWeek] = armMin;
    g_schedDisarmHr[dayOfWeek] = disarmHr;
    g_schedDisarmMin[dayOfWeek] = disarmMin;
}

uint8_t configGetScheduleMode() { return g_schedMode; }
void configSetScheduleMode(uint8_t mode) { g_schedMode = mode; }

void configSaveHeartbeat() {
    prefs.putBool(KEY_HEARTBEAT_EN, g_heartbeatEnabled);
}

void configSaveTimezone() {
    prefs.putString(KEY_TZ, g_timezone);
}

void configSaveSchedule() {
    prefs.putUChar("schMode", g_schedMode);
    for (int i=0; i<7; i++) {
        char k[16];
        snprintf(k, sizeof(k), "schAH%d", i);
        prefs.putChar(k, g_schedArmHr[i]);
        snprintf(k, sizeof(k), "schAM%d", i);
        prefs.putChar(k, g_schedArmMin[i]);
        snprintf(k, sizeof(k), "schDH%d", i);
        prefs.putChar(k, g_schedDisarmHr[i]);
        snprintf(k, sizeof(k), "schDM%d", i);
        prefs.putChar(k, g_schedDisarmMin[i]);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void configInit()
{
    // --- NVS BROWNOUT DEATH SPIRAL FIX ---
    // Standard prefs.begin() silently panics if the physical flash sector corrupts
    // during a power-loss write failure. We must interact with the raw IDF API
    // to trap the corruption and self-heal the physical partition.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        Serial.println("[CFG] CRITICAL: NVS partition corrupted (brownout)! Executing self-healing erase...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
        Serial.println("[CFG] NVS partition repaired. Rebooting system for clean state.");
        delay(1000);
        ESP.restart();
    }
    ESP_ERROR_CHECK(err);

    prefs.begin(NVS_NAMESPACE, false);
    Serial.println("[CFG] NVS namespace opened securely");
}

void configLoad()
{
    if (!prefs.getBool(KEY_CONFIGURED, false)) {
        Serial.println("[CFG] No saved configuration — using defaults");
        return;
    }

    Serial.println("[CFG] Loading configuration from NVS...");

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
    g_timezone = prefs.getString(KEY_TZ, "GMT0");
    setenv("TZ", g_timezone.c_str(), 1); // Set POSIX TZ environment var
    tzset();

    // --- Schedule ---
    g_schedMode = prefs.getUChar("schMode", 2);
    for (int i=0; i<7; i++) {
        char k[16];
        snprintf(k, sizeof(k), "schAH%d", i);
        g_schedArmHr[i] = prefs.getChar(k, -1);
        snprintf(k, sizeof(k), "schAM%d", i);
        g_schedArmMin[i] = prefs.getChar(k, -1);
        snprintf(k, sizeof(k), "schDH%d", i);
        g_schedDisarmHr[i] = prefs.getChar(k, -1);
        snprintf(k, sizeof(k), "schDM%d", i);
        g_schedDisarmMin[i] = prefs.getChar(k, -1);
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

    Serial.println("[CFG] Configuration loaded");
}

void configSavePin() {
    prefs.putString(KEY_PIN, alarmGetPin());
}

void configSaveTiming() {
    prefs.putUShort(KEY_EXIT_DELAY, alarmGetExitDelay());
    prefs.putUShort(KEY_ENTRY_DELAY, alarmGetEntryDelay());
    prefs.putUShort(KEY_SIREN_DUR, alarmGetSirenDuration());
    prefs.putUChar(KEY_SIREN_CH, alarmGetSirenOutput());
}

void configSaveWifi() {
    prefs.putString(KEY_WIFI_SSID, networkGetSsid());
    prefs.putString(KEY_WIFI_PASS, networkGetPass());
}

void configSavePhones() {
    int phoneCnt = smsCmdGetPhoneCount();
    prefs.putInt(KEY_PHONE_COUNT, phoneCnt);
    for (int i = 0; i < phoneCnt; i++) {
        char key[16];
        snprintf(key, sizeof(key), "phone%d", i);
        prefs.putString(key, smsCmdGetPhone(i));
    }
}

void configSaveRouter() {
    prefs.putString(KEY_ROUTER_IP, smsGatewayGetRouterIp());
    prefs.putString(KEY_ROUTER_USER, smsGatewayGetRouterUser());
    prefs.putString(KEY_ROUTER_PASS, smsGatewayGetRouterPass());
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
}

void configSavePeriodic() {
    prefs.putUShort(KEY_REPORT_DUR, smsCmdGetReportInterval());
    prefs.putString(KEY_RECOVERY_TXT, smsCmdGetRecoveryText());
    prefs.putUChar(KEY_ALARM_MODE, (uint8_t)smsCmdGetWorkingMode());
}

void configSaveWhatsapp() {
    prefs.putString(KEY_WA_PHONE, whatsappGetPhone());
    prefs.putString(KEY_WA_APIKEY, whatsappGetApiKey());
    prefs.putUChar(KEY_WA_MODE, (uint8_t)whatsappGetMode());
}

void configSaveMqtt() {
    prefs.putString(KEY_MQTT_SERVER, mqttGetServer());
    prefs.putUShort(KEY_MQTT_PORT, mqttGetPort());
    prefs.putString(KEY_MQTT_USER, mqttGetUser());
    prefs.putString(KEY_MQTT_PASS, mqttGetPass());
    prefs.putString(KEY_MQTT_CLIENTID, mqttGetClientId());
}

void configSaveOnvif() {
    prefs.putString(KEY_ONVIF_HOST, onvifGetHost());
    prefs.putUShort(KEY_ONVIF_PORT, onvifGetPort());
    prefs.putString(KEY_ONVIF_USER, onvifGetUser());
    prefs.putString(KEY_ONVIF_PASS, onvifGetPass());
    prefs.putUChar(KEY_ONVIF_ZONE, onvifGetTargetZone());
}

void configSave()
{
    Serial.println("[CFG] Saving full configuration to NVS...");
    prefs.putBool(KEY_CONFIGURED, true);
    
    configSavePin();
    configSaveTiming();
    configSaveWifi();
    configSavePhones();
    configSaveRouter();
    configSaveZones();
    configSavePeriodic();
    configSaveWhatsapp();
    configSaveMqtt();
    configSaveOnvif();
    configSaveHeartbeat();
    configSaveTimezone();
    configSaveSchedule();

    Serial.println("[CFG] Full configuration saved");
}

void configFactoryReset()
{
    Serial.println("[CFG] Factory reset — clearing NVS...");
    prefs.clear();
    Serial.println("[CFG] NVS cleared. Restart to apply defaults.");
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
    Serial.printf("  Timezone:    %s\n", g_timezone.c_str());
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
