#include "alarm_controller.h"
#include "io_expander.h"
#include "config.h"
#include <Arduino.h>
#include "logging.h"
#include <string.h>
#include "config_manager.h"
#include "alarm_zones.h"

static const char* TAG = "ALM";

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static AlarmState       currentState     = ALARM_DISARMED;
static AlarmState       returnState      = ALARM_DISARMED; 
static SemaphoreHandle_t stateMutex       = NULL;
static AlarmEventCallback eventCallback  = nullptr;

static const char* alarmGetStateStrInternal();

static char     alarmPin[MAX_PIN_LEN]    = "1234";  // Default PIN
static uint16_t exitDelaySec             = DEFAULT_EXIT_DELAY_S;
static uint16_t entryDelaySec            = DEFAULT_ENTRY_DELAY_S;
static uint16_t sirenDurationSec         = DEFAULT_SIREN_DURATION_S;
static uint8_t  sirenOutputChannel       = 0;   // Output channel 0 = siren

static uint16_t activeAlarmMask  = 0;    // Bitmask of all zones triggered in current cycle
static uint32_t delayStartMs     = 0;
static uint32_t sirenStartMs     = 0;
static uint32_t firstTriggerMs   = 0;    // Strict start for noise ordinance compliance
static bool     sirenActive      = false;
static bool     sirenMuted       = false;

static uint8_t  triggeringZone   = 0xFF; // First zone that tripped

uint16_t alarmGetActiveAlarmMask() { return activeAlarmMask; }

// Pending arm mode during exit delay (separate from triggeringZone)
enum PendingArmMode : uint8_t { ARM_PENDING_AWAY = 0, ARM_PENDING_HOME = 1 };
static PendingArmMode pendingArmMode     = ARM_PENDING_AWAY;

// Security: PIN Lockout
static uint8_t  failedAttempts           = 0;
static uint32_t lockoutStartMs           = 0;
static bool     lockedOut                = false;
static const uint8_t  MAX_FAILED_ATTEMPTS = 5;
static const uint32_t LOCKOUT_DURATION_MS = 300000; // 5 minutes

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void fireEvent(AlarmEvent event, int8_t zoneId = -1, const char* details = "")
{
    if (eventCallback) {
        AlarmEventInfo info;
        info.event = event;
        info.zoneId = zoneId;
        info.details = details;
        eventCallback(info);
    }
}

/// Constant-time string comparison — prevents timing side-channel PIN brute-force.
/// Returns true only if both strings are identical AND same length.
/// Constant-time string comparison — prevents timing side-channel PIN brute-force.
static bool pinEquals(const char* a, const char* b)
{
    // Fix: We must compare a fixed maximum length to avoid timing leaks based on string lengths.
    // MAX_PIN_LEN is used as the security boundary.
    uint8_t diff = 0;
    
    for (size_t i = 0; i < MAX_PIN_LEN; i++) {
        uint8_t ca = (i < MAX_PIN_LEN && a[i] != '\0') ? (uint8_t)a[i] : 0;
        uint8_t cb = (i < MAX_PIN_LEN && b[i] != '\0') ? (uint8_t)b[i] : 0;
        diff |= ca ^ cb;
        
        // We MUST NOT break early if one string ends; that creates the timing leak.
        // We continue to XOR zeros to finish the cycle.
    }
    
    // Also ensure they aren't both empty or one isn't a prefix of the other by checking total length difference
    // but doing so in a way that doesn't leak.
    size_t lenA = 0; while(lenA < MAX_PIN_LEN && a[lenA]) lenA++;
    size_t lenB = 0; while(lenB < MAX_PIN_LEN && b[lenB]) lenB++;
    
    return (diff == 0) && (lenA == lenB);
}

static void sirenOn(int8_t zoneId, const char* name)
{
    char details[64];
    snprintf(details, sizeof(details), "Zone %d (%s)", zoneId + 1, name);

    if (!sirenActive) {
        sirenActive  = true;
        sirenMuted   = false;
        
        // Strict Noise Ordinance Compliance: 
        // firstTriggerMs is set at the start of the TRIGGERED state and 
        // NEVER reset until DISARMED, ensuring total siren duration doesn't restart.
        if (firstTriggerMs == 0) {
            firstTriggerMs = millis();
            LOG_INFO(TAG, "Strict noise ordinance window started");
        }
        
        sirenStartMs = millis();
        ioExpanderSetOutput(sirenOutputChannel, true);
        LOG_WARN(TAG, "Siren: ON (Zone %d: %s)", zoneId + 1, name);
        fireEvent(EVT_SIREN_ON, zoneId, details);
    }
}

static void sirenOff()
{
    if (sirenActive) {
        sirenActive = false;
        // Note: firstTriggerMs is NOT reset here. It persists until Disarm.
        ioExpanderSetOutput(sirenOutputChannel, false);
        LOG_INFO(TAG, "Siren: OFF");
        fireEvent(EVT_SIREN_OFF, -1, "Manual or Timeout");
    }
}

/// Internal: verify PIN with full lockout side-effects (for arm/disarm)
static bool validatePin(const char* pin)
{
    // Overflow-safe lockout check
    if (lockedOut) {
        if ((millis() - lockoutStartMs) < LOCKOUT_DURATION_MS) {
            LOG_WARN(TAG, "SECURITY: PIN entry locked due to multiple failures");
            return false;
        } else {
            // Lockout expired — MUST reset counter to prevent instant re-lockout on next failure
            lockedOut = false;
            failedAttempts = 0;
            LOG_INFO(TAG, "SECURITY: Lockout expired");
        }
    }

    if (pin == nullptr || strlen(pin) == 0) return false;

    if (pinEquals(pin, alarmPin)) {
        failedAttempts = 0;
        return true;
    } else {
        failedAttempts++;
        if (failedAttempts >= MAX_FAILED_ATTEMPTS) {
            lockedOut = true;
            lockoutStartMs = millis();
            LOG_ERROR(TAG, "SECURITY: Too many failed attempts! Locked out for %lu minutes.", LOCKOUT_DURATION_MS / 60000);
        }
        return false;
    }
}

/// Public: check PIN and explicitly enforce the 5-minute lockout side-effects across all interfaces.
/// This prevents high-speed Web API local dictionary brute-forcing.
bool alarmValidatePin(const char* pin)
{
    return validatePin(pin);
}

static void setState(AlarmState newState)
{
    if (!stateMutex) return;
    
    // Infallible State Transition: Use a longer timeout or block to ensure DISARM/TRIGGER 
    // is never ignored due to transient contention with telemetry/MQTT polling.
    if (xSemaphoreTake(stateMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        if (currentState != newState) {
            // Capture return state when transitioning out of stable armed/disarmed states
            if (currentState == ALARM_DISARMED ||
                currentState == ALARM_ARMED_AWAY ||
                currentState == ALARM_ARMED_HOME) {
                returnState = currentState;
            }

            const char* oldStr = alarmGetStateStrInternal();
            currentState = newState;
            LOG_INFO(TAG, "State: %s -> %s", oldStr, alarmGetStateStrInternal());
            
            // Clean up noise ordinance timer on disarm
            if (newState == ALARM_DISARMED) {
                firstTriggerMs = 0;
            }

            // Industrial Hardening: Persist state to survive reboots/power cycles
            configSaveAlarmState(currentState);
        }
        xSemaphoreGive(stateMutex);
    } else {
        LOG_ERROR(TAG, "CRITICAL: State transition failed (Mutex DEADLOCK?)");
    }
}

// ---------------------------------------------------------------------------
// Zone event handler — called by zone manager
// ---------------------------------------------------------------------------

static void onZoneEvent(uint8_t zoneIndex, ZoneState newState)
{
    const ZoneInfo* info = zonesGetInfo(zoneIndex);
    if (!info) return;

    char details[80];
    snprintf(details, sizeof(details), "Zone %d (%s)", zoneIndex + 1, info->config.name);

    if (newState == ZONE_TRIGGERED || newState == ZONE_TAMPER) {
        if (newState == ZONE_TAMPER) {
            fireEvent(EVT_TAMPER, zoneIndex, details);
            triggeringZone = zoneIndex;
            setState(ALARM_TRIGGERED);
            sirenOn(zoneIndex, info->config.name);
            fireEvent(EVT_ALARM_TRIGGERED, zoneIndex, "Tamper Escallation");
            return;
        }

        fireEvent(EVT_ZONE_TRIGGERED, zoneIndex, details);

        switch (currentState) {
            case ALARM_DISARMED:
                // Only 24H zones trigger when disarmed
                if (info->config.type == ZONE_24H) {
                    triggeringZone = zoneIndex;
                    setState(ALARM_TRIGGERED);
                    sirenOn(zoneIndex, info->config.name);
                    snprintf(details, sizeof(details),
                             "24H Zone %d (%s) ALARM!", zoneIndex + 1, info->config.name);
                    fireEvent(EVT_ALARM_TRIGGERED, zoneIndex, details);
                }
                break;

            case ALARM_ARMED_AWAY:
            case ALARM_ARMED_HOME:
                // Check zone type
                if (info->config.type == ZONE_DELAYED) {
                    delayStartMs   = millis();
                    triggeringZone = zoneIndex;
                    setState(ALARM_ENTRY_DELAY);
                    fireEvent(EVT_ENTRY_DELAY, zoneIndex, "Entry delay started");
                } else if (info->config.type == ZONE_FOLLOWER) {
                    // Follower: instant unless entry delay already running
                    if (currentState == ALARM_ENTRY_DELAY) {
                        // Already in entry delay — do nothing extra
                    } else {
                        triggeringZone = zoneIndex;
                        setState(ALARM_TRIGGERED);
                        sirenOn(zoneIndex, info->config.name);
                        fireEvent(EVT_ALARM_TRIGGERED, zoneIndex, "Follower Alarm");
                    }
                } else {
                    // Instant or 24H — immediate alarm
                    triggeringZone = zoneIndex;
                    setState(ALARM_TRIGGERED);
                    sirenOn(zoneIndex, info->config.name);
                    fireEvent(EVT_ALARM_TRIGGERED, zoneIndex, "Instant Alarm");
                }
                break;

            case ALARM_ENTRY_DELAY:
                // Additional zone triggered during entry delay
                if (info->config.type == ZONE_INSTANT || info->config.type == ZONE_24H) {
                    triggeringZone = zoneIndex;
                    setState(ALARM_TRIGGERED);
                    sirenOn(zoneIndex, info->config.name);
                    fireEvent(EVT_ALARM_TRIGGERED, zoneIndex, "Instant Alarm during delay");
                }
                break;

            case ALARM_TRIGGERED:
                // Escalation: A NEW zone triggered while the alarm is already sounding
                activeAlarmMask |= (1 << zoneIndex);
                if (zoneIndex != triggeringZone) {
                    fireEvent(EVT_ALARM_TRIGGERED, zoneIndex, "Escalation");
                }
                break;

            default:
                break;
        }
    } else if (newState == ZONE_NORMAL) {
        fireEvent(EVT_ZONE_RESTORED, zoneIndex, details);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void alarmInit()
{
    // Register our zone event handler first so restoration doesn't cause missed transitions if they differ
    zonesSetCallback(onZoneEvent);

    if (stateMutex == NULL) {
        stateMutex = xSemaphoreCreateMutex();
    }
    
    // Restore persistent state from NVS
    AlarmState savedState = configLoadAlarmState();
    if (savedState == ALARM_ARMED_AWAY || savedState == ALARM_ARMED_HOME) {
        currentState = savedState;
        LOG_INFO(TAG, "Restored state from NVS: %s", alarmGetStateStr());
    } else {
        currentState = ALARM_DISARMED;
    }
    
    sirenActive     = false;
    sirenMuted      = false;
    triggeringZone  = 0xFF;

    LOG_INFO(TAG, "Controller initialized — %s", alarmGetStateStr());
}

void alarmSetCallback(AlarmEventCallback cb)
{
    eventCallback = cb;
}

void alarmUpdate()
{
    uint32_t now = millis();

    switch (currentState) {
        case ALARM_EXIT_DELAY: {
            uint32_t elapsed = now - delayStartMs;
            if (elapsed >= (uint32_t)exitDelaySec * 1000) {
                // Exit delay complete — verify doors actually closed before arming
                if (!zonesAllClear()) {
                    setState(ALARM_TRIGGERED);
                    sirenOn(-1, "Exit Failure");
                    fireEvent(EVT_ALARM_TRIGGERED, -1, "Zones open at arming");
                    triggeringZone = 0xFF;
                } else {
                    // System is armed normally
                    if (pendingArmMode == ARM_PENDING_HOME) {
                        setState(ALARM_ARMED_HOME);
                        fireEvent(EVT_ARMED_HOME, -1, "Success");
                    } else {
                        setState(ALARM_ARMED_AWAY);
                        fireEvent(EVT_ARMED_AWAY, -1, "Success");
                    }
                    triggeringZone = 0xFF;
                }
            }
            break;
        }

        case ALARM_ENTRY_DELAY: {
            uint32_t elapsed = now - delayStartMs;
            if (elapsed >= (uint32_t)entryDelaySec * 1000) {
                // Entry delay expired — trigger alarm
                setState(ALARM_TRIGGERED);
                sirenOn(-1, "Delay Timeout");
                fireEvent(EVT_ALARM_TRIGGERED, -1, "Entry delay expired");
            }
            break;
        }

        case ALARM_TRIGGERED: {
            // Auto-silence siren after strict duration (Noise Ordinance Compliance)
            if (sirenDurationSec > 0 && firstTriggerMs > 0) {
                uint32_t elapsed = now - firstTriggerMs;
                if (elapsed >= (uint32_t)sirenDurationSec * 1000) {
                    sirenOff();
                    LOG_INFO(TAG, "Siren auto-silenced after strict duration timeout.");
                    activeAlarmMask = 0;
                    triggeringZone = 0xFF;

                    // Re-arm system gracefully
                    setState(returnState);

                    if (returnState == ALARM_ARMED_AWAY) {
                        fireEvent(EVT_ARMED_AWAY, -1, "Auto-Restore");
                    } else if (returnState == ALARM_ARMED_HOME) {
                        fireEvent(EVT_ARMED_HOME, -1, "Auto-Restore");
                    } else {
                        fireEvent(EVT_DISARMED, -1, "Auto-Restore");
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

bool alarmArmAway(const char* pin)
{
    if (!validatePin(pin)) {
        LOG_WARN(TAG, "Arm AWAY failed — invalid PIN");
        return false;
    }

    if (!zonesAllClear()) {
        LOG_WARN(TAG, "Arm AWAY failed — zones not clear");
        zonesPrintStatus();
        return false;
    }

    // Start exit delay
    delayStartMs    = millis();
    pendingArmMode  = ARM_PENDING_AWAY;
    setState(ALARM_EXIT_DELAY);
    fireEvent(EVT_EXIT_DELAY, -1, "Arming AWAY");
    LOG_INFO(TAG, "Exit delay: %d seconds", exitDelaySec);
    return true;
}

bool alarmArmHome(const char* pin)
{
    if (!validatePin(pin)) {
        LOG_WARN(TAG, "Arm HOME failed — invalid PIN");
        return false;
    }

    if (!zonesAllClear()) {
        LOG_WARN(TAG, "Arm HOME failed — zones not clear");
        zonesPrintStatus();
        return false;
    }

    delayStartMs    = millis();
    pendingArmMode  = ARM_PENDING_HOME;
    setState(ALARM_EXIT_DELAY);
    fireEvent(EVT_EXIT_DELAY, -1, "Arming HOME");
    LOG_INFO(TAG, "Exit delay: %d seconds", exitDelaySec);
    return true;
}

bool alarmDisarm(const char* pin)
{
    if (!validatePin(pin)) {
        LOG_WARN(TAG, "Disarm failed — invalid PIN");
        return false;
    }

    sirenOff();
    setState(ALARM_DISARMED);
    triggeringZone = 0xFF;
    fireEvent(EVT_DISARMED, -1, "Manual Disarm");
    return true;
}

void alarmMuteSiren()
{
    if (sirenActive) {
        sirenMuted = true;
        ioExpanderSetOutput(sirenOutputChannel, false);
        LOG_INFO(TAG, "Siren MUTED (alarm still active)");
        fireEvent(EVT_SIREN_OFF, -1, "Manual Mute");
    }
}

AlarmState alarmGetState()
{
    AlarmState st = ALARM_DISARMED;
    if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        st = currentState;
        xSemaphoreGive(stateMutex);
    }
    return st;
}

static const char* alarmGetStateStrInternal()
{
    switch (currentState) {
        case ALARM_DISARMED:     return "DISARMED";
        case ALARM_EXIT_DELAY:   return "EXIT_DELAY";
        case ALARM_ARMED_AWAY:   return "ARMED_AWAY";
        case ALARM_ARMED_HOME:   return "ARMED_HOME";
        case ALARM_ENTRY_DELAY:  return "ENTRY_DELAY";
        case ALARM_TRIGGERED:    return "TRIGGERED";
        default:                 return "UNKNOWN";
    }
}

const char* alarmGetStateStr()
{
    const char* str = "UNKNOWN";
    if (stateMutex && xSemaphoreTake(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        str = alarmGetStateStrInternal();
        xSemaphoreGive(stateMutex);
    }
    return str;
}

uint16_t alarmGetDelayRemaining()
{
    uint32_t now = millis();
    uint32_t elapsed;

    switch (currentState) {
        case ALARM_EXIT_DELAY:
            elapsed = now - delayStartMs;
            if (elapsed >= (uint32_t)exitDelaySec * 1000) return 0;
            return (uint16_t)(exitDelaySec - elapsed / 1000);

        case ALARM_ENTRY_DELAY:
            elapsed = now - delayStartMs;
            if (elapsed >= (uint32_t)entryDelaySec * 1000) return 0;
            return (uint16_t)(entryDelaySec - elapsed / 1000);

        default:
            return 0;
    }
}

void alarmSetPin(const char* pin)
{
    strncpy(alarmPin, pin, MAX_PIN_LEN - 1);
    alarmPin[MAX_PIN_LEN - 1] = '\0';
    LOG_INFO(TAG, "PIN successfully updated");
}

const char* alarmGetPin()
{
    return alarmPin;
}



uint32_t alarmGetLockoutRemaining()
{
    if (!lockedOut) return 0;
    uint32_t elapsed = millis() - lockoutStartMs;
    if (elapsed >= LOCKOUT_DURATION_MS) return 0;
    return (LOCKOUT_DURATION_MS - elapsed) / 1000;
}

void alarmSetExitDelay(uint16_t seconds)
{
    exitDelaySec = seconds;
    LOG_INFO(TAG, "Exit delay set to %d seconds", seconds);
}

uint16_t alarmGetExitDelay() { return exitDelaySec; }

void alarmSetEntryDelay(uint16_t seconds)
{
    entryDelaySec = seconds;
    LOG_INFO(TAG, "Entry delay set to %d seconds", seconds);
}

uint16_t alarmGetEntryDelay() { return entryDelaySec; }

void alarmSetSirenDuration(uint16_t seconds)
{
    sirenDurationSec = seconds;
    LOG_INFO(TAG, "Siren duration set to %d seconds", seconds);
}

uint16_t alarmGetSirenDuration() { return sirenDurationSec; }

void alarmSetSirenOutput(uint8_t channel)
{
    if (channel < 16 && channel != sirenOutputChannel) {
        // If the siren is currently active, turn off the old relay before switching
        bool wasActive = sirenActive;
        if (wasActive) {
            ioExpanderSetOutput(sirenOutputChannel, false);
        }
        
        sirenOutputChannel = channel;
        
        // Turn on the new relay
        if (wasActive) {
            ioExpanderSetOutput(sirenOutputChannel, true);
        }
        LOG_INFO(TAG, "Siren output channel set to %d", channel);
    }
}

uint8_t alarmGetSirenOutput() { return sirenOutputChannel; }

void alarmPrintStatus()
{
    Serial.println("=== Alarm System Status ===");
    Serial.printf("  State:          %s\n", alarmGetStateStr());

    if (currentState == ALARM_EXIT_DELAY || currentState == ALARM_ENTRY_DELAY) {
        Serial.printf("  Delay remaining: %d sec\n", alarmGetDelayRemaining());
    }

    Serial.printf("  Siren:          %s\n",
                  sirenActive ? (sirenMuted ? "MUTED" : "ACTIVE") : "OFF");
    Serial.printf("  Siren channel:  %d\n", sirenOutputChannel);
    Serial.printf("  Exit delay:     %d sec\n", exitDelaySec);
    Serial.printf("  Entry delay:    %d sec\n", entryDelaySec);
    Serial.printf("  Siren duration: %d sec\n", sirenDurationSec);

    if (triggeringZone != 0xFF && triggeringZone < MAX_ZONES) {
        const ZoneInfo* zi = zonesGetInfo(triggeringZone);
        if (zi) {
            Serial.printf("  Trigger zone:   %d (%s)\n",
                          triggeringZone + 1, zi->config.name);
        }
    }
    Serial.println("===========================");
}
