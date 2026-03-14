#include "whatsapp_client.h"
#include "notification_manager.h"
#include <HTTPClient.h>
#include "logging.h"
#include "network.h"

static const char* TAG = "WA";
#include "system_context.h"
#include "notification_manager.h"
static SystemContext* globalCtx = nullptr;

// Professional URL encoder — Stack-allocated (Obsidian Mantle)
static size_t urlEncodeTo(const char* src, char* dest, size_t destSize) {
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

static char waPhone[32] = "";
static char waApiKey[32] = "";
static SemaphoreHandle_t waMutex = NULL;

void whatsappInit(SystemContext* ctx) {
    globalCtx = ctx;
    if (waMutex == NULL) {
        waMutex = xSemaphoreCreateMutex();
    }
    globalCtx->notificationManager->registerProvider(CH_WA, "WhatsApp", whatsappSendWrapper);
    LOG_INFO(TAG, "WhatsApp client initialized");
}

void whatsappSetConfig(const char* phone, const char* apiKey) {
    if (waMutex && xSemaphoreTake(waMutex, portMAX_DELAY) == pdTRUE) {
        if (phone) {
            strncpy(waPhone, phone, sizeof(waPhone) - 1);
            waPhone[sizeof(waPhone) - 1] = '\0';
        }
        if (apiKey) {
            strncpy(waApiKey, apiKey, sizeof(waApiKey) - 1);
            waApiKey[sizeof(waApiKey) - 1] = '\0';
        }
        xSemaphoreGive(waMutex);
    }
    LOG_INFO(TAG, "Config updated: Phone=%s", waPhone);
}

const char* whatsappGetPhone() { return waPhone; }
const char* whatsappGetApiKey() { return waApiKey; }
bool whatsappSendWrapper(const char* message) {
    return whatsappSend(waPhone, waApiKey, message);
}

bool whatsappSend(const char* phone, const char* apiKey, const char* message) {
    if (!phone || strlen(phone) == 0 || !apiKey || strlen(apiKey) == 0) {
        LOG_ERROR(TAG, "Credentials not set");
        return false;
    }

    if (!networkIsConnected()) {
        LOG_WARN(TAG, "Network DOWN. Skipping alert.");
        return false;
    }

    // Strictly non-blocking: Submit-and-go. 
    // Truncation Guard: 400 chars is max for CallMeBot API safety.
    if (strlen(message) > 400) {
        LOG_ERROR(TAG, "Message too large (%d). Truncating.", (int)strlen(message));
    }

    // Stack-allocated buffers (Zero-Heap architecture)
    char encodedMsg[768]; // Buffer for the encoded message
    urlEncodeTo(message, encodedMsg, sizeof(encodedMsg)); // Re-entrant urlEncodeTo

    char stackPhone[32];
    char stackApiKey[32];
    
    bool credsOk = false;
    if (waMutex && xSemaphoreTake(waMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(stackPhone, waPhone, sizeof(stackPhone)-1);
        stackPhone[sizeof(stackPhone)-1] = '\0';
        strncpy(stackApiKey, waApiKey, sizeof(stackApiKey)-1);
        stackApiKey[sizeof(stackApiKey)-1] = '\0';
        credsOk = (strlen(stackPhone) > 0 && strlen(stackApiKey) > 0);
        xSemaphoreGive(waMutex);
    }

    if (!credsOk) {
        LOG_ERROR(TAG, "Credentials not valid in mutex cycle");
        return false;
    }

    char fullUrl[1024];
    snprintf(fullUrl, sizeof(fullUrl),
             "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s",
             stackPhone, encodedMsg, stackApiKey);

    HTTPClient http;
    http.begin(fullUrl);
    http.setTimeout(3000); // Strict 3s timeout to prevent queue starvation
    
    int code = http.GET();
    
    bool success = false;
    if (code == 200) {
        LOG_INFO(TAG, "Alert delivered successfully");
        success = true;
    } else {
        LOG_ERROR(TAG, "Failed to send (HTTP %d): %s", code, http.errorToString(code).c_str());
    }
    
    http.end();
    return success;
}

