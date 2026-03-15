#ifndef SF_ALARM_SYSTEM_STATE_H
#define SF_ALARM_SYSTEM_STATE_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include "alarm_controller.h"
#include "zone_manager.h"

class IoService;
class NotificationManager;
class MqttService;
class OnvifService;

/**
 * @brief A snapshot of the entire system state for reporting (Web, MQTT, etc.)
 */
struct SystemSnapshot {
    // Alarm
    AlarmState alarmState;
    const char* alarmStateStr;
    uint16_t activeAlarmMask;
    uint8_t triggeringZone;
    uint32_t delayRemaining;

    // Zones
    struct ZoneSnapshot {
        char name[32];
        ZoneType type;
        ZoneWiring wiring;
        bool enabled;
        ZoneState state;
        bool isTriggered;
        bool isBypassed;
        bool rawInput;
    } zones[16];

    // Hardware
    uint16_t outputs;

    // Notifications
    uint8_t enabledNotificationChannels;

    // Connectivity
    bool wifiConnected;
    int rssi;
    bool mqttConnected;
    bool onvifConnected;

    // System
    uint32_t uptime;
    uint32_t freeHeap;
};

/**
 * @brief Helper to populate a snapshot and serialize it.
 */
class StateManager {
public:
    static void capture(AlarmController* alarm, ZoneManager* zones, IoService* io, 
                        NotificationManager* nm, MqttService* mqtt, OnvifService* onvif, 
                        SystemSnapshot& snap);
    static void serialize(const SystemSnapshot& snap, JsonObject& root);
};

#endif // SF_ALARM_SYSTEM_STATE_H
