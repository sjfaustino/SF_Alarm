#ifndef SF_ALARM_SMS_GATEWAY_H
#define SF_ALARM_SMS_GATEWAY_H

#include "notification_provider.h"
#include "config.h"

class NotificationManager;
class SmsCommandProcessor;

// ---------------------------------------------------------------------------
// SMS Message (received)
// ---------------------------------------------------------------------------
struct SmsMessage {
    int     id;                  // Message index in router inbox
    char    sender[24];          // Sender phone number
    char    body[160];           // Message body (ASCII)
    char    timestamp[24];       // Received timestamp
};

// ---------------------------------------------------------------------------
// Abstract SMS Gateway Interface
// ---------------------------------------------------------------------------
class ISmsGateway {
public:
    virtual ~ISmsGateway() {}

    /// Initialize the gateway with credentials.
    virtual void init(const char* host, const char* user, const char* pass, SmsCommandProcessor* processor = nullptr) = 0;

    /// Update credentials.
    virtual void setCredentials(const char* host, const char* user, const char* pass) = 0;

    /// Send an SMS message.
    virtual bool send(const char* phoneNumber, const char* message) = 0;

    /// Poll the inbox for new messages.
    virtual int pollInbox(SmsMessage* msgs, int maxMessages) = 0;

    /// Poll for sent messages.
    virtual int pollSent(SmsMessage* msgs, int maxMessages) = 0;

    /// Delete a message by ID.
    virtual bool deleteMessage(int messageId) = 0;

    /// Background processing loop.
    virtual void update() = 0;

    /// Connection status.
    virtual bool isReady() = 0;

    /// Diagnostic error string.
    virtual const char* getLastError() = 0;

    // Getters for current config
    virtual const char* getHost() = 0;
    virtual const char* getUser() = 0;
    virtual const char* getPass() = 0;

    /// Execute a raw command (for diagnostics).
    virtual bool execCommand(const char* cmd, char* response, size_t maxLen) = 0;
};

class SmsService : public NotificationProvider {
public:
    SmsService();
    virtual ~SmsService();

    // NotificationProvider implementation
    virtual const char* getName() const override { return "SMS"; }
    virtual bool send(const char* target, const char* message) override;
    virtual bool isReady() const override;

    void init(NotificationManager* nm, SmsCommandProcessor* processor);
    void update();

    void setCredentials(const char* routerIp, const char* user, const char* pass);
    void setProvider(SmsProvider prov);
    bool execCommand(const char* cmd, char* resp, size_t max);

    int pollInbox(SmsMessage* msgs, int maxMessages);
    int pollSent(SmsMessage* msgs, int maxMessages);
    bool deleteMessage(int messageId);

    bool isLoggedIn();
    const char* getLastError();
    const char* getRouterIp();
    const char* getRouterUser();
    const char* getRouterPass();

    ISmsGateway* getGateway() { return _gateway; }

private:
    NotificationManager* _nm;
    SmsCommandProcessor* _processor;
    ISmsGateway* _gateway;
    static SmsService* _instance;
};

#endif // SF_ALARM_SMS_GATEWAY_H
