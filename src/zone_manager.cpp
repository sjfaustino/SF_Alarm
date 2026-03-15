#include "zone_manager.h"
#include "io_service.h"
#include <string.h>

ZoneManager::ZoneManager() 
    : _io(nullptr), 
      _eventCallback(nullptr), 
      _virtualInputBitmask(0),
      _zoneMutex(NULL),
      _vInputMux(portMUX_INITIALIZER_UNLOCKED)
{
    memset(_zones, 0, sizeof(_zones));
    memset(_virtualInputLastMs, 0, sizeof(_virtualInputLastMs));
}

ZoneManager::~ZoneManager() {
    if (_zoneMutex) vSemaphoreDelete(_zoneMutex);
}

void ZoneManager::init(IoService* io) {
    _io = io;
    if (_zoneMutex == NULL) {
        _zoneMutex = xSemaphoreCreateMutex();
    }

    xSemaphoreTake(_zoneMutex, portMAX_DELAY);
    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        memset(&_zones[i], 0, sizeof(ZoneInfo));
        snprintf(_zones[i].config.name, MAX_ZONE_NAME_LEN, "Zone %d", i + 1);
        _zones[i].config.type    = ZONE_INSTANT;
        _zones[i].config.wiring  = ZONE_NO;
        _zones[i].config.enabled = true;
        _zones[i].state          = ZONE_NORMAL;
    }
    xSemaphoreGive(_zoneMutex);
    Serial.println("[ZONE] ZoneManager Initialized");
}

void ZoneManager::setCallback(EventCallback cb) {
    xSemaphoreTake(_zoneMutex, portMAX_DELAY);
    _eventCallback = cb;
    xSemaphoreGive(_zoneMutex);
}

void ZoneManager::update(uint16_t inputBitmask) {
    uint32_t now = millis();
    if (xSemaphoreTake(_zoneMutex, 0) != pdTRUE) return;

    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        if (!_zones[i].config.enabled || _zones[i].state == ZONE_BYPASSED) continue;

        portENTER_CRITICAL(&_vInputMux);
        if (_virtualInputBitmask & (1 << i)) {
            if (now - _virtualInputLastMs[i] >= VIRTUAL_INPUT_TIMEOUT_MS) {
                _virtualInputBitmask &= ~(1 << i);
            }
        }
        uint16_t vInput = _virtualInputBitmask;
        portEXIT_CRITICAL(&_vInputMux);

        bool isTampered = _io->isTampered();
        bool triggered = isInputTriggered(i, inputBitmask) || ((vInput >> i) & 1);

        if (isTampered) {
            _zones[i].rawInput = true;
            _zones[i].debouncing = false;
            setZoneState(i, ZONE_TAMPER);
        } else if (triggered != _zones[i].rawInput) {
            if (!_zones[i].debouncing) {
                _zones[i].debouncing = true;
                _zones[i].debounceStartMs = now;
                _zones[i].pendingLevel = triggered;
            } else if (triggered != _zones[i].pendingLevel) {
                _zones[i].debounceStartMs = now;
                _zones[i].pendingLevel = triggered;
            } else if ((now - _zones[i].debounceStartMs) >= INPUT_DEBOUNCE_MS) {
                _zones[i].rawInput = triggered;
                _zones[i].debouncing = false;
                setZoneState(i, triggered ? ZONE_TRIGGERED : ZONE_NORMAL);
            }
        } else {
            _zones[i].debouncing = false;
        }
    }
    xSemaphoreGive(_zoneMutex);
}

const ZoneInfo* ZoneManager::getInfo(uint8_t zoneIndex) {
    if (zoneIndex >= MAX_ZONES) return nullptr;
    return &_zones[zoneIndex];
}

ZoneConfig* ZoneManager::getConfig(uint8_t zoneIndex) {
    if (zoneIndex >= MAX_ZONES) return nullptr;
    return &_zones[zoneIndex].config;
}

void ZoneManager::setBypassed(uint8_t zoneIndex, bool bypassed) {
    if (zoneIndex >= MAX_ZONES) return;
    xSemaphoreTake(_zoneMutex, portMAX_DELAY);
    if (bypassed) {
        _zones[zoneIndex].state = ZONE_BYPASSED;
    } else {
        _zones[zoneIndex].state = ZONE_NORMAL;
        _zones[zoneIndex].rawInput = false;
        _zones[zoneIndex].debouncing = false;
    }
    xSemaphoreGive(_zoneMutex);
}

void ZoneManager::setVirtualInput(uint8_t zoneIndex, bool state) {
    if (zoneIndex >= MAX_ZONES) return;
    portENTER_CRITICAL(&_vInputMux);
    if (state) {
        _virtualInputBitmask |= (1 << zoneIndex);
        _virtualInputLastMs[zoneIndex] = millis();
    } else {
        _virtualInputBitmask &= ~(1 << zoneIndex);
    }
    portEXIT_CRITICAL(&_vInputMux);
}

bool ZoneManager::areAllClear() {
    xSemaphoreTake(_zoneMutex, portMAX_DELAY);
    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        if (!_zones[i].config.enabled || _zones[i].state == ZONE_BYPASSED) continue;
        if (_zones[i].state != ZONE_NORMAL) {
            xSemaphoreGive(_zoneMutex);
            return false;
        }
    }
    xSemaphoreGive(_zoneMutex);
    return true;
}

uint16_t ZoneManager::getTriggeredMask() {
    uint16_t mask = 0;
    xSemaphoreTake(_zoneMutex, portMAX_DELAY);
    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        if (_zones[i].state == ZONE_TRIGGERED) mask |= (1 << i);
    }
    xSemaphoreGive(_zoneMutex);
    return mask;
}

void ZoneManager::printStatus() {
    Serial.println("--- Zone Status ---");
    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        const char* stateStr;
        switch (_zones[i].state) {
            case ZONE_NORMAL:    stateStr = "NORMAL";    break;
            case ZONE_TRIGGERED: stateStr = "TRIGGERED"; break;
            case ZONE_TAMPER:    stateStr = "TAMPER";    break;
            case ZONE_BYPASSED:  stateStr = "BYPASSED";  break;
            default:             stateStr = "UNKNOWN";   break;
        }
        Serial.printf("  Z%02d %-20s [%s]\n", i+1, _zones[i].config.name, stateStr);
    }
}

void ZoneManager::setZoneState(uint8_t idx, ZoneState newState) {
    if (_zones[idx].state == newState) return;
    _zones[idx].state = newState;
    _zones[idx].lastChangeMs = millis();
    if (_eventCallback) _eventCallback(idx, newState);
}

bool ZoneManager::isInputTriggered(uint8_t zoneIndex, uint16_t inputBitmask) {
    bool inputHigh = (inputBitmask >> zoneIndex) & 1;
    return (_zones[zoneIndex].config.wiring == ZONE_NO) ? inputHigh : !inputHigh;
}
