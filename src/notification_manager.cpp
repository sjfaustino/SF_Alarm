#include "notification_manager.h"
#include "config.h"
#include "logging.h"
#include "sms_gateway.h"
#include "sms_commands.h"
#include "whatsapp_client.h"
#include "telegram_client.h"
#include "mqtt_client.h"
#include "system_context.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static const char* TAG = "NOTIF";

NotificationManager::NotificationManager()
    : _alertQueue(NULL), _lastAlertProcessedMs(0), _enabledChannels(CH_NONE), _providerCount(0) {
    memset(_lastZoneAlertMs, 0, sizeof(_lastZoneAlertMs));
    memset(_providers, 0, sizeof(_providers));
}

NotificationManager::~NotificationManager() {
    if (_alertQueue) {
        vQueueDelete((QueueHandle_t)_alertQueue);
    }
}

void NotificationManager::init() {
    _alertQueue = xQueueCreate(10, sizeof(PendingAlert));
    if (!_alertQueue) {
        LOG_ERROR(TAG, "Failed to create alert queue");
    }
    _providerCount = 0;
    _enabledChannels = CH_NONE;
}

void NotificationManager::registerProvider(AlertChannel channel, const char* name, NotificationSendFunc send) {
    if (_providerCount < 8) {
        _providers[_providerCount++] = {channel, name, send};
        LOG_INFO(TAG, "Registered provider: %s (0x%02X)", name, (uint8_t)channel);
    }
}

void NotificationManager::setChannels(uint8_t channels) {
    _enabledChannels = channels;
}

uint8_t NotificationManager::getChannels() {
    return _enabledChannels;
}

void NotificationManager::dispatch(const AlarmEventInfo& info, SystemContext* ctx) {
    char msg[160];
    const char* details = info.details ? info.details : "";

    // 1. Handle Throttling & Formatting
    switch (info.event) {
        case EVT_ALARM_TRIGGERED:
            if (info.zoneId >= 0 && info.zoneId < 16) {
                if (millis() - _lastZoneAlertMs[info.zoneId] < 60000) {
                    LOG_INFO(TAG, "Throttling redundant alert for Zone %d", info.zoneId + 1);
                    return;
                }
                _lastZoneAlertMs[info.zoneId] = millis();
            }
            snprintf(msg, sizeof(msg), "SF_Alarm ALERT: %s", details);
            broadcast(msg);
            break;

        case EVT_ARMED_AWAY: broadcast("SF_Alarm: System ARMED (Away)"); break;
        case EVT_ARMED_HOME: broadcast("SF_Alarm: System ARMED (Home)"); break;
        case EVT_DISARMED:   broadcast("SF_Alarm: System DISARMED"); break;
        case EVT_TAMPER:
            snprintf(msg, sizeof(msg), "SF_Alarm TAMPER: %s", details);
            broadcast(msg);
            break;

        default: break;
    }

    // 2. High-latency MQTT events
    switch (info.event) {
        case EVT_ALARM_TRIGGERED:
        case EVT_TAMPER:
        case EVT_ARMED_AWAY:
        case EVT_ARMED_HOME:
        case EVT_DISARMED: {
            char logMsg[128];
            snprintf(logMsg, sizeof(logMsg), "EVENT:%d Z:%d | %s", (int)info.event, info.zoneId, details);
            mqttPublish("SF_Alarm/events", logMsg); 
            break;
        }
        default: break;
    }
}

void NotificationManager::broadcast(const char* message) {
    if (!_alertQueue) return;
    PendingAlert alert;
    memset(&alert, 0, sizeof(alert));
    strncpy(alert.message, message, sizeof(alert.message) - 1);
    alert.targetPhone[0] = '\0';
    
    if (xQueueSend((QueueHandle_t)_alertQueue, &alert, 0) != pdTRUE) {
        LOG_ERROR(TAG, "Alert queue full!");
    }
}

void NotificationManager::queueReply(const char* phone, const char* message) {
    if (!_alertQueue) return;
    PendingAlert alert;
    memset(&alert, 0, sizeof(alert));
    strncpy(alert.message, message, sizeof(alert.message) - 1);
    strncpy(alert.targetPhone, phone, sizeof(alert.targetPhone) - 1);

    if (xQueueSend((QueueHandle_t)_alertQueue, &alert, 0) != pdTRUE) {
        LOG_ERROR(TAG, "Alert queue full!");
    }
}

void NotificationManager::update() {
    uint32_t now = millis();
    if (now - _lastAlertProcessedMs < 1000) return;
    if (!_alertQueue) return;

    PendingAlert alert;
    if (xQueueReceive((QueueHandle_t)_alertQueue, &alert, 0) == pdTRUE) {
        _lastAlertProcessedMs = now;
        
        if (strlen(alert.targetPhone) > 0) {
            smsGatewaySend(alert.targetPhone, alert.message);
        } else {
            for (int i = 0; i < _providerCount; i++) {
                if (_enabledChannels & _providers[i].channel) {
                    if (_providers[i].send) {
                        _providers[i].send(alert.message);
                    }
                }
            }
        }
    }
}

