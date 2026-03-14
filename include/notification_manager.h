#ifndef SF_ALARM_NOTIFICATION_MANAGER_H
#define SF_ALARM_NOTIFICATION_MANAGER_H

#include <Arduino.h>
#include "alarm_controller.h"

/**
 * @brief Notification channels (bitmask)
 */
enum AlertChannel {
    CH_NONE = 0x00,
    CH_SMS  = 0x01,
    CH_WA   = 0x02,
    CH_TG   = 0x04
};

/**
 * @brief Notification provider interface
 */
typedef bool (*NotificationSendFunc)(const char* message);

/**
 * @brief Initialize the notification system
 */
void notificationInit();

/**
 * @brief Register a notification provider
 * 
 * @param channel The channel bitmask (e.g., CH_SMS)
 * @param name    Provider name for logging
 * @param send    The function to call to send a broadcast message
 */
void notificationRegisterProvider(AlertChannel channel, const char* name, NotificationSendFunc send);

/**
 * @brief Set the enabled alert channels (bitmask)
 */
void notificationSetChannels(uint8_t channels);

/**
 * @brief Get the currently enabled channels
 */
uint8_t notificationGetChannels();

/**
 * @brief Dispatch an alarm event to all configured channels
 */
void notificationDispatch(const AlarmEventInfo& info);

/**
 * @brief Queue a generic broadcast message
 */
void notificationBroadcast(const char* message);

/**
 * @brief Queue a targeted reply message
 */
void notificationQueueReply(const char* phone, const char* message);

/**
 * @brief Process the internal alert queue (call from task)
 */
void notificationUpdate();

#endif // SF_ALARM_NOTIFICATION_MANAGER_H
