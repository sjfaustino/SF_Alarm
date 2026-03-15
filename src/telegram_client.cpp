#include "telegram_client.h"
#include <HTTPClient.h>
#include "logging.h"
#include "network.h"
#include "notification_manager.h"

static const char* TAG = "TG";

TelegramService* TelegramService::_instance = nullptr;

TelegramService::TelegramService() : _nm(nullptr), _mutex(NULL) {
    _instance = this;
    memset(_token, 0, sizeof(_token));
    memset(_chatId, 0, sizeof(_chatId));
}

TelegramService::~TelegramService() {
    if (_mutex) vSemaphoreDelete(_mutex);
}

void TelegramService::init(NotificationManager* nm) {
    _nm = nm;
    if (_mutex == NULL) {
        _mutex = xSemaphoreCreateMutex();
    }
    _nm->registerProvider(CH_TG, this);
    LOG_INFO(TAG, "Telegram Service initialized");
}

void TelegramService::setConfig(const char* token, const char* chatId) {
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (token) {
            strncpy(_token, token, sizeof(_token) - 1);
            _token[sizeof(_token) - 1] = '\0';
        }
        if (chatId) {
            strncpy(_chatId, chatId, sizeof(_chatId) - 1);
            _chatId[sizeof(_chatId) - 1] = '\0';
        }
        xSemaphoreGive(_mutex);
    }
}

bool TelegramService::send(const char* target, const char* message) {
    char token[64];
    char chatId[32];
    
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(token, _token, sizeof(token)-1);
        token[sizeof(token)-1] = '\0';
        if (target && strlen(target) > 0) {
            strncpy(chatId, target, sizeof(chatId)-1);
        } else {
            strncpy(chatId, _chatId, sizeof(chatId)-1);
        }
        chatId[sizeof(chatId)-1] = '\0';
        xSemaphoreGive(_mutex);
    } else {
        return false;
    }

    return internalSend(token, chatId, message);
}

bool TelegramService::send(const char* message) {
    return send(nullptr, message);
}

bool TelegramService::internalSend(const char* token, const char* chatId, const char* message) {
    if (strlen(token) == 0 || strlen(chatId) == 0) return false;
    if (!networkIsConnected()) return false;

    char encodedMsg[1024];
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

    char fullUrl[1024];
    snprintf(fullUrl, sizeof(fullUrl),
             "https://api.telegram.org/bot%s/sendMessage?chat_id=%s&text=%s",
             token, chatId, encodedMsg);

    HTTPClient http;
    http.begin(fullUrl);
    http.setTimeout(5000);
    
    int code = http.GET();
    bool success = (code == 200);
    if (!success) LOG_ERROR(TAG, "TG Failed (HTTP %d)", code);
    
    http.end();
    return success;
}
