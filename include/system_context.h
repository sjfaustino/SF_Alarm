#ifndef SF_ALARM_SYSTEM_CONTEXT_H
#define SF_ALARM_SYSTEM_CONTEXT_H

#include <Arduino.h>

// Forward declarations
class SmsService;
class NotificationManager;
class AlarmController;
class ConfigManager;
class MqttService;
class TelegramService;
class WhatsappService;
class OnvifService;

/**
 * @brief SystemContext stores references to all shared services,
 * eliminating the "Global State Addiction".
 */
struct SystemContext {
    SmsService*          sms;
    NotificationManager* notificationManager;
    AlarmController*     alarmController;
    ConfigManager*       configManager;
    MqttService*         mqtt;
    TelegramService*     telegram;
    WhatsappService*     whatsapp;
    OnvifService*        onvif;
    
    // System status flags
    volatile uint8_t*    taskHeartbeatBits;
    void*                i2cBusMutex; // SemaphoreHandle_t
};

#endif // SF_ALARM_SYSTEM_CONTEXT_H
