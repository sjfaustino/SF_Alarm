#ifndef SF_ALARM_CONFIG_MANAGER_H
#define SF_ALARM_CONFIG_MANAGER_H

#include <Arduino.h>
#include "alarm_controller.h"

enum ConfigSection {
    CFG_MAIN,
    CFG_WIFI,
    CFG_ROUTER,
    CFG_ZONES,
    CFG_ALERTS,
    CFG_MQTT,
    CFG_ONVIF,
    CFG_SCHEDULE,
    CFG_HEARTBEAT,
    CFG_TIMEZONE,
    CFG_ALL
};

/// Mark a configuration section as changed and needing a save.
void configMarkDirty(ConfigSection section);

/// Initialize the config manager (opens NVS namespace).
void configInit();

/// Load all configuration from NVS into the running modules.
/// Call after all modules are initialized.
void configLoad();

/// Save all current configuration to NVS.
void configSave();

/// Modular NVS saves to prevent flash wear-out during web requests
/// Modular NVS saves to prevent flash wear-out during web requests
void configSavePin(const char* pin);
void configSaveTiming();
void configSaveWifi();
void configSavePhones();
void configSaveRouter();
void configSaveZones();
void configSavePeriodic();
void configSaveWhatsapp();
void configSaveMqtt();
void configSaveOnvif();
void configSaveHeartbeat();
void configSaveTimezone();
void configSaveSchedule();

bool configGetHeartbeatEnabled();
void configSetHeartbeatEnabled(bool en);

const char* configGetTimezone();
void configSetTimezone(const char* tz);

void configGetSchedule(int dayOfWeek, int8_t &armHr, int8_t &armMin, int8_t &disarmHr, int8_t &disarmMin);
void configSetSchedule(int dayOfWeek, int8_t armHr, int8_t armMin, int8_t disarmHr, int8_t disarmMin);
uint8_t configGetScheduleMode();
void configSetScheduleMode(uint8_t mode);

/// Reset all configuration to factory defaults.
TaskHandle_t configGetLockOwner();
void configFactoryReset();

/// Print current configuration to Serial.
void configPrint();

void configSaveAlarmState(AlarmState state);
AlarmState configLoadAlarmState();

void configSaveSecurityState(uint8_t failedAttempts, bool lockedOut);
void configLoadSecurityState(uint8_t &failedAttempts, bool &lockedOut);

void configSaveSirenAccum(uint32_t seconds);
uint32_t configLoadSirenAccum();

// RTC Helpers for Chronos Anchor
uint32_t rtcGetDelayRemaining();
void rtcSetDelayRemaining(uint32_t s);
bool rtcIsValid();

#endif // SF_ALARM_CONFIG_MANAGER_H
