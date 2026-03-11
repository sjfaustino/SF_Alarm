#include "whatsapp_client.h"
#include <HTTPClient.h>
#include "sms_commands.h"
#include "network.h"
#include <esp_task_wdt.h>

// Safe URL encoder that prevents negative sign-extension panics on UTF-8 characters
static String safeUrlEncode(const char *msg) {
    const char *hex = "0123456789ABCDEF";
    String encodedMsg = "";
    encodedMsg.reserve(strlen(msg) * 3); // Prevent C++ String heap fragmentation
    while (*msg != '\0') {
        if ( ('a' <= *msg && *msg <= 'z') || 
             ('A' <= *msg && *msg <= 'Z') || 
             ('0' <= *msg && *msg <= '9') || 
             *msg == '-' || *msg == '_' || *msg == '.' || *msg == '~' ) {
            encodedMsg += *msg;
        } else {
            encodedMsg += '%';
            unsigned char c = (unsigned char)*msg;
            encodedMsg += hex[c >> 4];
            encodedMsg += hex[c & 0x0F];
        }
        msg++;
    }
    return encodedMsg;
}

static char waPhone[32] = "";
static char waApiKey[32] = "";
static WhatsAppMode waMode = WA_MODE_SMS;

void whatsappInit() {
    Serial.println("[WA] WhatsApp client initialized");
}

void whatsappSetConfig(const char* phone, const char* apiKey, WhatsAppMode mode) {
    strncpy(waPhone, phone, sizeof(waPhone) - 1);
    waPhone[sizeof(waPhone) - 1] = '\0';
    
    strncpy(waApiKey, apiKey, sizeof(waApiKey) - 1);
    waApiKey[sizeof(waApiKey) - 1] = '\0';
    
    waMode = mode;
    Serial.printf("[WA] Config updated: Phone=%s, Mode=%d\n", waPhone, (int)waMode);
}

const char* whatsappGetPhone() { return waPhone; }
const char* whatsappGetApiKey() { return waApiKey; }
WhatsAppMode whatsappGetMode() { return waMode; }

bool whatsappSend(const char* phone, const char* apiKey, const char* message) {
    if (phone == nullptr || strlen(phone) == 0 || apiKey == nullptr || strlen(apiKey) == 0) {
        Serial.println("[WA] Error: WhatsApp credentials not set");
        return false;
    }

    if (!networkIsConnected()) {
        Serial.println("[WA] Network DOWN. Skipping WhatsApp alert.");
        return false;
    }

    HTTPClient http;
    
    // Build URL using fixed buffer to avoid String heap fragmentation
    // CallMeBot API: https://api.callmebot.com/whatsapp.php?phone=X&text=Y&apikey=Z
    String encodedMsg = safeUrlEncode(message);
    int urlLen = 60 + strlen(phone) + encodedMsg.length() + strlen(apiKey);
    char* url = (char*)malloc(urlLen + 1);
    if (!url) {
        Serial.println("[WA] Error: URL buffer allocation failed");
        return false;
    }
    snprintf(url, urlLen + 1,
             "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s",
             phone, encodedMsg.c_str(), apiKey);

    Serial.printf("[WA] Sending alert to %s...\n", phone);
    
    http.begin(url);
    http.setTimeout(1500); // Fast-fail limit to prevent netWorkerTask queue throttling
    
    esp_task_wdt_reset(); // Prevent watchdog reboot during alert dispatch
    int httpResponseCode = http.GET();
    
    bool success = false;
    if (httpResponseCode > 0) {
        Serial.printf("[WA] HTTP Response code: %d\n", httpResponseCode);
        if (httpResponseCode == 200) success = true;
    } else {
        Serial.printf("[WA] Error code: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    
    http.end();
    free(url);
    return success;
}
