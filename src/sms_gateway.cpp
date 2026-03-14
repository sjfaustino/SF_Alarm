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
#include "string_utils.h"
#include "html_utils.h"

static const char* TAG = "SMS";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static String sysauthCookie = "";   // LuCI sysauth cookie
static String csrfToken     = "";   // LuCI CSRF token (from HTML pages)
static String sessionStok   = "";   // LuCI session token (from URL path)
static bool   loggedIn      = false;
static SemaphoreHandle_t smsMutex = NULL; // Protects lastError and session state

static char routerIp[64]   = "";
static char routerUser[32] = "";
static char routerPass[64] = "";

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
    if (smsMutex && xSemaphoreTake(smsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        va_list args;
        va_start(args, fmt);
        vsnprintf(lastError, sizeof(lastError), fmt, args);
        va_end(args);
        xSemaphoreGive(smsMutex);
    }
    LOG_ERROR(TAG, "%s", lastError);
}

static void buildUrlStr(const char* path, char* dest, size_t maxLen)
{
    // If we have a session token and the path starts with the LuCI prefix,
    // inject the stok into the URL path as required by modern LuCI versions.
    if (sessionStok.length() > 0 && strncmp(path, "/cgi-bin/luci/", 14) == 0) {
        snprintf(dest, maxLen, "http://%s/cgi-bin/luci/;stok=%s/%s", 
                 routerIp, sessionStok.c_str(), path + 14);
    } else {
        snprintf(dest, maxLen, "http://%s%s", routerIp, path);
    }
}


static String extractHiddenField(const String& page, const char* fieldName)
{
    int pos = 0;
    int tagStart, tagEnd;
    while (HtmlUtils::findTag(page, "input", tagStart, tagEnd, pos)) {
        String tag = page.substring(tagStart, tagEnd + 1);
        if (HtmlUtils::getAttribute(tag, "name") == fieldName) {
            String val = HtmlUtils::getAttribute(tag, "value");
            // Security check: Tokens shouldn't be absurdly long
            if (val.length() > 256) return "";
            return val;
        }
        pos = tagEnd + 1;
    }
    return "";
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
    char loginPageUrl[128];
    buildUrlStr("/cgi-bin/luci/", loginPageUrl, sizeof(loginPageUrl));
    LOG_INFO(TAG, "Fetching login page: %s", loginPageUrl);

    http.begin(loginPageUrl);
    http.setTimeout(10000);
    http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36");
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

    String _csrf = "", salt = "", token = "", zonename = "", timeclock = "";
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
            
            if (c == '>') {
                // Buffer likely contains a full tag (or at least the end of it)
                int tStart, tEnd;
                if (HtmlUtils::findTag(buffer, "input", tStart, tEnd)) {
                    String tagStr = String(buffer).substring(tStart, tEnd + 1);
                    String name = HtmlUtils::getAttribute(tagStr, "name");
                    String value = HtmlUtils::getAttribute(tagStr, "value");
                    
                    if (name == "_csrf") _csrf = value;
                    else if (name == "salt") salt = value;
                    else if (name == "token") token = value;
                    else if (name == "zonename") zonename = value;
                    else if (name == "timeclock") timeclock = value;
                }
            }

            // HARDENING: Prevent heap detonation by malformed/maliciously large tags
            if (_csrf.length() > 256) _csrf = "";
            if (salt.length() > 256) salt = "";
            if (token.length() > 256) token = "";

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

    LOG_INFO(TAG, "Login factors extracted (Scrubbing intermediates...)");
    
    // Scrub intermediate hashes and tokens
    scrubString(hashVal1);
    scrubString(token);

    Serial.printf("[SMS] Password hashed OK\n");

    // --- Step 3: POST login ---
    // NOTE: LuCI uses Referer as an additional CSRF check.
    // A missing or wrong Referer header causes a 403 even with a valid token.
    char loginUrl[128];
    buildUrlStr("/cgi-bin/luci/", loginUrl, sizeof(loginUrl));
    HTTPClient http2;
    http2.begin(loginUrl);
    http2.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36");
    http2.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http2.addHeader("Referer", loginUrl);  // Required by LuCI CSRF guard
    
    char originUrl[128];
    snprintf(originUrl, sizeof(originUrl), "http://%s", routerIp);
    http2.addHeader("Origin", originUrl);
    http2.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http2.setTimeout(10000);

    const char* headerKeys[] = {"Set-Cookie", "Location"};
    http2.collectHeaders(headerKeys, 2);

    char encCsrf[512] = "";
    urlEncodeTo(_csrf.c_str(), encCsrf, sizeof(encCsrf));
    
    // POST body — Mirroring the exact sequence from Wireshark capture
    char postData[1024];
    snprintf(postData, sizeof(postData), 
             "_csrf=%s&token=%s&salt=%s&zonename=%s&timeclock=%s&luci_language=auto&luci_username=%s&luci_password=%s",
             encCsrf, token.c_str(), salt.c_str(), zonename.c_str(), timeclock.c_str(), routerUser, finalHash.c_str());
    
    scrubString(finalHash); // Absolute final scrub of the credential hash

    LOG_INFO(TAG, "Login POST to %s (body len=%d)", loginUrl, strlen(postData));
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
    }
    // --- Step 4: Extract stok from Location header or redirect URL ---
    if (postCode == 302 || postCode == 301 || postCode == 200) {
        String location = http2.header("Location");
        LOG_INFO(TAG, "Login redirect: %s", location.c_str());
        
        // Search for ;stok=xxxxxxxxxxxxxxx/
        int stokStart = location.indexOf(";stok=");
        if (stokStart >= 0) {
            stokStart += 6;
            int stokEnd = location.indexOf('/', stokStart);
            if (stokEnd < 0) stokEnd = location.length();
            sessionStok = location.substring(stokStart, stokEnd);
            LOG_INFO(TAG, "Session token (stok) extracted: %s...", sessionStok.substring(0, 8).c_str());
        }
    }

    http2.end();

    if (sysauthCookie.length() == 0) {
        setError("Login failed — no session cookie (HTTP %d)", postCode);
        return false;
    }

    // --- Step 4: Fetch SMS page to get the page-level CSRF token ---
    HTTPClient http3;
    char smsUrl[128];
    buildUrlStr("/cgi-bin/luci/admin/network/gcom/sms?iface=4g", smsUrl, sizeof(smsUrl));
    http3.begin(smsUrl);
    http3.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36");
    http3.addHeader("Cookie", String("sysauth=") + sysauthCookie);
    http3.addHeader("Referer", String("http://") + routerIp + "/cgi-bin/luci/");
    http3.setTimeout(10000);

    int code3 = http3.GET();
    Serial.printf("[SMS] SMS page response: %d\n", code3);

    // Accept 200 or 403 (Cudy quirk)
    if (code3 == HTTP_CODE_OK || code3 == 403) {
        String body = http3.getString();
        Serial.printf("[SMS] SMS page size: %d bytes\n", body.length());

        // Use robust field extraction
        csrfToken = extractHiddenField(body, "token");
        if (csrfToken.length() == 0) {
            csrfToken = extractHiddenField(body, "_csrf");
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

static void smsGatewayCleanup()
{
    // Deep Memory Scrubbing: Leave no trace of session credentials in heap fragments
    scrubString(sysauthCookie);
    scrubString(csrfToken);
    scrubString(sessionStok);
    loggedIn = false;
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
        HTTPClient httpGet;
        char getterUrl[128];
        buildUrlStr("/cgi-bin/luci/admin/network/gcom/sms/smsnew?nomodal=&iface=4g", getterUrl, sizeof(getterUrl));

        httpGet.begin(getterUrl);
        httpGet.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36");
        httpGet.addHeader("Cookie", String("sysauth=") + sysauthCookie);
        httpGet.addHeader("Referer", String("http://") + routerIp + "/cgi-bin/luci/admin/network/gcom/sms?iface=4g");
        httpGet.setTimeout(3000); // Fast timeout for local router access
        
        esp_task_wdt_reset(); // Reset watchdog before blocking call
        int getCode = httpGet.GET();
        // Accept 200 or 403
        if (getCode != 200 && getCode != 403) {
            setError("Send GET error: %d (attempt %d)", getCode, attempt + 1);
            httpGet.end();
            if (getCode == 401) {
                smsGatewayCleanup();
                if (smsGatewayLogin()) continue;
                return false;
            }
            if (attempt < MAX_RETRIES - 1) vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Zero-Heap Stream Scraper for send tokens
        WiFiClient* stream = httpGet.getStreamPtr();
        String sendToken = "";
        if (stream) {
            char buffer[512];
            int bufPos = 0;
            unsigned long scraperStart = millis();
            while (httpGet.connected() && sendToken == "") {
                if (millis() - scraperStart > 5000) break;
                if (stream->available()) {
                    int c = stream->read();
                    if (c < 0) break;
                    buffer[bufPos++] = (char)c;
                    buffer[bufPos] = '\0';
                    
                    if (strcasestr(buffer, "name=\"token\"") || strcasestr(buffer, "name=\"_csrf\"")) {
                        sendToken = extractBetween(buffer, "value=\"", "\"");
                    }

                    if (bufPos >= (int)sizeof(buffer) - 1) {
                        const int OVERLAP = 128;
                        memmove(buffer, buffer + (sizeof(buffer) - OVERLAP - 1), OVERLAP);
                        bufPos = OVERLAP;
                        buffer[bufPos] = '\0';
                    }
                } else {
                    vTaskDelay(1);
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
        char targetUrl[128];
        buildUrlStr("/cgi-bin/luci/admin/network/gcom/sms/smsnew?nomodal=&iface=4g", targetUrl, sizeof(targetUrl));
        httpPost.begin(targetUrl);
        httpPost.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36");
        httpPost.addHeader("Content-Type", "application/x-www-form-urlencoded");
        httpPost.addHeader("Cookie", String("sysauth=") + sysauthCookie);
        httpPost.addHeader("Origin", String("http://") + routerIp);
        httpPost.addHeader("Referer", String("http://") + routerIp + "/cgi-bin/luci/admin/network/gcom/sms?iface=4g");
        httpPost.setTimeout(5000); // 5s is plenty for a local POST

        char sendTokenSnapshot[128];
        strncpy(sendTokenSnapshot, sendToken.c_str(), sizeof(sendTokenSnapshot) - 1);
        sendTokenSnapshot[sizeof(sendTokenSnapshot) - 1] = '\0';
        scrubString(sendToken); // Scrub the original transient token

        esp_task_wdt_reset(); // Reset watchdog before blocking call
        
        // URL-encode message (form encoding) - Robust Citadel Implementation
        char encodedMsg[1024];
        urlEncodeTo(message, encodedMsg, sizeof(encodedMsg));
        
        // Manual form encoding adjustment (replace %20 with + for standard form POST)
        for (int i = 0; encodedMsg[i] != '\0'; i++) {
            if (encodedMsg[i] == '%' && encodedMsg[i+1] == '2' && encodedMsg[i+2] == '0') {
                encodedMsg[i] = '+';
                // shift remainder left
                memmove(&encodedMsg[i+1], &encodedMsg[i+3], strlen(&encodedMsg[i+3]) + 1);
            }
        }

        char postData[2048];
        snprintf(postData, sizeof(postData), 
                 "token=%s&timeclock=0&cbid.smsnew.1.phone=%s&cbid.smsnew.1.content=%s&cbi.submit=1&cbid.smsnew.1.send=Send",
                 sendTokenSnapshot, phoneNumber, encodedMsg);
        
        memset(sendTokenSnapshot, 0, sizeof(sendTokenSnapshot)); // Scrub

        int httpCode = httpPost.POST(postData);
        Serial.printf("[SMS] Send POST response: %d\n", httpCode);

        if (httpCode == HTTP_CODE_OK || httpCode == 200 || httpCode == 302) {
            httpPost.end();
            Serial.printf("[SMS] Sent to %s: \"%s\"\n", phoneNumber, message);
            return true;
        } else if (httpCode == 403 || httpCode == 401) {
            httpPost.end();
            smsGatewayCleanup();
            if (smsGatewayLogin()) continue;
            return false;
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
    char pollUrl[128];
    buildUrlStr(pathStr.c_str(), pollUrl, sizeof(pollUrl));
    
    http.begin(pollUrl);
    http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36");
    http.addHeader("Cookie", String("sysauth=") + sysauthCookie);
    http.addHeader("Referer", String("http://") + routerIp + "/cgi-bin/luci/admin/network/gcom/sms?iface=4g");
    http.setConnectTimeout(2000);
    http.setTimeout(3000);

    int httpCode = http.GET();

    if (httpCode == 403 || httpCode == 401) {
        http.end();
        smsGatewayCleanup();
        if (smsGatewayLogin()) {
            http.begin(pollUrl);
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
                    int colOffset = (strcmp(boxParam, "rec") == 0) ? 1 : 0; // Inbox has an extra "Status" column

                    while (td && tdIdx < 7) {
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

                        if (tdIdx == 1 + colOffset) {
                            strncpy(msg.sender, trimmed, sizeof(msg.sender)-1);
                        } else if (tdIdx == 2 + colOffset) {
                            strncpy(msg.body, trimmed, sizeof(msg.body)-1);
                        } else if (tdIdx == 3 + colOffset) {
                            strncpy(msg.timestamp, trimmed, sizeof(msg.timestamp)-1);
                        } else if (tdIdx >= 4 + colOffset) {
                            // Look for ID in action buttons
                            char* cfg = strcasestr(trimmed, "cfg=");
                            if (cfg && msg.id == 0) {
                                cfg += 4;
                                char* endCfg = strpbrk(cfg, " \"'&");
                                if (endCfg) {
                                    char tmp = *endCfg;
                                    *endCfg = '\0';
                                    msg.id = (int)strtol(cfg + 3, nullptr, 16);
                                    *endCfg = tmp;
                                }
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
 
int smsGatewayPollSent(SmsMessage* msgs, int maxMessages)
{
    return smsGatewayPollMessages(msgs, maxMessages, "sto");
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
    char url[128];
    snprintf(url, sizeof(url), "http://%s/cgi-bin/luci/admin/network/gcom/sms/delsms?iface=4g&cfg=%s", routerIp, cfgBuf);

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
            char url2[128];
            snprintf(url2, sizeof(url2), "http://%s/cgi-bin/luci/admin/network/gcom/sms/delsms?iface=4g&cfg=%s", routerIp, cfgBuf);
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
    sysauthCookie = "";
    csrfToken = "";
    loggedIn = false;
    
    if (smsMutex == NULL) {
        smsMutex = xSemaphoreCreateMutex();
    }

    Serial.printf("[SMS] Gateway init — router: %s\n", routerIp);
}

void smsGatewaySetCredentials(const char* ip, const char* user, const char* pass)
{
    // Scrub old sensitive data
    memset(routerIp, 0, sizeof(routerIp));
    memset(routerUser, 0, sizeof(routerUser));
    memset(routerPass, 0, sizeof(routerPass));
    sysauthCookie = "";
    csrfToken = "";
    loggedIn = false;

    if (ip) {
        strncpy(routerIp, ip, sizeof(routerIp) - 1);
        routerIp[sizeof(routerIp) - 1] = '\0';
    }
    if (user) {
        strncpy(routerUser, user, sizeof(routerUser) - 1);
        routerUser[sizeof(routerUser) - 1] = '\0';
    }
    if (pass) {
        strncpy(routerPass, pass, sizeof(routerPass) - 1);
        routerPass[sizeof(routerPass) - 1] = '\0';
    }
    
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
