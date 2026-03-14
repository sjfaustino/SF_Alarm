#ifndef SF_ALARM_TELEGRAM_CLIENT_H
#define SF_ALARM_TELEGRAM_CLIENT_H

#include <Arduino.h>

struct SystemContext;

class TelegramService {
public:
    TelegramService();
    ~TelegramService();

    void init(SystemContext* ctx);
    void setConfig(const char* token, const char* chatId);
    bool send(const char* message);

    const char* getToken() const { return _token; }
    const char* getChatId() const { return _chatId; }

private:
    SystemContext* _ctx;
    char _token[64];
    char _chatId[32];
    SemaphoreHandle_t _mutex;

    static bool staticSendWrapper(const char* message);
    bool internalSend(const char* token, const char* chatId, const char* message);
    
    static TelegramService* _instance;
};

#endif // SF_ALARM_TELEGRAM_CLIENT_H
