#include "mqtt_client.h"
#include <WiFi.h>
#include <PubSubClient.h>
#include "alarm_controller.h"
#include "alarm_zones.h"
#include "io_expander.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <esp_task_wdt.h>
#include <esp_task_wdt.h>

static WiFiClient espClient;
static PubSubClient client(espClient);

struct MqttMsg {
    char topic[64];
    char payload[128];
    bool retained;
};

static QueueHandle_t mqttMsgQueue = NULL;
static volatile bool mqttSyncRequested = false;
static SemaphoreHandle_t mqttConfigMutex = NULL;

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

    // Redact PIN from log — commands with ':' contain a PIN (e.g. "DISARM:1234")
    if (strchr(message, ':')) {
        Serial.printf("[MQTT] Message arrived [%s]: [PIN REDACTED]\n", topic);
    } else {
        Serial.printf("[MQTT] Message arrived [%s]: %s\n", topic, message);
    }

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
    if (mqttConfigMutex == NULL) {
        mqttConfigMutex = xSemaphoreCreateMutex();
    }
    mqttMsgQueue = xQueueCreate(10, sizeof(MqttMsg));
    client.setCallback(mqttCallback);
    Serial.println("[MQTT] MQTT client initialized with RTOS async queue and mutex protection");
}

void mqttSetServer(const char* host, uint16_t port) {
    xSemaphoreTake(mqttConfigMutex, portMAX_DELAY);
    strncpy(mqttServer, host, sizeof(mqttServer) - 1);
    mqttServer[sizeof(mqttServer) - 1] = '\0';
    mqttPort = port;
    xSemaphoreGive(mqttConfigMutex);
    client.setServer(mqttServer, mqttPort);
}

void mqttSetCredentials(const char* user, const char* pass) {
    xSemaphoreTake(mqttConfigMutex, portMAX_DELAY);
    strncpy(mqttUser, user, sizeof(mqttUser) - 1);
    mqttUser[sizeof(mqttUser) - 1] = '\0';
    strncpy(mqttPass, pass, sizeof(mqttPass) - 1);
    mqttPass[sizeof(mqttPass) - 1] = '\0';
    xSemaphoreGive(mqttConfigMutex);
}

void mqttSetClientId(const char* clientId) {
    xSemaphoreTake(mqttConfigMutex, portMAX_DELAY);
    strncpy(mqttClientId, clientId, sizeof(mqttClientId) - 1);
    mqttClientId[sizeof(mqttClientId) - 1] = '\0';
    xSemaphoreGive(mqttConfigMutex);
}

// mqttSetConfig is removed as per instruction, replaced by mqttSetServer, mqttSetCredentials, mqttSetClientId

bool mqttReconnect() {
    // This function is no longer used directly by mqttUpdate, its logic is inlined there.
    // Keeping it for now in case other parts of the code still call it.
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

static void internalSyncState(); // Forward declaration

void mqttUpdate() {
    if (strlen(mqttServer) == 0) return;

    if (!client.connected()) {
        unsigned long now = millis();
        if (now - lastReconnectAttempt > 5000) {
            lastReconnectAttempt = now;
            
            xSemaphoreTake(mqttConfigMutex, portMAX_DELAY);
            Serial.printf("[MQTT] Attempting connection to %s:%d...\n", mqttServer, mqttPort);
            bool connected;
            if (strlen(mqttUser) > 0) {
                connected = client.connect(mqttClientId, mqttUser, mqttPass, "SF_Alarm/availability", 0, true, "offline");
            } else {
                connected = client.connect(mqttClientId, NULL, NULL, "SF_Alarm/availability", 0, true, "offline");
            }
            
            if (connected) {
                Serial.println("[MQTT] Connected!");
                client.publish("SF_Alarm/availability", "online", true);
                client.subscribe("SF_Alarm/cmd");
                mqttSyncRequested = true;
            } else {
                Serial.print("[MQTT] Connection failed, rc=");
                Serial.println(client.state());
            }
            xSemaphoreGive(mqttConfigMutex);
        }
    } else {
        client.loop();

        if (mqttSyncRequested) {
            mqttSyncRequested = false;
            internalSyncState();
        }

        if (mqttMsgQueue) {
            MqttMsg msg;
            while (xQueueReceive(mqttMsgQueue, &msg, 0) == pdTRUE) {
                client.publish(msg.topic, msg.payload, msg.retained);
            }
        }
    }
}

void mqttPublish(const char* topic, const char* payload, bool retained) {
    // Only queue if we have a queue initialized to prevent crashes
    if (mqttMsgQueue) {
        MqttMsg msg;
        strncpy(msg.topic, topic, sizeof(msg.topic)-1);
        msg.topic[sizeof(msg.topic)-1] = '\0';
        strncpy(msg.payload, payload, sizeof(msg.payload)-1);
        msg.payload[sizeof(msg.payload)-1] = '\0';
        msg.retained = retained;
        
        // Push to queue, dropping if full (10 item max to save heap)
        xQueueSend(mqttMsgQueue, &msg, 0);
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
    mqttSyncRequested = true;
}

static void internalSyncState() {
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
