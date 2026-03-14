#include "notification_manager.h"
#include "config.h"
#include "logging.h"
#include "sms_gateway.h"
#include "sms_commands.h"
#include "whatsapp_client.h"
#include "telegram_client.h"
#include "mqtt_client.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

static const char* TAG = "NOTIF";

struct PendingAlert {
    char message[160];
    char targetPhone[24]; 
};

static QueueHandle_t alertQueue = NULL;
static uint32_t lastAlertProcessedMs = 0;
static const uint32_t ALERT_PROCESS_INTERVAL_MS = 1000;
static uint32_t lastZoneAlertMs[16] = {0};
static uint8_t enabledChannels = CH_NONE;

struct ProviderEntry {
    AlertChannel channel;
    const char* name;
    NotificationSendFunc send;
};

static ProviderEntry providers[8]; // Room for future proofing
static int providerCount = 0;

void notificationInit() {
    alertQueue = xQueueCreate(10, sizeof(PendingAlert));
    if (!alertQueue) {
        LOG_ERROR(TAG, "Failed to create alert queue");
    }
    providerCount = 0;
    enabledChannels = CH_NONE;
}

void notificationRegisterProvider(AlertChannel channel, const char* name, NotificationSendFunc send) {
    if (providerCount < 8) {
        providers[providerCount++] = {channel, name, send};
        LOG_INFO(TAG, "Registered provider: %s (0x%02X)", name, (uint8_t)channel);
    }
}

void notificationSetChannels(uint8_t channels) {
    enabledChannels = channels;
}

uint8_t notificationGetChannels() {
    return enabledChannels;
}

void notificationDispatch(const AlarmEventInfo& info) {
    char msg[160];
    const char* details = info.details ? info.details : "";

    // 1. Handle Throttling & Formatting
    switch (info.event) {
        case EVT_ALARM_TRIGGERED:
            if (info.zoneId >= 0 && info.zoneId < 16) {
                if (millis() - lastZoneAlertMs[info.zoneId] < 60000) {
                    LOG_INFO(TAG, "Throttling redundant alert for Zone %d", info.zoneId + 1);
                    return;
                }
                lastZoneAlertMs[info.zoneId] = millis();
            }
            snprintf(msg, sizeof(msg), "SF_Alarm ALERT: %s", details);
            notificationBroadcast(msg);
            break;

        case EVT_ARMED_AWAY: notificationBroadcast("SF_Alarm: System ARMED (Away)"); break;
        case EVT_ARMED_HOME: notificationBroadcast("SF_Alarm: System ARMED (Home)"); break;
        case EVT_DISARMED:   notificationBroadcast("SF_Alarm: System DISARMED"); break;
        case EVT_TAMPER:
            snprintf(msg, sizeof(msg), "SF_Alarm TAMPER: %s", details);
            notificationBroadcast(msg);
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

void notificationBroadcast(const char* message) {
    if (!alertQueue) return;
    PendingAlert alert;
    memset(&alert, 0, sizeof(alert));
    strncpy(alert.message, message, sizeof(alert.message) - 1);
    alert.targetPhone[0] = '\0';
    
    if (xQueueSend(alertQueue, &alert, 0) != pdTRUE) {
        LOG_ERROR(TAG, "Alert queue full!");
    }
}

void notificationQueueReply(const char* phone, const char* message) {
    if (!alertQueue) return;
    PendingAlert alert;
    memset(&alert, 0, sizeof(alert));
    strncpy(alert.message, message, sizeof(alert.message) - 1);
    strncpy(alert.targetPhone, phone, sizeof(alert.targetPhone) - 1);

    if (xQueueSend(alertQueue, &alert, 0) != pdTRUE) {
        LOG_ERROR(TAG, "Alert queue full!");
    }
}

void notificationUpdate() {
    uint32_t now = millis();
    if (now - lastAlertProcessedMs < ALERT_PROCESS_INTERVAL_MS) return;
    if (!alertQueue) return;

    PendingAlert alert;
    if (xQueueReceive(alertQueue, &alert, 0) == pdTRUE) {
        lastAlertProcessedMs = now;
        
        if (strlen(alert.targetPhone) > 0) {
            smsGatewaySend(alert.targetPhone, alert.message);
        } else {
            for (int i = 0; i < providerCount; i++) {
                if (enabledChannels & providers[i].channel) {
                    if (providers[i].send) {
                        providers[i].send(alert.message);
                    }
                }
            }
        }
    }
}
