#ifndef SF_ALARM_CONFIG_KEYS_H
#define SF_ALARM_CONFIG_KEYS_H

// ---------------------------------------------------------------------------
// NVS Namespace
// ---------------------------------------------------------------------------
#define NVS_NAMESPACE "sf_alarm"

// ---------------------------------------------------------------------------
// Config Section Keys
// ---------------------------------------------------------------------------
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
static const char* KEY_ALERT_CHANNELS = "alertChans";
static const char* KEY_WA_PHONE       = "waPhone";
static const char* KEY_WA_APIKEY      = "waApiKey";
static const char* KEY_TG_TOKEN       = "tgToken";
static const char* KEY_TG_CHATID      = "tgChatId";
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
static const char* KEY_SMS_PROVIDER  = "smsProv";

#endif // SF_ALARM_CONFIG_KEYS_H
