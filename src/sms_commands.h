#ifndef SF_ALARM_SMS_COMMANDS_H
#define SF_ALARM_SMS_COMMANDS_H

#include <Arduino.h>
#include "config.h"

// ---------------------------------------------------------------------------
// GA09-compatible SMS command set (extended for 16 channels)
// ---------------------------------------------------------------------------
// Phone numbers:
//   #01#number#        Set SMS alert phone 1 (01–05)
//   @#num1#num2#...    Set multiple SMS alert phones at once
//
// Alarm text per zone:
//   #1#Alarm text      Set alarm SMS text for zone 1 (1–16)
//
// NO/NC wiring:
//   *NC246             Set zones 2,4,6 as NC (others NO)
//   *NC0               Set all zones as NO
//   *NCALL             Set all zones as NC
//
// Arm / Disarm:
//   ARM                Arm the system (away mode)
//   ARM HOME           Arm the system (home mode)
//   DISARM             Disarm the system
//   DISARM <pin>       Disarm with PIN
//
// Status:
//   @#STATUS?          Query system status
//   STATUS             Query system status (alias)
//
// Siren:
//   MUTE               Silence active siren
//
// Help:
//   HELP               List available commands
//
// Zone bypass:
//   BYPASS <n>         Bypass zone n
//   UNBYPASS <n>       Restore zone n
//
// Periodic Report:
//   %#Txx              Set report interval in minutes (0=off, max 9999)
//
// Working Mode (GA09):
//   %#M1 (SMS), %#M2 (Call), %#M3 (Both)
//
// Recovery Text (GA09):
//   #0#text            Set SMS text sent when all zones return to normal
//
// Zone Arming (Binary):
//   @@#ARMXXXXXXXX     Enable/disable zones 1-8 via binary (GA09 parity)
//   @#ARMXXXXXXXXXXXXXXXX Enable/disable zones 1-16 via binary (SF extension)
//
// Voice Call (Unsupported Placeholder):
//   &#num1#num2#...    Set call update phone numbers (replies with unsupported msg)
// ---------------------------------------------------------------------------

/// Initialize the SMS command processor.
void smsCmdInit();

/// Process a received SMS message. Executes the command and sends a reply.
void smsCmdProcess(const char* sender, const char* body);

/// Add an authorized phone number (returns slot index or -1).
int smsCmdAddPhone(const char* phone);

/// Set a specific phone slot (0-based index).
bool smsCmdSetPhone(int slot, const char* phone);

/// Remove an authorized phone number.
bool smsCmdRemovePhone(const char* phone);

/// Get the number of configured alert phone numbers.
int smsCmdGetPhoneCount();

/// Get a phone number by index.
const char* smsCmdGetPhone(int index);

/// Clear all phone numbers.
void smsCmdClearPhones();

/// Send an alert to all configured phone numbers.
void smsCmdSendAlert(const char* message);

/// Get the custom alarm text for a zone (0-based).
const char* smsCmdGetAlarmText(int zoneIndex);

/// Set the custom alarm text for a zone (0-based).
void smsCmdSetAlarmText(int zoneIndex, const char* text);

/// Get the periodic status report interval in minutes (0 = disabled).
uint16_t smsCmdGetReportInterval();

/// Set the periodic status report interval in minutes.
void smsCmdSetReportInterval(uint16_t minutes);

/// Get the custom recovery text (sent when all zones are clear).
const char* smsCmdGetRecoveryText();

/// Set the custom recovery text.
void smsCmdSetRecoveryText(const char* text);

/// Working modes for GA09 compatibility
enum WorkingMode {
    MODE_SMS = 1,
    MODE_CALL = 2,
    MODE_BOTH = 3
};

/// Get the current operational working mode.
WorkingMode smsCmdGetWorkingMode();

/// Set the current operational working mode.
void smsCmdSetWorkingMode(WorkingMode mode);

#endif // SF_ALARM_SMS_COMMANDS_H
