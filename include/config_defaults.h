#ifndef SF_ALARM_CONFIG_DEFAULTS_H
#define SF_ALARM_CONFIG_DEFAULTS_H

// ---------------------------------------------------------------------------
// Default configuration values (Fallback if NVS is empty)
// ---------------------------------------------------------------------------

#define DEFAULT_PIN              "1234"
#define DEFAULT_EXIT_DELAY_S     30
#define DEFAULT_ENTRY_DELAY_S    15
#define DEFAULT_SIREN_DURATION_S 180
#define DEFAULT_SIREN_CH         0

#define DEFAULT_REPORT_INTERVAL_MIN 0
#define DEFAULT_RECOVERY_TEXT    "SF_Alarm: All zones restored to normal."

#define DEFAULT_WIFI_SSID        ""
#define DEFAULT_WIFI_PASS        ""

#define DEFAULT_ROUTER_IP        "192.168.10.1"
#define DEFAULT_ROUTER_USER      "admin"
#define DEFAULT_ROUTER_PASS      "admin"

#ifndef DEFAULT_WA_PHONE
  #define DEFAULT_WA_PHONE         ""
#endif
#ifndef DEFAULT_WA_APIKEY
  #define DEFAULT_WA_APIKEY        ""
#endif

#ifndef DEFAULT_TG_TOKEN
  #define DEFAULT_TG_TOKEN         ""
#endif
#ifndef DEFAULT_TG_CHATID
  #define DEFAULT_TG_CHATID        ""
#endif

#define DEFAULT_MQTT_SERVER      ""
#define DEFAULT_MQTT_PORT        1883
#ifndef DEFAULT_MQTT_USER
  #define DEFAULT_MQTT_USER        ""
#endif
#ifndef DEFAULT_MQTT_PASS
  #define DEFAULT_MQTT_PASS        ""
#endif
#define DEFAULT_MQTT_CLIENTID    "SF_Alarm"

#define DEFAULT_ONVIF_HOST       ""
#define DEFAULT_ONVIF_PORT       80
#define DEFAULT_ONVIF_USER       ""
#define DEFAULT_ONVIF_PASS       ""
#define DEFAULT_ONVIF_ZONE       1

#define DEFAULT_TZ               "GMT0"
#define DEFAULT_HEARTBEAT_EN     true

#endif // SF_ALARM_CONFIG_DEFAULTS_H
