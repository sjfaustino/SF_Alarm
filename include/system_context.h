#ifndef SF_ALARM_SYSTEM_CONTEXT_H
#define SF_ALARM_SYSTEM_CONTEXT_H

#include <Arduino.h>

// Forward declarations
class ISmsGateway;
class NotificationManager;
class AlarmController;
class ConfigManager;
class MqttClient;

/**
 * @brief SystemContext stores references to all shared services,
 * eliminating the "Global State Addiction".
 */
struct SystemContext {
    ISmsGateway*         smsGateway;
    NotificationManager* notificationManager;
    AlarmController*     alarmController;
    ConfigManager*       configManager;
    MqttClient*          mqttClient;
    
    // System status flags
    volatile uint8_t*    taskHeartbeatBits;
};

#endif // SF_ALARM_SYSTEM_CONTEXT_H
