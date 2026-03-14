#include "whatsapp_client.h"
#include <HTTPClient.h>
#include "logging.h"
#include "network.h"
#include "system_context.h"
#include "notification_manager.h"

static const char* TAG = "WA";

WhatsappService* WhatsappService::_instance = nullptr;

WhatsappService::WhatsappService() : _ctx(nullptr), _mutex(NULL) {
    _instance = this;
    memset(_phone, 0, sizeof(_phone));
    memset(_apiKey, 0, sizeof(_apiKey));
}

WhatsappService::~WhatsappService() {
    if (_mutex) vSemaphoreDelete(_mutex);
}

size_t WhatsappService::urlEncodeTo(const char* src, char* dest, size_t destSize) {
    static const char *hexChars = "0123456789ABCDEF";
    size_t d = 0;
    while (*src && (d < destSize - 1)) {
        uint8_t c = (uint8_t)*src++;
        if (isAlphaNumeric(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dest[d++] = (char)c;
        } else {
            if (d + 3 >= destSize) break;
            dest[d++] = '%';
            dest[d++] = hexChars[c >> 4];
            dest[d++] = hexChars[c & 0x0F];
        }
    }
    dest[d] = '\0';
    return d;
}

void WhatsappService::init(SystemContext* ctx) {
    _ctx = ctx;
    if (_mutex == NULL) {
        _mutex = xSemaphoreCreateMutex();
    }
    _ctx->notificationManager->registerProvider(CH_WA, "WhatsApp", WhatsappService::staticSendWrapper);
    LOG_INFO(TAG, "WhatsApp Service initialized");
}

void WhatsappService::setConfig(const char* phone, const char* apiKey) {
    if (_mutex && xSemaphoreTake(_mutex, portMAX_DELAY) == pdTRUE) {
        if (phone) {
            strncpy(_phone, phone, sizeof(_phone) - 1);
            _phone[sizeof(_phone) - 1] = '\0';
        }
        if (apiKey) {
            strncpy(_apiKey, apiKey, sizeof(_apiKey) - 1);
            _apiKey[sizeof(_apiKey) - 1] = '\0';
        }
        xSemaphoreGive(_mutex);
    }
}

bool WhatsappService::staticSendWrapper(const char* message) {
    if (_instance) return _instance->send(message);
    return false;
}

bool WhatsappService::send(const char* message) {
    char phone[32];
    char apiKey[32];
    
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(phone, _phone, sizeof(phone)-1);
        phone[sizeof(phone)-1] = '\0';
        strncpy(apiKey, _apiKey, sizeof(apiKey)-1);
        apiKey[sizeof(apiKey)-1] = '\0';
        xSemaphoreGive(_mutex);
    } else {
        return false;
    }

    return internalSend(phone, apiKey, message);
}

bool WhatsappService::internalSend(const char* phone, const char* apiKey, const char* message) {
    if (strlen(phone) == 0 || strlen(apiKey) == 0) return false;
    if (!networkIsConnected()) return false;

    char encodedMsg[768];
    urlEncodeTo(message, encodedMsg, sizeof(encodedMsg));

    char fullUrl[1024];
    snprintf(fullUrl, sizeof(fullUrl),
             "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s",
             phone, encodedMsg, apiKey);

    HTTPClient http;
    http.begin(fullUrl);
    http.setTimeout(3000);
    
    int code = http.GET();
    bool success = (code == 200);
    if (!success) LOG_ERROR(TAG, "WA Failed (HTTP %d)", code);
    
    http.end();
    return success;
}

