#ifndef SF_ALARM_SERIAL_CLI_H
#define SF_ALARM_SERIAL_CLI_H

#include <Arduino.h>

/// Initialize the serial CLI.
void cliInit();

/// Process serial input. Call in loop().
void cliUpdate();

#endif // SF_ALARM_SERIAL_CLI_H
