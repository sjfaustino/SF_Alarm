#include "sms_gateway.h"
#include "config.h"
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>
#include "notification_manager.h"
#include "network.h"
#include "logging.h"
#include <esp_task_wdt.h>
#include "string_utils.h"
#include "html_utils.h"
#include "sms_command_processor.h"

static const char* TAG = "SMS";
static const char* UA = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36";

// ---------------------------------------------------------------------------
// AI-Thinker A6 GSM Implementation (Skeleton)
// ---------------------------------------------------------------------------
class A6SmsGateway : public ISmsGateway {
private:
    char _host[32], _user[32], _pass[32], _lastError[64];
    SmsCommandProcessor* _processor;
    HardwareSerial* _gsmSerial;
    int _txPin, _rxPin;
    bool _ready;
    unsigned long _lastPoll;
    
    bool sendAT(const char* cmd, const char* expected = "OK", uint32_t timeout = 2000) {
        if (!_gsmSerial) return false;
        while (_gsmSerial->available()) _gsmSerial->read(); // Flush
        
        LOG_INFO("GSM", "AT CMD: %s", cmd);
        _gsmSerial->println(cmd);
        
        unsigned long start = millis();
        String response = "";
        while (millis() - start < timeout) {
            if (_gsmSerial->available()) {
                String line = _gsmSerial->readStringUntil('\n');
                line.trim();
                if (line.length() > 0) {
                    LOG_INFO("GSM", "  Resp: %s", line.c_str());
                    if (line.indexOf(expected) != -1) return true;
                    if (line.indexOf("ERROR") != -1) return false;
                }
            }
            vTaskDelay(1);
        }
        return false;
    }

public:
    A6SmsGateway() : _processor(nullptr), _gsmSerial(nullptr), _txPin(DEFAULT_GSM_TX_PIN), _rxPin(DEFAULT_GSM_RX_PIN), _ready(false), _lastPoll(0) {
        memset(_host, 0, sizeof(_host));
        memset(_user, 0, sizeof(_user));
        memset(_pass, 0, sizeof(_pass));
        memset(_lastError, 0, sizeof(_lastError));
    }

    void init(const char* host, const char* user, const char* pass, SmsCommandProcessor* processor) override {
        _processor = processor;
        setCredentials(host, user, pass);
        
        if (!_gsmSerial) {
            _gsmSerial = new HardwareSerial(1); // Use UART1
            _gsmSerial->begin(GSM_BAUD_RATE, SERIAL_8N1, _rxPin, _txPin);
        }

        LOG_INFO("GSM", "Initializing A6 GSM Module...");
        
        // Basic Init Sequence
        int retry = 0;
        while (retry < 5) {
            if (sendAT("AT")) break;
            vTaskDelay(pdMS_TO_TICKS(500));
            retry++;
        }
        
        sendAT("AT+CMGF=1"); // Text mode
        sendAT("AT+CSMP=17,167,0,0"); // Text mode params
        sendAT("AT+CNMI=2,1,0,0,0"); // New message indication
        
        _ready = true;
        LOG_INFO("GSM", "A6 GSM Gateway Ready");
    }

    void setCredentials(const char* host, const char* user, const char* pass) override {
        if (host) strncpy(_host, host, sizeof(_host)-1);
        if (user) strncpy(_user, user, sizeof(_user)-1);
        if (pass) strncpy(_pass, pass, sizeof(_pass)-1);
    }

    bool send(const char* phone, const char* msg) override {
        if (!_ready) return false;
        
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "AT+CMGS=\"%s\"", phone);
        
        if (!_gsmSerial) return false;
        while (_gsmSerial->available()) _gsmSerial->read();
        
        _gsmSerial->println(cmd);
        
        // Wait for prompt '>'
        unsigned long start = millis();
        bool foundPrompt = false;
        while (millis() - start < 3000) {
            if (_gsmSerial->available()) {
                if (_gsmSerial->read() == '>') {
                    foundPrompt = true;
                    break;
                }
            }
            vTaskDelay(1);
        }
        
        if (!foundPrompt) return false;
        
        _gsmSerial->print(msg);
        _gsmSerial->write(26); // Ctrl+Z
        
        return true; // Simplified for now, should ideally wait for OK
    }

    int pollInbox(SmsMessage* msgs, int max) override {
        if (!_ready) return 0;
        // Simplified polling - in reality would parse AT+CMGL="ALL"
        return 0; 
    }

    int pollSent(SmsMessage* msgs, int max) override { return 0; }
    
    bool deleteMessage(int id) override {
        char cmd[32];
        snprintf(cmd, sizeof(cmd), "AT+CMGD=%d", id);
        return sendAT(cmd);
    }

    void update() override {
        if (!_ready || !_gsmSerial) return;
        
        // Check for unsolicited results like +CMTI: "SM",1
        if (_gsmSerial->available()) {
            String line = _gsmSerial->readStringUntil('\n');
            line.trim();
            if (line.indexOf("+CMTI:") != -1) {
                LOG_INFO("GSM", "New SMS Notification: %s", line.c_str());
                // Trigger an inbox poll or processing
            }
        }
    }

    bool isReady() override { return _ready; }
    const char* getLastError() override { return _lastError; }
    const char* getHost() override { return _host; }
    const char* getUser() override { return _user; }
    const char* getPass() override { return _pass; }

    bool execCommand(const char* cmd, char* response, size_t maxLen) override {
        if (!_gsmSerial) return false;
        while (_gsmSerial->available()) _gsmSerial->read();
        
        _gsmSerial->println(cmd);
        
        unsigned long start = millis();
        size_t pos = 0;
        if (response) response[0] = '\0';

        while (millis() - start < 2000) {
            if (_gsmSerial->available()) {
                int c = _gsmSerial->read();
                if (c >= 0 && response && pos < maxLen - 1) {
                    response[pos++] = (char)c;
                    response[pos] = '\0';
                }
            }
            vTaskDelay(1);
        }
        return pos > 0;
    }
};

// ---------------------------------------------------------------------------
// LuCI (Router) Implementation
// ---------------------------------------------------------------------------
class LuciSmsGateway : public ISmsGateway {
private:
    SmsCommandProcessor* _processor;
public:
    LuciSmsGateway() : _processor(nullptr), _loggedIn(false), _smsMutex(NULL) {
        memset(_routerIp, 0, sizeof(_routerIp));
        memset(_routerUser, 0, sizeof(_routerUser));
        memset(_routerPass, 0, sizeof(_routerPass));
        memset(_lastError, 0, sizeof(_lastError));
    }

    void init(const char* host, const char* user, const char* pass, SmsCommandProcessor* processor) override {
        _processor = processor;
        setCredentials(host, user, pass);
        if (_smsMutex == NULL) {
            _smsMutex = xSemaphoreCreateMutex();
        }
        _sysauthCookie = "";
        _csrfToken = "";
    }

    void setCredentials(const char* host, const char* user, const char* pass) override {
        // Scrub old sensitive data
        memset(_routerIp, 0, sizeof(_routerIp));
        memset(_routerUser, 0, sizeof(_routerUser));
        memset(_routerPass, 0, sizeof(_routerPass));
        _sysauthCookie = "";
        _csrfToken = "";
        _loggedIn = false;

        if (host) strncpy(_routerIp, host, sizeof(_routerIp) - 1);
        if (user) strncpy(_routerUser, user, sizeof(_routerUser) - 1);
        if (pass) strncpy(_routerPass, pass, sizeof(_routerPass) - 1);
        
        LOG_INFO(TAG, "Credentials updated — router: %s", _routerIp);
    }

    bool send(const char* phoneNumber, const char* message) override {
        if (!_loggedIn && !login()) return false;

        for (int attempt = 0; attempt < 3; attempt++) {
            HTTPClient httpGet;
            char getterUrl[128];
            buildUrlStr("/cgi-bin/luci/admin/network/gcom/sms/smsnew?nomodal=&iface=4g", getterUrl, sizeof(getterUrl));

            httpGet.begin(getterUrl);
            httpGet.setUserAgent(UA);
            httpGet.addHeader("Cookie", String("sysauth=") + _sysauthCookie);
            httpGet.addHeader("Referer", String("http://") + _routerIp + "/cgi-bin/luci/admin/network/gcom/sms?iface=4g");
            httpGet.setTimeout(3000);
            
            esp_task_wdt_reset();
            int getCode = httpGet.GET();
            if (getCode != 200 && getCode != 403) {
                setError("Send GET error: %d (attempt %d)", getCode, attempt + 1);
                httpGet.end();
                if (getCode == 401) {
                    cleanup();
                    if (login()) continue;
                    return false;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

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
                            memmove(buffer, buffer + (sizeof(buffer) - 128 - 1), 128);
                            bufPos = 128;
                            buffer[bufPos] = '\0';
                        }
                    } else vTaskDelay(pdMS_TO_TICKS(10));
                }
            }
            if (sendToken.length() == 0) {
                setError("No token on smsnew page (attempt %d)", attempt + 1);
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }

            HTTPClient httpPost;
            char targetUrl[128];
            buildUrlStr("/cgi-bin/luci/admin/network/gcom/sms/smsnew?nomodal=&iface=4g", targetUrl, sizeof(targetUrl));
            httpPost.begin(targetUrl);
            httpPost.setUserAgent(UA);
            httpPost.addHeader("Content-Type", "application/x-www-form-urlencoded");
            httpPost.addHeader("Cookie", String("sysauth=") + _sysauthCookie);
            httpPost.addHeader("Origin", String("http://") + _routerIp);
            httpPost.addHeader("Referer", String("http://") + _routerIp + "/cgi-bin/luci/admin/network/gcom/sms?iface=4g");
            httpPost.setTimeout(5000);

            char snapshot[128];
            strncpy(snapshot, sendToken.c_str(), sizeof(snapshot)-1);
            snapshot[sizeof(snapshot)-1] = '\0';
            scrubString(sendToken);

            char encodedMsg[1024];
            urlEncodeTo(message, encodedMsg, sizeof(encodedMsg));
            for (int i = 0; encodedMsg[i] != '\0'; i++) {
                if (encodedMsg[i] == '%' && encodedMsg[i+1] == '2' && encodedMsg[i+2] == '0') {
                    encodedMsg[i] = '+';
                    memmove(&encodedMsg[i+1], &encodedMsg[i+3], strlen(&encodedMsg[i+3]) + 1);
                }
            }

            char postData[2048];
            snprintf(postData, sizeof(postData), 
                     "token=%s&timeclock=0&cbid.smsnew.1.phone=%s&cbid.smsnew.1.content=%s&cbi.submit=1&cbid.smsnew.1.send=Send",
                     snapshot, phoneNumber, encodedMsg);
            memset(snapshot, 0, sizeof(snapshot));

            int httpCode = httpPost.POST(postData);
            if (httpCode == 200 || httpCode == 302) {
                httpPost.end();
                LOG_INFO(TAG, "Sent to %s: \"%s\"", phoneNumber, message);
                return true;
            } else if (httpCode == 403 || httpCode == 401) {
                httpPost.end();
                cleanup();
                if (login()) continue;
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        return false;
    }

    int pollInbox(SmsMessage* msgs, int maxMessages) override {
        return pollMessages(msgs, maxMessages, "rec");
    }

    int pollSent(SmsMessage* msgs, int maxMessages) override {
        return pollMessages(msgs, maxMessages, "sto");
    }

    bool deleteMessage(int messageId) override {
        if (!_loggedIn && !login()) return false;

        char cfgBuf[16];
        snprintf(cfgBuf, sizeof(cfgBuf), "cfg%06x", (unsigned int)messageId);

        HTTPClient http;
        char url[128];
        buildUrlStr((String("/cgi-bin/luci/admin/network/gcom/sms/delsms?iface=4g&cfg=") + cfgBuf).c_str(), url, sizeof(url));

        http.begin(url);
        http.setUserAgent(UA);
        http.addHeader("Cookie", String("sysauth=") + _sysauthCookie);
        http.addHeader("Origin", String("http://") + _routerIp);
        http.addHeader("Referer", String("http://") + _routerIp + "/cgi-bin/luci/admin/network/gcom/sms?iface=4g");
        http.setTimeout(10000);

        int code = http.GET();
        http.end();

        if (code == 401 || code == 403) {
            _loggedIn = false;
            if (login()) {
                http.begin(url);
                http.addHeader("Cookie", String("sysauth=") + _sysauthCookie);
                code = http.GET();
                http.end();
                if (code == 200 || code == 302) return true;
            }
            return false;
        }
        return (code == 200 || code == 302);
    }

    void update() override {
        if (!networkIsConnected()) return;
        if (!_loggedIn && !login()) return;

        SmsMessage msgs[5];
        int count = pollInbox(msgs, 5);
        for (int i = 0; i < count; i++) {
            if (_processor) _processor->process(msgs[i].sender, msgs[i].body);
            deleteMessage(msgs[i].id);
            esp_task_wdt_reset();
        }
    }

    bool isReady() override { return _loggedIn; }
    const char* getLastError() override { return _lastError; }
    const char* getHost() override { return _routerIp; }
    const char* getUser() override { return _routerUser; }
    const char* getPass() override { return _routerPass; }

private:
    char _routerIp[64];
    char _routerUser[32];
    char _routerPass[64];
    String _sysauthCookie;
    String _csrfToken;
    String _sessionStok;
    bool _loggedIn;
    char _lastError[128];
    SemaphoreHandle_t _smsMutex;

    void setError(const char* fmt, ...) {
        if (_smsMutex && xSemaphoreTake(_smsMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            va_list args;
            va_start(args, fmt);
            vsnprintf(_lastError, sizeof(_lastError), fmt, args);
            va_end(args);
            xSemaphoreGive(_smsMutex);
        }
    }

    void buildUrlStr(const char* path, char* dest, size_t maxLen) {
        if (_sessionStok.length() > 0 && strncmp(path, "/cgi-bin/luci/", 14) == 0) {
            snprintf(dest, maxLen, "http://%s/cgi-bin/luci/;stok=%s/%s", _routerIp, _sessionStok.c_str(), path + 14);
        } else {
            snprintf(dest, maxLen, "http://%s%s", _routerIp, path);
        }
    }

    bool login() {
        if (strlen(_routerIp) == 0) return false;
        HTTPClient http;
        char url[128];
        buildUrlStr("/cgi-bin/luci/", url, sizeof(url));
        http.begin(url);
        http.setTimeout(10000);
        http.setUserAgent(UA);
        http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);

        int code = http.GET();
        if (code != 200 && code != 403) { http.end(); return false; }

        WiFiClient* stream = http.getStreamPtr();
        if (!stream) { http.end(); return false; }

        String _scsrf = "", salt = "", token = "", zonename = "", timeclock = "";
        char buffer[512];
        int bufPos = 0;
        unsigned long start = millis();
        while (http.connected() && (_scsrf == "" || salt == "" || token == "")) {
            if (millis() - start > 10000) break;
            int available = stream->available();
            if (available > 0) {
                int toRead = std::min(available, (int)(sizeof(buffer) - bufPos - 1));
                int read = stream->read((uint8_t*)buffer + bufPos, toRead);
                if (read <= 0) break;

                int oldPos = bufPos;
                bufPos += read;
                buffer[bufPos] = '\0';

                for (int i = oldPos; i < bufPos; i++) {
                    if (buffer[i] == '>') {
                        int tS, tE;
                        if (HtmlUtils::findTag(buffer, "input", tS, tE)) {
                            String tag = String(buffer).substring(tS, tE + 1);
                            String name = HtmlUtils::getAttribute(tag, "name");
                            String value = HtmlUtils::getAttribute(tag, "value");
                            if (name == "_csrf") _scsrf = value;
                            else if (name == "salt") salt = value;
                            else if (name == "token") token = value;
                            else if (name == "zonename") zonename = value;
                            else if (name == "timeclock") timeclock = value;
                        }
                    }
                }

                if (bufPos >= (int)sizeof(buffer) - 128) {
                    memmove(buffer, buffer + (bufPos - 128), 128);
                    bufPos = 128;
                    buffer[bufPos] = '\0';
                }
            } else {
                vTaskDelay(1);
            }
        }
        http.end();

        if (_scsrf == "" || salt == "" || token == "") return false;

        String hash1 = sha256Hex(String(_routerPass) + salt);
        String finalHash = sha256Hex(hash1 + token);

        HTTPClient http2;
        buildUrlStr("/cgi-bin/luci/", url, sizeof(url));
        http2.begin(url);
        http2.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/145.0.0.0 Safari/537.36");
        http2.addHeader("Content-Type", "application/x-www-form-urlencoded");
        http2.addHeader("Referer", url);
        char origin[128];
        snprintf(origin, sizeof(origin), "http://%s", _routerIp);
        http2.addHeader("Origin", origin);
        http2.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
        const char* hKeys[] = {"Set-Cookie", "Location"};
        http2.collectHeaders(hKeys, 2);

        char post[1024];
        snprintf(post, sizeof(post), "_csrf=%s&token=%s&salt=%s&zonename=%s&timeclock=%s&luci_language=auto&luci_username=%s&luci_password=%s",
                 _scsrf.c_str(), token.c_str(), salt.c_str(), zonename.c_str(), timeclock.c_str(), _routerUser, finalHash.c_str());
        
        int pCode = http2.POST(post);
        if (pCode == 302 || pCode == 301 || pCode == 200) {
            String cookies = http2.header("Set-Cookie");
            if (cookies.indexOf("sysauth=") >= 0) {
                int s = cookies.indexOf("sysauth=") + 8;
                int e = cookies.indexOf(';', s);
                if (e < 0) e = cookies.length();
                _sysauthCookie = cookies.substring(s, e);
            }
            String loc = http2.header("Location");
            if (loc.indexOf(";stok=") >= 0) {
                int s = loc.indexOf(";stok=") + 6;
                int e = loc.indexOf('/', s);
                if (e < 0) e = loc.length();
                _sessionStok = loc.substring(s, e);
            }
        }
        http2.end();

        _loggedIn = (_sysauthCookie.length() > 0);
        return _loggedIn;
    }

    void cleanup() {
        scrubString(_sysauthCookie);
        _loggedIn = false;
    }

    int pollMessages(SmsMessage* msgs, int max, const char* box) {
        if (!_loggedIn && !login()) return 0;
        HTTPClient http;
        char url[128];
        buildUrlStr((String("/cgi-bin/luci/admin/network/gcom/sms/smslist?smsbox=") + box + "&iface=4g").c_str(), url, sizeof(url));
        http.begin(url);
        http.setUserAgent(UA);
        http.addHeader("Cookie", String("sysauth=") + _sysauthCookie);
        http.addHeader("Origin", String("http://") + _routerIp);
        http.addHeader("Referer", String("http://") + _routerIp + "/cgi-bin/luci/admin/network/gcom/sms?iface=4g");
        int code = http.GET();
        if (code != 200) { http.end(); return 0; }
        WiFiClient* stream = http.getStreamPtr();
        if (!stream) { http.end(); return 0; }
        char win[1024]; int winP = 0; int count = 0;
        while ((http.connected() || stream->available()) && count < max) {
            int available = stream->available();
            if (available > 0) {
                int toRead = std::min(available, (int)(sizeof(win) - winP - 1));
                int nRead = stream->read((uint8_t*)win + winP, toRead);
                if (nRead <= 0) break;

                int oldP = winP;
                winP += nRead;
                win[winP] = '\0';

                // Process new content for row endings
                for (int i = oldP; i < winP; i++) {
                    if (win[i] == '>') {
                        if (strcasestr(win, "</tr>")) {
                            char* row = strcasestr(win, "<tr class=\"cbi-section-table-row");
                            if (row) {
                                SmsMessage m; memset(&m, 0, sizeof(m));
                                int tdI = 0; char* td = strcasestr(row, "<td");
                                int off = (strcmp(box, "rec") == 0) ? 1 : 0;
                                while (td && tdI < 7) {
                                    char* cS = strchr(td, '>'); if (!cS) break; cS++;
                                    char* cE = strcasestr(cS, "</td>"); if (!cE) break;
                                    int len = cE - cS; char cell[128];
                                    if (len >= (int)sizeof(cell)) len = sizeof(cell)-1;
                                    strncpy(cell, cS, len); cell[len] = '\0';
                                    if (tdI == 1 + off) strncpy(m.sender, cell, sizeof(m.sender)-1);
                                    else if (tdI == 2 + off) strncpy(m.body, cell, sizeof(m.body)-1);
                                    else if (tdI == 3 + off) strncpy(m.timestamp, cell, sizeof(m.timestamp)-1);
                                    else if (tdI >= 4 + off) {
                                        char* cfg = strcasestr(cell, "cfg=");
                                        if (cfg && m.id == 0) m.id = (int)strtol(cfg + 7, nullptr, 16);
                                    }
                                    td = strcasestr(cE, "<td"); tdI++;
                                }
                                if (strlen(m.sender) > 0) memcpy(&msgs[count++], &m, sizeof(SmsMessage));
                            }
                            // Reset window after processing a row
                            winP = 0; win[0] = '\0';
                            break; // Row processed, buffer cleared
                        }
                    }
                }

                if (winP >= (int)sizeof(win)-256) { memmove(win, win+512, 512); winP = 512; win[winP] = '\0'; }
            } else {
                vTaskDelay(1);
            }
        }
        http.end();
        return count;
    }

    String sha256Hex(const String& input) {
        unsigned char hash[32]; mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, (const unsigned char*)input.c_str(), input.length());
        mbedtls_sha256_finish(&ctx, hash); mbedtls_sha256_free(&ctx);
        char hex[65]; for (int i = 0; i < 32; i++) snprintf(hex + i*2, 3, "%02x", hash[i]);
        return String(hex);
    }

    size_t urlEncodeTo(const char* src, char* dest, size_t size) {
        static const char *h = "0123456789ABCDEF"; size_t d = 0;
        while (*src && (d < size - 1)) {
            uint8_t c = (uint8_t)*src++;
            if (isalnum(c) || strchr("-_.~", c)) dest[d++] = (char)c;
            else { if (d + 3 >= size) break; dest[d++] = '%'; dest[d++] = h[c >> 4]; dest[d++] = h[c & 0x0F]; }
        }
        dest[d] = '\0'; return d;
    }

    bool execCommand(const char* cmd, char* response, size_t maxLen) override {
        if (response) strncpy(response, "Not supported on LuCI gateway", maxLen);
        return false;
    }
};

// ---------------------------------------------------------------------------
// SmsService Implementation
// ---------------------------------------------------------------------------
SmsService* SmsService::_instance = nullptr;

SmsService::SmsService() : _nm(nullptr), _gateway(new LuciSmsGateway()) {
    _instance = this;
}

SmsService::~SmsService() {
    delete _gateway;
}

void SmsService::init(NotificationManager* nm, SmsCommandProcessor* processor) {
    _nm = nm;
    _processor = processor;
    _nm->registerProvider(CH_SMS, this);
    if (_gateway) _gateway->init(nullptr, nullptr, nullptr, processor);
    LOG_INFO(TAG, "SMS Service initialized");
}

void SmsService::update() {
    _gateway->update();
}

void SmsService::setCredentials(const char* ip, const char* user, const char* pass) {
    _gateway->setCredentials(ip, user, pass);
}

void SmsService::setProvider(SmsProvider prov) {
    const char* ip = _gateway->getHost();
    const char* user = _gateway->getUser();
    const char* pass = _gateway->getPass();

    delete _gateway;

    if (prov == SMS_GSM_A6) {
        _gateway = new A6SmsGateway();
    } else {
        _gateway = new LuciSmsGateway();
    }

    _gateway->init(ip, user, pass, _processor);
    LOG_INFO(TAG, "SMS Provider switched to: %s", (prov == SMS_GSM_A6) ? "A6 GSM" : "LuCI Router");
}

bool SmsService::send(const char* phone, const char* msg) {
    return _gateway->send(phone, msg);
}

int SmsService::pollInbox(SmsMessage* msgs, int max) {
    return _gateway->pollInbox(msgs, max);
}

int SmsService::pollSent(SmsMessage* msgs, int max) {
    return _gateway->pollSent(msgs, max);
}

bool SmsService::deleteMessage(int id) {
    return _gateway->deleteMessage(id);
}

bool SmsService::isLoggedIn() {
    return _gateway->isReady();
}

bool SmsService::isReady() const {
    return _gateway->isReady();
}

const char* SmsService::getLastError() {
    return _gateway->getLastError();
}

const char* SmsService::getRouterIp() { return _gateway->getHost(); }
const char* SmsService::getRouterUser() { return _gateway->getUser(); }
const char* SmsService::getRouterPass() { return _gateway->getPass(); }

bool SmsService::execCommand(const char* cmd, char* resp, size_t max) {
    return _gateway->execCommand(cmd, resp, max);
}
