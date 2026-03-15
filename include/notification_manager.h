#ifndef SF_ALARM_NOTIFICATION_MANAGER_H
#define SF_ALARM_NOTIFICATION_MANAGER_H

#include <Arduino.h>
#include "alarm_controller.h"

#include "notification_provider.h"


/**
 * @brief Notification channels (bitmask)
 * Used to filter which providers are enabled for broadcasts.
 */
enum AlertChannel {
    CH_NONE = 0x00,
    CH_SMS  = 0x01,
    CH_WA   = 0x02,
    CH_TG   = 0x04,
    CH_ALL  = 0xFF
};

struct ProviderEntry {
    AlertChannel channel;
    NotificationProvider* provider;
};

struct PendingAlert {
    char message[160];
    char targetPhone[24]; 
};

/**
 * @brief Manages dispatching alerts through various providers.
 */
class NotificationManager {
public:
    NotificationManager();
    ~NotificationManager();

    /**
     * @brief Initialize the notification system
     */
    void init();

    /**
     * @brief Register a notification provider
     */
    void registerProvider(AlertChannel channel, NotificationProvider* provider);

    /**
     * @brief Set the enabled alert channels (bitmask)
     */
    void setChannels(uint8_t channels);

    /**
     * @brief Get the currently enabled channels
     */
    uint8_t getChannels();

    /**
     * @brief Dispatch an alarm event to all configured channels
     */
    void dispatch(const AlarmEventInfo& info);

    /**
     * @brief Queue a generic broadcast message
     */
    void broadcast(const char* message);

    /**
     * @brief Queue a targeted reply message
     */
    void queueReply(const char* phone, const char* message);

    /**
     * @brief Process the internal alert queue (call from task)
     */
    void update();

private:

    void* _alertQueue; // Internal QueueHandle_t
    uint32_t _lastAlertProcessedMs;
    uint32_t _lastZoneAlertMs[16];
    uint8_t _enabledChannels;
    ProviderEntry _providers[8];
    int _providerCount;
};

#endif // SF_ALARM_NOTIFICATION_MANAGER_H
