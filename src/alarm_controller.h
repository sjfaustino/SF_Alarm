#ifndef SF_ALARM_CONTROLLER_H
#define SF_ALARM_CONTROLLER_H

#include <Arduino.h>
#include "alarm_zones.h"

// ---------------------------------------------------------------------------
// Alarm System States
// ---------------------------------------------------------------------------
enum AlarmState : uint8_t {
    ALARM_DISARMED    = 0,
    ALARM_EXIT_DELAY  = 1,   // Counting down to armed
    ALARM_ARMED_AWAY  = 2,   // All zones active
    ALARM_ARMED_HOME  = 3,   // Perimeter only (24H + instant)
    ALARM_ENTRY_DELAY = 4,   // Entry delay countdown
    ALARM_TRIGGERED   = 5,   // Alarm active — siren on, SMS sent
};

// ---------------------------------------------------------------------------
// Alarm Event Type (for notification)
// ---------------------------------------------------------------------------
enum AlarmEvent : uint8_t {
    EVT_ARMED_AWAY,
    EVT_ARMED_HOME,
    EVT_DISARMED,
    EVT_ALARM_TRIGGERED,
    EVT_ALARM_RESTORED,
    EVT_ENTRY_DELAY,
    EVT_EXIT_DELAY,
    EVT_ZONE_TRIGGERED,
    EVT_ZONE_RESTORED,
    EVT_TAMPER,
    EVT_SIREN_ON,
    EVT_SIREN_OFF,
};

// ---------------------------------------------------------------------------
// Callback for alarm events (used by SMS notifier)
// ---------------------------------------------------------------------------
typedef void (*AlarmEventCallback)(AlarmEvent event, const char* details);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Initialize the alarm controller.
void alarmInit();

/// Set callback for alarm events (SMS notifications, logging, etc.)
void alarmSetCallback(AlarmEventCallback cb);

/// Main update loop — call frequently (e.g., every cycle in loop()).
void alarmUpdate();

/// Arm the system (away mode). Returns false if zones not clear.
bool alarmArmAway(const char* pin);

/// Arm the system (home mode). Returns false if zones not clear.
bool alarmArmHome(const char* pin);

/// Disarm the system. Returns false if PIN is wrong.
bool alarmDisarm(const char* pin);

/// Mute/silence the siren without disarming.
void alarmMuteSiren();

/// Get the current alarm state.
AlarmState alarmGetState();

/// Get a human-readable string for the current state.
const char* alarmGetStateStr();

/// Get the remaining delay time (entry or exit), in seconds.
uint16_t alarmGetDelayRemaining();

/// Set the alarm PIN code.
void alarmSetPin(const char* pin);

/// Get the current alarm PIN code.
const char* alarmGetPin();

/// Set entry/exit delay durations (in seconds).
void alarmSetExitDelay(uint16_t seconds);
void alarmSetEntryDelay(uint16_t seconds);

/// Get entry/exit delay durations (in seconds).
uint16_t alarmGetExitDelay();
uint16_t alarmGetEntryDelay();

/// Set siren duration (in seconds, 0 = unlimited until disarm).
void alarmSetSirenDuration(uint16_t seconds);

/// Get siren duration (in seconds).
uint16_t alarmGetSirenDuration();

/// Set the output channel used for the siren (0–15, default 0).
void alarmSetSirenOutput(uint8_t channel);

/// Get the siren output channel.
uint8_t alarmGetSirenOutput();

/// Print alarm status to Serial.
void alarmPrintStatus();

#endif // SF_ALARM_CONTROLLER_H
