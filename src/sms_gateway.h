#ifndef SF_ALARM_SMS_GATEWAY_H
#define SF_ALARM_SMS_GATEWAY_H

#include <Arduino.h>

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
    virtual void init(const char* host, const char* user, const char* pass) = 0;

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
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Initialize the SMS gateway with router credentials.
void smsGatewayInit(const char* routerIp, const char* user, const char* pass);

/// Update router credentials (e.g., after config change).
void smsGatewaySetCredentials(const char* routerIp, const char* user, const char* pass);

/// Authenticate with the LuCI web interface. Returns true on success.
/// Must be called before send/poll. Auto-called by send/poll if not logged in.
bool smsGatewayLogin();

/// Send an SMS via the Cudy LT500D router.
/// Returns true if the HTTP request succeeded.
bool smsGatewaySend(const char* phoneNumber, const char* message);

/// Poll the router inbox for new messages.
/// Fills the provided array with messages. Returns the number of messages found.
/// maxMessages: size of the msgs array.
int smsGatewayPollInbox(SmsMessage* msgs, int maxMessages);

/// Poll the router for sent messages for display.
/// For sent messages, the 'sender' field contains the destination number.
int smsGatewayPollSent(SmsMessage* msgs, int maxMessages);

/// Main update loop for background tasks (polling). 
/// Call from a background task at regular intervals.
void smsGatewayUpdate();

/// Delete a message from the inbox by its ID.
bool smsGatewayDeleteMessage(int messageId);

/// Check if the gateway is currently authenticated.
bool smsGatewayIsLoggedIn();

/// Get the last error message (for debugging).
const char* smsGatewayGetLastError();

/// Get current router credentials (for config persistence).
const char* smsGatewayGetRouterIp();
const char* smsGatewayGetRouterUser();
const char* smsGatewayGetRouterPass();

#endif // SF_ALARM_SMS_GATEWAY_H
