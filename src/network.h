#ifndef SF_ALARM_NETWORK_H
#define SF_ALARM_NETWORK_H

#include <Arduino.h>

/// Initialize network (Wi-Fi STA mode).
void networkInit();

/// Set Wi-Fi credentials and connect.
void networkSetWifi(const char* ssid, const char* password);

/// Main update — handles reconnection, etc. Call in loop().
void networkUpdate();

/// Check if connected to Wi-Fi.
bool networkIsConnected();

/// Get the local IP address as a static string (valid until next call).
const char* networkGetIP();

/// Get Wi-Fi signal strength (RSSI).
int networkGetRSSI();

/// Print network status to Serial.
void networkPrintStatus();

/// Get current WiFi SSID (for config persistence).
const char* networkGetSsid();

/// Get current WiFi password (for config persistence).
const char* networkGetPass();

#endif // SF_ALARM_NETWORK_H
