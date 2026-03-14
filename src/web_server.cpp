#include "web_server.h"
#include <LittleFS.h>
#include "config.h"
#include "io_expander.h"
#include "alarm_zones.h"
#include "alarm_controller.h"
#include "notification_manager.h"
#include "sms_commands.h"
#include "whatsapp_client.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include "config_manager.h"
#include "network.h"
#include "logging.h"
#include "system_health.h"
#include "telegram_client.h"

#include <PsychicHttp.h>
#include <PsychicStreamResponse.h>
#include <PsychicFileResponse.h>

static const char* TAG = "WEB";
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// PsychicHttp Server instance (port 80)
// ---------------------------------------------------------------------------
static PsychicHttpServer server;

// ---------------------------------------------------------------------------
// Helper: zone state enum → string
// ---------------------------------------------------------------------------
static const char* zoneStateStr(ZoneState s)
{
    switch (s) {
        case ZONE_NORMAL:    return "NORMAL";
        case ZONE_TRIGGERED: return "TRIGGERED";
        case ZONE_TAMPER:    return "TAMPER";
        case ZONE_FAULT:     return "FAULT";
        case ZONE_BYPASSED:  return "BYPASSED";
        default:             return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// Security: IP-based Rate Limiting
// ---------------------------------------------------------------------------
struct AuthTracker {
    uint32_t ip;
    uint32_t lastAttemptMs;
    uint8_t  failCount;
};
static AuthTracker authHistory[8]; // Track last 8 IPs
static uint8_t authHistoryIdx = 0;

static bool checkRateLimit(uint32_t ip)
{
    uint32_t now = millis();
    for (int i = 0; i < 8; i++) {
        if (authHistory[i].ip == ip) {
            // cooldown 5 seconds between attempts from same IP
            if (now - authHistory[i].lastAttemptMs < 5000) return false;
            // lockout IP for 1 minute after 5 consecutive failures
            if (authHistory[i].failCount >= 5 && (now - authHistory[i].lastAttemptMs < 60000)) return false;
            return true;
        }
    }
    return true;
}

static void recordAttempt(uint32_t ip, bool success)
{
    uint32_t now = millis();
    for (int i = 0; i < 8; i++) {
        if (authHistory[i].ip == ip) {
            authHistory[i].lastAttemptMs = now;
            if (success) authHistory[i].failCount = 0;
            else authHistory[i].failCount++;
            return;
        }
    }
    // New IP
    authHistory[authHistoryIdx].ip = ip;
    authHistory[authHistoryIdx].lastAttemptMs = now;
    authHistory[authHistoryIdx].failCount = success ? 0 : 1;
    authHistoryIdx = (authHistoryIdx + 1) % 8;
}

static const char* zoneTypeStr(ZoneType t)
{
    switch (t) {
        case ZONE_INSTANT:   return "INSTANT";
        case ZONE_DELAYED:   return "DELAYED";
        case ZONE_24H:       return "24H";
        case ZONE_FOLLOWER:  return "FOLLOWER";
        default:             return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// GET / — serve the dashboard HTML
// ---------------------------------------------------------------------------
static esp_err_t handleRoot(PsychicRequest* request, PsychicResponse* response)
{
    if (!LittleFS.exists("/index.html")) {
        return response->send(404, "text/plain", "File Not Found: /index.html. Did you upload the data folder?");
    }
    PsychicFileResponse* fileResponse = new PsychicFileResponse(response, LittleFS, "/index.html");
    return fileResponse->send();
}

// ---------------------------------------------------------------------------
// GET /api/status — full system status JSON
// ---------------------------------------------------------------------------
static esp_err_t handleApiStatus(PsychicRequest* request, PsychicResponse* response)
{
    sysHealthReport(HB_BIT_WEB);
    JsonDocument doc;

    JsonObject alarm = doc["alarm"].to<JsonObject>();

    // Security: Local read-only telemetry requires no PIN.
    // Calling alarmValidatePin() here causes a 5-minute system lockout DDoS vulnerability.
    
    alarm["state"]          = alarmGetStateStr();
    alarm["stateCode"]      = (uint8_t)alarmGetState();
    alarm["delayRemaining"] = alarmGetDelayRemaining();

    // Zones
    JsonArray zones = doc["zones"].to<JsonArray>();
    for (int i = 0; i < MAX_ZONES; i++) {
        const ZoneInfo* zi = zonesGetInfo(i);
        if (!zi) continue;

        JsonObject z = zones.add<JsonObject>();
        z["index"]     = i;
        z["name"]      = zi->config.name;
        z["type"]      = zoneTypeStr(zi->config.type);
        z["typeCode"]  = (uint8_t)zi->config.type;
        z["wiring"]    = zi->config.wiring == ZONE_NC ? "NC" : "NO";
        z["enabled"]   = zi->config.enabled;
        z["state"]     = zoneStateStr(zi->state);
        z["stateCode"] = (uint8_t)zi->state;
        z["rawInput"]  = zi->rawInput;
    }

    // Outputs
    doc["outputs"] = ioExpanderGetOutputs();

    // Network
    JsonObject net = doc["network"].to<JsonObject>();
    net["ip"]        = networkGetIP();
    net["rssi"]      = networkGetRSSI();
    net["connected"] = networkIsConnected();

    // System
    JsonObject sys = doc["system"].to<JsonObject>();
    sys["uptime"]   = esp_timer_get_time() / 1000000ULL;
    sys["freeHeap"] = ESP.getFreeHeap();
    sys["version"]  = FW_VERSION_STR;

    // --- Alerts/WhatsApp (Safe status only) ---
    JsonObject alerts = doc["alerts"].to<JsonObject>();
    alerts["mode"] = (int)notificationGetChannels();

    // --- MQTT (Safe status only) ---
    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["connected"] = mqttIsConnected();

    // --- ONVIF (Safe status only) ---
    JsonObject onvif = doc["onvif"].to<JsonObject>();
    onvif["connected"]  = onvifIsConnected();

    // Stream serialization directly to the response (Zero-Heap)
    PsychicStreamResponse stream(request->response(), "application/json");
    serializeJson(doc, stream);
    return stream.send();
}

// ---------------------------------------------------------------------------
// POST /api/settings/get — Retrieve full sensitive config (Requires PIN)
// Body: { "pin": "1234" }
// ---------------------------------------------------------------------------
static esp_err_t handleApiGetSettings(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body(), DeserializationOption::NestingLimit(10));
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid or nested JSON\"}");
    }

    const char* pin = doc["pin"] | "";
    bool valid = alarmValidatePin(pin);
    // Scrub PIN buffer if it exists in doc overhead
    if (doc["pin"].is<JsonVariant>()) doc["pin"] = "****"; 

    if (!valid) {
        doc.clear(); // Explicitly clear to zero-out internal pointers/refs
        return response->send(403, "application/json", "{\"ok\":false,\"msg\":\"ACCESS DENIED: PIN required\"}");
    }

    JsonDocument reply;
    reply["ok"] = true;

    JsonObject alerts = reply["alerts"].to<JsonObject>();
    alerts["mode"] = notificationGetChannels();
    alerts["waPhone"] = whatsappGetPhone();
    alerts["waApiKey"] = whatsappGetApiKey();
    alerts["tgToken"] = telegramGetToken();
    alerts["tgChatId"] = telegramGetChatId();

    JsonObject mqtt = reply["mqtt"].to<JsonObject>();
    mqtt["server"] = mqttGetServer();
    mqtt["port"] = mqttGetPort();
    mqtt["user"] = mqttGetUser();
    mqtt["pass"] = mqttGetPass();
    mqtt["clientId"] = mqttGetClientId();

    JsonObject onvif = reply["onvif"].to<JsonObject>();
    onvif["host"]       = onvifGetHost();
    onvif["port"]       = onvifGetPort();
    onvif["user"]       = onvifGetUser();
    onvif["pass"]       = onvifGetPass();
    onvif["targetZone"] = onvifGetTargetZone();

    String out;
    serializeJson(reply, out);
    doc.clear(); // Scavenge
    return response->send(200, "application/json", out.c_str());
}

// ---------------------------------------------------------------------------
// POST /api/settings/alerts — set WhatsApp credentials and mode
// Body: { "mode": 1..3, "phone": "+34...", "apikey": "..." }
// ---------------------------------------------------------------------------
static esp_err_t handlePostAlerts(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body(), DeserializationOption::NestingLimit(10));
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid or nested JSON\"}");
    }

    // PIN required — changing alert destination is a security-sensitive operation
    const char* pin = doc["pin"] | "";
    bool valid = (strlen(pin) > 0 && alarmValidatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (!valid) {
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }

    uint8_t channels = doc["mode"] | (uint8_t)CH_SMS;
    const char* phone = doc["phone"];
    const char* apikey = doc["apikey"];

    notificationSetChannels(channels);
    whatsappSetConfig(phone, apikey);
    configSaveWhatsapp();

    const char* tgToken = doc["tgToken"] | "";
    const char* tgChatId = doc["tgChatId"] | "";
    
    telegramSetConfig(tgToken, tgChatId);
    configSaveTelegram();
    doc.clear(); // Scavenge

    return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"Alert settings saved\"}");
}

// ---------------------------------------------------------------------------
// POST /api/settings/mqtt — set MQTT credentials
// Body: { "server": "...", "port": 1883, "user": "...", "pass": "...", "clientId": "..." }
// ---------------------------------------------------------------------------
static esp_err_t handlePostMqtt(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body(), DeserializationOption::NestingLimit(10));
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid or nested JSON\"}");
    }

    // PIN required — changing broker redirects all alarm events
    const char* pin = doc["pin"] | "";
    bool valid = (strlen(pin) > 0 && alarmValidatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (!valid) {
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }

    const char* server = doc["server"] | "";
    uint16_t port = doc["port"] | 1883;
    const char* user = doc["user"] | "";
    const char* pass = doc["pass"] | "";
    const char* clientId = doc["clientId"] | "SF_Alarm";

    mqttSetConfig(server, port, user, pass, clientId);
    configSaveMqtt();

    return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"MQTT settings saved\"}");
}

// ---------------------------------------------------------------------------
// POST /api/settings/onvif — set ONVIF camera credentials
// Body: { "host": "...", "port": 80, "user": "...", "pass": "...", "targetZone": 1 }
// ---------------------------------------------------------------------------
static esp_err_t handlePostOnvif(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body(), DeserializationOption::NestingLimit(10));
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid or nested JSON\"}");
    }

    // PIN required — changing camera config affects motion detection source
    const char* pin = doc["pin"] | "";
    bool valid = (strlen(pin) > 0 && alarmValidatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (!valid) {
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }

    const char* host = doc["host"] | "";
    uint16_t port    = doc["port"] | 80;
    const char* user = doc["user"] | "";
    const char* pass = doc["pass"] | "";
    uint8_t zone     = doc["targetZone"] | 1;

    onvifSetServer(host, port, user, pass, zone);
    configSaveOnvif();

    return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"Camera settings saved\"}");
}

// ---------------------------------------------------------------------------
// POST /api/arm — arm the system
// Body: {"pin":"1234", "mode":"away"|"home"}
// ---------------------------------------------------------------------------
static esp_err_t handleApiArm(PsychicRequest* request, PsychicResponse* response)
{
    if (request->body().length() > 512) {
        return response->send(413, "application/json", "{\"ok\":false,\"msg\":\"Payload too large\"}");
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body(), DeserializationOption::NestingLimit(10));
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid or nested JSON\"}");
    }

    const char* pin  = doc["pin"] | "";
    const char* mode = doc["mode"] | "away";

    bool ok = false;
    uint32_t remoteIp = request->client()->remoteIP();

    bool valid = alarmValidatePin(pin);
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (strcmp(mode, "home") == 0) {
        ok = alarmArmHome(pin);
    } else {
        ok = alarmArmAway(pin);
    }

    recordAttempt(remoteIp, ok);

    if (ok) {
        doc.clear();
        return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"System arming\"}");
    } else {
        doc.clear();
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"Failed — wrong PIN or zones not clear\"}");
    }
}

// ---------------------------------------------------------------------------
// POST /api/disarm — disarm the system
// Body: {"pin":"1234"}
// ---------------------------------------------------------------------------
static esp_err_t handleApiDisarm(PsychicRequest* request, PsychicResponse* response)
{
    if (request->body().length() > 512) {
        return response->send(413, "application/json", "{\"ok\":false,\"msg\":\"Payload too large\"}");
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body(), DeserializationOption::NestingLimit(10));
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid or nested JSON\"}");
    }

    const char* pin = doc["pin"] | "";
    uint32_t remoteIp = request->client()->remoteIP();

    if (!checkRateLimit(remoteIp)) {
        return response->send(429, "application/json", "{\"ok\":false,\"msg\":\"Too many attempts. Wait 1 minute.\"}");
    }

    bool valid = alarmDisarm(pin);
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";
    recordAttempt(remoteIp, valid);

    if (valid) {
        doc.clear();
        return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"System disarmed\"}");
    } else {
        doc.clear();
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"Wrong PIN\"}");
    }
}

// ---------------------------------------------------------------------------
// POST /api/mute — mute the siren
// ---------------------------------------------------------------------------
static esp_err_t handleApiMute(PsychicRequest* request, PsychicResponse* response)
{
    if (request->body().length() > 512) {
        return response->send(413, "application/json", "{\"ok\":false,\"msg\":\"Payload too large\"}");
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body(), DeserializationOption::NestingLimit(10));
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid or nested JSON\"}");
    }

    const char* pin = doc["pin"] | "";
    uint32_t remoteIp = request->client()->remoteIP();

    if (!checkRateLimit(remoteIp)) {
        return response->send(429, "application/json", "{\"ok\":false,\"msg\":\"Too many attempts.\"}");
    }

    bool valid = alarmMuteSiren(pin);
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";
    recordAttempt(remoteIp, valid);

    if (valid) {
        return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"Siren muted\"}");
    } else {
        return response->send(403, "application/json", "{\"ok\":false,\"msg\":\"ACCESS DENIED: PIN required\"}");
    }
}

// ---------------------------------------------------------------------------
// POST /api/bypass — bypass or unbypass a zone
// Body: {"zone":0, "bypass":true}
// ---------------------------------------------------------------------------
static esp_err_t handleApiBypass(PsychicRequest* request, PsychicResponse* response)
{
    if (request->body().length() > 512) {
        return response->send(413, "application/json", "{\"ok\":false,\"msg\":\"Payload too large\"}");
    }

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body(), DeserializationOption::NestingLimit(10));
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid or nested JSON\"}");
    }

    // Require PIN for zone bypass/unbypass
    const char* pin = doc["pin"] | "";
    uint32_t remoteIp = request->client()->remoteIP();
    
    if (!checkRateLimit(remoteIp)) {
        return response->send(429, "application/json", "{\"ok\":false,\"msg\":\"Too many attempts.\"}");
    }

    bool pinOk = (strlen(pin) > 0 && alarmValidatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";
    recordAttempt(remoteIp, pinOk);

    if (!pinOk) {
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }

    int zone     = doc["zone"] | -1;
    bool bypass  = doc["bypass"] | false;

    if (zone < 0 || zone >= MAX_ZONES) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid zone index\"}");
    }

    zonesSetBypassed((uint8_t)zone, bypass);

    char resp[80];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"msg\":\"Zone %d %s\"}", zone + 1, bypass ? "bypassed" : "unbypassed");
    return response->send(200, "application/json", resp);
}

// ---------------------------------------------------------------------------
// GET /api/outputs — get current output bitmask
// ---------------------------------------------------------------------------
static esp_err_t handleApiOutputs(PsychicRequest* request, PsychicResponse* response)
{
    // Security: Local read-only telemetry requires no PIN.
    // Calling alarmValidatePin() here causes a 5-minute system lockout DDoS vulnerability.
    
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"outputs\":%u}", ioExpanderGetOutputs());
    return response->send(200, "application/json", resp);
}

// ---------------------------------------------------------------------------
// POST /api/output — set a single output
// Body: {"channel":0, "state":true}
// ---------------------------------------------------------------------------
static esp_err_t handleApiOutput(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body(), DeserializationOption::NestingLimit(10));
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid or nested JSON\"}");
    }

    // Require PIN for output control
    const char* pin = doc["pin"] | "";
    bool valid = (strlen(pin) > 0 && alarmValidatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (!valid) {
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }

    int ch     = doc["channel"] | -1;
    bool state = doc["state"] | false;

    if (ch < 0 || ch >= 16) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid channel\"}");
    }

    ioExpanderSetOutput((uint8_t)ch, state);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"msg\":\"Output %d %s\"}", ch + 1, state ? "ON" : "OFF");
    return response->send(200, "application/json", resp);
}

// ---------------------------------------------------------------------------
// Public: webServerInit()
// ---------------------------------------------------------------------------
void webServerInit()
{
    if (!LittleFS.begin(true)) {
        LOG_ERROR(TAG, "LittleFS Mount Failed");
    }

    // Increase max URI handlers — we have 8 endpoints
    server.config.max_uri_handlers = 20;

    // Serve the dashboard
    server.on("/", HTTP_GET, handleRoot);

    // REST API — GET
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/outputs", HTTP_GET, handleApiOutputs);

    // REST API — POST
    server.on("/api/settings/get", HTTP_POST, handleApiGetSettings);
    server.on("/api/settings/onvif", HTTP_POST, handlePostOnvif);
    
    server.on("/api/arm", HTTP_POST, handleApiArm);
    server.on("/api/disarm", HTTP_POST, handleApiDisarm);
    server.on("/api/settings/alerts", HTTP_POST, handlePostAlerts);
    server.on("/api/settings/mqtt", HTTP_POST, handlePostMqtt);
    server.on("/api/mute", HTTP_POST, handleApiMute);
    server.on("/api/bypass", HTTP_POST, handleApiBypass);
    server.on("/api/output", HTTP_POST, handleApiOutput);

    server.begin();
    LOG_INFO(TAG, "Dashboard started on port 80");
}
