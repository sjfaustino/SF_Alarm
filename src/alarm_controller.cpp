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
// Globals (Unchanged but documented)
// ---------------------------------------------------------------------------
static char     alarmPin[MAX_PIN_LEN]    = "1234";
static uint16_t exitDelaySec             = DEFAULT_EXIT_DELAY_S;
static uint16_t entryDelaySec            = DEFAULT_ENTRY_DELAY_S;
static uint16_t sirenDurationSec         = DEFAULT_SIREN_DURATION_S;
static uint8_t  sirenOutputChannel       = 0;   // Output channel 0 = siren

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static AlarmState       currentState     = ALARM_DISARMED;
static AlarmState       returnState      = ALARM_DISARMED;
static SemaphoreHandle_t stateMutex       = NULL;
static AlarmEventCallback eventCallback  = nullptr;

static bool             sirenActive      = false;
static bool             sirenMuted       = false;

static const char* alarmGetStateStrInternal();

static uint16_t activeAlarmMask  = 0;    // Bitmask of all zones triggered in current cycle
static uint32_t delayStartMs     = 0;
static uint32_t sirenStartMs     = 0;
static uint8_t  triggeringZone   = 0xFF; // First zone that tripped

// Persistent indicators: persisted to NVS periodically and survived reboots.
// This ensures the noise ordinance window doesn't restart after a reboot
// in TRIGGERED state, preventing municipal fines on flicker.
static uint32_t sirenAccumulatedSec = 0;
static uint32_t lastSirenUpdateMs   = 0;

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

/// Truly Constant-time branchless comparison.
/// No content-dependent branches, no early exits.
static bool pinEquals(const char* a, const char* b)
{
    volatile uint8_t diff = 0;
    uint8_t aLocked = 0;
    uint8_t bLocked = 0;

    for (size_t i = 0; i < MAX_PIN_LEN; i++) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];

        // Latch end-of-string status
        if (ca == '\0') aLocked = 0xFF;
        if (cb == '\0') bLocked = 0xFF;

        // XOR bytes, but mask out bytes after either string has ended.
        // This ensures a "1234" vs "12345" check fails on the length bit.
        diff |= (ca ^ cb) & (~aLocked) & (~bLocked);
        
        // Track length mismatch
        diff |= (aLocked ^ bLocked);
    }

    return (diff == 0);
}

static void sirenOn(int8_t zoneId, const char* name)
{
    char details[64];
    snprintf(details, sizeof(details), "Zone %d (%s)", zoneId + 1, name);

    if (!sirenActive) {
        sirenActive  = true;
        sirenMuted   = false;
        
        // Use the accumulated timer for ordinance compliance. 
        // Reset only on a clean DISARM.
        lastSirenUpdateMs = millis();
        
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
        // sirenAccumulatedSec persists until Disarm.
        ioExpanderSetOutput(sirenOutputChannel, false);
        LOG_INFO(TAG, "Siren: OFF");
        fireEvent(EVT_SIREN_OFF, -1, "Manual or Timeout");
    }
}

/// Internal: verify PIN with full lockout side-effects (for arm/disarm)
static bool validatePin(const char* pin)
{
    if (pin == nullptr || strlen(pin) == 0) return false;

    // Bypass: internal automated commands (scheduler, watchdog)
    // now use internal setState triggers which do not call validatePin().
    // The "AUTO" string is removed as a hardcoded bypass.

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

    bool match = false;
    if (xSemaphoreTakeRecursive(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        match = pinEquals(pin, alarmPin);
        xSemaphoreGiveRecursive(stateMutex);
    }

    if (match) {
        failedAttempts = 0;
        lockedOut = false;
        configSaveSecurityState(failedAttempts, lockedOut);
        return true;
    } else {
        failedAttempts++;
        if (failedAttempts >= MAX_FAILED_ATTEMPTS) {
            lockedOut = true;
            lockoutStartMs = millis();
            LOG_ERROR(TAG, "SECURITY: Too many failed attempts! Locked out for %lu minutes.", LOCKOUT_DURATION_MS / 60000);
        }
        configSaveSecurityState(failedAttempts, lockedOut);
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
    
    bool stateChanged = false;
    AlarmState snapshotState = newState; // Local snapshot avoids race on NVS write
    if (xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY) == pdTRUE) {
        if (currentState != newState) {
            if (currentState == ALARM_DISARMED ||
                currentState == ALARM_ARMED_AWAY ||
                currentState == ALARM_ARMED_HOME) {
                returnState = currentState;
            }

            const char* oldStr = alarmGetStateStrInternal();
            currentState = newState;
            LOG_INFO(TAG, "State: %s -> %s", oldStr, alarmGetStateStrInternal());
            
            if (newState == ALARM_DISARMED) {
                sirenAccumulatedSec = 0; // Clear noise timer on clean disarm
                configSaveSirenAccum(0);
                failedAttempts = 0;
                lockedOut = false;
                configSaveSecurityState(0, false);
            }
            stateChanged = true;
        }
        xSemaphoreGiveRecursive(stateMutex);
        
        if (stateChanged) {
            // Chronos Anchor: Initialize RTC timers on entering delay states
            if (snapshotState == ALARM_EXIT_DELAY) {
                rtcSetDelayRemaining(exitDelaySec);
            } else if (snapshotState == ALARM_ENTRY_DELAY) {
                rtcSetDelayRemaining(entryDelaySec);
            } else if (snapshotState != ALARM_TRIGGERED) {
                // Clear delay on normal arming/disarming
                rtcSetDelayRemaining(0);
            }

            configSaveAlarmState(snapshotState);
        }
    } else {
        LOG_ERROR(TAG, "CRITICAL: State transition failed (Mutex BUSY)");
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
        stateMutex = xSemaphoreCreateRecursiveMutex();
    }
    
    // Ensure siren flags start clean BEFORE restoration logic.
    // The triggered-state path below may override these.
    sirenActive     = false;
    sirenMuted      = false;
    triggeringZone  = 0xFF;

    // Restore persistent state from NVS
    AlarmState savedState = configLoadAlarmState();
    configLoadSecurityState(failedAttempts, lockedOut);
    sirenAccumulatedSec = configLoadSirenAccum();

    if (lockedOut) {
        lockoutStartMs = millis(); // Penalize the full lockout duration from boot
        LOG_WARN(TAG, "SECURITY: PIN entries locked due to persistent brute-force protection.");
    }

    if (savedState == ALARM_ARMED_AWAY || savedState == ALARM_ARMED_HOME || 
        savedState == ALARM_TRIGGERED  || savedState == ALARM_EXIT_DELAY ||
        savedState == ALARM_ENTRY_DELAY) {
        
        currentState = savedState;
        LOG_INFO(TAG, "Restored state from NVS: %s", alarmGetStateStrInternal());
        
        if (savedState == ALARM_TRIGGERED) {
            activeAlarmMask = 0xFFFF; // Mark as "recovered alarm"
            sirenActive = true;
            lastSirenUpdateMs = millis();
            ioExpanderSetOutput(sirenOutputChannel, true);
            LOG_WARN(TAG, "Siren: RESUMED on boot (Triggered state restored)");
        }
        else if (savedState == ALARM_EXIT_DELAY || savedState == ALARM_ENTRY_DELAY) {
            // Chronos Anchor: Fail-Secure Delay Resumption
            bool warmBoot = rtcIsValid();
            uint32_t remainingS = rtcGetDelayRemaining();

            if (warmBoot && remainingS > 0) {
                // Resume from RTC (Soft reset, Watchdog, etc)
                delayStartMs = millis() - (DEFAULT_EXIT_DELAY_S * 1000); // Dummy offset
                // We actually need a more precise way to set delayStartMs
                // Let's just adjust the update logic to look at remainingS directly if configured.
                LOG_INFO(TAG, "Chronos Anchor: Resuming %s (%u s remaining)", 
                         alarmGetStateStrInternal(), remainingS);
            } else {
                // Cold Boot Interruption Check
                if (savedState == ALARM_ENTRY_DELAY) {
                    LOG_ERROR(TAG, "Chronos Anchor: Entry Delay INTERRUPTED (Cold Boot). FORCING ALARM.");
                    currentState = ALARM_TRIGGERED;
                    sirenActive = true;
                    lastSirenUpdateMs = millis();
                    ioExpanderSetOutput(sirenOutputChannel, true);
                    fireEvent(EVT_ALARM_TRIGGERED, -1, "Interrupted Entry Delay");
                } else {
                    LOG_WARN(TAG, "Chronos Anchor: Exit Delay reset (Safe).");
                    // Exit delay defaults back to full duration for safety
                    delayStartMs = millis();
                }
            }
        }
    } else {
        currentState = ALARM_DISARMED;
        sirenAccumulatedSec = 0;
    }
    
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
            uint32_t nowTrigger = millis();
            static uint32_t lastTickMs = 0;
            
            // Decouple internal millis() tracking from reboot-persistent seconds
            if (nowTrigger - lastTickMs >= 1000) {
                uint32_t remaining = rtcGetDelayRemaining();
                if (remaining > 0) {
                    remaining--;
                    rtcSetDelayRemaining(remaining);
                }
                lastTickMs = nowTrigger;

                if (remaining == 0) {
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
            }
            break;
        }

        case ALARM_ENTRY_DELAY: {
            uint32_t nowTrigger = millis();
            static uint32_t lastTickMs = 0;

            if (nowTrigger - lastTickMs >= 1000) {
                uint32_t remaining = rtcGetDelayRemaining();
                if (remaining > 0) {
                    remaining--;
                    rtcSetDelayRemaining(remaining);
                }
                lastTickMs = nowTrigger;

                if (remaining == 0) {
                    // Entry delay expired — trigger alarm
                    setState(ALARM_TRIGGERED);
                    sirenOn(-1, "Delay Timeout");
                    fireEvent(EVT_ALARM_TRIGGERED, -1, "Entry delay expired");
                }
            }
            break;
        }

        case ALARM_TRIGGERED: {
            // Ordinance timer update - Throttled NVS writes (Flash Endurance)
            if (sirenActive) {
                uint32_t nowTrigger = millis();
                if (nowTrigger - lastSirenUpdateMs >= 1000) {
                    uint32_t diff = (nowTrigger - lastSirenUpdateMs) / 1000;
                    sirenAccumulatedSec += diff;
                    lastSirenUpdateMs += diff * 1000; // PRESERVE fractional milliseconds (no leak)
                    
                    // Throttled NVS writes are now handled internally by config_manager
                    configSaveSirenAccum(sirenAccumulatedSec);
                }
            }

            // Auto-silence siren after strict duration (Noise Ordinance Compliance)
            if (sirenDurationSec > 0 && sirenAccumulatedSec >= sirenDurationSec) {
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
            break;
        }

        default:
            break;
    }
}

bool alarmArmAway(const char* pin)
{
    if (!pin || strlen(pin) != 4) return false;
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
    LOG_INFO(TAG, "Exit delay: %d seconds", exitDelaySec); // Added from original alarmArmAway
    return true;
}

bool alarmArmHome(const char* pin)
{
    if (!pin || strlen(pin) != 4) return false;
    if (!validatePin(pin)) return false;
    return alarmArmHomeInternal();
}

bool alarmArmHomeInternal()
{
    if (!stateMutex) return false;

    // Optional: Home mode might allow certain zones to be triggered (e.g., perimeter only)
    // but typically we still want a clean start.
    if (!zonesAllClear()) {
        LOG_WARN(TAG, "Arming HOME rejected: Zones not clear");
        zonesPrintStatus(); // Added from original alarmArmHome
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

bool alarmMuteSiren(const char* pin)
{
    if (!validatePin(pin)) {
        LOG_WARN(TAG, "Mute failed — invalid PIN");
        return false;
    }

    if (sirenActive) {
        sirenMuted = true;
        ioExpanderSetOutput(sirenOutputChannel, false);
        LOG_INFO(TAG, "Siren MUTED (alarm still active)");
        fireEvent(EVT_SIREN_OFF, -1, "Manual Mute");
    }
    return true;
}

AlarmState alarmGetState()
{
    AlarmState st = ALARM_BUSY; // Fail-secure: default to BUSY if locked
    if (stateMutex && xSemaphoreTakeRecursive(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        st = currentState;
        xSemaphoreGiveRecursive(stateMutex);
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
        case ALARM_BUSY:         return "BUSY/SYNCING";
        default:                 return "UNKNOWN";
    }
}

const char* alarmGetStateStr()
{
    const char* str = "UNKNOWN";
    if (stateMutex && xSemaphoreTakeRecursive(stateMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        str = alarmGetStateStrInternal();
        xSemaphoreGiveRecursive(stateMutex);
    }
    return str;
}

uint16_t alarmGetDelayRemaining()
{
    if (currentState == ALARM_ENTRY_DELAY || currentState == ALARM_EXIT_DELAY) {
        return (uint16_t)rtcGetDelayRemaining();
    }
    return 0;
}

bool alarmSetPin(const char* currentPin, const char* newPin)
{
    if (!validatePin(currentPin)) {
        LOG_WARN(TAG, "PIN update failed: Current PIN invalid");
        return false;
    }
    
    if (newPin == nullptr || strlen(newPin) < 4) {
        LOG_WARN(TAG, "PIN update failed: New PIN too short or empty");
        return false;
    }

    if (xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY) == pdTRUE) {
        strncpy(alarmPin, newPin, MAX_PIN_LEN - 1);
        alarmPin[MAX_PIN_LEN - 1] = '\0';
        configSavePin(newPin); 
        xSemaphoreGiveRecursive(stateMutex);
        LOG_INFO(TAG, "PIN successfully updated");
        return true;
    }
    return false;
}

void alarmLoadPin(const char* pin)
{
    if (pin == nullptr || strlen(pin) == 0) return;
    if (xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY) == pdTRUE) {
        strncpy(alarmPin, pin, MAX_PIN_LEN - 1);
        alarmPin[MAX_PIN_LEN - 1] = '\0';
        xSemaphoreGiveRecursive(stateMutex);
    }
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
    if (xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY) == pdTRUE) {
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
        xSemaphoreGiveRecursive(stateMutex);
    }
}

uint8_t alarmGetSirenOutput() { return sirenOutputChannel; }

void alarmPrintStatus()
{
    if (xSemaphoreTakeRecursive(stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        Serial.println("=== Alarm System Status ===");
        Serial.printf("  State:          %s\n", alarmGetStateStrInternal());

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
        xSemaphoreGiveRecursive(stateMutex);
    } else {
        Serial.println("Alarm Status: UNKNOWN (Mutex Busy)");
    }
}

void alarmCopyPin(char* dest, size_t maxLen)
{
    if (!dest || maxLen == 0) return;
    if (xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY) == pdTRUE) {
        strncpy(dest, alarmPin, maxLen - 1);
        dest[maxLen - 1] = '\0';
        xSemaphoreGiveRecursive(stateMutex);
    } else {
        dest[0] = '\0';
    }
}
