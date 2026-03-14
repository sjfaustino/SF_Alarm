#ifndef SF_ALARM_SERIAL_CLI_H
#define SF_ALARM_SERIAL_CLI_H

#include <Arduino.h>

/// Initialize the serial CLI.
struct SystemContext;
void cliInit(SystemContext* ctx);
void cliUpdate();

#endif // SF_ALARM_SERIAL_CLI_H
