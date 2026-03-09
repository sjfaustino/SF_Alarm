#include "mqtt_client.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "alarm_controller.h"
#include "alarm_zones.h"
#include "io_expander.h"

static WiFiClient espClient;
static PubSubClient client(espClient);

static char mqttServer[64] = "";
static uint16_t mqttPort = 1883;
static char mqttUser[32] = "";
static char mqttPass[32] = "";
static char mqttClientId[32] = "SF_Alarm";

static unsigned long lastReconnectAttempt = 0;

// Topic structure
// SF_Alarm/state -> disarmed, armed_home, armed_away, triggered, pending
// SF_Alarm/cmd   -> DISARM, ARM_HOME, ARM_AWAY, MUTE
// SF_Alarm/zone/N -> ON/OFF
// SF_Alarm/output/N -> ON/OFF

/// Parse "COMMAND:PIN" format safely without mutating the input buffer
static char mqttPinBuffer[32];

static const char* extractPin(const char* message) {
    const char* sep = strchr(message, ':');
    if (sep) {
        strncpy(mqttPinBuffer, sep + 1, sizeof(mqttPinBuffer) - 1);
        mqttPinBuffer[sizeof(mqttPinBuffer) - 1] = '\0';
        return mqttPinBuffer;
    }
    return "";
}

void mqttCallback(char* topic, byte* payload, unsigned long length) {
    char message[64];
    if (length >= sizeof(message)) length = sizeof(message) - 1;
    memcpy(message, payload, length);
    message[length] = '\0';

    Serial.printf("[MQTT] Message arrived [%s]: %s\n", topic, message);

    if (strstr(topic, "/cmd")) {
        // All arm/disarm commands require PIN: "COMMAND:pin"
        // MUTE is non-destructive and allowed without PIN
        if (strncmp(message, "DISARM", 6) == 0) {
            const char* pin = extractPin(message);
            if (alarmDisarm(pin)) {
                mqttPublish("SF_Alarm/events", "DISARMED via MQTT");
            } else {
                mqttPublish("SF_Alarm/events", "DISARM failed (wrong PIN)");
            }
        }
        else if (strncmp(message, "ARM_HOME", 8) == 0) {
            const char* pin = extractPin(message);
            if (alarmArmHome(pin)) {
                mqttPublish("SF_Alarm/events", "ARM_HOME via MQTT");
            } else {
                mqttPublish("SF_Alarm/events", "ARM_HOME failed (wrong PIN)");
            }
        }
        else if (strncmp(message, "ARM_AWAY", 8) == 0) {
            const char* pin = extractPin(message);
            if (alarmArmAway(pin)) {
                mqttPublish("SF_Alarm/events", "ARM_AWAY via MQTT");
            } else {
                mqttPublish("SF_Alarm/events", "ARM_AWAY failed (wrong PIN)");
            }
        }
        else if (strcmp(message, "MUTE") == 0) {
            alarmMuteSiren();
        }
    }
}

void mqttInit() {
    client.setCallback(mqttCallback);
    Serial.println("[MQTT] MQTT client initialized");
}

void mqttSetConfig(const char* server, uint16_t port, const char* user, const char* pass, const char* clientId) {
    strncpy(mqttServer, server, sizeof(mqttServer)-1);
    mqttServer[sizeof(mqttServer)-1] = '\0';
    mqttPort = port;
    strncpy(mqttUser, user, sizeof(mqttUser)-1);
    mqttUser[sizeof(mqttUser)-1] = '\0';
    strncpy(mqttPass, pass, sizeof(mqttPass)-1);
    mqttPass[sizeof(mqttPass)-1] = '\0';
    strncpy(mqttClientId, clientId, sizeof(mqttClientId)-1);
    mqttClientId[sizeof(mqttClientId)-1] = '\0';
    
    client.setServer(mqttServer, mqttPort);
}

bool mqttReconnect() {
    if (strlen(mqttServer) == 0) return false;

    Serial.print("[MQTT] Attempting connection...");
    bool connected = false;
    if (strlen(mqttUser) > 0) {
        connected = client.connect(mqttClientId, mqttUser, mqttPass, "SF_Alarm/availability", 0, true, "offline");
    } else {
        connected = client.connect(mqttClientId, NULL, NULL, "SF_Alarm/availability", 0, true, "offline");
    }

    if (connected) {
        Serial.println("connected");
        client.publish("SF_Alarm/availability", "online", true);
        client.subscribe("SF_Alarm/cmd");
        mqttSyncState();
    } else {
        Serial.print("failed, rc=");
        Serial.println(client.state());
    }
    return connected;
}

void mqttUpdate() {
    if (strlen(mqttServer) == 0) return;

    if (!client.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt > 5000) {
            lastReconnectAttempt = now;
            if (mqttReconnect()) {
                lastReconnectAttempt = 0;
            }
        }
    } else {
        client.loop();
    }
}

void mqttPublish(const char* topic, const char* payload, bool retained) {
    if (client.connected()) {
        client.publish(topic, payload, retained);
    }
}

static const char* haStateMap[] = {
    "disarmed",   // MODE_DISARMED
    "pending",    // MODE_EXIT_DELAY
    "armed_away", // MODE_ARMED_AWAY
    "armed_home", // MODE_ARMED_HOME
    "pending",    // MODE_ENTRY_DELAY
    "triggered"   // MODE_TRIGGERED
};

// Change detection for MQTT sync
static int lastPublishedState = -1;
static uint16_t lastPublishedZones = 0xFFFF;  // bitmask of zone rawInput
static uint16_t lastPublishedOutputs = 0xFFFF;

void mqttSyncState() {
    if (!client.connected()) return;

    // 1. Alarm State (only publish on change)
    AlarmState st = alarmGetState();
    if ((int)st != lastPublishedState && (int)st < 6) {
        client.publish("SF_Alarm/state", haStateMap[(int)st], true);
        lastPublishedState = (int)st;
    }

    // 2. Zones (build current bitmask and compare)
    uint16_t currentZones = 0;
    for (int i = 0; i < 16; i++) {
        const ZoneInfo* zi = zonesGetInfo(i);
        if (zi && zi->rawInput) currentZones |= (1 << i);
    }
    if (currentZones != lastPublishedZones) {
        for (int i = 0; i < 16; i++) {
            bool current = (currentZones >> i) & 1;
            bool previous = (lastPublishedZones >> i) & 1;
            if (current != previous) {
                char topic[32];
                snprintf(topic, sizeof(topic), "SF_Alarm/zone/%d", i + 1);
                client.publish(topic, current ? "ON" : "OFF", true);
            }
        }
        lastPublishedZones = currentZones;
    }

    // 3. Outputs (only publish changed)
    uint16_t outs = ioExpanderGetOutputs();
    if (outs != lastPublishedOutputs) {
        for (int i = 0; i < 16; i++) {
            bool current = (outs >> i) & 1;
            bool previous = (lastPublishedOutputs >> i) & 1;
            if (current != previous) {
                char topic[32];
                snprintf(topic, sizeof(topic), "SF_Alarm/output/%d", i + 1);
                client.publish(topic, current ? "ON" : "OFF", true);
            }
        }
        lastPublishedOutputs = outs;
    }
}

const char* mqttGetServer() { return mqttServer; }
uint16_t mqttGetPort() { return mqttPort; }
const char* mqttGetUser() { return mqttUser; }
const char* mqttGetPass() { return mqttPass; }
const char* mqttGetClientId() { return mqttClientId; }

bool mqttIsConnected() { return client.connected(); }
