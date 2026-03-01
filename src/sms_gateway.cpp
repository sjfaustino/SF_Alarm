#include "sms_gateway.h"
#include "config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static char routerIp[64]   = "";
static char routerUser[32] = "";
static char routerPass[64] = "";

static String sessionToken = "";   // LuCI sysauth cookie/token
static bool   loggedIn     = false;

static char lastError[128] = "";

// Retry configuration
static const int    MAX_RETRIES     = 3;
static const int    RETRY_DELAY_MS  = 2000;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void setError(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(lastError, sizeof(lastError), fmt, args);
    va_end(args);
    Serial.printf("[SMS] Error: %s\n", lastError);
}

static String buildUrl(const char* path)
{
    return String("http://") + routerIp + path;
}

// ---------------------------------------------------------------------------
// LuCI Authentication
// ---------------------------------------------------------------------------
// The Cudy LT500D runs OpenWrt/LuCI. Authentication flow:
// 1. POST to /cgi-bin/luci with username & password
// 2. LuCI returns a sysauth cookie on success
// 3. Use this cookie for all subsequent requests

bool smsGatewayLogin()
{
    if (strlen(routerIp) == 0) {
        setError("Router IP not configured");
        return false;
    }

    HTTPClient http;
    String loginUrl = buildUrl("/cgi-bin/luci");

    http.begin(loginUrl);
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.setConnectTimeout(5000);
    http.setTimeout(10000);

    String postData = String("luci_username=") + routerUser +
                      "&luci_password=" + routerPass;

    int httpCode = http.POST(postData);

    if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY ||
        httpCode == HTTP_CODE_FOUND || httpCode == 302 || httpCode == 301) {

        // Extract sysauth cookie from response headers
        String cookies = http.header("Set-Cookie");
        if (cookies.length() > 0) {
            // Parse sysauth= from cookie string
            int start = cookies.indexOf("sysauth=");
            if (start >= 0) {
                start += 8;  // skip "sysauth="
                int end = cookies.indexOf(';', start);
                if (end < 0) end = cookies.length();
                sessionToken = cookies.substring(start, end);
                loggedIn = true;
                Serial.printf("[SMS] Logged in to LuCI (token: %s...)\n",
                              sessionToken.substring(0, 8).c_str());
                http.end();
                return true;
            }
        }

        // Some LuCI versions embed the token in the redirect URL
        String location = http.header("Location");
        if (location.length() > 0) {
            int tokenStart = location.indexOf("/stok=");
            if (tokenStart >= 0) {
                tokenStart += 6;
                int tokenEnd = location.indexOf('/', tokenStart);
                if (tokenEnd < 0) tokenEnd = location.length();
                sessionToken = location.substring(tokenStart, tokenEnd);
                loggedIn = true;
                Serial.printf("[SMS] Logged in via stok (token: %s...)\n",
                              sessionToken.substring(0, 8).c_str());
                http.end();
                return true;
            }
        }

        // Fallback: try to grab the token from response body
        String body = http.getString();
        if (body.indexOf("stok=") >= 0) {
            int s = body.indexOf("stok=") + 5;
            int e = body.indexOf('\"', s);
            if (e < 0) e = body.indexOf('\'', s);
            if (e < 0) e = body.indexOf('/', s);
            if (e > s) {
                sessionToken = body.substring(s, e);
                loggedIn = true;
                Serial.printf("[SMS] Logged in via body token\n");
                http.end();
                return true;
            }
        }

        setError("Login OK but no session token found");
    } else {
        setError("Login HTTP error: %d", httpCode);
    }

    http.end();
    loggedIn = false;
    return false;
}

// ---------------------------------------------------------------------------
// Send SMS
// ---------------------------------------------------------------------------

bool smsGatewaySend(const char* phoneNumber, const char* message)
{
    if (!loggedIn) {
        if (!smsGatewayLogin()) {
            return false;
        }
    }

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        HTTPClient http;

        // LuCI SMS send endpoint (typical for Cudy/OpenWrt with gcom)
        // Try the gcom SMS endpoint first
        String url = buildUrl("/cgi-bin/luci/admin/network/gcom/sms?iface=4g");

        http.begin(url);
        http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        http.addHeader("Cookie", String("sysauth=") + sessionToken);
        http.setConnectTimeout(5000);
        http.setTimeout(15000);

        // Build form data for sending SMS
        String postData = String("action=send") +
                          "&phone=" + phoneNumber +
                          "&message=" + message +
                          "&token=" + sessionToken;

        int httpCode = http.POST(postData);

        if (httpCode == HTTP_CODE_OK || httpCode == 200) {
            String response = http.getString();
            http.end();

            // Check for success indicators in the response
            if (response.indexOf("error") < 0 || response.indexOf("success") >= 0) {
                Serial.printf("[SMS] Sent to %s: \"%s\"\n", phoneNumber, message);
                return true;
            } else {
                setError("Send response indicates error");
            }
        } else if (httpCode == 403 || httpCode == 401) {
            // Session expired — re-login
            http.end();
            loggedIn = false;
            if (smsGatewayLogin()) {
                continue;  // Retry with new token
            }
            return false;
        } else {
            setError("Send HTTP error: %d (attempt %d)", httpCode, attempt + 1);
            http.end();
        }

        if (attempt < MAX_RETRIES - 1) {
            delay(RETRY_DELAY_MS * (attempt + 1));  // Simple backoff
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// Poll Inbox
// ---------------------------------------------------------------------------

int smsGatewayPollInbox(SmsMessage* msgs, int maxMessages)
{
    if (!loggedIn) {
        if (!smsGatewayLogin()) {
            return 0;
        }
    }

    HTTPClient http;

    String url = buildUrl("/cgi-bin/luci/admin/network/gcom/sms?iface=4g&action=read");

    http.begin(url);
    http.addHeader("Cookie", String("sysauth=") + sessionToken);
    http.setConnectTimeout(5000);
    http.setTimeout(10000);

    int httpCode = http.GET();

    if (httpCode == 403 || httpCode == 401) {
        http.end();
        loggedIn = false;
        if (smsGatewayLogin()) {
            // Retry once
            http.begin(url);
            http.addHeader("Cookie", String("sysauth=") + sessionToken);
            httpCode = http.GET();
        }
    }

    if (httpCode != HTTP_CODE_OK) {
        setError("Poll HTTP error: %d", httpCode);
        http.end();
        return 0;
    }

    String body = http.getString();
    http.end();

    // Parse the response.
    // LuCI/gcom may return HTML or JSON depending on firmware.
    // We'll try JSON first, then fall back to HTML scraping.

    int count = 0;

    // Try JSON parsing
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, body);

    if (!err && doc.is<JsonArray>()) {
        JsonArray arr = doc.as<JsonArray>();
        for (JsonObject obj : arr) {
            if (count >= maxMessages) break;

            msgs[count].id = obj["id"] | count;

            const char* sender = obj["sender"] | (const char*)nullptr;
            if (!sender) sender = obj["from"] | "unknown";
            strncpy(msgs[count].sender, sender, sizeof(msgs[count].sender) - 1);
            msgs[count].sender[sizeof(msgs[count].sender) - 1] = '\0';

            const char* msgBody = obj["body"] | (const char*)nullptr;
            if (!msgBody) msgBody = obj["text"] | (const char*)nullptr;
            if (!msgBody) msgBody = obj["content"] | "";
            strncpy(msgs[count].body, msgBody, sizeof(msgs[count].body) - 1);
            msgs[count].body[sizeof(msgs[count].body) - 1] = '\0';

            const char* ts = obj["timestamp"] | (const char*)nullptr;
            if (!ts) ts = obj["date"] | "";
            strncpy(msgs[count].timestamp, ts, sizeof(msgs[count].timestamp) - 1);
            msgs[count].timestamp[sizeof(msgs[count].timestamp) - 1] = '\0';

            count++;
        }
    } else if (!err && doc.is<JsonObject>()) {
        // Some routers wrap messages in an object
        JsonArray arr = doc["messages"].as<JsonArray>();
        if (arr.isNull()) {
            arr = doc["sms"].as<JsonArray>();
        }
        if (!arr.isNull()) {
            for (JsonObject obj : arr) {
                if (count >= maxMessages) break;

                msgs[count].id = obj["id"] | count;

                const char* sender = obj["sender"] | (const char*)nullptr;
                if (!sender) sender = obj["from"] | "unknown";
                strncpy(msgs[count].sender, sender, sizeof(msgs[count].sender) - 1);
                msgs[count].sender[sizeof(msgs[count].sender) - 1] = '\0';

                const char* msgBody = obj["body"] | (const char*)nullptr;
                if (!msgBody) msgBody = obj["text"] | (const char*)nullptr;
                if (!msgBody) msgBody = obj["content"] | "";
                strncpy(msgs[count].body, msgBody, sizeof(msgs[count].body) - 1);
                msgs[count].body[sizeof(msgs[count].body) - 1] = '\0';

                const char* ts = obj["timestamp"] | (const char*)nullptr;
                if (!ts) ts = obj["date"] | "";
                strncpy(msgs[count].timestamp, ts, sizeof(msgs[count].timestamp) - 1);
                msgs[count].timestamp[sizeof(msgs[count].timestamp) - 1] = '\0';

                count++;
            }
        }
    } else {
        // HTML scraping fallback — look for SMS content in HTML
        // This is router-specific and may need adjustment
        Serial.println("[SMS] Response is not JSON, attempting HTML parse");

        // Simple extraction: look for patterns like phone numbers and message text
        // between known HTML tags. This is fragile but a starting point.
        int searchPos = 0;
        while (count < maxMessages) {
            // Look for SMS entries in the HTML
            int msgStart = body.indexOf("sms-message", searchPos);
            if (msgStart < 0) {
                msgStart = body.indexOf("message-item", searchPos);
            }
            if (msgStart < 0) break;

            // Try to extract sender (phone number pattern)
            int numStart = body.indexOf('+', msgStart);
            if (numStart < 0) numStart = body.indexOf("tel:", msgStart);
            if (numStart >= 0) {
                int numEnd = numStart;
                while (numEnd < (int)body.length() &&
                       (isdigit(body[numEnd]) || body[numEnd] == '+')) {
                    numEnd++;
                }
                String sender = body.substring(numStart, numEnd);
                strncpy(msgs[count].sender, sender.c_str(),
                        sizeof(msgs[count].sender) - 1);
            } else {
                strncpy(msgs[count].sender, "unknown",
                        sizeof(msgs[count].sender) - 1);
            }

            msgs[count].id = count;
            msgs[count].body[0] = '\0';
            msgs[count].timestamp[0] = '\0';
            count++;
            searchPos = msgStart + 10;
        }
    }

    if (count > 0) {
        Serial.printf("[SMS] Polled %d message(s)\n", count);
    }

    return count;
}

// ---------------------------------------------------------------------------
// Delete Message
// ---------------------------------------------------------------------------

bool smsGatewayDeleteMessage(int messageId)
{
    if (!loggedIn) {
        if (!smsGatewayLogin()) {
            return false;
        }
    }

    HTTPClient http;

    String url = buildUrl("/cgi-bin/luci/admin/network/gcom/sms?iface=4g");

    http.begin(url);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.addHeader("Cookie", String("sysauth=") + sessionToken);

    String postData = String("action=delete") +
                      "&id=" + String(messageId) +
                      "&token=" + sessionToken;

    int httpCode = http.POST(postData);
    http.end();

    if (httpCode == HTTP_CODE_OK || httpCode == 200) {
        Serial.printf("[SMS] Deleted message %d\n", messageId);
        return true;
    }

    setError("Delete HTTP error: %d", httpCode);
    return false;
}

// ---------------------------------------------------------------------------
// Init & Utility
// ---------------------------------------------------------------------------

void smsGatewayInit(const char* ip, const char* user, const char* pass)
{
    strncpy(routerIp, ip, sizeof(routerIp) - 1);
    strncpy(routerUser, user, sizeof(routerUser) - 1);
    strncpy(routerPass, pass, sizeof(routerPass) - 1);
    loggedIn = false;
    sessionToken = "";
    Serial.printf("[SMS] Gateway init — router: %s\n", routerIp);
}

void smsGatewaySetCredentials(const char* ip, const char* user, const char* pass)
{
    strncpy(routerIp, ip, sizeof(routerIp) - 1);
    strncpy(routerUser, user, sizeof(routerUser) - 1);
    strncpy(routerPass, pass, sizeof(routerPass) - 1);
    loggedIn = false;
    sessionToken = "";
    Serial.printf("[SMS] Credentials updated — router: %s\n", routerIp);
}

bool smsGatewayIsLoggedIn()
{
    return loggedIn;
}

const char* smsGatewayGetLastError()
{
    return lastError;
}
