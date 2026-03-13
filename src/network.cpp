#include "network.h"
#include "config.h"
#include <WiFi.h>
#include <ETH.h>
#include "config_manager.h"
#include "logging.h"

static const char* TAG = "NET";
#include <time.h>

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static char wifiSsid[64] = "";
static char wifiPass[64] = "";
static bool connecting    = false;
static uint32_t lastReconnectAttempt = 0;
static bool ethConnected  = false;

static void NetworkEvent(WiFiEvent_t event)
{
    switch (event) {
        case ARDUINO_EVENT_ETH_START:
            LOG_INFO(TAG, "ETH Started");
            ETH.setHostname("sf-alarm");
            break;
        case ARDUINO_EVENT_ETH_CONNECTED:
            LOG_INFO(TAG, "ETH Link Up");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            LOG_INFO(TAG, "ETH MAC: %s, IPv4: %s, FULL_DUPLEX: %d, Mbps: %d",
                          ETH.macAddress().c_str(),
                          ETH.localIP().toString().c_str(),
                          ETH.fullDuplex(),
                          ETH.linkSpeed());
            ethConnected = true;
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            LOG_INFO(TAG, "ETH Link Down");
            ethConnected = false;
            break;
        case ARDUINO_EVENT_ETH_STOP:
            LOG_INFO(TAG, "ETH Stopped");
            ethConnected = false;
            break;
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void networkInit()
{
    WiFi.onEvent(NetworkEvent);
    
    // Initialize Kincony KC868-A16 LAN8720 Ethernet PHY
    // (uint8_t phy_addr, int power, int mdc, int mdio, eth_phy_type_t type, eth_clock_mode_t clk_mode)
    if (ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO, ETH_PHY_TYPE, ETH_CLOCK_GPIO17_OUT)) {
        LOG_INFO(TAG, "Ethernet PHY initialized");
    } else {
        LOG_ERROR(TAG, "Ethernet PHY init failed");
    }

    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);

    // Initialize Network Time Protocol (SNTP)
    // The underlying LwIP stack will automatically sync when network is up.
    configTzTime(configGetTimezone(), "pool.ntp.org", "time.nist.gov");
    Serial.println("[NET] NTP Subsystem initialized");

    // WiFi credentials loaded by configLoad() -> networkSetWifi()
    Serial.println("[NET] WiFi subsystem initialized");
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
    WiFi.setHostname("sf-alarm");
    WiFi.begin(wifiSsid, wifiPass);
    connecting = true;
    lastReconnectAttempt = millis();

    LOG_INFO(TAG, "Wi-Fi credentials updated: %s (use 'save' to persist)", wifiSsid);
}

void networkUpdate()
{
    // Let ESP-IDF handle auto-reconnect in the background.
    // Just optional state tracking here.
    if (strlen(wifiSsid) == 0) return;

    if (WiFi.status() == WL_CONNECTED) {
        if (connecting) {
            connecting = false;
            LOG_INFO(TAG, "WiFi Connected: %s  RSSI: %d dBm",
                          WiFi.localIP().toString().c_str(),
                          WiFi.RSSI());
        }
    } else {
        if (!connecting && !ethConnected) {
            connecting = true;
            LOG_WARN(TAG, "Wi-Fi Connection lost. ESP-IDF AutoReconnect active for %s...", wifiSsid);
        }
    }
}

bool networkIsConnected()
{
    return ethConnected || (WiFi.status() == WL_CONNECTED);
}

const char* networkGetIP()
{
    static char ipBuf[16];
    if (ethConnected) {
        strncpy(ipBuf, ETH.localIP().toString().c_str(), sizeof(ipBuf) - 1);
    } else if (WiFi.status() == WL_CONNECTED) {
        strncpy(ipBuf, WiFi.localIP().toString().c_str(), sizeof(ipBuf) - 1);
    } else {
        strncpy(ipBuf, "0.0.0.0", sizeof(ipBuf));
    }
    ipBuf[sizeof(ipBuf) - 1] = '\0';
    return ipBuf;
}

int networkGetRSSI()
{
    if (ethConnected) {
        return 0; // Ethernet gets perfect 0 dBm "RSSI"
    } else if (WiFi.status() == WL_CONNECTED) {
        return WiFi.RSSI();
    }
    return -100;
}

void networkPrintStatus()
{
    Serial.println("--- Network Status ---");
    Serial.printf("  Ethernet:   %s\n", ethConnected ? "CONNECTED" : "DOWN");
    if (ethConnected) {
        Serial.printf("    IP:       %s\n", ETH.localIP().toString().c_str());
        Serial.printf("    Gateway:  %s\n", ETH.gatewayIP().toString().c_str());
        Serial.printf("    MAC:      %s\n", ETH.macAddress().c_str());
        Serial.printf("    Speed:    %d Mbps %s\n", ETH.linkSpeed(), ETH.fullDuplex() ? "Full-Duplex" : "Half-Duplex");
    }

    Serial.printf("  Wi-Fi SSID: %s\n", wifiSsid);
    Serial.printf("  Wi-Fi Link: %s\n", WiFi.status() == WL_CONNECTED ? "CONNECTED" : "DOWN");
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("    IP:       %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("    Gateway:  %s\n", WiFi.gatewayIP().toString().c_str());
        Serial.printf("    RSSI:     %d dBm\n", WiFi.RSSI());
        Serial.printf("    MAC:      %s\n", WiFi.macAddress().c_str());
    }
    Serial.println("----------------------");
}

const char* networkGetSsid() { return wifiSsid; }
const char* networkGetPass() { return wifiPass; }
