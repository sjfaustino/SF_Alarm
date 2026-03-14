#ifndef SF_ALARM_ONVIF_CLIENT_H
#define SF_ALARM_ONVIF_CLIENT_H

#include <Arduino.h>

struct SystemContext;

class OnvifService {
public:
    OnvifService();
    ~OnvifService();

    void init(SystemContext* ctx);
    void setServer(const char* host, uint16_t port, const char* user, const char* pass, uint8_t targetZone);
    void disconnect();
    bool isConnected();

    const char* getHost();
    uint16_t getPort();
    const char* getUser();
    const char* getPass();
    uint8_t getTargetZone();

private:
    SystemContext* _ctx;
    struct OnvifState* _state;
    static void onvifTask(void* pvParameters);
    static OnvifService* _instance;
};

#endif // SF_ALARM_ONVIF_CLIENT_H
