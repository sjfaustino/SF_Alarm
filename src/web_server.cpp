#include "web_server.h"
#include <LittleFS.h>
#include "config.h"
#include "system_state.h"
#include "io_service.h"
#include "zone_manager.h"
#include "alarm_controller.h"
#include "notification_manager.h"
#include "sms_command_processor.h"
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
static AlarmController*     _alarm    = nullptr;
static ZoneManager*         _zones    = nullptr;
static IoService*           _io       = nullptr;
static NotificationManager* _nm       = nullptr;
static MqttService*         _mqtt     = nullptr;
static OnvifService*        _onvif    = nullptr;
static PhoneAuthenticator*  _auth     = nullptr;
static SmsCommandProcessor* _smsProc  = nullptr;
static WhatsappService*    _whatsapp = nullptr;
static TelegramService*    _telegram = nullptr;

// ---------------------------------------------------------------------------
// PsychicHttp Server instance (port 80)
// ---------------------------------------------------------------------------
static PsychicHttpServer server;

static uint32_t _failedAttempts = 0;
static unsigned long _lockoutEnd = 0;

static bool checkGlobalAuthLockout(PsychicResponse* response) {
    unsigned long now = millis();
    if (now < _lockoutEnd) {
        response->send(429, "application/json", "{\"ok\":false,\"msg\":\"System locked due to too many failed attempts. Try again later.\"}");
        return true;
    }
    return false;
}

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
// Zone Type Helper
// ---------------------------------------------------------------------------
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
static esp_err_t handleApiStatus(PsychicRequest* request, PsychicResponse* response) {
    if (!request) return ESP_FAIL;

    sysHealthReport(HB_BIT_WEB);
    SystemSnapshot snap;
    StateManager::capture(_alarm, _zones, _io, _nm, _mqtt, _onvif, snap);

    JsonDocument doc;
    JsonObject root = doc.to<JsonObject>();
    StateManager::serialize(snap, root);

    String body;
    serializeJson(doc, body);
    return response->send(200, "application/json", body.c_str());
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

    if (checkGlobalAuthLockout(response)) return ESP_OK;

    const char* pin = doc["pin"] | "";
    bool valid = _alarm->validatePin(pin);
    // Scrub PIN buffer if it exists in doc overhead
    if (doc["pin"].is<JsonVariant>()) doc["pin"] = "****"; 

    if (!valid) {
        _failedAttempts++;
        _lockoutEnd = millis() + (std::min(_failedAttempts, (uint32_t)30) * 1000);
        doc.clear(); // Explicitly clear to zero-out internal pointers/refs
        return response->send(403, "application/json", "{\"ok\":false,\"msg\":\"ACCESS DENIED: PIN required\"}");
    }
    _failedAttempts = 0;

    JsonDocument reply;
    reply["ok"] = true;

    JsonObject alerts = reply["alerts"].to<JsonObject>();
    alerts["mode"] = _nm->getChannels();
    alerts["waPhone"] = _whatsapp->getPhone();
    alerts["waApiKey"] = _whatsapp->getApiKey();
    alerts["tgToken"] = _telegram->getToken();
    alerts["tgChatId"] = _telegram->getChatId();

    JsonObject mqtt = reply["mqtt"].to<JsonObject>();
    mqtt["server"] = _mqtt->getServer();
    mqtt["port"] = _mqtt->getPort();
    mqtt["user"] = _mqtt->getUser();
    mqtt["pass"] = _mqtt->getPass();
    mqtt["clientId"] = _mqtt->getClientId();

    JsonObject onvif = reply["onvif"].to<JsonObject>();
    onvif["host"]       = _onvif->getHost();
    onvif["port"]       = _onvif->getPort();
    onvif["user"]       = _onvif->getUser();
    onvif["pass"]       = _onvif->getPass();
    onvif["targetZone"] = _onvif->getTargetZone();

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

    if (checkGlobalAuthLockout(response)) return ESP_OK;

    // PIN required — changing alert destination is a security-sensitive operation
    const char* pin = doc["pin"] | "";
    bool valid = (strlen(pin) > 0 && _alarm->validatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (!valid) {
        _failedAttempts++;
        _lockoutEnd = millis() + (std::min(_failedAttempts, (uint32_t)30) * 1000);
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }
    _failedAttempts = 0;

    uint8_t channels = doc["mode"] | (uint8_t)CH_SMS;
    const char* phone = doc["phone"];
    const char* apikey = doc["apikey"];

    _nm->setChannels(channels);
    _whatsapp->setConfig(phone, apikey);
    configSaveWhatsapp();

    const char* tgToken = doc["tgToken"] | "";
    const char* tgChatId = doc["tgChatId"] | "";
    
    _telegram->setConfig(tgToken, tgChatId);
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

    if (checkGlobalAuthLockout(response)) return ESP_OK;

    // PIN required — changing broker redirects all alarm events
    const char* pin = doc["pin"] | "";
    bool valid = (strlen(pin) > 0 && _alarm->validatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (!valid) {
        _failedAttempts++;
        _lockoutEnd = millis() + (std::min(_failedAttempts, (uint32_t)30) * 1000);
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }
    _failedAttempts = 0;

    const char* server = doc["server"] | "";
    uint16_t port = doc["port"] | 1883;
    const char* user = doc["user"] | "";
    const char* pass = doc["pass"] | "";
    const char* clientId = doc["clientId"] | "SF_Alarm";

    _mqtt->setConfig(server, port, user, pass, clientId);
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

    if (checkGlobalAuthLockout(response)) return ESP_OK;

    // PIN required — changing camera config affects motion detection source
    const char* pin = doc["pin"] | "";
    bool valid = (strlen(pin) > 0 && _alarm->validatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (!valid) {
        _failedAttempts++;
        _lockoutEnd = millis() + (std::min(_failedAttempts, (uint32_t)30) * 1000);
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }
    _failedAttempts = 0;

    const char* host = doc["host"] | "";
    uint16_t port    = doc["port"] | 80;
    const char* user = doc["user"] | "";
    const char* pass = doc["pass"] | "";
    uint8_t zone     = doc["targetZone"] | 1;

    _onvif->setServer(host, port, user, pass, zone);
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
    bool valid = _alarm->validatePin(pin);

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

    bool valid = _alarm->disarm(pin);

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

    bool valid = _alarm->muteSiren(pin);

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
    
    bool pinOk = (strlen(pin) > 0 && _alarm->validatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (!pinOk) {
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }

    int zone     = doc["zone"] | -1;
    bool bypass  = doc["bypass"] | false;

    if (zone < 0 || zone >= MAX_ZONES) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid zone index\"}");
    }

    _zones->setBypassed((uint8_t)zone, bypass);

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
    snprintf(resp, sizeof(resp), "{\"outputs\":%u}", _io->getOutputs());
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
    bool valid = (strlen(pin) > 0 && _alarm->validatePin(pin));
    if (doc["pin"].is<const char*>()) doc["pin"] = "****";

    if (!valid) {
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"PIN required\"}");
    }

    int ch     = doc["channel"] | -1;
    bool state = doc["state"] | false;

    if (ch < 0 || ch >= 16) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid channel\"}");
    }

    _io->setOutput((uint8_t)ch, state);

    char resp[64];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"msg\":\"Output %d %s\"}", ch + 1, state ? "ON" : "OFF");
    return response->send(200, "application/json", resp);
}

// ---------------------------------------------------------------------------
// Public: webServerInit()
// ---------------------------------------------------------------------------
void webServerInit(AlarmController* alarm, ZoneManager* zones, IoService* io,
                  NotificationManager* nm, MqttService* mqtt, OnvifService* onvif,
                  PhoneAuthenticator* auth, SmsCommandProcessor* smsProc,
                  WhatsappService* whatsapp, TelegramService* telegram)
{
    _alarm = alarm;
    _zones = zones;
    _io = io;
    _nm = nm;
    _mqtt = mqtt;
    _onvif = onvif;
    _auth = auth;
    _smsProc = smsProc;
    _whatsapp = whatsapp;
    _telegram = telegram;

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
