#include "system_state.h"
#include "network.h"
#include "config.h"
#include "sms_gateway.h"
#include "whatsapp_client.h"
#include "telegram_client.h"
#include "mqtt_client.h"
#include "notification_manager.h"
#include "alarm_zones.h"
#include "io_service.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include "alarm_controller.h"
#include "zone_manager.h"
#include <esp_timer.h>

void StateManager::capture(AlarmController* alarm, ZoneManager* zones, IoService* io, 
                        NotificationManager* nm, MqttService* mqtt, OnvifService* onvif, 
                        SystemSnapshot& snap) {
    // Alarm
    snap.alarmState = alarm->getState();
    snap.alarmStateStr = alarm->getStateStr();
    snap.activeAlarmMask = alarm->getActiveAlarmMask();
    snap.triggeringZone = alarm->getTriggeringZone();
    snap.delayRemaining = alarm->getDelayRemaining();

    // Zones
    for (int i = 0; i < 16; i++) {
        const ZoneInfo* zi = zones->getInfo(i);
        if (zi) {
            strncpy(snap.zones[i].name, zi->config.name, sizeof(snap.zones[i].name) - 1);
            snap.zones[i].type = zi->config.type;
            snap.zones[i].wiring = zi->config.wiring;
            snap.zones[i].enabled = zi->config.enabled;
            snap.zones[i].state = zi->state;
            snap.zones[i].isTriggered = (zi->state == ZONE_TRIGGERED);
            snap.zones[i].isBypassed = (zi->state == ZONE_BYPASSED);
            snap.zones[i].rawInput = zi->rawInput;
        }
    }

    // Hardware
    snap.outputs = io->getOutputs();

    // Notifications
    snap.enabledNotificationChannels = nm->getChannels();

    // Connectivity
    snap.wifiConnected = networkIsConnected();
    snap.rssi = networkGetRSSI();
    snap.mqttConnected = mqtt->isConnected();
    snap.onvifConnected = onvif->isConnected();

    // System
    snap.uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    snap.freeHeap = ESP.getFreeHeap();
}

void StateManager::serialize(const SystemSnapshot& snap, JsonObject& root) {
    JsonObject alarm = root["alarm"].to<JsonObject>();
    alarm["state"] = snap.alarmStateStr;
    alarm["stateCode"] = (int)snap.alarmState;
    alarm["delayRemaining"] = snap.delayRemaining;
    alarm["activeAlarmMask"] = snap.activeAlarmMask;
    alarm["triggeringZone"] = snap.triggeringZone;

    JsonArray zones = root["zones"].to<JsonArray>();
    for (int i = 0; i < 16; i++) {
        JsonObject z = zones.add<JsonObject>();
        z["id"] = i + 1;
        z["name"] = snap.zones[i].name;
        z["type"] = (int)snap.zones[i].type;
        z["wiring"] = snap.zones[i].wiring == ZONE_NC ? "NC" : "NO";
        z["enabled"] = snap.zones[i].enabled;
        z["state"] = (int)snap.zones[i].state;
        z["triggered"] = snap.zones[i].isTriggered;
        z["bypassed"] = snap.zones[i].isBypassed;
        z["rawInput"] = snap.zones[i].rawInput;
    }

    root["outputs"] = snap.outputs;

    JsonObject alerts = root["alerts"].to<JsonObject>();
    alerts["mode"] = (int)snap.enabledNotificationChannels;

    JsonObject net = root["network"].to<JsonObject>();
    net["connected"] = snap.wifiConnected;
    net["rssi"] = snap.rssi;
    net["mqtt"] = snap.mqttConnected;
    net["onvif"] = snap.onvifConnected;

    JsonObject sys = root["system"].to<JsonObject>();
    sys["uptime"] = snap.uptime;
    sys["freeHeap"] = snap.freeHeap;
    sys["version"] = FW_VERSION_STR;
}
