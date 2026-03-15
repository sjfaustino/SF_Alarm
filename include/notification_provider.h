#ifndef SF_ALARM_NOTIFICATION_PROVIDER_H
#define SF_ALARM_NOTIFICATION_PROVIDER_H

#include <Arduino.h>

/**
 * Interface for notification providers (SMS, WhatsApp, Telegram, MQTT, etc.)
 */
class NotificationProvider {
public:
    virtual ~NotificationProvider() {}

    /**
     * Get the unique name of the provider.
     */
    virtual const char* getName() const = 0;

    /**
     * Send a notification.
     * @param target The recipient (phone number, chat ID, topic, etc.)
     * @param message The message content.
     * @return true if successfully queued/sent.
     */
    virtual bool send(const char* target, const char* message) = 0;

    /**
     * Check if the provider is currently available/online.
     */
    virtual bool isReady() const = 0;
};

#endif // SF_ALARM_NOTIFICATION_PROVIDER_H
