#include <Arduino.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <esp_task_wdt.h>
#include "config.h"
#include "config_keys.h"
#include "config_defaults.h"
#include "config_utils.h"
#include "logging.h"
#include "config_manager.h"
#include "alarm_controller.h"
#include "zone_manager.h"
#include "io_service.h"
#include "sms_gateway.h"
#include "sms_command_processor.h"
#include "whatsapp_client.h"
#include "notification_manager.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include "telegram_client.h"
#include "phone_authenticator.h"

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #include "secrets_template.h"
#endif
#include "network.h"
#include "string_utils.h"

static const char* TAG = "CFG";
static AlarmController*     _alarm    = nullptr;
static ZoneManager*         _zones    = nullptr;
static IoService*           _io       = nullptr;
static NotificationManager* _nm       = nullptr;
static MqttService*         _mqtt     = nullptr;
static OnvifService*        _onvif    = nullptr;
static PhoneAuthenticator*  _auth     = nullptr;
static SmsCommandProcessor* _smsProc  = nullptr;
static SmsService*          _sms      = nullptr;
static WhatsappService*    _whatsapp = nullptr;
static TelegramService*    _telegram = nullptr;

// Dirty Flags for granular saving (Protected by ConfigUtils::lock)
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
    if (ConfigUtils::lock(50)) {
        flag = true;
        ConfigUtils::unlock();
    }
}

static bool isAnyDirty() {
    bool dirty = false;
    if (ConfigUtils::lock(50)) {
        dirty = dirtyMain || dirtyWifi || dirtyRouter || dirtyZones || dirtyAlerts || 
                dirtyMqtt || dirtyOnvif || dirtySchedule || dirtyHeartbeat || dirtyTimezone;
        ConfigUtils::unlock();
    }
    return dirty;
}

static volatile bool g_heartbeatEnabled = true;
static char           g_timezone[32]     = "GMT0";
static SmsProvider    g_smsProvider      = SMS_LUCI;

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
    return g_heartbeatEnabled;
}

void configSetHeartbeatEnabled(bool en) { 
    if (g_heartbeatEnabled != en) {
        g_heartbeatEnabled = en; 
        setDirty(dirtyHeartbeat);
    }
}

const char* configGetTimezone() { 
    static char buf[32]; // Return snapshot to prevent use-after-change
    if (ConfigUtils::lock(50)) {
        strncpy(buf, g_timezone, sizeof(buf)-1);
        buf[sizeof(buf)-1] = '\0';
        ConfigUtils::unlock();
    }
    return buf;
}

void configSetTimezone(const char* tz) { 
    if (tz == nullptr) return;
    if (ConfigUtils::lock(100)) {
        if (strcmp(g_timezone, tz) != 0) {
            strncpy(g_timezone, tz, sizeof(g_timezone)-1);
            g_timezone[sizeof(g_timezone)-1] = '\0';
            dirtyTimezone = true;
        }
        ConfigUtils::unlock();
    }
}

SmsProvider configGetSmsProvider() { return g_smsProvider; }
void configSetSmsProvider(SmsProvider prov) {
    if (g_smsProvider != prov) {
        g_smsProvider = prov;
        setDirty(dirtyRouter); // SMS provider is grouped with router/network config
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
    if (!ConfigUtils::lock(100)) return;
    
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
    ConfigUtils::unlock();
}

uint8_t configGetScheduleMode() { return g_schedMode; }
void configSetScheduleMode(uint8_t mode) { g_schedMode = mode; }

void configSaveHeartbeat() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        sess.p().putBool(KEY_HEARTBEAT_EN, g_heartbeatEnabled);
        dirtyHeartbeat = false;
    }
}

void configSaveTimezone() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        sess.p().putString(KEY_TZ, g_timezone);
        dirtyTimezone = false;
    }
}

void configSaveSchedule() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        uint8_t blob[29];
        blob[0] = g_schedMode;
        for (int i = 0; i < 7; i++) {
            blob[1 + i]      = (uint8_t)g_schedArmHr[i];
            blob[8 + i]      = (uint8_t)g_schedArmMin[i];
            blob[15 + i]     = (uint8_t)g_schedDisarmHr[i];
            blob[22 + i]     = (uint8_t)g_schedDisarmMin[i];
        }
        sess.p().putBytes("sched", blob, sizeof(blob));
        dirtySchedule = false;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void configInit(AlarmController* alarm, ZoneManager* zones, IoService* io,
               NotificationManager* nm, MqttService* mqtt, OnvifService* onvif,
               PhoneAuthenticator* auth, SmsCommandProcessor* smsProc, SmsService* sms,
               WhatsappService* whatsapp, TelegramService* telegram)
{
    _alarm = alarm;
    _zones = zones;
    _io = io;
    _nm = nm;
    _mqtt = mqtt;
    _onvif = onvif;
    _auth = auth;
    _smsProc = smsProc;
    _sms = sms;
    _whatsapp = whatsapp;
    _telegram = telegram;

    esp_err_t err = nvs_flash_init();
    
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
        for(int i=0; i<300; i++) {
            digitalWrite(HEARTBEAT_LED_PIN, HIGH);
            esp_task_wdt_reset();
            delay(50);
            digitalWrite(HEARTBEAT_LED_PIN, LOW);
            delay(50);
        }
        while(true) {
            digitalWrite(HEARTBEAT_LED_PIN, HIGH);
            delay(50);
            digitalWrite(HEARTBEAT_LED_PIN, LOW);
            delay(50);
        }
    }

    ConfigUtils::init();
    rtcInit();
    LOG_INFO(TAG, "NVS manager initialized with RTC protection");
}

TaskHandle_t configGetLockOwner()
{
    return xSemaphoreGetMutexHolder(xSemaphoreCreateRecursiveMutex()); // Placeholder/Fix: ConfigUtils should expose this if needed
}

void configLoad(AlarmController* alarm, ZoneManager* zones, IoService* io,
               NotificationManager* nm, MqttService* mqtt, OnvifService* onvif,
               PhoneAuthenticator* auth, SmsCommandProcessor* smsProc, SmsService* sms,
               WhatsappService* whatsapp, TelegramService* telegram)
{
    _alarm = alarm;
    _zones = zones;
    _io = io;
    _nm = nm;
    _mqtt = mqtt;
    _onvif = onvif;
    _auth = auth;
    _smsProc = smsProc;
    _sms = sms;
    _whatsapp = whatsapp;
    _telegram = telegram;

    ConfigUtils::Session sess(true);
    if (!sess.isValid()) {
        LOG_ERROR(TAG, "Config load FAILED: Mutex timeout");
        return;
    }

    if (!sess.p().getBool(KEY_CONFIGURED, false)) {
        LOG_INFO(TAG, "No saved configuration — using defaults");
        return;
    }

    LOG_INFO(TAG, "Loading configuration from NVS...");

    Preferences &p = sess.p();

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
    _alarm->loadPin(pin.c_str());

    // --- Alarm timing ---
    _alarm->setExitDelay(p.getUShort(KEY_EXIT_DELAY, DEFAULT_EXIT_DELAY_S));
    _alarm->setEntryDelay(p.getUShort(KEY_ENTRY_DELAY, DEFAULT_ENTRY_DELAY_S));
    _alarm->setSirenDuration(p.getUShort(KEY_SIREN_DUR, DEFAULT_SIREN_DURATION_S));
    _alarm->setSirenOutput(p.getUChar(KEY_SIREN_CH, 0));

    // --- Periodic Report ---
    _smsProc->setReportInterval(p.getUShort(KEY_REPORT_DUR, DEFAULT_REPORT_INTERVAL_MIN));

    // --- Recovery & Mode ---
    _smsProc->setRecoveryText(p.getString(KEY_RECOVERY_TXT, "SF_Alarm: All zones restored to normal.").c_str());
    _smsProc->setWorkingMode((WorkingMode)p.getUChar(KEY_ALARM_MODE, (uint8_t)MODE_SMS));

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
    _nm->setChannels(channels);
    _whatsapp->setConfig(buf, keyBuf);

    // --- Telegram ---
    char tgTok[80];
    len = p.getString(KEY_TG_TOKEN, tgTok, sizeof(tgTok));
    if (len == 0) strncpy(tgTok, DEFAULT_TG_TOKEN, sizeof(tgTok));

    char tgCid[32];
    len = p.getString(KEY_TG_CHATID, tgCid, sizeof(tgCid));
    if (len == 0) strncpy(tgCid, DEFAULT_TG_CHATID, sizeof(tgCid));

    _telegram->setConfig(tgTok, tgCid);

    // --- MQTT ---
    String mqServer = p.getString(KEY_MQTT_SERVER, "");
    uint16_t mqPort = p.getUShort(KEY_MQTT_PORT, 1883);
    String mqUser = p.getString(KEY_MQTT_USER, "");
    String mqPass = p.getString(KEY_MQTT_PASS, "");
    String mqClientId = p.getString(KEY_MQTT_CLIENTID, "SF_Alarm");
    _mqtt->setConfig(mqServer.c_str(), mqPort, mqUser.c_str(), mqPass.c_str(), mqClientId.c_str());

    // --- ONVIF ---
    String ovHost = p.getString(KEY_ONVIF_HOST, "");
    uint16_t ovPort = p.getUShort(KEY_ONVIF_PORT, 80);
    String ovUser = p.getString(KEY_ONVIF_USER, "");
    String ovPass = p.getString(KEY_ONVIF_PASS, "");
    uint8_t ovZone = p.getUChar(KEY_ONVIF_ZONE, 1);
    _onvif->setServer(ovHost.c_str(), ovPort, ovUser.c_str(), ovPass.c_str(), ovZone);


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
    _auth->clearPhones();
    int phoneCnt = p.getInt(KEY_PHONE_COUNT, 0);
    for (int i = 0; i < phoneCnt && i < MAX_PHONE_NUMBERS; i++) {
        char key[16];
        snprintf(key, sizeof(key), "phone%d", i);
        String phone = p.getString(key, "");
        if (phone.length() > 0) {
            _auth->addPhone(phone.c_str());
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
    g_smsProvider = (SmsProvider)p.getUChar(KEY_SMS_PROVIDER, (uint8_t)SMS_LUCI);
    
    _sms->setProvider(g_smsProvider);
    _sms->setCredentials(routerIp.c_str(), routerUser.c_str(), routerPass.c_str());

    // --- Alarm Texts ---
    for (int i = 0; i < MAX_ZONES; i++) {
        char key[16];
        snprintf(key, sizeof(key), "zTxt%d", i);
        String txt = p.getString(key, "");
        if (txt.length() > 0) {
            _smsProc->setAlarmText(i, txt.c_str());
        }
    }

    // Scrub all local string objects from heap/stack
    ConfigUtils::scrubString(pin);
    ConfigUtils::scrubString(mqServer);
    ConfigUtils::scrubString(mqUser);
    ConfigUtils::scrubString(mqPass);
    ConfigUtils::scrubString(mqClientId);
    ConfigUtils::scrubString(ovHost);
    ConfigUtils::scrubString(ovUser);
    ConfigUtils::scrubString(ovPass);
    ConfigUtils::scrubString(tz);
    ConfigUtils::scrubString(wifiSsid);
    ConfigUtils::scrubString(wifiPassStr);
    ConfigUtils::scrubString(routerIp);
    ConfigUtils::scrubString(routerUser);
    ConfigUtils::scrubString(routerPass);

    LOG_INFO(TAG, "Configuration loaded");
}

void configSavePin(const char* pin) {
    if (pin == nullptr || strlen(pin) == 0) return;
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        char safePin[MAX_PIN_LEN]; 
        strncpy(safePin, pin, sizeof(safePin) - 1);
        safePin[sizeof(safePin) - 1] = '\0';
        ConfigUtils::scrubFmt(safePin);

        sess.p().putString("pin_sh", safePin);
        
        uint32_t chk = 0;
        for (int i=0; i<strlen(safePin); i++) chk += safePin[i];
        sess.p().putUInt("pin_chk", chk);

        sess.p().putString(KEY_PIN, safePin);
        dirtyMain = false;
    }
}

void configSaveTiming() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        sess.p().putUShort(KEY_EXIT_DELAY, _alarm->getExitDelay());
        sess.p().putUShort(KEY_ENTRY_DELAY, _alarm->getEntryDelay());
        sess.p().putUShort(KEY_SIREN_DUR, _alarm->getSirenDuration());
        sess.p().putUChar(KEY_SIREN_CH, _alarm->getSirenOutput());
        dirtyMain = false;
    }
}

void configSaveWifi() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        char safeSsid[33];
        strncpy(safeSsid, networkGetSsid(), sizeof(safeSsid) - 1);
        safeSsid[sizeof(safeSsid) - 1] = '\0';
        ConfigUtils::scrubFmt(safeSsid);

        sess.p().putString(KEY_WIFI_SSID, safeSsid);
        sess.p().putString(KEY_WIFI_PASS, networkGetPass());
        memset(safeSsid, 0, sizeof(safeSsid));
        dirtyWifi = false;
    }
}

void configSavePhones() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        int phoneCnt = _auth->getPhoneCount();
        sess.p().putInt(KEY_PHONE_COUNT, phoneCnt);
        for (int i = 0; i < phoneCnt; i++) {
            char key[16];
            snprintf(key, sizeof(key), "phone%d", i);
            sess.p().putString(key, _auth->getPhone(i));
        }
        dirtyAlerts = false;
    }
}

void configSaveRouter() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        char ip[32], user[32], pass[64];
        strncpy(ip, _sms->getRouterIp(), sizeof(ip)-1); ip[sizeof(ip)-1] = '\0';
        strncpy(user, _sms->getRouterUser(), sizeof(user)-1); user[sizeof(user)-1] = '\0';
        strncpy(pass, _sms->getRouterPass(), sizeof(pass)-1); pass[sizeof(pass)-1] = '\0';
        
        sess.p().putString(KEY_ROUTER_IP, ip);
        sess.p().putString(KEY_ROUTER_USER, user);
        sess.p().putString(KEY_ROUTER_PASS, pass);
        sess.p().putUChar(KEY_SMS_PROVIDER, (uint8_t)g_smsProvider);
        
        memset(ip, 0, sizeof(ip));
        memset(user, 0, sizeof(user));
        memset(pass, 0, sizeof(pass));
        dirtyRouter = false;
    }
}

void configSaveZones() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        for (int i = 0; i < MAX_ZONES; i++) {
            const ZoneInfo* info = _zones->getInfo(i);
            if (!info) continue;

            char safeName[MAX_ZONE_NAME_LEN];
            strncpy(safeName, info->config.name, sizeof(safeName) - 1);
            safeName[sizeof(safeName) - 1] = '\0';
            ConfigUtils::scrubFmt(safeName);

            char key[16];
            snprintf(key, sizeof(key), "zName%d", i);
            sess.p().putString(key, safeName);
            snprintf(key, sizeof(key), "zType%d", i);
            sess.p().putUChar(key, (uint8_t)info->config.type);
            snprintf(key, sizeof(key), "zWire%d", i);
            sess.p().putUChar(key, (uint8_t)info->config.wiring);
            snprintf(key, sizeof(key), "zEn%d", i);
            sess.p().putBool(key, info->config.enabled);
            snprintf(key, sizeof(key), "zTxt%d", i);
            sess.p().putString(key, _smsProc->getAlarmText(i));
        }
        dirtyZones = false;
    }
}

void configSavePeriodic() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        sess.p().putUShort(KEY_REPORT_DUR, _smsProc->getReportInterval());
        sess.p().putString(KEY_RECOVERY_TXT, _smsProc->getRecoveryText());
        sess.p().putUChar(KEY_ALARM_MODE, (uint8_t)_smsProc->getWorkingMode());
        dirtyAlerts = false;
    }
}

void configSaveMqtt() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        sess.p().putString(KEY_MQTT_SERVER, _mqtt->getServer());
        sess.p().putUShort(KEY_MQTT_PORT, _mqtt->getPort());
        sess.p().putString(KEY_MQTT_USER, _mqtt->getUser());
        sess.p().putString(KEY_MQTT_PASS, _mqtt->getPass());
        sess.p().putString(KEY_MQTT_CLIENTID, _mqtt->getClientId());
        dirtyMqtt = false;
    }
}

void configSaveOnvif() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        sess.p().putString(KEY_ONVIF_HOST, _onvif->getHost());
        sess.p().putUShort(KEY_ONVIF_PORT, _onvif->getPort());
        sess.p().putString(KEY_ONVIF_USER, _onvif->getUser());
        sess.p().putString(KEY_ONVIF_PASS, _onvif->getPass());
        sess.p().putUChar(KEY_ONVIF_ZONE, _onvif->getTargetZone());
        dirtyOnvif = false;
    }
}

void configSave()
{
    if (!ConfigUtils::lock(500)) {
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

    ConfigUtils::unlock(); // Release early, sub-saves take it back or use sessions

    if (sMain) {
        char pin[MAX_PIN_LEN];
        _alarm->copyPin(pin, sizeof(pin));
        configSavePin(pin);
        ConfigUtils::scrubFmt(pin); // Actually scrubBuffer but let's be consistent
        memset(pin, 0, sizeof(pin));
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

    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        if (!sess.p().getBool(KEY_CONFIGURED, false)) {
            sess.p().putBool(KEY_CONFIGURED, true);
            LOG_INFO(TAG, "NVS: First-time 'configured' bit set.");
        }
    }

    LOG_INFO(TAG, "Granular save complete");
}

static uint32_t lastFlushMs = 0;
const uint32_t CONFIG_FLUSH_INTERVAL_MS = 10000; // 10 seconds background flush

void configTick() {
    if (isAnyDirty()) {
        uint32_t now = millis();
        if (now - lastFlushMs > CONFIG_FLUSH_INTERVAL_MS) {
            lastFlushMs = now;
            configSave();
        }
    }
}

void configSaveAlarmState(AlarmState state)
{
    if (ConfigUtils::lock(100)) {
        // Optimization: Check existing state before writing
        AlarmState current = ALARM_DISARMED;
        {
            ConfigUtils::Session sess(true);
            if (sess.isValid()) {
                current = (AlarmState)sess.p().getUChar("state", (uint8_t)ALARM_DISARMED);
            }
        }

        if (current == state) {
            ConfigUtils::unlock();
            return; 
        }

        ConfigUtils::Session sess(false);
        if (sess.isValid()) {
            sess.p().putUChar("state_sh", (uint8_t)state);
            sess.p().putUChar("state_chk", (uint8_t)(state ^ 0xFF));
            sess.p().putUChar("state", (uint8_t)state);
            LOG_INFO(TAG, "NVS: Alarm state persisted: %d", (int)state);
        }

        if (state == ALARM_DISARMED) {
            configSaveSecurityState(rtc_failedAttempts, rtc_lockedOut);
        }

        ConfigUtils::unlock();
    }
}

AlarmState configLoadAlarmState()
{
    AlarmState state = ALARM_DISARMED;
    ConfigUtils::Session sess(true);
    if (sess.isValid()) {
        uint8_t mainVal = sess.p().getUChar("state", (uint8_t)ALARM_DISARMED);
        uint8_t shVal = sess.p().getUChar("state_sh", 0xFF);
        uint8_t chkVal = sess.p().getUChar("state_chk", 0);
        
        if (shVal == (uint8_t)(chkVal ^ 0xFF)) {
            state = (AlarmState)shVal;
            if (mainVal != shVal) {
                LOG_WARN(TAG, "NVS: Recovered state from shadow copy!");
            }
        } else {
            state = (AlarmState)mainVal;
        }
    }
    return state;
}

void configSaveSecurityState(uint8_t failedAttempts, bool lockedOut)
{
    rtc_failedAttempts = failedAttempts;
    rtc_lockedOut      = lockedOut;

    if (lockedOut || failedAttempts == 0) {
        ConfigUtils::Session sess(false);
        if (sess.isValid()) {
            if (sess.p().getUChar("failedAtts", 0) != failedAttempts || sess.p().getBool("isLocked", false) != lockedOut) {
                sess.p().putUChar("failedAtts", failedAttempts);
                sess.p().putBool("isLocked", lockedOut);
                LOG_INFO(TAG, "NVS: Security state committed.");
            }
        }
    }
}

void configLoadSecurityState(uint8_t &failedAttempts, bool &lockedOut)
{
    // If RTC is valid, use it (Fastest, saves Flash)
    if (ConfigUtils::lock(100)) {
        if (rtc_magic == RTC_CONFIG_MAGIC) {
            failedAttempts = rtc_failedAttempts;
            lockedOut      = rtc_lockedOut;
            ConfigUtils::unlock();
            return;
        }
        ConfigUtils::unlock();
    }

    ConfigUtils::Session sess(true);
    if (sess.isValid()) {
        failedAttempts = sess.p().getUChar("failedAtts", 0);
        lockedOut      = sess.p().getBool("isLocked", false);
        // Populate RTC for next time
        rtc_failedAttempts = failedAttempts;
    }
}
void configUpdateSirenTime(uint32_t seconds)
{
    // Throttled NVS writes to preserve flash longevity
    static uint32_t lastNvsCommit = 0;
    bool forceCommit = (seconds == 0);
    
    if (forceCommit || (millis() - lastNvsCommit >= 300000)) {
        ConfigUtils::Session sess(false);
        if (sess.isValid()) {
            if (sess.p().getUInt("sirenTime", 0xFFFFFFFF) != seconds) {
                sess.p().putUInt("sirenTime", seconds);
                LOG_INFO(TAG, "NVS: Siren active time updated (%u s)", seconds);
            }
            lastNvsCommit = millis();
        }
    }
}


uint32_t configLoadSirenTime()
{
    uint32_t seconds = 0;
    ConfigUtils::Session sess(true);
    if (sess.isValid()) {
        seconds = sess.p().getUInt("sirenTime", 0);
    }
    return seconds;
}

void configSaveWhatsapp() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        sess.p().putString(KEY_WA_PHONE, _whatsapp->getPhone());
        sess.p().putString(KEY_WA_APIKEY, _whatsapp->getApiKey());
        sess.p().putUChar(KEY_ALERT_CHANNELS, _nm->getChannels());
        dirtyAlerts = false; 
    }
}

void configSaveTelegram() {
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        sess.p().putString(KEY_TG_TOKEN, _telegram->getToken());
        sess.p().putString(KEY_TG_CHATID, _telegram->getChatId());
        sess.p().putUChar(KEY_ALERT_CHANNELS, _nm->getChannels());
        dirtyAlerts = false;
    }
}

void configFactoryReset()
{
    LOG_INFO(TAG, "Factory reset — clearing NVS...");
    ConfigUtils::Session sess(false);
    if (sess.isValid()) {
        sess.p().clear();
    }
    LOG_INFO(TAG, "NVS cleared. Restart to apply defaults.");
}

void configPrint()
{
    Serial.println("=== Configuration ===");

    Serial.println("Network:");
    Serial.printf("  WiFi SSID:     %s\n", networkGetSsid());
    Serial.printf("  WiFi Pass:     %s\n", strlen(networkGetPass()) ? "********" : "");
    Serial.printf("  Router IP:     %s\n", _sms->getRouterIp());
    Serial.printf("  Router User:   %s\n", _sms->getRouterUser());
    Serial.printf("  Router Pass:   %s\n", strlen(_sms->getRouterPass()) ? "********" : "");
    Serial.printf("  Exit delay:  %d sec\n", _alarm->getExitDelay());
    Serial.printf("  Entry delay: %d sec\n", _alarm->getEntryDelay());
    Serial.printf("  Siren dur:   %d sec\n", _alarm->getSirenDuration());
    Serial.printf("  Siren ch:    %d\n", _alarm->getSirenOutput());
    Serial.printf("  Report int:  %d min\n", _smsProc->getReportInterval());
    Serial.printf("  Alarm mode:  %d (1:SMS, 2:Call, 3:Both)\n", (int)_smsProc->getWorkingMode());
    Serial.printf("  Recovery:    %s\n", _smsProc->getRecoveryText());
    Serial.printf("  WA Phone:    %s\n", _whatsapp->getPhone());
    Serial.printf("  Alert Chans: 0x%02X\n", _nm->getChannels());
    Serial.printf("  TG ChatID:   %s\n", _telegram->getChatId());
    Serial.printf("  MQTT Server: %s:%d\n", _mqtt->getServer(), _mqtt->getPort());
    Serial.printf("  MQTT User:   %s\n", _mqtt->getUser());
    Serial.printf("  ONVIF Cam:   %s:%d (Zone %d)\n", _onvif->getHost(), _onvif->getPort(), (int)_onvif->getTargetZone());
    Serial.printf("  Heartbeat:   %s\n", g_heartbeatEnabled ? "ON" : "OFF");
    Serial.printf("  Timezone:    %s\n", g_timezone);
    Serial.printf("  Auto-Arm:    %s Mode\n", g_schedMode == 3 ? "HOME" : "AWAY");
    for (int i=0; i<7; i++) {
        Serial.printf("    Day %d: Arm %02d:%02d, Disarm %02d:%02d\n", 
            i, g_schedArmHr[i], g_schedArmMin[i], g_schedDisarmHr[i], g_schedDisarmMin[i]);
    }

    int phoneCnt = _auth->getPhoneCount();
    Serial.printf("  Phones (%d):\n", phoneCnt);
    for (int i = 0; i < phoneCnt; i++) {
        Serial.printf("    [%d] %s\n", i, _auth->getPhone(i));
    }

    Serial.println("=====================");
}
