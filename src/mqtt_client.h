#ifndef SF_ALARM_MQTT_CLIENT_H
#define SF_ALARM_MQTT_CLIENT_H

#include <Arduino.h>

/**
 * @brief Initialize the MQTT client
 */
void mqttInit();

/**
 * @brief Run the MQTT client loop (call in main loop)
 */
void mqttUpdate();

/**
 * @brief Set the MQTT configuration
 */
void mqttSetConfig(const char* server, uint16_t port, const char* user, const char* pass, const char* clientId);

/**
 * @brief Check if MQTT is connected
 */
bool mqttIsConnected();

/**
 * @brief Publish a message to a topic
 */
void mqttPublish(const char* topic, const char* payload, bool retained = false);

/**
 * @brief Trigger a full state sync to MQTT
 */
void mqttSyncState();

/**
 * @brief Getters for configuration
 */
const char* mqttGetServer();
uint16_t mqttGetPort();
const char* mqttGetUser();
const char* mqttGetPass();
const char* mqttGetClientId();

#endif // SF_ALARM_MQTT_CLIENT_H
