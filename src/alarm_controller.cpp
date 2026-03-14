#include "alarm_controller.h"
#include "io_expander.h"
#include "config.h"
#include <Arduino.h>
#include "logging.h"
#include <string.h>
#include "config_manager.h"
#include "alarm_zones.h"
#include "system_context.h"
#include "notification_manager.h"
#include "network.h"

static const char* TAG = "ALM";

// Global instance for C wrappers
static AlarmController globalAlarmController;

AlarmController::AlarmController()
    : _ctx(nullptr), _exitDelaySec(DEFAULT_EXIT_DELAY_S), _entryDelaySec(DEFAULT_ENTRY_DELAY_S),
      _sirenDurationSec(DEFAULT_SIREN_DURATION_S), _sirenOutputChannel(0),
      _currentState(ALARM_DISARMED), _returnState(ALARM_DISARMED), _stateMutex(NULL),
      _eventCallback(nullptr), _sirenActive(false), _sirenMuted(false),
      _activeAlarmMask(0), _delayStartMs(0), _sirenStartMs(0), _triggeringZone(0xFF),
      _sirenActiveTime(0), _lastSirenUpdateMs(0), _failedAttempts(0),
      _lockoutStartMs(0), _lockedOut(false), _pendingArmMode(ARM_PENDING_AWAY)
{
    memset(_alarmPin, 0, sizeof(_alarmPin));
    strncpy(_alarmPin, "1234", sizeof(_alarmPin) - 1);
}

AlarmController::~AlarmController() {
    if (_stateMutex) {
        vSemaphoreDelete((SemaphoreHandle_t)_stateMutex);
    }
}

void AlarmController::init(SystemContext* ctx) {
    _ctx = ctx;
    if (_stateMutex == NULL) {
        _stateMutex = xSemaphoreCreateRecursiveMutex();
    }
    
    // Wire up zone events
    zonesSetCallback([](uint8_t zone, ZoneState state) {
        globalAlarmController.update(); // Keep state fresh
        // The actual logic is in the zone task, but we can hook if needed
    });
}

void AlarmController::setCallback(AlarmEventCallback cb) {
    _eventCallback = cb;
}

void AlarmController::fireEvent(AlarmEvent event, int8_t zoneId, const char* details) {
    if (_eventCallback) {
        AlarmEventInfo info = {event, zoneId, details};
        _eventCallback(info);
    }
    
    // Auto-dispatch to notification manager if context exists
    if (_ctx && _ctx->notificationManager) {
        AlarmEventInfo info = {event, zoneId, details};
        _ctx->notificationManager->dispatch(info, _ctx);
    }
}

bool AlarmController::pinEquals(const char* a, const char* b) {
    volatile uint8_t diff = 0;
    uint8_t aLocked = 0, bLocked = 0;
    for (size_t i = 0; i < 16; i++) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];
        if (ca == '\0') aLocked = 0xFF;
        if (cb == '\0') bLocked = 0xFF;
        diff |= (ca ^ cb) & (~aLocked) & (~bLocked);
        diff |= (aLocked ^ bLocked);
    }
    return (diff == 0);
}

void AlarmController::sirenOn(int8_t zoneId, const char* name) {
    char details[64];
    snprintf(details, sizeof(details), "Zone %d (%s)", zoneId + 1, name);
    if (!_sirenActive) {
        _sirenActive = true;
        _sirenMuted = false;
        _lastSirenUpdateMs = millis();
        _sirenStartMs = millis();
        ioExpanderSetOutput(_sirenOutputChannel, true);
        LOG_WARN(TAG, "Siren: ON (Zone %d: %s)", zoneId + 1, name);
        fireEvent(EVT_SIREN_ON, zoneId, details);
    }
}

void AlarmController::sirenOff() {
    if (_sirenActive) {
        _sirenActive = false;
        ioExpanderSetOutput(_sirenOutputChannel, false);
        LOG_INFO(TAG, "Siren: OFF");
        fireEvent(EVT_SIREN_OFF, -1, "Manual or Timeout");
    }
}

bool AlarmController::validatePin(const char* pin) {
    if (!pin || strlen(pin) == 0) return false;
    if (_lockedOut) {
        if ((millis() - _lockoutStartMs) < 300000) return false;
        _lockedOut = false; _failedAttempts = 0;
    }
    bool match = false;
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        match = pinEquals(pin, _alarmPin);
        xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
    }
    if (match) {
        _failedAttempts = 0; _lockedOut = false;
        configSaveSecurityState(0, false);
        return true;
    } else {
        _failedAttempts++;
        if (_failedAttempts >= 5) {
            _lockedOut = true; _lockoutStartMs = millis();
            LOG_ERROR(TAG, "SECURITY: Too many failed attempts! Locked out.");
        }
        configSaveSecurityState(_failedAttempts, _lockedOut);
        return false;
    }
}

void AlarmController::update() {
    if (!_stateMutex) return;
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;

    uint32_t now = millis();
    
    // Handle Delays
    if (_currentState == ALARM_EXIT_DELAY) {
        if (now - _delayStartMs >= (uint32_t)_exitDelaySec * 1000) {
            _currentState = (_pendingArmMode == ARM_PENDING_AWAY) ? ALARM_ARMED_AWAY : ALARM_ARMED_HOME;
            fireEvent((_pendingArmMode == ARM_PENDING_AWAY) ? EVT_ARMED_AWAY : EVT_ARMED_HOME);
            LOG_INFO(TAG, "Armed!");
        }
    } else if (_currentState == ALARM_ENTRY_DELAY) {
        if (now - _delayStartMs >= (uint32_t)_entryDelaySec * 1000) {
            _currentState = ALARM_TRIGGERED;
            sirenOn(_triggeringZone, "Entry Delay Timeout");
            fireEvent(EVT_ALARM_TRIGGERED, _triggeringZone, "Entry Timeout");
        }
    }

    // Handle Siren Timeout
    if (_sirenActive) {
        uint32_t elapsed = now - _lastSirenUpdateMs;
        _sirenActiveTime += elapsed;
        _lastSirenUpdateMs = now;
        if (_sirenDurationSec > 0 && _sirenActiveTime >= (uint32_t)_sirenDurationSec * 1000) {
            sirenOff();
        }
    }

    xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
}

// Implement the rest of methods by delegating...
bool AlarmController::armAway(const char* pin) {
    if (!validatePin(pin)) return false;
    return armAwayInternal();
}

bool AlarmController::armAwayInternal() {
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, portMAX_DELAY) == pdTRUE) {
        _pendingArmMode = ARM_PENDING_AWAY;
        _delayStartMs = millis();
        _currentState = ALARM_EXIT_DELAY;
        fireEvent(EVT_EXIT_DELAY);
        xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
        return true;
    }
    return false;
}

bool AlarmController::disarm(const char* pin) {
    if (!validatePin(pin)) return false;
    return disarmInternal();
}

bool AlarmController::disarmInternal() {
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, portMAX_DELAY) == pdTRUE) {
        _currentState = ALARM_DISARMED;
        sirenOff();
        _sirenActiveTime = 0;
        fireEvent(EVT_DISARMED);
        xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
        return true;
    }
    return false;
}

AlarmState AlarmController::getState() { return _currentState; }
const char* AlarmController::getStateStr() {
    switch(_currentState) {
        case ALARM_DISARMED: return "DISARMED";
        case ALARM_EXIT_DELAY: return "EXIT DELAY";
        case ALARM_ARMED_AWAY: return "ARMED AWAY";
        case ALARM_ARMED_HOME: return "ARMED HOME";
        case ALARM_ENTRY_DELAY: return "ENTRY DELAY";
        case ALARM_TRIGGERED: return "TRIGGERED";
        default: return "UNKNOWN";
    }
}

// C Wrappers
void alarmInit() { globalAlarmController.init(); }
void alarmSetCallback(AlarmEventCallback cb) { globalAlarmController.setCallback(cb); }
void alarmUpdate() { globalAlarmController.update(); }
bool alarmArmAway(const char* p) { return globalAlarmController.armAway(p); }
bool alarmArmHome(const char* p) { return globalAlarmController.armHome(p); }
bool alarmDisarm(const char* p) { return globalAlarmController.disarm(p); }
bool alarmArmAwayInternal() { return globalAlarmController.armAwayInternal(); }
bool alarmArmHomeInternal() { return globalAlarmController.armHomeInternal(); }
bool alarmDisarmInternal() { return globalAlarmController.disarmInternal(); }
AlarmState alarmGetState() { return globalAlarmController.getState(); }
const char* alarmGetStateStr() { return globalAlarmController.getStateStr(); }
void alarmLoadPin(const char* p) { globalAlarmController.loadPin(p); }
bool alarmValidatePin(const char* p) { return globalAlarmController.validatePin(p); }
void alarmBroadcast(const char* m) { globalAlarmController.broadcast(m); }

// Stub implementations for the rest to keep it building...
bool AlarmController::armHome(const char* p) { if (!validatePin(p)) return false; return armHomeInternal(); }
bool AlarmController::armHomeInternal() { if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, portMAX_DELAY) == pdTRUE) { _pendingArmMode = ARM_PENDING_HOME; _delayStartMs = millis(); _currentState = ALARM_EXIT_DELAY; fireEvent(EVT_EXIT_DELAY); xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex); return true; } return false; }
bool AlarmController::muteSiren(const char* p) { if (!validatePin(p)) return false; sirenOff(); return true; }
uint16_t AlarmController::getActiveAlarmMask() { return _activeAlarmMask; }
uint16_t AlarmController::getDelayRemaining() { if (_currentState == ALARM_EXIT_DELAY || _currentState == ALARM_ENTRY_DELAY) { uint32_t elapsed = (millis() - _delayStartMs) / 1000; uint32_t dur = (_currentState == ALARM_EXIT_DELAY) ? _exitDelaySec : _entryDelaySec; if (elapsed >= dur) return 0; return dur - elapsed; } return 0; }
bool AlarmController::setPin(const char* c, const char* n) { if (!validatePin(c)) return false; loadPin(n); return true; }
void AlarmController::loadPin(const char* p) { if (p) strncpy(_alarmPin, p, sizeof(_alarmPin)-1); }
void AlarmController::copyPin(char* d, size_t m) { strncpy(d, _alarmPin, m-1); }
void AlarmController::setExitDelay(uint16_t s) { _exitDelaySec = s; }
void AlarmController::setEntryDelay(uint16_t s) { _entryDelaySec = s; }
uint16_t AlarmController::getExitDelay() { return _exitDelaySec; }
uint16_t AlarmController::getEntryDelay() { return _entryDelaySec; }
void AlarmController::setSirenDuration(uint16_t s) { _sirenDurationSec = s; }
uint16_t AlarmController::getSirenDuration() { return _sirenDurationSec; }
void AlarmController::setSirenOutput(uint8_t c) { _sirenOutputChannel = c; }
uint8_t AlarmController::getSirenOutput() { return _sirenOutputChannel; }
void AlarmController::printStatus() { LOG_INFO(TAG, "Status: %s", getStateStr()); }
void AlarmController::broadcast(const char* m) { if (_ctx && _ctx->notificationManager) _ctx->notificationManager->broadcast(m); }

// More C Wrappers
bool alarmMuteSiren(const char* p) { return globalAlarmController.muteSiren(p); }
uint16_t alarmGetActiveAlarmMask() { return globalAlarmController.getActiveAlarmMask(); }
uint16_t alarmGetDelayRemaining() { return globalAlarmController.getDelayRemaining(); }
bool alarmSetPin(const char* c, const char* n) { return globalAlarmController.setPin(c, n); }
void alarmCopyPin(char* d, size_t m) { globalAlarmController.copyPin(d, m); }
void alarmSetExitDelay(uint16_t s) { globalAlarmController.setExitDelay(s); }
void alarmSetEntryDelay(uint16_t s) { globalAlarmController.setEntryDelay(s); }
uint16_t alarmGetExitDelay() { return globalAlarmController.getExitDelay(); }
uint16_t alarmGetEntryDelay() { return globalAlarmController.getEntryDelay(); }
void alarmSetSirenDuration(uint16_t s) { globalAlarmController.setSirenDuration(s); }
uint16_t alarmGetSirenDuration() { return globalAlarmController.getSirenDuration(); }
void alarmSetSirenOutput(uint8_t c) { globalAlarmController.setSirenOutput(c); }
uint8_t alarmGetSirenOutput() { return globalAlarmController.getSirenOutput(); }
void alarmPrintStatus() { globalAlarmController.printStatus(); }
