#include "alarm_controller.h"
#include "io_expander.h"
#include "config.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static AlarmState       currentState     = ALARM_DISARMED;
static AlarmEventCallback eventCallback  = nullptr;

static char     alarmPin[MAX_PIN_LEN]    = "1234";  // Default PIN
static uint16_t exitDelaySec             = DEFAULT_EXIT_DELAY_S;
static uint16_t entryDelaySec            = DEFAULT_ENTRY_DELAY_S;
static uint16_t sirenDurationSec         = DEFAULT_SIREN_DURATION_S;
static uint8_t  sirenOutputChannel       = 0;   // Output channel 0 = siren

static uint32_t delayStartMs             = 0;
static uint32_t sirenStartMs             = 0;
static bool     sirenActive              = false;
static bool     sirenMuted               = false;

static uint8_t  triggeringZone           = 0xFF; // Which zone triggered the alarm

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

static void fireEvent(AlarmEvent event, const char* details = "")
{
    if (eventCallback) {
        eventCallback(event, details);
    }
}

static void sirenOn()
{
    if (!sirenActive) {
        sirenActive  = true;
        sirenMuted   = false;
        sirenStartMs = millis();
        ioExpanderSetOutput(sirenOutputChannel, true);
        Serial.println("[ALARM] Siren ON");
        fireEvent(EVT_SIREN_ON);
    }
}

static void sirenOff()
{
    if (sirenActive) {
        sirenActive = false;
        ioExpanderSetOutput(sirenOutputChannel, false);
        Serial.println("[ALARM] Siren OFF");
        fireEvent(EVT_SIREN_OFF);
    }
}

static bool validatePin(const char* pin)
{
    // Overflow-safe lockout check
    if (lockedOut && (millis() - lockoutStartMs) < LOCKOUT_DURATION_MS) {
        Serial.println("[ALARM] SECURITY: PIN entry locked due to multiple failures");
        return false;
    }
    lockedOut = false; // Lockout expired

    if (pin == nullptr || strlen(pin) == 0) return false;
    
    if (strcmp(pin, alarmPin) == 0) {
        failedAttempts = 0;
        return true;
    } else {
        failedAttempts++;
        if (failedAttempts >= MAX_FAILED_ATTEMPTS) {
            lockedOut = true;
            lockoutStartMs = millis();
            Serial.println("[ALARM] SECURITY: Too many failed attempts! Locked out for 5 minutes.");
        }
        return false;
    }
}

static void setState(AlarmState newState)
{
    if (currentState == newState) return;
    const char* oldStr = alarmGetStateStr();
    currentState = newState;
    Serial.printf("[ALARM] State: %s -> %s\n", oldStr, alarmGetStateStr());
}

// ---------------------------------------------------------------------------
// Zone event handler — called by zone manager
// ---------------------------------------------------------------------------

static void onZoneEvent(uint8_t zoneIndex, ZoneState newState)
{
    const ZoneInfo* info = zonesGetInfo(zoneIndex);
    if (!info) return;

    char details[64];
    snprintf(details, sizeof(details), "Zone %d (%s)", zoneIndex + 1, info->config.name);

    if (newState == ZONE_TRIGGERED) {
        fireEvent(EVT_ZONE_TRIGGERED, details);

        switch (currentState) {
            case ALARM_DISARMED:
                // Only 24H zones trigger when disarmed
                if (info->config.type == ZONE_24H) {
                    triggeringZone = zoneIndex;
                    setState(ALARM_TRIGGERED);
                    sirenOn();
                    snprintf(details, sizeof(details),
                             "24H Zone %d (%s) ALARM!", zoneIndex + 1, info->config.name);
                    fireEvent(EVT_ALARM_TRIGGERED, details);
                }
                break;

            case ALARM_ARMED_AWAY:
            case ALARM_ARMED_HOME:
                // Check zone type
                if (info->config.type == ZONE_DELAYED) {
                    // Start entry delay
                    delayStartMs   = millis();
                    triggeringZone = zoneIndex;
                    setState(ALARM_ENTRY_DELAY);
                    snprintf(details, sizeof(details),
                             "Entry delay — Zone %d (%s)", zoneIndex + 1, info->config.name);
                    fireEvent(EVT_ENTRY_DELAY, details);
                } else if (info->config.type == ZONE_FOLLOWER) {
                    // Follower: instant unless entry delay already running
                    if (currentState == ALARM_ENTRY_DELAY) {
                        // Already in entry delay — do nothing extra
                    } else {
                        triggeringZone = zoneIndex;
                        setState(ALARM_TRIGGERED);
                        sirenOn();
                        snprintf(details, sizeof(details),
                                 "Zone %d (%s) ALARM!", zoneIndex + 1, info->config.name);
                        fireEvent(EVT_ALARM_TRIGGERED, details);
                    }
                } else {
                    // Instant or 24H — immediate alarm
                    triggeringZone = zoneIndex;
                    setState(ALARM_TRIGGERED);
                    sirenOn();
                    snprintf(details, sizeof(details),
                             "Zone %d (%s) ALARM!", zoneIndex + 1, info->config.name);
                    fireEvent(EVT_ALARM_TRIGGERED, details);
                }
                break;

            case ALARM_ENTRY_DELAY:
                // Additional zone triggered during entry delay
                if (info->config.type == ZONE_INSTANT || info->config.type == ZONE_24H) {
                    // Instant zone overrides entry delay
                    triggeringZone = zoneIndex;
                    setState(ALARM_TRIGGERED);
                    sirenOn();
                    snprintf(details, sizeof(details),
                             "Zone %d (%s) ALARM! (during entry delay)", zoneIndex + 1, info->config.name);
                    fireEvent(EVT_ALARM_TRIGGERED, details);
                }
                break;

            default:
                break;
        }
    } else if (newState == ZONE_NORMAL) {
        fireEvent(EVT_ZONE_RESTORED, details);
    } else if (newState == ZONE_TAMPER) {
        fireEvent(EVT_TAMPER, details);
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void alarmInit()
{
    currentState    = ALARM_DISARMED;
    sirenActive     = false;
    sirenMuted      = false;
    triggeringZone  = 0xFF;

    // Register our zone event handler
    zonesSetCallback(onZoneEvent);

    Serial.println("[ALARM] Controller initialized — DISARMED");
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
                // Exit delay complete — system is armed
                if (pendingArmMode == ARM_PENDING_HOME) {
                    setState(ALARM_ARMED_HOME);
                    fireEvent(EVT_ARMED_HOME, "System armed (HOME)");
                } else {
                    setState(ALARM_ARMED_AWAY);
                    fireEvent(EVT_ARMED_AWAY, "System armed (AWAY)");
                }
                triggeringZone = 0xFF;
            }
            break;
        }

        case ALARM_ENTRY_DELAY: {
            uint32_t elapsed = now - delayStartMs;
            if (elapsed >= (uint32_t)entryDelaySec * 1000) {
                // Entry delay expired — trigger alarm
                setState(ALARM_TRIGGERED);
                sirenOn();
                fireEvent(EVT_ALARM_TRIGGERED, "Entry delay expired — ALARM!");
            }
            break;
        }

        case ALARM_TRIGGERED: {
            // Auto-silence siren after duration
            if (sirenActive && !sirenMuted && sirenDurationSec > 0) {
                uint32_t elapsed = now - sirenStartMs;
                if (elapsed >= (uint32_t)sirenDurationSec * 1000) {
                    sirenOff();
                    Serial.println("[ALARM] Siren auto-silenced after timeout");
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
        Serial.println("[ALARM] Arm AWAY failed — invalid PIN");
        return false;
    }

    if (!zonesAllClear()) {
        Serial.println("[ALARM] Arm AWAY failed — zones not clear");
        zonesPrintStatus();
        return false;
    }

    // Start exit delay
    delayStartMs    = millis();
    pendingArmMode  = ARM_PENDING_AWAY;
    setState(ALARM_EXIT_DELAY);
    fireEvent(EVT_EXIT_DELAY, "Arming AWAY — exit delay started");
    Serial.printf("[ALARM] Exit delay: %d seconds\n", exitDelaySec);
    return true;
}

bool alarmArmHome(const char* pin)
{
    if (!validatePin(pin)) {
        Serial.println("[ALARM] Arm HOME failed — invalid PIN");
        return false;
    }

    if (!zonesAllClear()) {
        Serial.println("[ALARM] Arm HOME failed — zones not clear");
        zonesPrintStatus();
        return false;
    }

    delayStartMs    = millis();
    pendingArmMode  = ARM_PENDING_HOME;
    setState(ALARM_EXIT_DELAY);
    fireEvent(EVT_EXIT_DELAY, "Arming HOME — exit delay started");
    Serial.printf("[ALARM] Exit delay: %d seconds\n", exitDelaySec);
    return true;
}

bool alarmDisarm(const char* pin)
{
    if (!validatePin(pin)) {
        Serial.println("[ALARM] Disarm failed — invalid PIN");
        return false;
    }

    sirenOff();
    setState(ALARM_DISARMED);
    triggeringZone = 0xFF;
    fireEvent(EVT_DISARMED, "System disarmed");
    return true;
}

void alarmMuteSiren()
{
    if (sirenActive) {
        sirenMuted = true;
        ioExpanderSetOutput(sirenOutputChannel, false);
        Serial.println("[ALARM] Siren MUTED (alarm still active)");
        fireEvent(EVT_SIREN_OFF, "Siren muted");
    }
}

AlarmState alarmGetState()
{
    return currentState;
}

const char* alarmGetStateStr()
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
    Serial.println("[ALARM] PIN updated");
}

const char* alarmGetPin()
{
    return alarmPin;
}

bool alarmValidatePin(const char* pin)
{
    return validatePin(pin);
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
    Serial.printf("[ALARM] Exit delay set to %d seconds\n", seconds);
}

uint16_t alarmGetExitDelay() { return exitDelaySec; }

void alarmSetEntryDelay(uint16_t seconds)
{
    entryDelaySec = seconds;
    Serial.printf("[ALARM] Entry delay set to %d seconds\n", seconds);
}

uint16_t alarmGetEntryDelay() { return entryDelaySec; }

void alarmSetSirenDuration(uint16_t seconds)
{
    sirenDurationSec = seconds;
    Serial.printf("[ALARM] Siren duration set to %d seconds\n", seconds);
}

uint16_t alarmGetSirenDuration() { return sirenDurationSec; }

void alarmSetSirenOutput(uint8_t channel)
{
    if (channel < 16) {
        sirenOutputChannel = channel;
        Serial.printf("[ALARM] Siren output channel set to %d\n", channel);
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
