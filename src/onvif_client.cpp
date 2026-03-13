#include "onvif_client.h"
#include "config_manager.h"
#include "alarm_zones.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>
#include "logging.h"
#include <mbedtls/sha1.h>
#include <base64.h>

static const char* TAG = "ONVIF";

// ---------------------------------------------------------------------------
// Constants & Templates
// ---------------------------------------------------------------------------

#include <esp_task_wdt.h>

static const char* SOAP_ENV_START = 
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<s:Envelope xmlns:s=\"http://www.w3.org/2003/05/soap-envelope\" "
    "xmlns:a=\"http://www.w3.org/2005/08/addressing\" "
    "xmlns:e=\"http://www.onvif.org/ver10/events/wsdl\">";

static const char* SOAP_ENV_END = "</s:Envelope>";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------

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
};

static OnvifState state = {0};
static SemaphoreHandle_t stateMutex = NULL;

// Forward declaration of the task
static void onvifTask(void* pvParameters);

// ---------------------------------------------------------------------------
// Security Helpers
// ---------------------------------------------------------------------------

static String getTimestamp() {
    // We use the ESP32 internal SNTP/RTC time if synchronized, 
    // otherwise we fallback to the hardcoded base + uptime logic for a valid ISO string.
    time_t now;
    struct tm timeinfo;
    time(&now);
    gmtime_r(&now, &timeinfo);
    
    // If the year is very low (e.g. 1970), it means NTP hasn't synced yet.
    // We apply our offset 1772956800ULL (2026-03-08) to make it "legal" for camera digest auth.
    if (timeinfo.tm_year < (2020 - 1900)) {
        now += 1772956800ULL;
        gmtime_r(&now, &timeinfo);
    }

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &timeinfo);
    return String(buf);
}

static String generateDigest(const char* nonce, const char* created, const char* password) {
    // nonce is binary here
    unsigned char hash[20];
    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    
    // Nonce (binary) + Created (string) + Password (string)
    mbedtls_sha1_update(&ctx, (const unsigned char*)nonce, 20);
    mbedtls_sha1_update(&ctx, (const unsigned char*)created, strlen(created));
    mbedtls_sha1_update(&ctx, (const unsigned char*)password, strlen(password));
    
    mbedtls_sha1_finish(&ctx, hash);
    mbedtls_sha1_free(&ctx);
    
    return base64::encode(hash, 20);
}

static String getAuthHeader() {
    unsigned char rawNonce[20];
    for(int i=0; i<20; i++) rawNonce[i] = (unsigned char)random(256);
    
    String nonceB64 = base64::encode(rawNonce, 20);
    String created = getTimestamp();
    String digest = generateDigest((const char*)rawNonce, created.c_str(), state.pass);

    String header;
    header.reserve(512); // Pre-allocate to prevent heap fragmentation
    header = "<s:Header>";
    header += "<Security s:mustUnderstand=\"1\" xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd\">";
    header += "<UsernameToken><Username>";
    header += state.user;
    header += "</Username><Password Type=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-username-token-profile-1.0#PasswordDigest\">";
    header += digest;
    header += "</Password><Nonce EncodingType=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-soap-message-security-1.0#Base64Binary\">";
    header += nonceB64;
    header += "</Nonce><Created xmlns=\"http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd\">";
    header += created;
    header += "</Created></UsernameToken></Security></s:Header>";
    return header;
}

// ---------------------------------------------------------------------------
// ONVIF Operations
// ---------------------------------------------------------------------------

static bool createSubscription() {
    if (strlen(state.host) == 0) return false;

    HTTPClient http;
    String url = "http://" + String(state.host) + ":" + String(state.port) + "/onvif/event_service";
    
    http.begin(url);
    http.setTimeout(2500); // Prevent initial connection stall (dead camera)
    http.addHeader("Content-Type", "application/soap+xml; charset=utf-8");

    String body;
    body.reserve(1024); // Pre-allocate to prevent heap fragmentation
    body = SOAP_ENV_START;
    body += getAuthHeader();
    body += "<s:Body><e:CreatePullPointSubscription/></s:Body>";
    body += SOAP_ENV_END;

    int code = http.POST(body);
    if (code == 200) {
        String res = http.getString();
        // Extract Address from SubscriptionReference
        // Simple search: <Address>...</Address>
        int start = res.indexOf("<tt:Address>");
        if (start != -1) {
            start += 12;
            int end = res.indexOf("</tt:Address>", start);
            state.subscriptionAddress = res.substring(start, end);
            state.connected = true;
            LOG_INFO(TAG, "Subscription created: %s", state.subscriptionAddress.c_str());
            http.end();
            return true;
        }
    }

    LOG_WARN(TAG, "Subscription failed, code: %d", code);
    http.end();
    return false;
}

static void pollMessages() {
    if (!state.connected || state.subscriptionAddress.length() == 0) return;

    HTTPClient http;
    http.begin(state.subscriptionAddress);
    http.addHeader("Content-Type", "application/soap+xml; charset=utf-8");
    http.setTimeout(5000); // 5s timeout for PullMessages

    String body;
    body.reserve(1024); // Pre-allocate to prevent heap fragmentation
    body = SOAP_ENV_START;
    body += getAuthHeader();
    body += "<s:Body><e:PullMessages><e:Timeout>PT2S</e:Timeout><e:MessageLimit>10</e:MessageLimit></e:PullMessages></s:Body>";
    body += SOAP_ENV_END;

    int code = http.POST(body);
    if (code == 200) {
        // --- THE XML HEAP DETONATOR FIX ---
        // DO NOT DO: String res = http.getString();
        // The XML payload from cameras can routinely exceed 64KB. Pulling it
        // fully into a contiguous RAM String array will instantly shatter the
        // ESP32 Heap and cause an Out-Of-Memory kernel crash.
        // We use a sliding-window stream scanner.
        
        WiFiClient* stream = http.getStreamPtr();
        if (!stream) {
            http.end();
            return;
        }
        
        char buffer[512];
        int bufferPos = 0;
        bool motion = false;
        unsigned long timeoutMs = millis();
        
        while (http.connected() || stream->available()) {
            if (millis() - timeoutMs > 5000) break; // Emergency timeout
            
            if (stream->available()) {
                timeoutMs = millis();
                // We read one byte at a time or in small chunks to the end of our fixed buffer
                int c = stream->read();
                if (c < 0) break;

                buffer[bufferPos++] = (char)c;
                buffer[bufferPos] = '\0';

                // Keyword detection: split "IsMotion" + "Value=\"true\""
                // We check for the combined pattern or fragments.
                // Improvement: check both fragments in the window to handle splits.
                if (strcasestr(buffer, "IsMotion") && strcasestr(buffer, "Value=\"true\"")) {
                    motion = true;
                    break;
                }

                // If buffer is full, shift it to keep the last 128 bytes (overlapping window)
                // This ensures we never miss a keyword split across the 512-byte boundary.
                if (bufferPos >= (int)sizeof(buffer) - 1) {
                    const int OVERLAP = 128;
                    memmove(buffer, buffer + (sizeof(buffer) - OVERLAP - 1), OVERLAP);
                    bufferPos = OVERLAP;
                    buffer[bufferPos] = '\0';
                }
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        
        // Drain stream safely
        while (stream->available()) stream->read();
        
        zonesSetVirtualInput(state.targetZone, motion);
        if (motion) {
            LOG_INFO(TAG, "Motion detected on camera!");
        }
    } else if (code > 0) {
        if (code == 401) {
            LOG_ERROR(TAG, "ONVIF Auth failed - check credentials");
        }
        if (code == 400 || code == 404 || code == 500) {
            state.connected = false;
            LOG_WARN(TAG, "Connection lost, retrying subscription");
        }
    }
    http.end();
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void onvifInit() {
    state.port = 80;
    state.connected = false;
    state.taskHandle = NULL;

    // Create mutex to protect state struct from Core 0/Core 1 concurrent access
    stateMutex = xSemaphoreCreateMutex();

    xTaskCreatePinnedToCore(
        onvifTask,
        "ONVIF_Poll",
        8192,
        NULL,
        1,
        &state.taskHandle,
        0
    );
    
    Serial.println("[ONVIF] Client initialized (FreeRTOS Task created)");
}

void onvifSetServer(const char* host, uint16_t port, const char* user, const char* pass, uint8_t targetZone) {
    if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);

    strncpy(state.host, host, sizeof(state.host)-1);
    state.host[sizeof(state.host)-1] = '\0';
    state.port = port;
    strncpy(state.user, user, sizeof(state.user)-1);
    state.user[sizeof(state.user)-1] = '\0';
    strncpy(state.pass, pass, sizeof(state.pass)-1);
    state.pass[sizeof(state.pass)-1] = '\0';
    state.targetZone = (targetZone > 0 && targetZone <= MAX_ZONES) ? targetZone - 1 : 0;
    state.connected = false;
    state.subscriptionAddress = "";
    state.lastRenewMs = 0;

    if (stateMutex) xSemaphoreGive(stateMutex);
}

// The FreeRTOS Task Loop
static void onvifTask(void* pvParameters) {
    esp_task_wdt_add(NULL); // Add to watchdog
    while (true) {
        esp_task_wdt_reset(); // Pet watchdog

        // Take a local snapshot of config under mutex to prevent TOCTOU race with onvifSetServer()
        if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);
        bool hasHost = (strlen(state.host) > 0);
        bool isConnected = state.connected;
        if (stateMutex) xSemaphoreGive(stateMutex);

        if (WiFi.status() != WL_CONNECTED || !hasHost) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        uint32_t now = millis();

        if (!isConnected) {
            if (createSubscription()) {
                if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);
                state.lastRenewMs = now;
                if (stateMutex) xSemaphoreGive(stateMutex);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5000)); // Reduced from 10s
            }
        } else {
            bool renewNeeded = false;
            if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);
            renewNeeded = (now - state.lastRenewMs > 50000);
            if (stateMutex) xSemaphoreGive(stateMutex);

            if (renewNeeded) {
                if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);
                state.lastRenewMs = now;
                if (stateMutex) xSemaphoreGive(stateMutex);
                if (!createSubscription()) {
                    if (stateMutex) xSemaphoreTake(stateMutex, portMAX_DELAY);
                    state.connected = false;
                    if (stateMutex) xSemaphoreGive(stateMutex);
                    Serial.println("[ONVIF] Subscription renewal failed");
                    continue;
                }
            }

            pollMessages();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

// onvifUpdate() removed — functionality is purely in the FreeRTOS task

bool onvifIsConnected() {
    return state.connected;
}

const char* onvifGetHost() { return state.host; }
uint16_t onvifGetPort() { return state.port; }
const char* onvifGetUser() { return state.user; }
const char* onvifGetPass() { return state.pass; }
uint8_t onvifGetTargetZone() { return state.targetZone + 1; }
