#ifndef SF_ALARM_ONVIF_CLIENT_H
#define SF_ALARM_ONVIF_CLIENT_H

#include <Arduino.h>

/// Initialize ONVIF client.
void onvifInit();

/// Update ONVIF client (poll events). Call in main loop.
void onvifUpdate();

/// Set camera configuration.
void onvifSetServer(const char* host, uint16_t port, const char* user, const char* pass, uint8_t targetZone);

/// Get current connection status.
bool onvifIsConnected();

const char* onvifGetHost();
uint16_t onvifGetPort();
const char* onvifGetUser();
const char* onvifGetPass();
uint8_t onvifGetTargetZone();

#endif // SF_ALARM_ONVIF_CLIENT_H
