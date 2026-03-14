#ifndef SF_ALARM_WHATSAPP_CLIENT_H
#define SF_ALARM_WHATSAPP_CLIENT_H

#include <Arduino.h>

/**
 * @brief WhatsApp alert mode
 */
#include "notification_manager.h"

struct SystemContext;
void whatsappInit(SystemContext* ctx);

/**
 * @brief Send a WhatsApp message via CallMeBot gateway
 * 
 * @param phone   The phone number (including country code, e.g., +34600123456)
 * @param apiKey  The CallMeBot API key
 * @param message The message to send
 * @return bool   True if sent successfully
 */
bool whatsappSend(const char* phone, const char* apiKey, const char* message);

void whatsappSetConfig(const char* phone, const char* apiKey);
bool whatsappSendWrapper(const char* message);

/**
 * @brief Get the WhatsApp phone number
 */
const char* whatsappGetPhone();

/**
 * @brief Get the WhatsApp API key
 */
const char* whatsappGetApiKey();

// Channels are now managed by NotificationManager

#endif // SF_ALARM_WHATSAPP_CLIENT_H
