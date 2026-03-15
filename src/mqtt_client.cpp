#include "mqtt_client.h"
#include "logging.h"
#include "alarm_controller.h"
#include "zone_manager.h"
#include "io_service.h"
#include "system_state.h"
#include <esp_task_wdt.h>
#include "notification_manager.h"

static const char* TAG = "MQTT";

MqttService* MqttService::_instance = nullptr;

MqttService::MqttService() 
    : _alarm(nullptr),
      _zones(nullptr),
      _io(nullptr),
      _nm(nullptr),
      _mqttClient(_espClient),
      _msgQueue(NULL),
      _configMutex(NULL),
      _port(1883),
      _lastReconnectAttempt(0),
      _syncRequested(false),
      _lastPublishedState(-1),
      _lastPublishedZones(0xFFFF),
      _lastPublishedOutputs(0xFFFF),
      _connected(false)
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
    if (_mqttMutex) vSemaphoreDelete(_mqttMutex);
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
            if (_alarm && _alarm->disarm(pin)) {
                publish("SF_Alarm/events", "DISARMED via MQTT");
            } else {
                publish("SF_Alarm/events", "DISARM failed (wrong PIN)");
            }
        }
        else if (strncmp(message, "ARM_HOME", 8) == 0) {
            const char* pin = extractPin(message, pinBuf, sizeof(pinBuf));
            if (_alarm && _alarm->armHome(pin)) {
                publish("SF_Alarm/events", "ARM_HOME via MQTT");
            } else {
                publish("SF_Alarm/events", "ARM_HOME failed (wrong PIN)");
            }
        }
        else if (strncmp(message, "ARM_AWAY", 8) == 0) {
            const char* pin = extractPin(message, pinBuf, sizeof(pinBuf));
            if (_alarm && _alarm->armAway(pin)) {
                publish("SF_Alarm/events", "ARM_AWAY via MQTT");
            } else {
                publish("SF_Alarm/events", "ARM_AWAY failed (wrong PIN)");
            }
        }
        else if (strncmp(message, "MUTE", 4) == 0) {
            const char* pin = extractPin(message, pinBuf, sizeof(pinBuf));
            if (_alarm && _alarm->muteSiren(pin)) {
                publish("SF_Alarm/events", "MUTE via MQTT");
            } else {
                publish("SF_Alarm/events", "MUTE failed (wrong PIN)");
            }
        }
    }
}

void MqttService::init(AlarmController* alarm, ZoneManager* zones, IoService* io, NotificationManager* nm) {
    _alarm = alarm;
    _zones = zones;
    _io = io;
    _nm = nm;

    if (_configMutex == NULL) {
        _configMutex = xSemaphoreCreateMutex();
    }
    if (_mqttMutex == NULL) {
        _mqttMutex = xSemaphoreCreateMutex();
    }
    if (_msgQueue == NULL) {
        _msgQueue = xQueueCreate(30, sizeof(MqttMsg));
    }
    
    if (xSemaphoreTake(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _mqttClient.setSocketTimeout(2);
        _mqttClient.setKeepAlive(15);
        _mqttClient.setCallback(MqttService::staticCallback);
        xSemaphoreGive(_mqttMutex);
    }
    
    _nm->registerProvider(CH_ALL, this);
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
    
    if (xSemaphoreTake(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _mqttClient.setServer(_server, _port);
        xSemaphoreGive(_mqttMutex);
    }
    xSemaphoreGive(_configMutex);
}

bool MqttService::isConnected() const {
    return _connected;
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

bool MqttService::send(const char* target, const char* message) {
    const char* topic = (target && strlen(target) > 0) ? target : "SF_Alarm/notifications";
    publish(topic, message, false);
    return true;
}

void MqttService::syncState() {
    _syncRequested = true;
}

void MqttService::update() {
    if (strlen(_server) == 0) return;

    if (xSemaphoreTake(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _connected = _mqttClient.connected();
        xSemaphoreGive(_mqttMutex);
    }

    if (!_connected) {
        unsigned long now = millis();
        static uint32_t reconnectJitter = 0;
        if (reconnectJitter == 0) reconnectJitter = 5000 + random(5000); 

        if (now - _lastReconnectAttempt > reconnectJitter) {
            _lastReconnectAttempt = now;
            reconnectJitter = 5000 + random(5000);
            
            xSemaphoreTake(_configMutex, portMAX_DELAY);
            LOG_INFO(TAG, "Attempting connection to %s:%d...", _server, _port);
            
            bool connected = false;
            if (xSemaphoreTake(_mqttMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
                if (strlen(_user) > 0) {
                    connected = _mqttClient.connect(_clientId, _user, _pass, "SF_Alarm/availability", 0, true, "offline");
                } else {
                    connected = _mqttClient.connect(_clientId, NULL, NULL, "SF_Alarm/availability", 0, true, "offline");
                }
                
                if (connected) {
                    LOG_INFO(TAG, "Connected to broker");
                    _mqttClient.publish("SF_Alarm/availability", "online", true);
                    _mqttClient.subscribe("SF_Alarm/cmd");
                    publishDiscovery();
                    _syncRequested = true;
                    _connected = true;
                } else {
                    LOG_WARN(TAG, "Connection failed, rc=%d", _mqttClient.state());
                }
                xSemaphoreGive(_mqttMutex);
            }
            xSemaphoreGive(_configMutex);
        }
    } else {
        if (xSemaphoreTake(_mqttMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
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
            xSemaphoreGive(_mqttMutex);
        }
    }
}

void MqttService::internalSyncState() {
    if (!_mqttClient.connected()) return;

    SystemSnapshot snap;
    StateManager::capture(_alarm, _zones, _io, _nm, this, nullptr, snap);

    static const char* haStateMap[] = {
        "disarmed", "pending", "armed_away", "armed_home", "pending", "triggered", "unavailable"
    };

    if ((int)snap.alarmState != _lastPublishedState && (int)snap.alarmState <= 6) {
        _mqttClient.publish("SF_Alarm/state", haStateMap[(int)snap.alarmState], true);
        _lastPublishedState = (int)snap.alarmState;
    }

    uint16_t currentZones = 0;
    for (int i = 0; i < 16; i++) {
        if (snap.zones[i].rawInput) currentZones |= (1 << i);
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

    if (snap.outputs != _lastPublishedOutputs) {
        for (int i = 0; i < 16; i++) {
            bool current = (snap.outputs >> i) & 1;
            bool previous = (_lastPublishedOutputs >> i) & 1;
            if (current != previous) {
                char topic[32];
                snprintf(topic, sizeof(topic), "SF_Alarm/output/%d", i + 1);
                _mqttClient.publish(topic, current ? "ON" : "OFF", true);
            }
        }
        _lastPublishedOutputs = snap.outputs;
    }
}

void MqttService::publishDiscovery() {
    if (!_mqttClient.connected()) return;

    LOG_INFO(TAG, "Publishing Home Assistant discovery payloads...");

    // 1. Alarm Control Panel
    {
        JsonDocument doc;
        doc["name"] = "SF_Alarm";
        doc["state_topic"] = "SF_Alarm/state";
        doc["command_topic"] = "SF_Alarm/cmd";
        doc["availability_topic"] = "SF_Alarm/availability";
        doc["payload_available"] = "online";
        doc["payload_not_available"] = "offline";
        doc["code_arm_required"] = true;
        doc["code_disarm_required"] = true;
        doc["unique_id"] = String(_clientId) + "_alarm";
        
        JsonObject dev = doc["device"].to<JsonObject>();
        dev["identifiers"][0] = _clientId;
        dev["name"] = "SF_Alarm System";
        dev["model"] = "KC868-A16";
        dev["sw_version"] = "2.0.0-hardened";

        String output;
        serializeJson(doc, output);
        _mqttClient.publish("homeassistant/alarm_control_panel/sf_alarm/config", output.c_str(), true);
    }

    // 2. Binary Sensors (Zones)
    for (int i = 0; i < 16; i++) {
        char objId[32], name[32], stateTopic[64];
        snprintf(objId, sizeof(objId), "zone_%d", i + 1);
        snprintf(name, sizeof(name), "Zone %d", i + 1);
        snprintf(stateTopic, sizeof(stateTopic), "SF_Alarm/zone/%d", i + 1);
        publishHAConfig("binary_sensor", objId, name, (i < 8) ? "motion" : "door", stateTopic, nullptr);
    }

    // 3. Switches (Outputs/Relays)
    for (int i = 0; i < 16; i++) {
        char objId[32], name[32], stateTopic[64], cmdTopic[64];
        snprintf(objId, sizeof(objId), "output_%d", i + 1);
        snprintf(name, sizeof(name), "Output %d", i + 1);
        snprintf(stateTopic, sizeof(stateTopic), "SF_Alarm/output/%d", i + 1);
        snprintf(cmdTopic, sizeof(cmdTopic), "SF_Alarm/output/%d/set", i + 1);
        publishHAConfig("switch", objId, name, nullptr, stateTopic, cmdTopic);
    }
}

void MqttService::publishHAConfig(const char* component, const char* objectId, const char* name, 
                                 const char* deviceClass, const char* stateTopic, const char* cmdTopic) {
    JsonDocument doc;
    char discTopic[128];
    snprintf(discTopic, sizeof(discTopic), "homeassistant/%s/sf_alarm/%s/config", component, objectId);

    doc["name"] = name;
    if (stateTopic) doc["state_topic"] = stateTopic;
    if (cmdTopic) doc["command_topic"] = cmdTopic;
    if (deviceClass) doc["device_class"] = deviceClass;
    
    doc["unique_id"] = String(_clientId) + "_" + objectId;
    doc["availability_topic"] = "SF_Alarm/availability";
    
    JsonObject dev = doc["device"].to<JsonObject>();
    dev["identifiers"][0] = _clientId;

    String output;
    serializeJson(doc, output);
    _mqttClient.publish(discTopic, output.c_str(), true);
}
