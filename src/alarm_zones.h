#ifndef SF_ALARM_ZONES_H
#define SF_ALARM_ZONES_H

#include <Arduino.h>
#include "config.h"

// ---------------------------------------------------------------------------
// Zone Types
// ---------------------------------------------------------------------------
enum ZoneType : uint8_t {
    ZONE_INSTANT   = 0,   // Triggers alarm immediately when armed
    ZONE_DELAYED   = 1,   // Uses entry/exit delay
    ZONE_24H       = 2,   // Always active (fire, tamper, etc.)
    ZONE_FOLLOWER  = 3,   // Instant unless an entry delay is already running
};

// ---------------------------------------------------------------------------
// Zone Wiring
// ---------------------------------------------------------------------------
enum ZoneWiring : uint8_t {
    ZONE_NO  = 0,   // Normally Open  — triggered when closed (input = 1)
    ZONE_NC  = 1,   // Normally Closed — triggered when open  (input = 0)
};

// ---------------------------------------------------------------------------
// Zone State
// ---------------------------------------------------------------------------
enum ZoneState : uint8_t {
    ZONE_NORMAL    = 0,
    ZONE_TRIGGERED = 1,
    ZONE_TAMPER    = 2,
    ZONE_FAULT     = 3,
    ZONE_BYPASSED  = 4,
};

// ---------------------------------------------------------------------------
// Zone Configuration (stored in NVS)
// ---------------------------------------------------------------------------
struct ZoneConfig {
    char     name[MAX_ZONE_NAME_LEN];
    ZoneType type;
    ZoneWiring wiring;
    bool     enabled;
};

// ---------------------------------------------------------------------------
// Zone Runtime State
// ---------------------------------------------------------------------------
struct ZoneInfo {
    ZoneConfig config;
    ZoneState  state;
    bool       rawInput;         // Current debounced input level (1=triggered for NO)
    uint32_t   lastChangeMs;     // Timestamp of last state change
    uint32_t   debounceStartMs;  // Debounce timer start
    bool       debouncing;       // Debounce in progress
    bool       pendingLevel;     // Level being debounced
};

// ---------------------------------------------------------------------------
// Callback type for zone events
// ---------------------------------------------------------------------------
typedef void (*ZoneEventCallback)(uint8_t zoneIndex, ZoneState newState);

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/// Initialize zone manager with default configurations.
void zonesInit();

/// Set event callback — called whenever a zone changes state.
void zonesSetCallback(ZoneEventCallback cb);

/// Update zone states from raw input bitmask. Call periodically (e.g. every 20ms).
void zonesUpdate(uint16_t inputBitmask);

/// Get info for a specific zone.
const ZoneInfo* zonesGetInfo(uint8_t zoneIndex);

/// Get configuration for a specific zone (mutable for editing).
ZoneConfig* zonesGetConfig(uint8_t zoneIndex);

/// Bypass/unbypass a zone.
void zonesSetBypassed(uint8_t zoneIndex, bool bypassed);

/// Check if all enabled zones are in NORMAL state.
bool zonesAllClear();

/// Get a bitmask of all triggered zones.
uint16_t zonesGetTriggeredMask();

/// Print zone status summary to Serial.
void zonesPrintStatus();

#endif // SF_ALARM_ZONES_H
