#include "whatsapp_client.h"
#include <HTTPClient.h>
#include <UrlEncode.h>
#include "sms_commands.h"

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
    if (strlen(phone) == 0 || strlen(apiKey) == 0) {
        Serial.println("[WA] Error: WhatsApp credentials not set");
        return false;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WA] Error: WiFi not connected");
        return false;
    }

    HTTPClient http;
    
    // CallMeBot API URL format:
    // https://api.callmebot.com/whatsapp.php?phone=[phone]&text=[text]&apikey=[apikey]
    String url = "https://api.callmebot.com/whatsapp.php?phone=";
    url += phone;
    url += "&text=";
    url += urlEncode(message);
    url += "&apikey=";
    url += apiKey;

    Serial.printf("[WA] Sending alert to %s...\n", phone);
    
    http.begin(url);
    int httpResponseCode = http.GET();
    
    bool success = false;
    if (httpResponseCode > 0) {
        Serial.printf("[WA] HTTP Response code: %d\n", httpResponseCode);
        if (httpResponseCode == 200) success = true;
    } else {
        Serial.printf("[WA] Error code: %s\n", http.errorToString(httpResponseCode).c_str());
    }
    
    http.end();
    return success;
}

void alarmBroadcast(const char* message) {
    // 1. Send via SMS if needed
    if (waMode == WA_MODE_SMS || waMode == WA_MODE_BOTH) {
        smsCmdSendAlert(message);
    }

    // 2. Send via WhatsApp if needed
    if (waMode == WA_MODE_WHATSAPP || waMode == WA_MODE_BOTH) {
        if (strlen(waPhone) > 0 && strlen(waApiKey) > 0) {
            whatsappSend(waPhone, waApiKey, message);
        } else {
            Serial.println("[WA] Skipped WhatsApp (not configured)");
        }
    }
}
