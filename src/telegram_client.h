#ifndef SF_ALARM_TELEGRAM_CLIENT_H
#define SF_ALARM_TELEGRAM_CLIENT_H

#include "notification_provider.h"

class NotificationManager;

class TelegramService : public NotificationProvider {
public:
    TelegramService();
    virtual ~TelegramService();

    // NotificationProvider implementation
    virtual const char* getName() const override { return "Telegram"; }
    virtual bool send(const char* target, const char* message) override;
    virtual bool isReady() const override { return strlen(_token) > 0; }

    void init(NotificationManager* nm);
    void setConfig(const char* token, const char* chatId);
    bool send(const char* message);

    const char* getToken() const { return _token; }
    const char* getChatId() const { return _chatId; }

private:
    NotificationManager* _nm;
    char _token[64];
    char _chatId[32];
    SemaphoreHandle_t _mutex;

    static bool staticSendWrapper(const char* message);
    bool internalSend(const char* token, const char* chatId, const char* message);
    
    static TelegramService* _instance;
};

#endif // SF_ALARM_TELEGRAM_CLIENT_H
