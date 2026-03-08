#include "onvif_client.h"
#include "config_manager.h"
#include "alarm_zones.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <mbedtls/sha1.h>
#include <base64.h>

// ---------------------------------------------------------------------------
// Constants & Templates
// ---------------------------------------------------------------------------

static const char* SOAP_ENV_START = 
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:a=\"http://www.w3.org/2005/08/addressing\" "
    "xmlns:e=\"http://www.onvif.org/ver10/events/wsdl\">";

static const char* SOAP_ENV_END = "</s:Envelope>";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------

struct OnvifState {
    char host[64];
    uint16_t port;
    char user[32];
    char pass[32];
    uint8_t targetZone;
    bool connected;
    String subscriptionAddress;
    uint32_t lastPollMs;
    uint32_t lastRenewMs;
};

static OnvifState state = {0};

// ---------------------------------------------------------------------------
// Security Helpers
// ---------------------------------------------------------------------------

static String getTimestamp() {
    // Use millis()-based epoch estimate. Real ONVIF should use NTP-synced time,
    // but many cameras accept approximate timestamps for digest auth.
    // Base: 2026-03-08T00:00:00Z epoch = 1772956800
    unsigned long uptimeSec = millis() / 1000;
    unsigned long epoch = 1772956800UL + uptimeSec;
    
    // Convert epoch to ISO 8601 (simplified — good enough for digest nonce)
    unsigned long days = epoch / 86400;
    unsigned long rem  = epoch % 86400;
    int hours = rem / 3600;
    int mins  = (rem % 3600) / 60;
    int secs  = rem % 60;
    
    // Approximate year/month/day from days since epoch 1970
    int year = 1970;
    while (true) {
        int daysInYear = (year % 4 == 0) ? 366 : 365;
        if ((int)days < daysInYear) break;
        days -= daysInYear;
        year++;
    }
    int monthDays[] = {31,28+(year%4==0?1:0),31,30,31,30,31,31,30,31,30,31};
    int month = 1;
    for (int i = 0; i < 12; i++) {
        if ((int)days < monthDays[i]) break;
        days -= monthDays[i];
        month++;
    }
    int day = days + 1;
    
    char buf[32];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", year, month, day, hours, mins, secs);
    return String(buf);
}

static String generateDigest(const char* nonce, const char* created, const char* password) {
    // nonce is binary here
    unsigned char hash[20];
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    
    // Nonce (binary) + Created (string) + Password (string)
    mbedtls_sha1_update(&ctx, (const unsigned char*)nonce, 20);
    mbedtls_sha1_update(&ctx, (const unsigned char*)created, strlen(created));
    mbedtls_sha1_update(&ctx, (const unsigned char*)password, strlen(password));
    
    mbedtls_sha1_finish(&ctx, hash);
    mbedtls_sha1_free(&ctx);
    
    return base64::encode(hash, 20);
}

static String getAuthHeader() {
    unsigned char rawNonce[20];
    for(int i=0; i<20; i++) rawNonce[i] = (unsigned char)random(256);
    
    String nonceB64 = base64::encode(rawNonce, 20);
    String created = getTimestamp();
    String digest = generateDigest((const char*)rawNonce, created.c_str(), state.pass);

    String header = "<s:Header>";
    header += "<Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">";
    header += "<UsernameToken>";
    header += "<Username>" + String(state.user) + "</Username>";
    header += "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">" + digest + "</Password>";
    header += "<Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">" + nonceB64 + "</Nonce>";
    header += "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">" + created + "</Created>";
    header += "</UsernameToken></Security></s:Header>";
    return header;
}

// ---------------------------------------------------------------------------
// ONVIF Operations
// ---------------------------------------------------------------------------

static bool createSubscription() {
    if (strlen(state.host) == 0) return false;

    HTTPClient http;
    String url = "http://" + String(state.host) + ":" + String(state.port) + "/onvif/event_service";
    
    http.begin(url);
    http.addHeader("Content-Type", "application/soap+xml; charset=utf-8");

    String body = SOAP_ENV_START;
    body += getAuthHeader();
    body += "<s:Body><e:CreatePullPointSubscription/></s:Body>";
    body += SOAP_ENV_END;

    int code = http.POST(body);
    if (code == 200) {
        String res = http.getString();
        // Extract Address from SubscriptionReference
        // Simple search: <Address>...</Address>
        int start = res.indexOf("<tt:Address>");
        if (start != -1) {
            start += 12;
            int end = res.indexOf("</tt:Address>", start);
            state.subscriptionAddress = res.substring(start, end);
            state.connected = true;
            Serial.println("[ONVIF] Subscription created: " + state.subscriptionAddress);
            http.end();
            return true;
        }
    }

    Serial.printf("[ONVIF] Subscription failed, code: %d\n", code);
    http.end();
    return false;
}

static void pollMessages() {
    if (!state.connected || state.subscriptionAddress.length() == 0) return;

    HTTPClient http;
    http.begin(state.subscriptionAddress);
    http.addHeader("Content-Type", "application/soap+xml; charset=utf-8");
    http.setTimeout(5000); // 5s timeout for PullMessages

    String body = SOAP_ENV_START;
    body += getAuthHeader();
    body += "<s:Body><e:PullMessages><e:Timeout>PT2S</e:Timeout><e:MessageLimit>10</e:MessageLimit></e:PullMessages></s:Body>";
    body += SOAP_ENV_END;

    int code = http.POST(body);
    if (code == 200) {
        String res = http.getString();
        // Look for Motion Detection events
        // Match specifically: Name="IsMotion" ... Value="true" within the same SimpleItem
        // Avoids false positives from unrelated "true" values in the response
        bool motion = false;
        int searchPos = 0;
        while (true) {
            int namePos = res.indexOf("IsMotion", searchPos);
            if (namePos < 0) break;
            // Look for Value="true" within 100 chars after IsMotion
            int valuePos = res.indexOf("Value=\"true\"", namePos);
            if (valuePos >= 0 && valuePos - namePos < 100) {
                motion = true;
                break;
            }
            searchPos = namePos + 8;
        }
        
        zonesSetVirtualInput(state.targetZone, motion);
        
        if (motion) {
            Serial.println("[ONVIF] Motion detected on camera!");
        }
    } else if (code > 0) {
        // Handle error/expiration
        if (code == 400 || code == 404 || code == 500) {
            state.connected = false;
            Serial.println("[ONVIF] Connection lost, will retry subscription");
        }
    }
    http.end();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void onvifInit() {
    // Loaded from NVS in main or here
    state.port = 80;
    state.connected = false;
    state.lastPollMs = 0;
}

void onvifSetServer(const char* host, uint16_t port, const char* user, const char* pass, uint8_t targetZone) {
    strncpy(state.host, host, sizeof(state.host)-1);
    state.port = port;
    strncpy(state.user, user, sizeof(state.user)-1);
    strncpy(state.pass, pass, sizeof(state.pass)-1);
    state.targetZone = (targetZone > 0 && targetZone <= MAX_ZONES) ? targetZone - 1 : 0;
    
    state.connected = false; // Reset to force new subscription
    state.subscriptionAddress = "";
}

void onvifUpdate() {
    if (WiFi.status() != WL_CONNECTED) return;
    if (strlen(state.host) == 0) return;

    uint32_t now = millis();

    if (!state.connected) {
        // Throttle reconnection attempts
        if (now - state.lastPollMs > 10000) {
            createSubscription();
            state.lastPollMs = now;
        }
    } else {
        // Poll every 1 second
        if (now - state.lastPollMs > 1000) {
            pollMessages();
            state.lastPollMs = now;
        }
    }
}

bool onvifIsConnected() {
    return state.connected;
}

const char* onvifGetHost() { return state.host; }
uint16_t onvifGetPort() { return state.port; }
const char* onvifGetUser() { return state.user; }
const char* onvifGetPass() { return state.pass; }
uint8_t onvifGetTargetZone() { return state.targetZone + 1; }
