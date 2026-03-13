#include "sms_gateway.h"
#include "config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#include "sms_commands.h"
#include "network.h"
#include "logging.h"
#include <esp_task_wdt.h>

static const char* TAG = "SMS";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static char routerIp[64]   = "";
static char routerUser[32] = "";
static char routerPass[64] = "";

static String sysauthCookie = "";   // LuCI sysauth cookie
static String csrfToken     = "";   // LuCI CSRF token (from HTML pages)
static bool   loggedIn      = false;

static size_t urlEncodeTo(const char* src, char* dest, size_t destSize) {
    static const char *hexChars = "0123456789ABCDEF";
    size_t d = 0;
    while (*src && (d < destSize - 1)) {
        uint8_t c = (uint8_t)*src++;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            dest[d++] = (char)c;
        } else {
            if (d + 3 >= destSize) break;
            dest[d++] = '%';
            dest[d++] = hexChars[c >> 4];
            dest[d++] = hexChars[c & 0x0F];
        }
    }
    dest[d] = '\0';
    return d;
}

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
    LOG_ERROR(TAG, "%s", lastError);
}

static String buildUrl(const char* path)
{
    return String("http://") + routerIp + path;
}

/// Extract a substring between two markers from an HTML body safely.
static String extractBetween(const String& body, const char* before, const char* after, int startPos = 0)
{
    int s = body.indexOf(before, startPos);
    if (s < 0) return "";
    s += strlen(before);
    int e = body.indexOf(after, s);
    if (e < 0) return "";
    return body.substring(s, e);
}

/// Robustly extract a value from a named hidden input tag.
/// Handles unordered attributes and ensures parsing stays within tag boundaries.
static String extractHiddenField(const String& page, const char* fieldName)
{
    String searchName = String("name=\"") + fieldName + "\"";
    int pos = page.indexOf(searchName);
    if (pos < 0) return "";

    // Strictly locate the start and end of the containing <input tag
    String lowerPage = page;
    lowerPage.toLowerCase();
    int tagStart = lowerPage.lastIndexOf("<input", pos);
    int tagEnd = lowerPage.indexOf(">", pos);
    
    // Boundary check: ensure the search name is actually inside THIS input tag
    if (tagStart < 0 || tagEnd < 0 || pos < tagStart || pos > tagEnd) return "";

    String inputTag = page.substring(tagStart, tagEnd + 1);
    
    // Within this specific, isolated tag, find value="..."
    // Use tag-aware extraction to ensure we don't bleed into next element
    String val = extractBetween(inputTag, "value=\"", "\"");
    
    // Security check: Tokens shouldn't be empty or absurdly long
    if (val.length() == 0 || val.length() > 256) return "";
    
    return val;
}

/// Extract the CSRF token from a LuCI HTML page.
static String extractCsrfToken(const String& body)
{
    return extractHiddenField(body, "token");
}

/// Compute SHA-256 hash and return as lowercase hex string.
static String sha256Hex(const String& input)
{
    unsigned char hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0); // 0 = SHA-256
    mbedtls_sha256_update(&ctx, (const unsigned char*)input.c_str(), input.length());
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    char hex[65];
    for (int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, sizeof(hex) - (i * 2), "%02x", hash[i]);
    }
    hex[64] = '\0';
    return String(hex);
}

// ---------------------------------------------------------------------------
// LuCI Authentication — Cudy LT500D Secure Login
// ---------------------------------------------------------------------------
// The Cudy LT500D uses a multi-step login:
// 1. GET /cgi-bin/luci/ → extract _csrf, token, salt from hidden fields
// 2. hash1 = SHA256(password + salt)
// 3. finalHash = SHA256(hash1 + token)
// 4. POST with _csrf, luci_username=admin, luci_password=finalHash
// 5. Extract sysauth cookie from response
// 6. GET SMS page to extract page-level CSRF token

bool smsGatewayLogin()
{
    if (strlen(routerIp) == 0) {
        setError("Router IP not configured");
        return false;
    }

    // --- Step 1: GET the login page to extract _csrf, token, salt ---
    // Note: Cudy LT500D returns the login page with HTTP 403 status.
    // We accept both 200 and 403 as valid responses.
    HTTPClient http;
    String loginPageUrl = buildUrl("/cgi-bin/luci/");
    LOG_INFO(TAG, "Fetching login page: %s", loginPageUrl.c_str());

    http.begin(loginPageUrl);
    http.setTimeout(10000);
    http.setUserAgent("Mozilla/5.0");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int code = http.GET();
    LOG_INFO(TAG, "Login page response: %d", code);

    if (code != HTTP_CODE_OK && code != 403) {
        setError("Network failure accessing router (HTTP %d)", code);
        http.end();
        return false;
    }

    // --- ZERO-HEAP STREAM SCRAPER ---
    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        setError("Failed to get response stream");
        http.end();
        return false;
    }

    String _csrf = "", salt = "", token = "";
    char buffer[512];
    int bufPos = 0;
    unsigned long start = millis();
    
    while (http.connected() && (_csrf == "" || salt == "" || token == "")) {
        if (millis() - start > 10000) break; // Timeout
        
        if (stream->available()) {
            int c = stream->read();
            if (c < 0) break;
            buffer[bufPos++] = (char)c;
            buffer[bufPos] = '\0';
            
            // Search for tokens in isolated input tags
            // We search for tokens in the sliding window. 
            // Clearing bufPos on every '>' is too aggressive if tokens cross boundaries.
            // Instead, we just let the overlapping window handle it.
            if (_csrf == "" && strcasestr(buffer, "name=\"_csrf\"")) _csrf = extractBetween(buffer, "value=\"", "\"");
            if (salt == "" && strcasestr(buffer, "name=\"salt\""))  salt  = extractBetween(buffer, "value=\"", "\"");
            if (token == "" && strcasestr(buffer, "name=\"token\"")) token = extractBetween(buffer, "value=\"", "\"");

            // If buffer is full, shift it to keep the last 128 bytes (overlapping window)
            if (bufPos >= (int)sizeof(buffer) - 1) {
                const int OVERLAP = 128;
                memmove(buffer, buffer + (sizeof(buffer) - OVERLAP - 1), OVERLAP);
                bufPos = OVERLAP;
                buffer[bufPos] = '\0';
            }
        } else {
            vTaskDelay(10);
        }
    }
    http.end();

    if (_csrf == "" || salt == "" || token == "") {
        setError("Failed to scrape LuCI login tokens (Router firmare mismatch?)");
        return false;
    }

    // Store login _csrf as fallback for send operations
    csrfToken = _csrf;

    // --- Step 2: Double SHA-256 hash ---
    // hash1 = SHA256(password + salt)
    String hashVal1 = sha256Hex(String(routerPass) + salt);
    // finalHash = SHA256(hash1 + token)
    String finalHash = sha256Hex(hashVal1 + token);

    LOG_INFO(TAG, "Login factors: _csrf=%.8s... token=%.8s... salt=%.8s...",
                   _csrf.c_str(), token.c_str(), salt.c_str());

    Serial.printf("[SMS] Password hashed OK\n");

    // --- Step 3: POST login ---
    // NOTE: LuCI uses Referer as an additional CSRF check.
    // A missing or wrong Referer header causes a 403 even with a valid token.
    String loginUrl = buildUrl("/cgi-bin/luci/");
    HTTPClient http2;
    http2.begin(loginUrl);
    http2.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http2.addHeader("Referer", loginUrl);  // Required by LuCI CSRF guard
    http2.addHeader("Origin", String("http://") + routerIp);
    http2.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http2.setTimeout(10000);

    const char* headerKeys[] = {"Set-Cookie", "Location"};
    http2.collectHeaders(headerKeys, 2);

    char encCsrf[512] = "";
    urlEncodeTo(_csrf.c_str(), encCsrf, sizeof(encCsrf));
    
    // Minimal POST body — only the fields Cudy LuCI actually requires
    String postData = String("_csrf=") + encCsrf +
                      "&luci_username=" + routerUser +
                      "&luci_password=" + finalHash;

    LOG_INFO(TAG, "Login POST to %s (body len=%d)", loginUrl.c_str(), postData.length());
    // SECURITY: Do NOT log CSRF token, password hash, or credentials.

    int postCode = http2.POST(postData);
    LOG_INFO(TAG, "Login POST result: %d", postCode);
    // LuCI responds with 302 redirect on successful login
    if (postCode == 302 || postCode == 301 || postCode == 200) {
        // Extract sysauth cookie
        String cookies = http2.header("Set-Cookie");
        Serial.printf("[SMS] Set-Cookie: %s\n", cookies.c_str());
        if (cookies.length() > 0) {
            int start = cookies.indexOf("sysauth=");
            if (start >= 0) {
                start += 8;
                int end = cookies.indexOf(';', start);
                if (end < 0) end = cookies.length();
                sysauthCookie = cookies.substring(start, end);
                Serial.printf("[SMS] Logged in! sysauth=%s...\n",
                              sysauthCookie.substring(0, 8).c_str());
            }
        }

        // Also check the Location header for sysauth tokens
        if (sysauthCookie.length() == 0) {
            String location = http2.header("Location");
            Serial.printf("[SMS] Location: %s\n", location.c_str());
            if (location.indexOf("sysauth=") >= 0) {
                int s = location.indexOf("sysauth=") + 8;
                int e = location.indexOf('&', s);
                if (e < 0) e = location.length();
                sysauthCookie = location.substring(s, e);
            }
        }
    } else {
        // Log response body to see what the router actually says
        String body = http2.getString();
        Serial.printf("[SMS] POST response body (first 200): %.200s\n", body.c_str());
    }

    http2.end();

    if (sysauthCookie.length() == 0) {
        setError("Login failed — no session cookie (HTTP %d)", postCode);
        return false;
    }

    // --- Step 4: Fetch SMS page to get the page-level CSRF token ---
    HTTPClient http3;
    String smsUrl = buildUrl("/cgi-bin/luci/admin/network/gcom/sms?iface=4g");
    http3.begin(smsUrl);
    http3.addHeader("Cookie", String("sysauth=") + sysauthCookie);
    http3.setTimeout(10000);

    int code3 = http3.GET();
    Serial.printf("[SMS] SMS page response: %d\n", code3);

    // Accept 200 or 403 (Cudy quirk)
    if (code3 == HTTP_CODE_OK || code3 == 403) {
        String body = http3.getString();
        Serial.printf("[SMS] SMS page size: %d bytes\n", body.length());

        // Try extracting "token" field first, then "_csrf"
        csrfToken = extractCsrfToken(body);
        if (csrfToken.length() == 0) {
            // Try _csrf field instead
            String searchName = "name=\"_csrf\"";
            int pos = body.indexOf(searchName);
            if (pos >= 0) {
                int inputStart = body.lastIndexOf("<input", pos);
                int inputEnd = body.indexOf(">", pos);
                if (inputStart >= 0 && inputEnd >= 0) {
                    String inputTag = body.substring(inputStart, inputEnd + 1);
                    csrfToken = extractBetween(inputTag, "value=\"", "\"");
                }
            }
        }

        if (csrfToken.length() > 0) {
            Serial.printf("[SMS] CSRF token: %s...\n", csrfToken.substring(0, 8).c_str());
        } else {
            Serial.println("[SMS] Warning: no CSRF token on SMS page");
        }
    } else {
        Serial.printf("[SMS] Warning: SMS page returned %d\n", code3);
    }
    http3.end();

    loggedIn = true;
    return true;
}

// ---------------------------------------------------------------------------
// Send SMS
// ---------------------------------------------------------------------------
// POST /cgi-bin/luci/admin/network/gcom/sms/smsnew?nomodal=&iface=4g
// Fields: token, cbid.smsnew.1.phone, cbid.smsnew.1.content, cbi.submit=1

bool smsGatewaySend(const char* phoneNumber, const char* message)
{
    if (!loggedIn) {
        if (!smsGatewayLogin()) return false;
    }

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        // Step 1: GET the smsnew page to extract its CSRF token
        HTTPClient httpGet;
        String url = buildUrl("/cgi-bin/luci/admin/network/gcom/sms/smsnew?nomodal=&iface=4g");

        httpGet.begin(url);
        httpGet.addHeader("Cookie", String("sysauth=") + sysauthCookie);
        httpGet.setTimeout(3000); // Fast timeout for local router access
        
        esp_task_wdt_reset(); // Reset watchdog before blocking call
        int getCode = httpGet.GET();
        // Accept 200 or 403
        if (getCode != 200 && getCode != 403) {
            setError("Send GET error: %d (attempt %d)", getCode, attempt + 1);
            httpGet.end();
            if (getCode == 401) {
                loggedIn = false;
                if (smsGatewayLogin()) continue;
                return false;
            }
            if (attempt < MAX_RETRIES - 1) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        String page = httpGet.getString();
        httpGet.end();

        // Extract token from the smsnew page
        // The smsnew form uses name="token" (not _csrf)
        String sendToken = extractCsrfToken(page);
        if (sendToken.length() == 0) {
            // Fallback: try _csrf
            String searchName = "name=\"_csrf\"";
            int pos = page.indexOf(searchName);
            if (pos >= 0) {
                int inputStart = page.lastIndexOf("<input", pos);
                int inputEnd = page.indexOf(">", pos);
                if (inputStart >= 0 && inputEnd >= 0) {
                    String tag = page.substring(inputStart, inputEnd + 1);
                    sendToken = extractBetween(tag, "value=\"", "\"");
                }
            }
        }

        if (sendToken.length() == 0) {
            setError("No token on smsnew page (attempt %d)", attempt + 1);
            if (attempt < MAX_RETRIES - 1) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        Serial.printf("[SMS] Send token: %s...\n", sendToken.substring(0, 8).c_str());

        // Step 2: POST the SMS
        HTTPClient httpPost;
        httpPost.begin(url);
        httpPost.addHeader("Content-Type", "application/x-www-form-urlencoded");
        httpPost.addHeader("Cookie", String("sysauth=") + sysauthCookie);
        httpPost.setTimeout(5000); // 5s is plenty for a local POST

        esp_task_wdt_reset(); // Reset watchdog before blocking call
        
        // URL-encode message (form encoding) - Robust Citadel Implementation
        String encodedMsg = "";
        encodedMsg.reserve(strlen(message) * 3);
        const char* hex = "0123456789ABCDEF";
        for (unsigned int i = 0; i < strlen(message); i++) {
            unsigned char c = (unsigned char)message[i];
            if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encodedMsg += (char)c;
            } else if (c == ' ') {
                encodedMsg += '+';
            } else {
                encodedMsg += '%';
                encodedMsg += hex[c >> 4];
                encodedMsg += hex[c & 0x0F];
            }
        }

        if (encodedMsg.length() > 1024) {
            setError("Payload too large after URL encoding (%d bytes)", (int)encodedMsg.length());
            httpPost.end();
            return false;
        }

        String postData = String("token=") + sendToken +
                          "&cbid.smsnew.1.phone=" + phoneNumber +
                          "&cbid.smsnew.1.content=" + encodedMsg +
                          "&cbi.submit=1" +
                          "&cbid.smsnew.1.send=Send";

        int httpCode = httpPost.POST(postData);
        Serial.printf("[SMS] Send POST response: %d\n", httpCode);

        if (httpCode == HTTP_CODE_OK || httpCode == 200 || httpCode == 302) {
            httpPost.end();
            Serial.printf("[SMS] Sent to %s: \"%s\"\n", phoneNumber, message);
            return true;
        } else if (httpCode == 403 || httpCode == 401) {
            httpPost.end();
            loggedIn = false;
            if (smsGatewayLogin()) continue;
            return false;
        } else {
            setError("Send HTTP error: %d (attempt %d)", httpCode, attempt + 1);
            httpPost.end();
        }

        if (attempt < MAX_RETRIES - 1) {
            // Replaced delay() with non-blocking yield to keep netWorkerTask alive
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Poll Messages Helper
// ---------------------------------------------------------------------------

static int smsGatewayPollMessages(SmsMessage* msgs, int maxMessages, const char* boxParam)
{
    if (!loggedIn) {
        if (!smsGatewayLogin()) return 0;
    }

    HTTPClient http;
    String pathStr = String("/cgi-bin/luci/admin/network/gcom/sms/smslist?smsbox=") + boxParam + "&iface=4g";
    String url = buildUrl(pathStr.c_str());

    http.begin(url);
    http.addHeader("Cookie", String("sysauth=") + sysauthCookie);
    http.setConnectTimeout(2000);
    http.setTimeout(3000);

    int httpCode = http.GET();

    if (httpCode == 403 || httpCode == 401) {
        http.end();
        loggedIn = false;
        if (smsGatewayLogin()) {
            http.begin(url);
            http.addHeader("Cookie", String("sysauth=") + sysauthCookie);
            http.setConnectTimeout(2000);
            http.setTimeout(3000); 
            httpCode = http.GET();
        }
    }

    if (httpCode != HTTP_CODE_OK) {
        setError("Poll %s HTTP error: %d", boxParam, httpCode);
        http.end();
        return 0;
    }

    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        http.end();
        return 0;
    }

    char window[1024];
    int windowPos = 0;
    int count = 0;
    unsigned long streamStartMs = millis();

    while ((http.connected() || stream->available()) && count < maxMessages && (millis() - streamStartMs) < 8000) {
        if (stream->available()) {
            streamStartMs = millis();
            int c = stream->read();
            if (c < 0) break;

            if (c == '\0') c = ' ';
            window[windowPos++] = (char)c;
            window[windowPos] = '\0';

            // Look for row end
            if (strcasestr(window, "</tr>")) {
                // Parse the completed row in the window
                char* rowStart = strcasestr(window, "<tr class=\"cbi-section-table-row");
                if (rowStart) {
                    SmsMessage msg;
                    memset(&msg, 0, sizeof(msg));
                    
                    int tdIdx = 0;
                    char* td = strcasestr(rowStart, "<td");
                    while (td && tdIdx < 6) {
                        char* contentStart = strchr(td, '>');
                        if (!contentStart) break;
                        contentStart++;
                        
                        char* contentEnd = strcasestr(contentStart, "</td>");
                        if (!contentEnd) break;
                        
                        int len = contentEnd - contentStart;
                        char cell[128];
                        if (len >= (int)sizeof(cell)) len = sizeof(cell) - 1;
                        strncpy(cell, contentStart, len);
                        cell[len] = '\0';
                        
                        // Trim cell bits
                        char* trimmed = cell;
                        while (*trimmed && isspace(*trimmed)) trimmed++;
                        char* e = trimmed + strlen(trimmed) - 1;
                        while (e > trimmed && isspace(*e)) *e-- = '\0';

                        switch (tdIdx) {
                            case 1: strncpy(msg.sender, trimmed, sizeof(msg.sender)-1); break;
                            case 2: strncpy(msg.body, trimmed, sizeof(msg.body)-1); break;
                            case 3: strncpy(msg.timestamp, trimmed, sizeof(msg.timestamp)-1); break;
                            case 4: {
                                char* cfg = strcasestr(trimmed, "cfg=");
                                if (cfg) {
                                    cfg += 4;
                                    char* endCfg = strpbrk(cfg, " \"'&");
                                    if (endCfg) *endCfg = '\0';
                                    msg.id = (int)strtol(cfg + 3, nullptr, 16);
                                }
                                break;
                            }
                        }
                        td = strcasestr(contentEnd, "<td");
                        tdIdx++;
                    }
                    
                    if (strlen(msg.sender) > 0) {
                        memcpy(&msgs[count++], &msg, sizeof(SmsMessage));
                    }
                }
                // Clear window for next row scan
                windowPos = 0;
                window[0] = '\0';
            }

            // Guard: If window is full and no row end found, shift it
            if (windowPos >= (int)sizeof(window) - 1) {
                memmove(window, window + 512, 512);
                windowPos = 512;
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    http.end();

    if (count > 0) {
        Serial.printf("[SMS] Polled %s: %d message(s)\n", boxParam, count);
    }
    return count;
}

// ---------------------------------------------------------------------------
// Public API Wrappers
// ---------------------------------------------------------------------------

int smsGatewayPollInbox(SmsMessage* msgs, int maxMessages)
{
    return smsGatewayPollMessages(msgs, maxMessages, "rec");
}

int smsGatewayPollOutbox(SmsMessage* msgs, int maxMessages)
{
    return smsGatewayPollMessages(msgs, maxMessages, "sent");
}

// ---------------------------------------------------------------------------
// Delete Message
// ---------------------------------------------------------------------------
// GET /cgi-bin/luci/admin/network/gcom/sms/delsms?iface=4g&cfg=<id>

bool smsGatewayDeleteMessage(int messageId)
{
    if (!loggedIn) {
        if (!smsGatewayLogin()) return false;
    }

    char cfgBuf[16];
    snprintf(cfgBuf, sizeof(cfgBuf), "cfg%06x", (unsigned int)messageId);

    HTTPClient http;
    String url = buildUrl("/cgi-bin/luci/admin/network/gcom/sms/delsms?iface=4g&cfg=");
    url += cfgBuf;

    http.begin(url);
    http.addHeader("Cookie", String("sysauth=") + sysauthCookie);
    http.setTimeout(10000);

    int httpCode = http.GET();
    http.end();

    if (httpCode == 401 || httpCode == 403) {
        // Session expired — re-authenticate and retry once
        loggedIn = false;
        if (smsGatewayLogin()) {
            HTTPClient http2;
            String url2 = buildUrl("/cgi-bin/luci/admin/network/gcom/sms/delsms?iface=4g&cfg=");
            url2 += cfgBuf;
            http2.begin(url2);
            http2.addHeader("Cookie", String("sysauth=") + sysauthCookie);
            http2.setTimeout(10000);
            int code2 = http2.GET();
            http2.end();
            if (code2 == HTTP_CODE_OK || code2 == 302) {
                Serial.printf("[SMS] Deleted message %s (after re-auth)\n", cfgBuf);
                return true;
            }
        }
        setError("Delete failed after re-auth");
        return false;
    }

    if (httpCode == HTTP_CODE_OK || httpCode == 200 || httpCode == 302) {
        Serial.printf("[SMS] Deleted message %s\n", cfgBuf);
        return true;
    }

    setError("Delete HTTP error: %d", httpCode);
    return false;
}

void smsGatewayUpdate()
{
    if (!networkIsConnected()) return;
    
    // Auto-login logic
    if (!loggedIn) {
        smsGatewayLogin();
        return;
    }

    // Poll for new messages
    SmsMessage msgs[5];
    int count = smsGatewayPollMessages(msgs, 5, "rec");

    for (int i = 0; i < count; i++) {
        // Process the command via the command module
        smsCmdProcess(msgs[i].sender, msgs[i].body);

        // Delete from router to keep inbox clean
        smsGatewayDeleteMessage(msgs[i].id);
        
        esp_task_wdt_reset(); // Yield to watchdog after potentially slow delete
    }
}

// ---------------------------------------------------------------------------
// Init & Utility
// ---------------------------------------------------------------------------

void smsGatewayInit(const char* ip, const char* user, const char* pass)
{
    strncpy(routerIp, ip, sizeof(routerIp) - 1);
    routerIp[sizeof(routerIp) - 1] = '\0';
    strncpy(routerUser, user, sizeof(routerUser) - 1);
    routerUser[sizeof(routerUser) - 1] = '\0';
    strncpy(routerPass, pass, sizeof(routerPass) - 1);
    routerPass[sizeof(routerPass) - 1] = '\0';
    loggedIn = false;
    sysauthCookie = "";
    csrfToken = "";
    Serial.printf("[SMS] Gateway init — router: %s\n", routerIp);
}

void smsGatewaySetCredentials(const char* ip, const char* user, const char* pass)
{
    strncpy(routerIp, ip, sizeof(routerIp) - 1);
    routerIp[sizeof(routerIp) - 1] = '\0';
    strncpy(routerUser, user, sizeof(routerUser) - 1);
    routerUser[sizeof(routerUser) - 1] = '\0';
    strncpy(routerPass, pass, sizeof(routerPass) - 1);
    routerPass[sizeof(routerPass) - 1] = '\0';
    loggedIn = false;
    sysauthCookie = "";
    csrfToken = "";
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

const char* smsGatewayGetRouterIp()
{
    return routerIp;
}

const char* smsGatewayGetRouterUser()
{
    return routerUser;
}

const char* smsGatewayGetRouterPass()
{
    return routerPass;
}
