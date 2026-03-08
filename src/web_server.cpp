#include "web_server.h"
#include "web_ui.h"
#include "config.h"
#include "io_expander.h"
#include "alarm_zones.h"
#include "alarm_controller.h"
#include "sms_commands.h"
#include "whatsapp_client.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include "config_manager.h"
#include "network.h"

#include <PsychicHttp.h>
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
    return response->send(200, "text/html", WEB_UI_HTML);
}

// ---------------------------------------------------------------------------
// GET /api/status — full system status JSON
// ---------------------------------------------------------------------------
static esp_err_t handleApiStatus(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;

    // Alarm
    JsonObject alarm = doc["alarm"].to<JsonObject>();
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
    sys["uptime"]   = millis() / 1000;
    sys["freeHeap"] = ESP.getFreeHeap();
    sys["version"]  = FW_VERSION_STR;

    // --- Alerts/WhatsApp (mask secrets) ---
    JsonObject alerts = doc["alerts"].to<JsonObject>();
    alerts["mode"] = (int)whatsappGetMode();
    alerts["waPhone"] = whatsappGetPhone();
    alerts["waApiKey"] = strlen(whatsappGetApiKey()) > 0 ? "****" : "";

    // --- MQTT (mask password) ---
    JsonObject mqtt = doc["mqtt"].to<JsonObject>();
    mqtt["server"] = mqttGetServer();
    mqtt["port"] = mqttGetPort();
    mqtt["user"] = mqttGetUser();
    mqtt["pass"] = strlen(mqttGetPass()) > 0 ? "****" : "";
    mqtt["clientId"] = mqttGetClientId();
    mqtt["connected"] = mqttIsConnected();

    // --- ONVIF (mask password) ---
    JsonObject onvif = doc["onvif"].to<JsonObject>();
    onvif["host"]       = onvifGetHost();
    onvif["port"]       = onvifGetPort();
    onvif["user"]       = onvifGetUser();
    onvif["pass"]       = strlen(onvifGetPass()) > 0 ? "****" : "";
    onvif["targetZone"] = onvifGetTargetZone();
    onvif["connected"]  = onvifIsConnected();

    String json;
    serializeJson(doc, json);
    return response->send(200, "application/json", json.c_str());
}

// ---------------------------------------------------------------------------
// POST /api/settings/alerts — set WhatsApp credentials and mode
// Body: { "mode": 1..3, "phone": "+34...", "apikey": "..." }
// ---------------------------------------------------------------------------
static esp_err_t handlePostAlerts(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body());
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    }

    if (doc["mode"].isNull() || doc["phone"].isNull() || doc["apikey"].isNull()) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Missing fields\"}");
    }

    WhatsAppMode mode = (WhatsAppMode)doc["mode"].as<int>();
    const char* phone = doc["phone"];
    const char* apikey = doc["apikey"];

    whatsappSetConfig(phone, apikey, mode);
    configSave();

    return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"Alert settings saved\"}");
}

// ---------------------------------------------------------------------------
// POST /api/settings/mqtt — set MQTT credentials
// Body: { "server": "...", "port": 1883, "user": "...", "pass": "...", "clientId": "..." }
// ---------------------------------------------------------------------------
static esp_err_t handlePostMqtt(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body());
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    }

    const char* server = doc["server"] | "";
    uint16_t port = doc["port"] | 1883;
    const char* user = doc["user"] | "";
    const char* pass = doc["pass"] | "";
    const char* clientId = doc["clientId"] | "SF_Alarm";

    mqttSetConfig(server, port, user, pass, clientId);
    configSave();

    return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"MQTT settings saved\"}");
}

// ---------------------------------------------------------------------------
// POST /api/settings/onvif — set ONVIF camera credentials
// Body: { "host": "...", "port": 80, "user": "...", "pass": "...", "targetZone": 1 }
// ---------------------------------------------------------------------------
static esp_err_t handlePostOnvif(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body());
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    }

    const char* host = doc["host"] | "";
    uint16_t port    = doc["port"] | 80;
    const char* user = doc["user"] | "";
    const char* pass = doc["pass"] | "";
    uint8_t zone     = doc["targetZone"] | 1;

    onvifSetServer(host, port, user, pass, zone);
    configSave();

    return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"Camera settings saved\"}");
}

// ---------------------------------------------------------------------------
// POST /api/arm — arm the system
// Body: {"pin":"1234", "mode":"away"|"home"}
// ---------------------------------------------------------------------------
static esp_err_t handleApiArm(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body());
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    }

    const char* pin  = doc["pin"] | "";
    const char* mode = doc["mode"] | "away";

    bool ok;
    if (strcmp(mode, "home") == 0) {
        ok = alarmArmHome(pin);
    } else {
        ok = alarmArmAway(pin);
    }

    if (ok) {
        return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"System arming\"}");
    } else {
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"Failed — wrong PIN or zones not clear\"}");
    }
}

// ---------------------------------------------------------------------------
// POST /api/disarm — disarm the system
// Body: {"pin":"1234"}
// ---------------------------------------------------------------------------
static esp_err_t handleApiDisarm(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body());
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
    }

    const char* pin = doc["pin"] | "";
    bool ok = alarmDisarm(pin);

    if (ok) {
        return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"System disarmed\"}");
    } else {
        return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"Wrong PIN\"}");
    }
}

// ---------------------------------------------------------------------------
// POST /api/mute — mute the siren
// ---------------------------------------------------------------------------
static esp_err_t handleApiMute(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body());
    // If PIN is provided, validate it before muting
    if (!err && !doc["pin"].isNull()) {
        const char* pin = doc["pin"] | "";
        if (strlen(pin) > 0 && !alarmDisarm(pin)) {
            return response->send(200, "application/json", "{\"ok\":false,\"msg\":\"Wrong PIN\"}");
        }
    }
    alarmMuteSiren();
    return response->send(200, "application/json", "{\"ok\":true,\"msg\":\"Siren muted\"}");
}

// ---------------------------------------------------------------------------
// POST /api/bypass — bypass or unbypass a zone
// Body: {"zone":0, "bypass":true}
// ---------------------------------------------------------------------------
static esp_err_t handleApiBypass(PsychicRequest* request, PsychicResponse* response)
{
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, request->body());
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
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
    DeserializationError err = deserializeJson(doc, request->body());
    if (err) {
        return response->send(400, "application/json", "{\"ok\":false,\"msg\":\"Invalid JSON\"}");
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
    // Increase max URI handlers — we have 8 endpoints
    server.config.max_uri_handlers = 20;

    // Serve the dashboard
    server.on("/", HTTP_GET, handleRoot);

    // REST API — GET
    server.on("/api/status", HTTP_GET, handleApiStatus);
    server.on("/api/outputs", HTTP_GET, handleApiOutputs);

    // REST API — POST
    server.on("/api/settings/onvif", HTTP_POST, handlePostOnvif);
    
    server.on("/api/arm", HTTP_POST, handleApiArm);
    server.on("/api/disarm", HTTP_POST, handleApiDisarm);
    server.on("/api/settings/alerts", HTTP_POST, handlePostAlerts);
    server.on("/api/settings/mqtt", HTTP_POST, handlePostMqtt);
    server.on("/api/mute", HTTP_POST, handleApiMute);
    server.on("/api/bypass", HTTP_POST, handleApiBypass);
    server.on("/api/output", HTTP_POST, handleApiOutput);

    server.begin();
    Serial.println("[WEB] Dashboard started on port 80");
}
