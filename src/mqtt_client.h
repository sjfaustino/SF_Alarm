#ifndef SF_ALARM_MQTT_CLIENT_H
#define SF_ALARM_MQTT_CLIENT_H

#include <Arduino.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "notification_provider.h"

class AlarmController;
class ZoneManager;
class IoService;
class NotificationManager;

struct MqttMsg {
    char topic[64];
    char payload[128];
    bool retained;
};

class MqttService : public NotificationProvider {
public:
    MqttService();
    virtual ~MqttService();

    // NotificationProvider implementation
    virtual const char* getName() const override { return "MQTT"; }
    virtual bool send(const char* target, const char* message) override;
    virtual bool isReady() const override { return isConnected(); }

    void init(AlarmController* alarm, ZoneManager* zones, IoService* io, NotificationManager* nm);
    void update();
    
    void setConfig(const char* server, uint16_t port, const char* user, const char* pass, const char* clientId);
    bool isConnected() const;
    void publish(const char* topic, const char* payload, bool retained = false);
    void publishDiscovery();
    void syncState();

    // Getters
    const char* getServer() const { return _server; }
    uint16_t getPort() const { return _port; }
    const char* getUser() const { return _user; }
    const char* getPass() const { return _pass; }
    const char* getClientId() const { return _clientId; }

private:
    AlarmController*     _alarm;
    ZoneManager*         _zones;
    IoService*           _io;
    NotificationManager* _nm;
    WiFiClient _espClient;
    PubSubClient _mqttClient;
    QueueHandle_t _msgQueue;
    SemaphoreHandle_t _configMutex;
    SemaphoreHandle_t _mqttMutex;
    
    char _server[64];
    uint16_t _port;
    char _user[32];
    char _pass[32];
    char _clientId[32];
    bool _connected;

    unsigned long _lastReconnectAttempt;
    volatile bool _syncRequested;
    int _lastPublishedState;
    uint16_t _lastPublishedZones;
    uint16_t _lastPublishedOutputs;

    static MqttService* _instance;
    static void staticCallback(char* topic, byte* payload, unsigned int length);
    void handleMessage(char* topic, byte* payload, unsigned int length);
    void internalSyncState();
    void publishHAConfig(const char* component, const char* objectId, const char* name, 
                         const char* deviceClass, const char* stateTopic, const char* cmdTopic);
};

#endif // SF_ALARM_MQTT_CLIENT_H
