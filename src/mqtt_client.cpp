#include "mqtt_client.h"
#include "logging.h"
#include "alarm_controller.h"
#include "alarm_zones.h"
#include "io_expander.h"
#include "system_context.h"
#include <esp_task_wdt.h>

static const char* TAG = "MQTT";

MqttService* MqttService::_instance = nullptr;

MqttService::MqttService() 
    : _ctx(nullptr), 
      _mqttClient(_espClient),
      _msgQueue(NULL),
      _configMutex(NULL),
      _port(1883),
      _lastReconnectAttempt(0),
      _syncRequested(false),
      _lastPublishedState(-1),
      _lastPublishedZones(0xFFFF),
      _lastPublishedOutputs(0xFFFF)
{
    _instance = this;
    memset(_server, 0, sizeof(_server));
    memset(_user, 0, sizeof(_user));
    memset(_pass, 0, sizeof(_pass));
    strncpy(_clientId, "SF_Alarm", sizeof(_clientId)-1);
}

MqttService::~MqttService() {
    if (_msgQueue) vQueueDelete(_msgQueue);
    if (_configMutex) vSemaphoreDelete(_configMutex);
}

void MqttService::staticCallback(char* topic, byte* payload, unsigned int length) {
    if (_instance) _instance->handleMessage(topic, payload, length);
}

void MqttService::handleMessage(char* topic, byte* payload, unsigned int length) {
    char message[64];
    if (length >= sizeof(message)) length = sizeof(message) - 1;
    memcpy(message, payload, length);
    message[length] = '\0';

    if (strchr(message, ':')) {
        LOG_INFO(TAG, "Message arrived [%s]: [PIN REDACTED]", topic);
    } else {
        LOG_INFO(TAG, "Message arrived [%s]: %s", topic, message);
    }

    if (strstr(topic, "/cmd")) {
        char pinBuf[16];
        auto extractPin = [](const char* msg, char* out, size_t sz) -> const char* {
            const char* sep = strchr(msg, ':');
            if (sep) {
                strncpy(out, sep + 1, sz - 1);
                out[sz - 1] = '\0';
                return out;
            }
            return "";
        };

        if (strncmp(message, "DISARM", 6) == 0) {
            const char* pin = extractPin(message, pinBuf, sizeof(pinBuf));
            if (_ctx->alarmController->disarm(pin)) {
                publish("SF_Alarm/events", "DISARMED via MQTT");
            } else {
                publish("SF_Alarm/events", "DISARM failed (wrong PIN)");
            }
        }
        else if (strncmp(message, "ARM_HOME", 8) == 0) {
            const char* pin = extractPin(message, pinBuf, sizeof(pinBuf));
            if (_ctx->alarmController->armHome(pin)) {
                publish("SF_Alarm/events", "ARM_HOME via MQTT");
            } else {
                publish("SF_Alarm/events", "ARM_HOME failed (wrong PIN)");
            }
        }
        else if (strncmp(message, "ARM_AWAY", 8) == 0) {
            const char* pin = extractPin(message, pinBuf, sizeof(pinBuf));
            if (_ctx->alarmController->armAway(pin)) {
                publish("SF_Alarm/events", "ARM_AWAY via MQTT");
            } else {
                publish("SF_Alarm/events", "ARM_AWAY failed (wrong PIN)");
            }
        }
        else if (strncmp(message, "MUTE", 4) == 0) {
            const char* pin = extractPin(message, pinBuf, sizeof(pinBuf));
            if (_ctx->alarmController->muteSiren(pin)) {
                publish("SF_Alarm/events", "MUTE via MQTT");
            } else {
                publish("SF_Alarm/events", "MUTE failed (wrong PIN)");
            }
        }
    }
}

void MqttService::init(SystemContext* ctx) {
    _ctx = ctx;
    if (_configMutex == NULL) {
        _configMutex = xSemaphoreCreateMutex();
    }
    if (_msgQueue == NULL) {
        _msgQueue = xQueueCreate(30, sizeof(MqttMsg));
    }
    
    _mqttClient.setSocketTimeout(2);
    _mqttClient.setKeepAlive(15);
    _mqttClient.setCallback(MqttService::staticCallback);
    
    LOG_INFO(TAG, "MQTT Service initialized");
}

void MqttService::setConfig(const char* server, uint16_t port, const char* user, const char* pass, const char* clientId) {
    xSemaphoreTake(_configMutex, portMAX_DELAY);
    strncpy(_server, server ? server : "", sizeof(_server) - 1);
    _port = port;
    
    memset(_user, 0, sizeof(_user));
    memset(_pass, 0, sizeof(_pass));
    if (user) strncpy(_user, user, sizeof(_user) - 1);
    if (pass) strncpy(_pass, pass, sizeof(_pass) - 1);
    
    if (clientId) strncpy(_clientId, clientId, sizeof(_clientId) - 1);
    
    _mqttClient.setServer(_server, _port);
    xSemaphoreGive(_configMutex);
}

bool MqttService::isConnected() {
    return _mqttClient.connected();
}

void MqttService::publish(const char* topic, const char* payload, bool retained) {
    if (_msgQueue) {
        MqttMsg msg;
        strncpy(msg.topic, topic, sizeof(msg.topic)-1);
        msg.topic[sizeof(msg.topic)-1] = '\0';
        strncpy(msg.payload, payload, sizeof(msg.payload)-1);
        msg.payload[sizeof(msg.payload)-1] = '\0';
        msg.retained = retained;
        xQueueSend(_msgQueue, &msg, pdMS_TO_TICKS(50));
    }
}

void MqttService::syncState() {
    _syncRequested = true;
}

void MqttService::update() {
    if (strlen(_server) == 0) return;

    if (!_mqttClient.connected()) {
        unsigned long now = millis();
        static uint32_t reconnectJitter = 0;
        if (reconnectJitter == 0) reconnectJitter = 5000 + random(5000); 

        if (now - _lastReconnectAttempt > reconnectJitter) {
            _lastReconnectAttempt = now;
            reconnectJitter = 5000 + random(5000);
            
            xSemaphoreTake(_configMutex, portMAX_DELAY);
            LOG_INFO(TAG, "Attempting connection to %s:%d...", _server, _port);
            
            bool connected;
            if (strlen(_user) > 0) {
                connected = _mqttClient.connect(_clientId, _user, _pass, "SF_Alarm/availability", 0, true, "offline");
            } else {
                connected = _mqttClient.connect(_clientId, NULL, NULL, "SF_Alarm/availability", 0, true, "offline");
            }
            
            if (connected) {
                LOG_INFO(TAG, "Connected to broker");
                _mqttClient.publish("SF_Alarm/availability", "online", true);
                _mqttClient.subscribe("SF_Alarm/cmd");
                _syncRequested = true;
            } else {
                LOG_WARN(TAG, "Connection failed, rc=%d", _mqttClient.state());
            }
            xSemaphoreGive(_configMutex);
        }
    } else {
        _mqttClient.loop();

        if (_syncRequested) {
            _syncRequested = false;
            internalSyncState();
        }

        if (_msgQueue) {
            MqttMsg msg;
            while (xQueueReceive(_msgQueue, &msg, 0) == pdTRUE) {
                _mqttClient.publish(msg.topic, msg.payload, msg.retained);
            }
        }
    }
}

void MqttService::internalSyncState() {
    if (!_mqttClient.connected()) return;

    static const char* haStateMap[] = {
        "disarmed", "pending", "armed_away", "armed_home", "pending", "triggered", "unavailable"
    };

    AlarmState st = _ctx->alarmController->getState();
    if ((int)st != _lastPublishedState && (int)st <= 6) {
        _mqttClient.publish("SF_Alarm/state", haStateMap[(int)st], true);
        _lastPublishedState = (int)st;
    }

    uint16_t currentZones = 0;
    for (int i = 0; i < 16; i++) {
        const ZoneInfo* zi = zonesGetInfo(i);
        if (zi && zi->rawInput) currentZones |= (1 << i);
    }
    if (currentZones != _lastPublishedZones) {
        for (int i = 0; i < 16; i++) {
            bool current = (currentZones >> i) & 1;
            bool previous = (_lastPublishedZones >> i) & 1;
            if (current != previous) {
                char topic[32];
                snprintf(topic, sizeof(topic), "SF_Alarm/zone/%d", i + 1);
                _mqttClient.publish(topic, current ? "ON" : "OFF", true);
            }
        }
        _lastPublishedZones = currentZones;
    }

    uint16_t outs = ioExpanderGetOutputs();
    if (outs != _lastPublishedOutputs) {
        for (int i = 0; i < 16; i++) {
            bool current = (outs >> i) & 1;
            bool previous = (_lastPublishedOutputs >> i) & 1;
            if (current != previous) {
                char topic[32];
                snprintf(topic, sizeof(topic), "SF_Alarm/output/%d", i + 1);
                _mqttClient.publish(topic, current ? "ON" : "OFF", true);
            }
        }
        _lastPublishedOutputs = outs;
    }
}
