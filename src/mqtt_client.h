#ifndef SF_ALARM_MQTT_CLIENT_H
#define SF_ALARM_MQTT_CLIENT_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

struct SystemContext;

struct MqttMsg {
    char topic[64];
    char payload[128];
    bool retained;
};

class MqttService {
public:
    MqttService();
    ~MqttService();

    void init(SystemContext* ctx);
    void update();
    
    void setConfig(const char* server, uint16_t port, const char* user, const char* pass, const char* clientId);
    bool isConnected();
    void publish(const char* topic, const char* payload, bool retained = false);
    void syncState();

    // Getters
    const char* getServer() const { return _server; }
    uint16_t getPort() const { return _port; }
    const char* getUser() const { return _user; }
    const char* getPass() const { return _pass; }
    const char* getClientId() const { return _clientId; }

private:
    SystemContext* _ctx;
    WiFiClient _espClient;
    PubSubClient _mqttClient;
    QueueHandle_t _msgQueue;
    SemaphoreHandle_t _configMutex;
    
    char _server[64];
    uint16_t _port;
    char _user[32];
    char _pass[32];
    char _clientId[32];

    unsigned long _lastReconnectAttempt;
    volatile bool _syncRequested;
    int _lastPublishedState;
    uint16_t _lastPublishedZones;
    uint16_t _lastPublishedOutputs;

    static MqttService* _instance;
    static void staticCallback(char* topic, byte* payload, unsigned int length);
    void handleMessage(char* topic, byte* payload, unsigned int length);
    void internalSyncState();
};

#endif // SF_ALARM_MQTT_CLIENT_H
