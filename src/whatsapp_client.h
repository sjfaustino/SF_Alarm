#ifndef SF_ALARM_WHATSAPP_CLIENT_H
#define SF_ALARM_WHATSAPP_CLIENT_H

#include "notification_provider.h"

class NotificationManager;

class WhatsappService : public NotificationProvider {
public:
    WhatsappService();
    virtual ~WhatsappService();

    // NotificationProvider implementation
    virtual const char* getName() const override { return "WhatsApp"; }
    virtual bool send(const char* target, const char* message) override;
    virtual bool isReady() const override { return strlen(_apiKey) > 0; }

    void init(NotificationManager* nm);
    void setConfig(const char* phone, const char* apiKey);
    bool send(const char* message);

    const char* getPhone() const { return _phone; }
    const char* getApiKey() const { return _apiKey; }

private:
    NotificationManager* _nm;
    char _phone[32];
    char _apiKey[32];
    SemaphoreHandle_t _mutex;

    static bool staticSendWrapper(const char* message);
    bool internalSend(const char* phone, const char* apiKey, const char* message);
    size_t urlEncodeTo(const char* src, char* dest, size_t destSize);

    static WhatsappService* _instance;
};

#endif // SF_ALARM_WHATSAPP_CLIENT_H
