#include "alarm_controller.h"
#include "io_service.h"
#include "zone_manager.h"
#include "config.h"
#include <Arduino.h>
#include "logging.h"
#include <string.h>
#include "config_manager.h"
#include "notification_manager.h"
#include "network.h"

static const char* TAG = "ALM";

AlarmController::AlarmController()
    : _zones(nullptr), _notificationManager(nullptr), _io(nullptr),
      _exitDelaySec(DEFAULT_EXIT_DELAY_S), _entryDelaySec(DEFAULT_ENTRY_DELAY_S),
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

void AlarmController::init(ZoneManager* zones, NotificationManager* nm, IoService* io) {
    _zones = zones;
    _notificationManager = nm;
    _io = io;

    if (_stateMutex == NULL) {
        _stateMutex = xSemaphoreCreateRecursiveMutex();
    }
    
    // Wire up zone events via direct registration (Observer Pattern)
    if (_zones) {
        _zones->setCallback([this](uint8_t zone, ZoneState state) {
            this->onZoneEvent(zone, state);
        });
    }
}

void AlarmController::setCallback(AlarmEventCallback cb) {
    _eventCallback = cb;
}

void AlarmController::fireEvent(AlarmEvent event, int8_t zoneId, const char* details) {
    if (_eventCallback) {
        AlarmEventInfo info = {event, zoneId, details};
        _eventCallback(info);
    }
    
    // Auto-dispatch to notification manager if it exists
    if (_notificationManager) {
        AlarmEventInfo info = {event, zoneId, details};
        _notificationManager->dispatch(info);
    }
}

void AlarmController::onZoneEvent(uint8_t zoneId, ZoneState state) {
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    
    uint32_t now = millis();
    ZoneConfig* config = (_zones) ? _zones->getConfig(zoneId) : nullptr;
    if (!config) { xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex); return; }

    if (state == ZONE_TAMPER) {
        fireEvent(EVT_TAMPER, zoneId, config->name);
        if (_currentState != ALARM_DISARMED) {
            _currentState = ALARM_TRIGGERED;
            sirenOn(zoneId, config->name);
        }
    } else if (state == ZONE_TRIGGERED) {
        fireEvent(EVT_ZONE_TRIGGERED, zoneId, config->name);
        
        if (_currentState == ALARM_ARMED_AWAY || _currentState == ALARM_ARMED_HOME) {
            if (config->type == ZONE_DELAYED) {
                _currentState = ALARM_ENTRY_DELAY;
                _delayStartMs = now;
                _triggeringZone = zoneId;
                fireEvent(EVT_ENTRY_DELAY, zoneId, config->name);
            } else if (config->type == ZONE_INSTANT || config->type == ZONE_24H) {
                _currentState = ALARM_TRIGGERED;
                _triggeringZone = zoneId;
                sirenOn(zoneId, config->name);
                fireEvent(EVT_ALARM_TRIGGERED, zoneId, config->name);
            }
        } else if (_currentState == ALARM_DISARMED && config->type == ZONE_24H) {
             _currentState = ALARM_TRIGGERED;
             _triggeringZone = zoneId;
             sirenOn(zoneId, config->name);
             fireEvent(EVT_ALARM_TRIGGERED, zoneId, config->name);
        }
    } else if (state == ZONE_NORMAL) {
        fireEvent(EVT_ZONE_RESTORED, zoneId, config->name);
    }

    xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
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
        if (_io) {
            _io->setOutput(_sirenOutputChannel, true);
        }
        LOG_WARN(TAG, "Siren: ON (Zone %d: %s)", zoneId + 1, name);
        fireEvent(EVT_SIREN_ON, zoneId, details);
    }
}

void AlarmController::sirenOff() {
    if (_sirenActive) {
        _sirenActive = false;
        if (_io) {
            _io->setOutput(_sirenOutputChannel, false);
        }
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

bool AlarmController::armHome(const char* pin) {
    if (!validatePin(pin)) return false;
    return armHomeInternal();
}

bool AlarmController::armHomeInternal() {
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, portMAX_DELAY) == pdTRUE) {
        _pendingArmMode = ARM_PENDING_HOME;
        _delayStartMs = millis();
        _currentState = ALARM_EXIT_DELAY;
        fireEvent(EVT_EXIT_DELAY);
        xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
        return true;
    }
    return false;
}

bool AlarmController::muteSiren(const char* pin) {
    if (!validatePin(pin)) return false;
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (_sirenActive) {
            _sirenMuted = true;
            if (_io) {
                _io->setOutput(_sirenOutputChannel, false);
            }
            LOG_INFO(TAG, "Siren: MUTED (Manual)");
        }
        xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
        return true;
    }
    return false;
}

uint16_t AlarmController::getActiveAlarmMask() { return _activeAlarmMask; }

uint16_t AlarmController::getDelayRemaining() {
    if (_currentState == ALARM_EXIT_DELAY) {
        uint32_t elapsed = (millis() - _delayStartMs) / 1000;
        return (elapsed < _exitDelaySec) ? (_exitDelaySec - elapsed) : 0;
    }
    if (_currentState == ALARM_ENTRY_DELAY) {
        uint32_t elapsed = (millis() - _delayStartMs) / 1000;
        return (elapsed < _entryDelaySec) ? (_entryDelaySec - elapsed) : 0;
    }
    return 0;
}

void AlarmController::broadcast(const char* message) {
    if (_notificationManager) {
        _notificationManager->broadcast(message);
    }
}

void AlarmController::loadPin(const char* pin) {
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, portMAX_DELAY) == pdTRUE) {
        strncpy(_alarmPin, pin ? pin : "1234", sizeof(_alarmPin) - 1);
        _alarmPin[sizeof(_alarmPin) - 1] = '\0';
        xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
    }
}

void AlarmController::copyPin(char* dest, size_t maxLen) {
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, portMAX_DELAY) == pdTRUE) {
        strncpy(dest, _alarmPin, maxLen - 1);
        dest[maxLen - 1] = '\0';
        xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
    }
}

bool AlarmController::setPin(const char* currentPin, const char* newPin) {
    if (!validatePin(currentPin)) return false;
    if (!newPin || strlen(newPin) < 4) return false;
    
    if (xSemaphoreTakeRecursive((QueueHandle_t)_stateMutex, portMAX_DELAY) == pdTRUE) {
        strncpy(_alarmPin, newPin, sizeof(_alarmPin) - 1);
        _alarmPin[sizeof(_alarmPin) - 1] = '\0';
        configSavePin(newPin);
        xSemaphoreGiveRecursive((QueueHandle_t)_stateMutex);
        return true;
    }
    return false;
}

void AlarmController::setExitDelay(uint16_t seconds)   { _exitDelaySec = seconds; }
void AlarmController::setEntryDelay(uint16_t seconds)  { _entryDelaySec = seconds; }
uint16_t AlarmController::getExitDelay()              { return _exitDelaySec; }
uint16_t AlarmController::getEntryDelay()             { return _entryDelaySec; }

void AlarmController::setSirenDuration(uint16_t seconds) { _sirenDurationSec = seconds; }
uint16_t AlarmController::getSirenDuration()           { return _sirenDurationSec; }
void AlarmController::setSirenOutput(uint8_t channel)    { _sirenOutputChannel = channel; }
uint8_t AlarmController::getSirenOutput()               { return _sirenOutputChannel; }

void AlarmController::printStatus() {
    Serial.println("--- Alarm Controller Status ---");
    Serial.printf("  State:    %s\n", getStateStr());
    Serial.printf("  Siren:    %s (Output %d)\n", _sirenActive ? "ON" : "OFF", _sirenOutputChannel);
    if (_currentState == ALARM_EXIT_DELAY || _currentState == ALARM_ENTRY_DELAY) {
        Serial.printf("  Delay:    %d s remaining\n", getDelayRemaining());
    }
    Serial.printf("  Lockout:  %s\n", _lockedOut ? "YES" : "NO");
    Serial.println("-------------------------------");
}
