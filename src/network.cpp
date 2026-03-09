#include "network.h"
#include "config.h"
#include <WiFi.h>

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static char wifiSsid[64] = "";
static char wifiPass[64] = "";
static bool connecting    = false;
static uint32_t lastReconnectAttempt = 0;

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void networkInit()
{
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    // WiFi credentials loaded by configLoad() -> networkSetWifi()
    Serial.println("[NET] WiFi initialized (waiting for config)");
}

void networkSetWifi(const char* ssid, const char* password)
{
    strncpy(wifiSsid, ssid, sizeof(wifiSsid) - 1);
    wifiSsid[sizeof(wifiSsid) - 1] = '\0';
    strncpy(wifiPass, password, sizeof(wifiPass) - 1);
    wifiPass[sizeof(wifiPass) - 1] = '\0';

    // Disconnect and reconnect
    WiFi.disconnect();
    delay(100);
    WiFi.begin(wifiSsid, wifiPass);
    connecting = true;
    lastReconnectAttempt = millis();

    Serial.printf("[NET] Wi-Fi credentials updated: %s (use 'save' to persist)\n", wifiSsid);
}

void networkUpdate()
{
    // Let ESP-IDF handle auto-reconnect in the background.
    // Just optional state tracking here.
    if (strlen(wifiSsid) == 0) return;

    if (WiFi.status() == WL_CONNECTED) {
        if (connecting) {
            connecting = false;
            Serial.printf("[NET] Connected! IP: %s  RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.RSSI());
        }
    } else {
        if (!connecting) {
            connecting = true;
            Serial.printf("[NET] Connection lost. ESP-IDF AutoReconnect active for %s...\n", wifiSsid);
        }
    }
}

bool networkIsConnected()
{
    return WiFi.status() == WL_CONNECTED;
}

const char* networkGetIP()
{
    static char ipBuf[16];
    if (WiFi.status() == WL_CONNECTED) {
        strncpy(ipBuf, WiFi.localIP().toString().c_str(), sizeof(ipBuf) - 1);
        ipBuf[sizeof(ipBuf) - 1] = '\0';
    } else {
        strncpy(ipBuf, "0.0.0.0", sizeof(ipBuf));
    }
    return ipBuf;
}

int networkGetRSSI()
{
    if (WiFi.status() == WL_CONNECTED) {
        return WiFi.RSSI();
    }
    return -100;
}

void networkPrintStatus()
{
    Serial.println("--- Network Status ---");
    Serial.printf("  SSID:       %s\n", wifiSsid);
    Serial.printf("  Status:     %s\n",
                  WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DISCONNECTED");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("  IP:         %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("  Gateway:    %s\n", WiFi.gatewayIP().toString().c_str());
        Serial.printf("  RSSI:       %d dBm\n", WiFi.RSSI());
        Serial.printf("  MAC:        %s\n", WiFi.macAddress().c_str());
    }
    Serial.println("----------------------");
}

const char* networkGetSsid() { return wifiSsid; }
const char* networkGetPass() { return wifiPass; }
