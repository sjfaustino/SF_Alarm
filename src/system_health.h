#ifndef SF_ALARM_SYSTEM_HEALTH_H
#define SF_ALARM_SYSTEM_HEALTH_H

#include <Arduino.h>

// Tungsten Aegis Heartbeat Bits
#define HB_BIT_ZONE   (1 << 0)
#define HB_BIT_NET    (1 << 1)
#define HB_BIT_MQTT   (1 << 2)
#define HB_BIT_MAINT  (1 << 3)
#define HB_BIT_CLI    (1 << 4)
#define HB_BIT_ALERT  (1 << 5)
#define HB_BIT_WEB    (1 << 6)
#define HB_BIT_I2C    (1 << 7)
#define HB_BIT_CONFIG (1 << 8)

#define HB_ALL_HEALTHY (HB_BIT_ZONE | HB_BIT_NET | HB_BIT_MQTT | HB_BIT_MAINT | HB_BIT_CLI | HB_BIT_ALERT | HB_BIT_WEB | HB_BIT_I2C | HB_BIT_CONFIG)

/// Report a heartbeat to the system watchdog
void sysHealthReport(uint16_t bit);

#endif // SF_ALARM_SYSTEM_HEALTH_H
