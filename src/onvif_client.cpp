#include "onvif_client.h"
#include "config_manager.h"
#include "zone_manager.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include "logging.h"
#include <mbedtls/sha1.h>
#include <base64.h>
#include "string_utils.h"
#include <esp_sntp.h>
#include <esp_task_wdt.h>

static const char* TAG = "ONVIF";

static const char* SOAP_ENV_START = 
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:a=\"http://www.w3.org/2005/08/addressing\" "
    "xmlns:e=\"http://www.onvif.org/ver10/events/wsdl\">";

static const char* SOAP_ENV_END = "</s:Envelope>";

struct OnvifState {
    char host[64];
    uint16_t port;
    char user[32];
    char pass[32];
    uint8_t targetZone;
    bool connected;
    String subscriptionAddress;
    uint32_t lastRenewMs;
    TaskHandle_t taskHandle;
    SemaphoreHandle_t mutex;
};

OnvifService* OnvifService::_instance = nullptr;

OnvifService::OnvifService() : _zones(nullptr), _state(new OnvifState()) {
    _instance = this;
    memset(_state, 0, sizeof(OnvifState));
    _state->port = 80;
    _state->mutex = xSemaphoreCreateMutex();
}

OnvifService::~OnvifService() {
    if (_state->mutex) vSemaphoreDelete(_state->mutex);
    delete _state;
}

static String getTimestamp() {
    time_t now;
    struct tm timeinfo;
    time(&now);
    gmtime_r(&now, &timeinfo);
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET) {
        return "";
    }
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(buf);
}

static String generateDigest(const char* nonce, const char* created, const char* password) {
    unsigned char hash[20];
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, (const unsigned char*)nonce, 20);
    mbedtls_sha1_update(&ctx, (const unsigned char*)created, strlen(created));
    mbedtls_sha1_update(&ctx, (const unsigned char*)password, strlen(password));
    mbedtls_sha1_finish(&ctx, hash);
    mbedtls_sha1_free(&ctx);
    return base64::encode(hash, 20);
}

String getAuthHeaderInternal(OnvifState* state) {
    unsigned char rawNonce[20];
    for(int i=0; i<20; i++) rawNonce[i] = (unsigned char)random(256);
    String nonceB64 = base64::encode(rawNonce, 20);
    String created = getTimestamp();
    if (created.length() == 0) return "";
    String digest = generateDigest((const char*)rawNonce, created.c_str(), state->pass);
    char headerBuf[512];
    snprintf(headerBuf, sizeof(headerBuf),
        "<s:Header><Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">"
        "<UsernameToken><Username>%s</Username>"
        "<Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">%s</Password>"
        "<Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">%s</Nonce>"
        "<Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">%s</Created>"
        "</UsernameToken></Security></s:Header>",
        state->user, digest.c_str(), nonceB64.c_str(), created.c_str());
    return String(headerBuf);
}

/**
 * StreamScanner: Zero-allocation state machine for finding patterns in a stream.
 * Mitigates the "XML Heap Detonator" by avoiding sliding window memmoves and 
 * temporary String/buffer allocations.
 */
class StreamScanner {
public:
    StreamScanner(const char* pattern) : _pattern(pattern), _len(strlen(pattern)), _pos(0) {}

    bool feed(char c) {
        if (tolower(c) == tolower(_pattern[_pos])) {
            _pos++;
            if (_pos == _len) {
                _pos = 0; // Reset for next potential match
                return true;
            }
        } else {
            // Brute force reset - for small patterns this is fine.
            // If the char matches the START of the pattern, start from 1.
            _pos = (tolower(c) == tolower(_pattern[0])) ? 1 : 0;
        }
        return false;
    }

    void reset() { _pos = 0; }

private:
    const char* _pattern;
    size_t _len;
    size_t _pos;
};

static bool createSubscriptionInternal(OnvifState* state) {
    if (strlen(state->host) == 0) return false;
    HTTPClient http;
    
    // Use fixed buffer for URL to avoid heap churn
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d/onvif/event_service", state->host, state->port);
    http.begin(url);
    http.setTimeout(2500);
    http.addHeader("Content-Type", "application/soap+xml; charset=utf-8");

    String auth = getAuthHeaderInternal(state);
    if (auth.length() == 0) { http.end(); return false; }

    // Construct body using pre-defined parts to minimize String reallocations
    String body;
    body.reserve(1024);
    body += SOAP_ENV_START;
    body += auth;
    body += "<s:Body><e:CreatePullPointSubscription/></s:Body>";
    body += SOAP_ENV_END;

    int code = http.POST(body);
    if (code == 200) {
        WiFiClient* stream = http.getStreamPtr();
        if (stream) {
            StreamScanner startTag("<tt:Address>");
            StreamScanner endTag("</tt:Address>");
            
            char addrBuf[128];
            int addrPos = 0;
            bool inAddress = false;
            bool foundAddress = false;
            unsigned long scraperStart = millis();

            while (http.connected() && !foundAddress) {
                if (millis() - scraperStart > 5000) break;
                int available = stream->available();
                if (available > 0) {
                    uint8_t tempBuf[128];
                    int n = stream->read(tempBuf, std::min((int)sizeof(tempBuf), available));
                    if (n <= 0) break;

                    for (int i = 0; i < n && !foundAddress; i++) {
                        char c = (char)tempBuf[i];
                        if (!inAddress) {
                            if (startTag.feed(c)) {
                                inAddress = true;
                                addrPos = 0;
                            }
                        } else {
                            if (endTag.feed(c)) {
                                addrBuf[addrPos] = '\0';
                                state->subscriptionAddress = String(addrBuf);
                                foundAddress = true;
                            } else if (addrPos < (int)sizeof(addrBuf) - 1) {
                                addrBuf[addrPos++] = c;
                            } else {
                                inAddress = false;
                                startTag.reset();
                                endTag.reset();
                            }
                        }
                    }
                } else {
                    vTaskDelay(1);
                }
            }

            if (foundAddress) {
                state->connected = true;
                LOG_INFO(TAG, "Subscription created: %s", state->subscriptionAddress.c_str());
                http.end();
                return true;
            }
        }
    }
    LOG_WARN(TAG, "Subscription failed, code: %d", code);
    http.end();
    state->subscriptionAddress = "";
    state->connected = false;
    return false;
}

static void pollMessagesInternal(OnvifState* state) {
    if (!state->connected || state->subscriptionAddress.length() == 0) return;
    HTTPClient http;
    http.begin(state->subscriptionAddress);
    http.addHeader("Content-Type", "application/soap+xml; charset=utf-8");
    http.setTimeout(5000);

    String auth = getAuthHeaderInternal(state);
    if (auth.length() == 0) { http.end(); return; }

    // Minimize String reallocations
    String body;
    body.reserve(1024);
    body += SOAP_ENV_START;
    body += auth;
    body += "<s:Body><e:PullMessages><e:Timeout>PT2S</e:Timeout><e:MessageLimit>10</e:MessageLimit></e:PullMessages></s:Body>";
    body += SOAP_ENV_END;

    int code = http.POST(body);
    if (code == 200) {
        WiFiClient* stream = http.getStreamPtr();
        if (!stream) { http.end(); return; }

        StreamScanner isMotion("IsMotion");
        StreamScanner valueTrue("Value=\"true\"");
        
        bool motion = false;
        bool foundIsMotion = false;
        unsigned long timeoutMs = millis();

        while (http.connected() || stream->available()) {
            if (millis() - timeoutMs > 5000) break;
            if (stream->available()) {
                timeoutMs = millis();
                char c = (char)stream->read();
                
                if (!foundIsMotion) {
                    if (isMotion.feed(c)) foundIsMotion = true;
                } else {
                    if (valueTrue.feed(c)) {
                        motion = true;
                        break;
                    }
                }
            } else vTaskDelay(pdMS_TO_TICKS(10));
        }
        // Drain remaining stream
        while (stream->available()) stream->read();

        if (state->connected && OnvifService::_instance && OnvifService::_instance->_zones) {
            OnvifService::_instance->_zones->setVirtualInput(state->targetZone, motion);
        }
        if (motion) LOG_INFO(TAG, "Motion detected on camera!");
    } else if (code > 0) {
        if (code == 401) LOG_ERROR(TAG, "ONVIF Auth failed");
        if (code == 400 || code == 404 || code == 500) state->connected = false;
    }
    http.end();
}

void OnvifService::init(ZoneManager* zones) {
    _zones = zones;
    xTaskCreatePinnedToCore(onvifTask, "ONVIF_Poll", 8192, this, 1, &_state->taskHandle, 0);
    LOG_INFO(TAG, "ONVIF Service initialized");
}

void OnvifService::setServer(const char* host, uint16_t port, const char* user, const char* pass, uint8_t zone) {
    if (xSemaphoreTake(_state->mutex, portMAX_DELAY) == pdTRUE) {
        strncpy(_state->host, host, sizeof(_state->host)-1);
        _state->port = port;
        strncpy(_state->user, user, sizeof(_state->user)-1);
        strncpy(_state->pass, pass, sizeof(_state->pass)-1);
        _state->targetZone = (zone > 0 && zone <= MAX_ZONES) ? zone - 1 : 0;
        _state->connected = false;
        _state->subscriptionAddress = "";
        _state->lastRenewMs = 0;
        xSemaphoreGive(_state->mutex);
    }
}

void OnvifService::disconnect() {
    if (xSemaphoreTake(_state->mutex, portMAX_DELAY) == pdTRUE) {
        _state->connected = false;
        scrubBuffer(_state->pass, sizeof(_state->pass));
        xSemaphoreGive(_state->mutex);
    }
}

bool OnvifService::isConnected() { return _state->connected; }
const char* OnvifService::getHost() { return _state->host; }
uint16_t OnvifService::getPort() { return _state->port; }
const char* OnvifService::getUser() { return _state->user; }
const char* OnvifService::getPass() { return _state->pass; }
uint8_t OnvifService::getTargetZone() { return _state->targetZone + 1; }

void OnvifService::onvifTask(void* pvParameters) {
    OnvifService* self = (OnvifService*)pvParameters;
    esp_task_wdt_add(NULL);
    while (true) {
        esp_task_wdt_reset();
        if (xSemaphoreTake(self->_state->mutex, portMAX_DELAY) == pdTRUE) {
            bool hasHost = (strlen(self->_state->host) > 0);
            bool isConnected = self->_state->connected;
            uint32_t lastRenew = self->_state->lastRenewMs;
            xSemaphoreGive(self->_state->mutex);

            if (WiFi.status() != WL_CONNECTED || !hasHost) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }

            uint32_t now = millis();
            if (!isConnected) {
                if (createSubscriptionInternal(self->_state)) {
                    if (xSemaphoreTake(self->_state->mutex, portMAX_DELAY) == pdTRUE) {
                        self->_state->lastRenewMs = now;
                        xSemaphoreGive(self->_state->mutex);
                    }
                } else vTaskDelay(pdMS_TO_TICKS(5000));
            } else {
                if (now - lastRenew > 50000) {
                    if (!createSubscriptionInternal(self->_state)) {
                        if (xSemaphoreTake(self->_state->mutex, portMAX_DELAY) == pdTRUE) {
                            self->_state->connected = false;
                            xSemaphoreGive(self->_state->mutex);
                        }
                        continue;
                    }
                    if (xSemaphoreTake(self->_state->mutex, portMAX_DELAY) == pdTRUE) {
                        self->_state->lastRenewMs = now;
                        xSemaphoreGive(self->_state->mutex);
                    }
                }
                pollMessagesInternal(self->_state);
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }
}
