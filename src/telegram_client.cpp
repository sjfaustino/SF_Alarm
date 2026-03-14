#include "telegram_client.h"
#include <HTTPClient.h>
#include "logging.h"
#include "network.h"

static const char* TAG = "TG";

static char tgToken[64] = "";
static char tgChatId[32] = "";
static SemaphoreHandle_t tgMutex = NULL;

void telegramInit() {
    if (tgMutex == NULL) {
        tgMutex = xSemaphoreCreateMutex();
    }
    notificationRegisterProvider(CH_TG, "Telegram", telegramSendWrapper);
    LOG_INFO(TAG, "Telegram client initialized");
}

void telegramSetConfig(const char* token, const char* chatId) {
    if (tgMutex && xSemaphoreTake(tgMutex, portMAX_DELAY) == pdTRUE) {
        if (token) {
            strncpy(tgToken, token, sizeof(tgToken) - 1);
            tgToken[sizeof(tgToken) - 1] = '\0';
        }
        if (chatId) {
            strncpy(tgChatId, chatId, sizeof(tgChatId) - 1);
            tgChatId[sizeof(tgChatId) - 1] = '\0';
        }
        xSemaphoreGive(tgMutex);
    }
    LOG_INFO(TAG, "Config updated: ChatID=%s", tgChatId);
}

const char* telegramGetToken() { return tgToken; }
const char* telegramGetChatId() { return tgChatId; }
bool telegramSendWrapper(const char* message) {
    return telegramSend(tgToken, tgChatId, message);
}

bool telegramSend(const char* token, const char* chatId, const char* message) {
    if (!token || strlen(token) == 0 || !chatId || strlen(chatId) == 0) {
        LOG_ERROR(TAG, "Credentials not set");
        return false;
    }

    if (!networkIsConnected()) {
        LOG_WARN(TAG, "Network DOWN. Skipping TG alert.");
        return false;
    }

    // Telegram Bot API URL: https://api.telegram.org/bot<token>/sendMessage
    char fullUrl[1024];
    
    // We need to URL encode the message properly. 
    // We'll use a simple approach for now, assuming string_utils or similar.
    // However, for consistency with whatsapp_client, let's look at its encoder.
    
    char encodedMsg[1024];
    // Re-implementing a simple URL encoder here if not available globally
    // Actually, I should probably expose the one from whatsapp_client if it's good.
    // For now, let's just use the one from whatsapp_client if I can find it in a common place.
    // It's static in whatsapp_client.cpp. I'll just copy it or move it to string_utils.h.
    
    // Let's just implement a quick one for TG.
    static const char *hexChars = "0123456789ABCDEF";
    size_t d = 0;
    const char* src = message;
    while (*src && (d < sizeof(encodedMsg) - 4)) {
        uint8_t c = (uint8_t)*src++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encodedMsg[d++] = (char)c;
        } else {
            encodedMsg[d++] = '%';
            encodedMsg[d++] = hexChars[c >> 4];
            encodedMsg[d++] = hexChars[c & 0x0F];
        }
    }
    encodedMsg[d] = '\0';

    snprintf(fullUrl, sizeof(fullUrl),
             "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s",
             token, chatId, encodedMsg);

    HTTPClient http;
    http.begin(fullUrl);
    http.setTimeout(5000); // 5s timeout for Telegram
    
    int code = http.GET();
    
    bool success = false;
    if (code == 200) {
        LOG_INFO(TAG, "Telegram alert delivered");
        success = true;
    } else {
        LOG_ERROR(TAG, "TG Failed (HTTP %d)", code);
    }
    
    http.end();
    return success;
}
