#ifndef SF_ALARM_TELEGRAM_CLIENT_H
#define SF_ALARM_TELEGRAM_CLIENT_H

#include <Arduino.h>
#include "notification_manager.h"

struct SystemContext;
void telegramInit(SystemContext* ctx);

/**
 * @brief Send a Telegram message via Bot API
 * 
 * @param token   The Telegram Bot Token
 * @param chatId  The destination Chat ID
 * @param message The message to send
 * @return bool   True if sent successfully
 */
bool telegramSend(const char* token, const char* chatId, const char* message);

void telegramSetConfig(const char* token, const char* chatId);
bool telegramSendWrapper(const char* message);

/**
 * @brief Get the Telegram Token
 */
const char* telegramGetToken();

/**
 * @brief Get the Telegram Chat ID
 */
const char* telegramGetChatId();

// Channels managed by NotificationManager

#endif // SF_ALARM_TELEGRAM_CLIENT_H
