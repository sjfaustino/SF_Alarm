#include "sms_gateway.h"
#include "config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static char routerIp[64]   = "";
static char routerUser[32] = "";
static char routerPass[64] = "";

static String sysauthCookie = "";   // LuCI sysauth cookie
static String csrfToken     = "";   // LuCI CSRF token (from HTML pages)
static bool   loggedIn      = false;

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

/// Extract a substring between two markers from an HTML body.
static String extractBetween(const String& body, const String& before, const String& after, int startPos = 0)
{
    int s = body.indexOf(before, startPos);
    if (s < 0) return "";
    s += before.length();
    int e = body.indexOf(after, s);
    if (e < 0) return "";
    return body.substring(s, e);
}

/// Extract the CSRF token from a LuCI HTML page.
/// Searches for <input name="token" ... value="..."> with any attribute order.
static String extractCsrfToken(const String& body)
{
    String searchName = "name=\"token\"";
    int pos = body.indexOf(searchName);
    if (pos < 0) return "";
    int inputStart = body.lastIndexOf("<input", pos);
    int inputEnd = body.indexOf(">", pos);
    if (inputStart < 0 || inputEnd < 0) return "";
    String inputTag = body.substring(inputStart, inputEnd + 1);
    return extractBetween(inputTag, "value=\"", "\"");
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
        sprintf(hex + i * 2, "%02x", hash[i]);
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
    String loginPageUrl = String("http://") + routerIp + "/cgi-bin/luci/";
    Serial.printf("[SMS] GET login page: %s\n", loginPageUrl.c_str());

    http.begin(loginPageUrl);
    http.setTimeout(10000);
    http.setUserAgent("Mozilla/5.0");
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

    int code = http.GET();
    Serial.printf("[SMS] Login page response: %d\n", code);

    // Accept 200 OK or 403 Forbidden (Cudy sends login form as 403)
    if (code != HTTP_CODE_OK && code != 403) {
        setError("Login page GET error: %d", code);
        http.end();
        return false;
    }

    String loginPage = http.getString();
    http.end();

    // Extract hidden fields — handle any attribute order

    // Generic helper: find <input with name="X" and extract its value="Y"
    auto extractHiddenField = [&](const String& page, const String& fieldName) -> String {
        String searchName = String("name=\"") + fieldName + "\"";
        int pos = page.indexOf(searchName);
        if (pos < 0) return "";
        // Search backward for '<input' and forward for '>'
        int inputStart = page.lastIndexOf("<input", pos);
        int inputEnd = page.indexOf(">", pos);
        if (inputStart < 0 || inputEnd < 0) return "";
        String inputTag = page.substring(inputStart, inputEnd + 1);
        // Now extract value="..." from this tag
        return extractBetween(inputTag, "value=\"", "\"");
    };

    String csrf = extractHiddenField(loginPage, "_csrf");
    String token = extractHiddenField(loginPage, "token");
    String salt = extractHiddenField(loginPage, "salt");

    if (csrf.length() == 0 || token.length() == 0 || salt.length() == 0) {
        setError("Could not extract login fields (csrf=%d, token=%d, salt=%d)",
                 csrf.length(), token.length(), salt.length());
        return false;
    }

    Serial.printf("[SMS] Login fields: csrf=%s... token=%s... salt=%s...\n",
                  csrf.substring(0, 8).c_str(),
                  token.substring(0, 8).c_str(),
                  salt.substring(0, 8).c_str());

    // Store login _csrf as fallback for send operations
    csrfToken = csrf;

    // --- Step 2: Double SHA-256 hash ---
    // hash1 = SHA256(password + salt)
    String hash1 = sha256Hex(String(routerPass) + salt);
    // finalHash = SHA256(hash1 + token)
    String finalHash = sha256Hex(hash1 + token);

    Serial.printf("[SMS] Password hashed OK\n");

    // --- Step 3: POST login ---
    HTTPClient http2;
    http2.begin(buildUrl("/cgi-bin/luci/"));
    http2.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http2.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http2.setTimeout(10000);

    const char* headerKeys[] = {"Set-Cookie", "Location"};
    http2.collectHeaders(headerKeys, 2);

    String postData = String("_csrf=") + csrf +
                      "&token=" + token +
                      "&salt=" + salt +
                      "&luci_username=" + routerUser +
                      "&luci_password=" + finalHash +
                      "&luci_language=auto" +
                      "&zonename=UTC" +
                      "&timeclock=" + String(millis() / 1000) +
                      "&cbi.submit=1";

    int httpCode = http2.POST(postData);

    Serial.printf("[SMS] Login POST response: %d\n", httpCode);

    // LuCI responds with 302 redirect on successful login
    if (httpCode == 302 || httpCode == 301 || httpCode == 200) {
        // Extract sysauth cookie
        String cookies = http2.header("Set-Cookie");
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

        // Also check the Location header for stok-style tokens
        if (sysauthCookie.length() == 0) {
            String location = http2.header("Location");
            if (location.indexOf("sysauth=") >= 0) {
                int s = location.indexOf("sysauth=") + 8;
                int e = location.indexOf('&', s);
                if (e < 0) e = location.length();
                sysauthCookie = location.substring(s, e);
            }
        }
    }

    http2.end();

    if (sysauthCookie.length() == 0) {
        setError("Login failed — no session cookie (HTTP %d)", httpCode);
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
        httpGet.setTimeout(10000);

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
            if (attempt < MAX_RETRIES - 1) delay(RETRY_DELAY_MS);
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
            if (attempt < MAX_RETRIES - 1) delay(RETRY_DELAY_MS);
            continue;
        }

        Serial.printf("[SMS] Send token: %s...\n", sendToken.substring(0, 8).c_str());

        // Step 2: POST the SMS
        HTTPClient httpPost;
        httpPost.begin(url);
        httpPost.addHeader("Content-Type", "application/x-www-form-urlencoded");
        httpPost.addHeader("Cookie", String("sysauth=") + sysauthCookie);
        httpPost.setTimeout(15000);

        // URL-encode message (form encoding)
        String encodedMsg = "";
        for (unsigned int i = 0; i < strlen(message); i++) {
            char c = message[i];
            if (c == ' ') encodedMsg += '+';
            else if (c == '%') encodedMsg += "%25";
            else if (c == '&') encodedMsg += "%26";
            else if (c == '=') encodedMsg += "%3D";
            else if (c == '#') encodedMsg += "%23";
            else if (c == '+') encodedMsg += "%2B";
            else if (c == '\n') encodedMsg += "%0A";
            else if (c == '\r') encodedMsg += "%0D";
            else if (c == '"') encodedMsg += "%22";
            else if (c == '<') encodedMsg += "%3C";
            else if (c == '>') encodedMsg += "%3E";
            else encodedMsg += c;
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
            delay(RETRY_DELAY_MS * (attempt + 1));
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Poll Inbox
// ---------------------------------------------------------------------------
// GET /cgi-bin/luci/admin/network/gcom/sms/smslist?smsbox=rec&iface=4g
// Returns HTML table with <tr class="cbi-section-table-row ...">

int smsGatewayPollInbox(SmsMessage* msgs, int maxMessages)
{
    if (!loggedIn) {
        if (!smsGatewayLogin()) return 0;
    }

    HTTPClient http;
    String url = buildUrl("/cgi-bin/luci/admin/network/gcom/sms/smslist?smsbox=rec&iface=4g");

    http.begin(url);
    http.addHeader("Cookie", String("sysauth=") + sysauthCookie);
    http.setConnectTimeout(5000);
    http.setTimeout(10000);

    int httpCode = http.GET();

    if (httpCode == 403 || httpCode == 401) {
        http.end();
        loggedIn = false;
        if (smsGatewayLogin()) {
            http.begin(url);
            http.addHeader("Cookie", String("sysauth=") + sysauthCookie);
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

    // --- Parse HTML table rows ---
    int count = 0;
    int searchPos = 0;

    while (count < maxMessages) {
        int rowStart = body.indexOf("<tr class=\"cbi-section-table-row", searchPos);
        if (rowStart < 0) break;

        int rowEnd = body.indexOf("</tr>", rowStart);
        if (rowEnd < 0) break;

        String row = body.substring(rowStart, rowEnd);

        // Extract <td> contents
        int tdPos = 0;
        int tdIdx = 0;
        String phone = "";
        String content = "";
        String timestamp = "";
        String cfgId = "";

        while (tdIdx < 6) {
            int tdStart = row.indexOf("<td", tdPos);
            if (tdStart < 0) break;
            int tdContentStart = row.indexOf(">", tdStart) + 1;
            int tdEnd = row.indexOf("</td>", tdContentStart);
            if (tdEnd < 0) break;

            String cellContent = row.substring(tdContentStart, tdEnd);
            cellContent.trim();

            switch (tdIdx) {
                case 1: phone = cellContent; break;
                case 2: content = cellContent; break;
                case 3: timestamp = cellContent; break;
                case 4: {
                    int cfgStart = cellContent.indexOf("cfg=");
                    if (cfgStart >= 0) {
                        cfgStart += 4;
                        int cfgEnd = cellContent.indexOf("&", cfgStart);
                        if (cfgEnd < 0) cfgEnd = cellContent.indexOf("\"", cfgStart);
                        if (cfgEnd < 0) cfgEnd = cellContent.indexOf("'", cfgStart);
                        if (cfgEnd > cfgStart) cfgId = cellContent.substring(cfgStart, cfgEnd);
                    }
                    break;
                }
            }

            tdPos = tdEnd + 5;
            tdIdx++;
        }

        if (phone.length() > 0) {
            msgs[count].id = count;
            strncpy(msgs[count].sender, phone.c_str(), sizeof(msgs[count].sender) - 1);
            msgs[count].sender[sizeof(msgs[count].sender) - 1] = '\0';
            strncpy(msgs[count].body, content.c_str(), sizeof(msgs[count].body) - 1);
            msgs[count].body[sizeof(msgs[count].body) - 1] = '\0';
            strncpy(msgs[count].timestamp, timestamp.c_str(), sizeof(msgs[count].timestamp) - 1);
            msgs[count].timestamp[sizeof(msgs[count].timestamp) - 1] = '\0';

            if (cfgId.length() > 0) {
                msgs[count].id = (int)strtol(cfgId.c_str() + 3, nullptr, 16);
            }
            count++;
        }

        searchPos = rowEnd + 5;
    }

    if (count > 0) {
        Serial.printf("[SMS] Polled %d message(s)\n", count);
    }
    return count;
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

    if (httpCode == HTTP_CODE_OK || httpCode == 200 || httpCode == 302) {
        Serial.printf("[SMS] Deleted message %s\n", cfgBuf);
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
    sysauthCookie = "";
    csrfToken = "";
    Serial.printf("[SMS] Gateway init — router: %s\n", routerIp);
}

void smsGatewaySetCredentials(const char* ip, const char* user, const char* pass)
{
    strncpy(routerIp, ip, sizeof(routerIp) - 1);
    strncpy(routerUser, user, sizeof(routerUser) - 1);
    strncpy(routerPass, pass, sizeof(routerPass) - 1);
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
