#include "alarm_zones.h"
#include "io_expander.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static ZoneInfo zones[MAX_ZONES];
static ZoneEventCallback eventCallback = nullptr;
static volatile uint16_t virtualInputBitmask = 0;
static uint32_t virtualInputLastMs[MAX_ZONES] = {0};
static const uint32_t VIRTUAL_INPUT_TIMEOUT_MS = 30000; // 30s ghost timeout
static portMUX_TYPE vInputMux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t zoneMutex = NULL;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isInputTriggered(uint8_t zoneIndex, uint16_t inputBitmask)
{
    bool inputHigh = (inputBitmask >> zoneIndex) & 1;

    // For NO wiring: input high (closed) = triggered
    // For NC wiring: input low  (open)   = triggered
    if (zones[zoneIndex].config.wiring == ZONE_NO) {
        return inputHigh;
    } else {
        return !inputHigh;
    }
}

static void setZoneState(uint8_t idx, ZoneState newState)
{
    if (zones[idx].state == newState) return;

    ZoneState oldState = zones[idx].state;
    zones[idx].state = newState;
    zones[idx].lastChangeMs = millis();

    Serial.printf("[ZONE] Zone %d (%s): %d -> %d\n",
                  idx + 1, zones[idx].config.name,
                  (int)oldState, (int)newState);

    if (eventCallback) {
        eventCallback(idx, newState);
    }
}

// Internal helper for mutex-safe reads (caller must hold mutex)
static const ZoneInfo* getZoneInfoInternal(uint8_t index)
{
    if (index >= MAX_ZONES) return nullptr;
    return &zones[index];
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void zonesInit()
{
    if (zoneMutex == NULL) {
        zoneMutex = xSemaphoreCreateMutex();
    }

    xSemaphoreTake(zoneMutex, portMAX_DELAY);
    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        memset(&zones[i], 0, sizeof(ZoneInfo));
        snprintf(zones[i].config.name, MAX_ZONE_NAME_LEN, "Zone %d", i + 1);
        zones[i].config.type    = ZONE_INSTANT;
        zones[i].config.wiring  = ZONE_NO;
        zones[i].config.enabled = true;
        zones[i].state          = ZONE_NORMAL;
        zones[i].rawInput       = false;
        zones[i].debouncing     = false;
    }
    xSemaphoreGive(zoneMutex);

    Serial.println("[ZONE] Initialized 16 zones (mutex protected)");
}

void zonesSetCallback(ZoneEventCallback cb)
{
    xSemaphoreTake(zoneMutex, portMAX_DELAY);
    eventCallback = cb;
    xSemaphoreGive(zoneMutex);
}

void zonesUpdate(uint16_t inputBitmask)
{
    uint32_t now = millis();

    if (xSemaphoreTake(zoneMutex, 0) != pdTRUE) return; // Skip if busy (scanned 50 times/sec, overlap is fine to skip once)

    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        if (!zones[i].config.enabled) continue;
        if (zones[i].state == ZONE_BYPASSED) continue;

        // Read virtual inputs under spinlock (shared with ONVIF task on Core 0)
        portENTER_CRITICAL(&vInputMux);
        if (virtualInputBitmask & (1 << i)) {
            if (now - virtualInputLastMs[i] >= VIRTUAL_INPUT_TIMEOUT_MS) {
                virtualInputBitmask &= ~(1 << i);
                Serial.printf("[ZONE] Virtual ghost on Zone %d EXPIRED.\n", i + 1);
            }
        }
        uint16_t vInput = virtualInputBitmask;
        portEXIT_CRITICAL(&vInputMux);

        bool isTampered = ioExpanderIsTampered();
        bool triggered = isInputTriggered(i, inputBitmask) || ((vInput >> i) & 1);

        // --- Tamper & Debounce logic ---
        if (isTampered) {
            // If the I2C physical wire is cut, instantly fault all zones to prevent bypass
            zones[i].rawInput = true;
            zones[i].debouncing = false;
            setZoneState(i, ZONE_TAMPER);
        }
        else if (triggered != zones[i].rawInput) {
            if (!zones[i].debouncing) {
                // Start debounce
                zones[i].debouncing     = true;
                zones[i].debounceStartMs = now;
                zones[i].pendingLevel   = triggered;
            } else if (triggered != zones[i].pendingLevel) {
                // Direction changed during debounce — restart
                zones[i].debounceStartMs = now;
                zones[i].pendingLevel   = triggered;
            } else if ((now - zones[i].debounceStartMs) >= INPUT_DEBOUNCE_MS) {
                // Debounce period passed — accept new level
                zones[i].rawInput   = triggered;
                zones[i].debouncing = false;

                if (triggered) {
                    setZoneState(i, ZONE_TRIGGERED);
                } else {
                    setZoneState(i, ZONE_NORMAL);
                }
            }
        } else {
            // Input matches current state — cancel any pending debounce
            zones[i].debouncing = false;
        }
    }
    xSemaphoreGive(zoneMutex);
}

const ZoneInfo* zonesGetInfo(uint8_t zoneIndex)
{
    if (zoneIndex >= MAX_ZONES) return nullptr;
    return &zones[zoneIndex];
}

ZoneConfig* zonesGetConfig(uint8_t zoneIndex)
{
    if (zoneIndex >= MAX_ZONES) return nullptr;
    return &zones[zoneIndex].config;
}

void zonesSetBypassed(uint8_t zoneIndex, bool bypassed)
{
    if (zoneIndex >= MAX_ZONES) return;

    if (bypassed) {
        zones[zoneIndex].state = ZONE_BYPASSED;
        Serial.printf("[ZONE] Zone %d (%s) BYPASSED\n",
                      zoneIndex + 1, zones[zoneIndex].config.name);
    } else {
        zones[zoneIndex].state = ZONE_NORMAL;
        zones[zoneIndex].rawInput = false;
        zones[zoneIndex].debouncing = false;
        Serial.printf("[ZONE] Zone %d (%s) UN-BYPASSED\n",
                      zoneIndex + 1, zones[zoneIndex].config.name);
    }
}

void zonesSetVirtualInput(uint8_t zoneIndex, bool state)
{
    if (zoneIndex >= MAX_ZONES) return;

    portENTER_CRITICAL(&vInputMux);
    if (state) {
        virtualInputBitmask |= (1 << zoneIndex);
        virtualInputLastMs[zoneIndex] = millis();
    } else {
        virtualInputBitmask &= ~(1 << zoneIndex);
    }
    portEXIT_CRITICAL(&vInputMux);
}

bool zonesAllClear()
{
    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        if (!zones[i].config.enabled) continue;
        if (zones[i].state == ZONE_BYPASSED) continue;
        if (zones[i].state != ZONE_NORMAL) return false;
    }
    return true;
}

uint16_t zonesGetTriggeredMask()
{
    uint16_t mask = 0;
    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        if (zones[i].state == ZONE_TRIGGERED) {
            mask |= (1 << i);
        }
    }
    return mask;
}

void zonesPrintStatus()
{
    Serial.println("--- Zone Status ---");
    for (uint8_t i = 0; i < MAX_ZONES; i++) {
        const char* stateStr;
        switch (zones[i].state) {
            case ZONE_NORMAL:    stateStr = "NORMAL";    break;
            case ZONE_TRIGGERED: stateStr = "TRIGGERED"; break;
            case ZONE_TAMPER:    stateStr = "TAMPER";    break;
            case ZONE_FAULT:     stateStr = "FAULT";     break;
            case ZONE_BYPASSED:  stateStr = "BYPASSED";  break;
            default:             stateStr = "UNKNOWN";   break;
        }

        const char* typeStr;
        switch (zones[i].config.type) {
            case ZONE_INSTANT:  typeStr = "INST"; break;
            case ZONE_DELAYED:  typeStr = "DLY";  break;
            case ZONE_24H:      typeStr = "24H";  break;
            case ZONE_FOLLOWER: typeStr = "FLW";  break;
            default:            typeStr = "?";    break;
        }

        Serial.printf("  Z%02d %-20s [%s] %s %s %s\n",
                      i + 1,
                      zones[i].config.name,
                      stateStr,
                      typeStr,
                      zones[i].config.wiring == ZONE_NC ? "NC" : "NO",
                      zones[i].config.enabled ? "" : "(disabled)");
    }
    Serial.println("-------------------");
}
