#ifndef SF_ALARM_TELEGRAM_CLIENT_H
#define SF_ALARM_TELEGRAM_CLIENT_H

#include "whatsapp_client.h"

// AlertChannel bitmask is defined in whatsapp_client.h

/**
 * @brief Initialize the Telegram client
 */
void telegramInit();

/**
 * @brief Send a Telegram message via Bot API
 * 
 * @param token   The Telegram Bot Token
 * @param chatId  The destination Chat ID
 * @param message The message to send
 * @return bool   True if sent successfully
 */
bool telegramSend(const char* token, const char* chatId, const char* message);

/**
 * @brief Set the global Telegram configuration
 */
void telegramSetConfig(const char* token, const char* chatId, uint8_t channels);

/**
 * @brief Get the Telegram Token
 */
const char* telegramGetToken();

/**
 * @brief Get the Telegram Chat ID
 */
const char* telegramGetChatId();

/**
 * @brief Get the Telegram mode
 */
uint8_t telegramGetChannels();

#endif // SF_ALARM_TELEGRAM_CLIENT_H
