#include "config_manager.h"
#include "config.h"
#include "alarm_controller.h"
#include "alarm_zones.h"
#include "sms_gateway.h"
#include "sms_commands.h"
#include <Preferences.h>

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
static const char* KEY_CONFIGURED    = "configured";

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void configInit()
{
    prefs.begin(NVS_NAMESPACE, false);
    Serial.println("[CFG] NVS namespace opened");
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

                snprintf(key, sizeof(key), "zType%d", i);
                cfg->type = (ZoneType)prefs.getUChar(key, ZONE_INSTANT);

                snprintf(key, sizeof(key), "zWire%d", i);
                cfg->wiring = (ZoneWiring)prefs.getUChar(key, ZONE_NO);

                snprintf(key, sizeof(key), "zEn%d", i);
                cfg->enabled = prefs.getBool(key, true);
            }
        }
    }

    Serial.println("[CFG] Configuration loaded");
}

void configSave()
{
    Serial.println("[CFG] Saving configuration to NVS...");

    prefs.putBool(KEY_CONFIGURED, true);

    // We don't have direct getters for PIN etc., so the user must set them
    // via CLI before saving. The alarm controller stores them internally.
    // For robust persistence, we save what we can get.

    // --- Phone numbers ---
    int phoneCnt = smsCmdGetPhoneCount();
    prefs.putInt(KEY_PHONE_COUNT, phoneCnt);
    for (int i = 0; i < phoneCnt; i++) {
        char key[16];
        snprintf(key, sizeof(key), "phone%d", i);
        prefs.putString(key, smsCmdGetPhone(i));
    }

    // --- Zone configurations ---
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
    }

    // --- Periodic Report & Recovery ---
    prefs.putUShort(KEY_REPORT_DUR, smsCmdGetReportInterval());
    prefs.putString(KEY_RECOVERY_TXT, smsCmdGetRecoveryText());
    prefs.putUChar(KEY_ALARM_MODE, (uint8_t)smsCmdGetWorkingMode());

    Serial.println("[CFG] Configuration saved");
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

    Serial.printf("  Router IP:   %s\n", prefs.getString(KEY_ROUTER_IP, DEFAULT_ROUTER_IP).c_str());
    Serial.printf("  Router User: %s\n", prefs.getString(KEY_ROUTER_USER, DEFAULT_ROUTER_USER).c_str());
    Serial.printf("  Wi-Fi SSID:  %s\n", prefs.getString(KEY_WIFI_SSID, "").c_str());
    Serial.printf("  Exit delay:  %d sec\n", prefs.getUShort(KEY_EXIT_DELAY, DEFAULT_EXIT_DELAY_S));
    Serial.printf("  Entry delay: %d sec\n", prefs.getUShort(KEY_ENTRY_DELAY, DEFAULT_ENTRY_DELAY_S));
    Serial.printf("  Siren dur:   %d sec\n", prefs.getUShort(KEY_SIREN_DUR, DEFAULT_SIREN_DURATION_S));
    Serial.printf("  Siren ch:    %d\n", prefs.getUChar(KEY_SIREN_CH, 0));
    Serial.printf("  Report int:  %d min\n", smsCmdGetReportInterval());
    Serial.printf("  Alarm mode:  %d (1:SMS, 2:Call, 3:Both)\n", (int)smsCmdGetWorkingMode());
    Serial.printf("  Recovery:    %s\n", smsCmdGetRecoveryText());

    int phoneCnt = smsCmdGetPhoneCount();
    Serial.printf("  Phones (%d):\n", phoneCnt);
    for (int i = 0; i < phoneCnt; i++) {
        Serial.printf("    [%d] %s\n", i, smsCmdGetPhone(i));
    }

    Serial.println("=====================");
}
