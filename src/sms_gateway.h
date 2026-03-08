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
