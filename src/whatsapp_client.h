#ifndef SF_ALARM_WHATSAPP_CLIENT_H
#define SF_ALARM_WHATSAPP_CLIENT_H

#include <Arduino.h>

/**
 * @brief WhatsApp alert mode
 */
enum WhatsAppMode {
    WA_MODE_SMS = 1,
    WA_MODE_WHATSAPP = 2,
    WA_MODE_BOTH = 3
};

/**
 * @brief Initialize the WhatsApp client
 */
void whatsappInit();

/**
 * @brief Send a WhatsApp message via CallMeBot gateway
 * 
 * @param phone   The phone number (including country code, e.g., +34600123456)
 * @param apiKey  The CallMeBot API key
 * @param message The message to send
 * @return bool   True if sent successfully
 */
bool whatsappSend(const char* phone, const char* apiKey, const char* message);

/**
 * @brief Set the global WhatsApp configuration
 */
void whatsappSetConfig(const char* phone, const char* apiKey, WhatsAppMode mode);

/**
 * @brief Get the WhatsApp phone number
 */
const char* whatsappGetPhone();

/**
 * @brief Get the WhatsApp API key
 */
const char* whatsappGetApiKey();

/**
 * @brief Get the WhatsApp mode
 */
WhatsAppMode whatsappGetMode();

#endif // SF_ALARM_WHATSAPP_CLIENT_H
