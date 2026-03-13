#include "whatsapp_client.h"
#include <HTTPClient.h>
#include "logging.h"
#include "network.h"

static const char* TAG = "WA";

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
static WhatsAppMode waMode = WA_MODE_SMS;

void whatsappInit() {
    LOG_INFO(TAG, "WhatsApp client initialized");
}

void whatsappSetConfig(const char* phone, const char* apiKey, WhatsAppMode mode) {
    if (phone) {
        strncpy(waPhone, phone, sizeof(waPhone) - 1);
        waPhone[sizeof(waPhone) - 1] = '\0';
    }
    if (apiKey) {
        strncpy(waApiKey, apiKey, sizeof(waApiKey) - 1);
        waApiKey[sizeof(waApiKey) - 1] = '\0';
    }
    waMode = mode;
    LOG_INFO(TAG, "Config updated: Phone=%s, Mode=%d", waPhone, (int)waMode);
}

const char* whatsappGetPhone() { return waPhone; }
const char* whatsappGetApiKey() { return waApiKey; }
WhatsAppMode whatsappGetMode() { return waMode; }

bool whatsappSend(const char* phone, const char* apiKey, const char* message) {
    if (!phone || strlen(phone) == 0 || !apiKey || strlen(apiKey) == 0) {
        LOG_ERROR(TAG, "Credentials not set");
        return false;
    }

    if (!networkIsConnected()) {
        LOG_WARN(TAG, "Network DOWN. Skipping alert.");
        return false;
    }

    // Stack-allocated buffers (Zero-Heap architecture)
    char url[768];
    char encodedMsg[512];
    
    urlEncodeTo(message, encodedMsg, sizeof(encodedMsg));
    
    snprintf(url, sizeof(url), 
             "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s",
             phone, encodedMsg, apiKey);

    LOG_INFO(TAG, "Dispatching alert to %s...", phone);
    
    HTTPClient http;
    http.begin(url);
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

