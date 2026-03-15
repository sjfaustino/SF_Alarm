#ifndef SF_ALARM_ONVIF_CLIENT_H
#define SF_ALARM_ONVIF_CLIENT_H

#include <Arduino.h>

class ZoneManager;

class OnvifService {
public:
    OnvifService();
    ~OnvifService();

    void init(ZoneManager* zones);
    void setServer(const char* host, uint16_t port, const char* user, const char* pass, uint8_t targetZone);
    void disconnect();
    bool isConnected();

    const char* getHost();
    uint16_t getPort();
    const char* getUser();
    const char* getPass();
    uint8_t getTargetZone();

    ZoneManager* _zones;
    static OnvifService* _instance;

private:
    struct OnvifState* _state;
    static void onvifTask(void* pvParameters);
};

#endif // SF_ALARM_ONVIF_CLIENT_H
