#ifndef SF_ALARM_CONFIG_MANAGER_H
#define SF_ALARM_CONFIG_MANAGER_H

#include <Arduino.h>

/// Initialize the config manager (opens NVS namespace).
void configInit();

/// Load all configuration from NVS into the running modules.
/// Call after all modules are initialized.
void configLoad();

/// Save all current configuration to NVS.
void configSave();

/// Modular NVS saves to prevent flash wear-out during web requests
void configSavePin();
void configSaveTiming();
void configSaveWifi();
void configSavePhones();
void configSaveRouter();
void configSaveZones();
void configSavePeriodic();
void configSaveWhatsapp();
void configSaveMqtt();
void configSaveOnvif();

/// Reset all configuration to factory defaults.
void configFactoryReset();

/// Print current configuration to Serial.
void configPrint();

#endif // SF_ALARM_CONFIG_MANAGER_H
