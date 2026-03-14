#ifndef SF_ALARM_WHATSAPP_CLIENT_H
#define SF_ALARM_WHATSAPP_CLIENT_H

#include <Arduino.h>

struct SystemContext;

class WhatsappService {
public:
    WhatsappService();
    ~WhatsappService();

    void init(SystemContext* ctx);
    void setConfig(const char* phone, const char* apiKey);
    bool send(const char* message);

    const char* getPhone() const { return _phone; }
    const char* getApiKey() const { return _apiKey; }

private:
    SystemContext* _ctx;
    char _phone[32];
    char _apiKey[32];
    SemaphoreHandle_t _mutex;

    static bool staticSendWrapper(const char* message);
    bool internalSend(const char* phone, const char* apiKey, const char* message);
    size_t urlEncodeTo(const char* src, char* dest, size_t destSize);

    static WhatsappService* _instance;
};

#endif // SF_ALARM_WHATSAPP_CLIENT_H
