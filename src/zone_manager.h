#ifndef SF_ALARM_ZONE_MANAGER_H
#define SF_ALARM_ZONE_MANAGER_H

#include <Arduino.h>
#include <stdint.h>
#include "alarm_zones.h" // Reuse existing structs for compatibility

class IoService;



/**
 * ZoneManager: Manages the state, debouncing, and virtual inputs for all alarm zones.
 * Eliminates global arrays and provides a clean, mutex-protected API.
 */
class ZoneManager {
public:
    typedef std::function<void(uint8_t zoneIndex, ZoneState state)> EventCallback;

    ZoneManager();
    ~ZoneManager();

    void init(IoService* io);

    /// Set the callback for zone events.
    void setCallback(EventCallback cb);

    /// Update all zones based on the provided hardware input bitmask.
    void update(uint16_t inputBitmask);

    /// Get information for a specific zone.
    const ZoneInfo* getInfo(uint8_t zoneIndex);

    /// Get configuration for a specific zone.
    ZoneConfig* getConfig(uint8_t zoneIndex);

    /// Set bypass state for a zone.
    void setBypassed(uint8_t zoneIndex, bool bypassed);

    /// Set virtual input state (e.g. from ONVIF) for a zone.
    void setVirtualInput(uint8_t zoneIndex, bool state);

    /// Check if all enabled, non-bypassed zones are clear (normal).
    bool areAllClear();

    /// Get a bitmask of all currently triggered zones.
    uint16_t getTriggeredMask();

    /// Print the status of all zones to Serial.
    void printStatus();

private:
    void setZoneState(uint8_t idx, ZoneState newState);
    bool isInputTriggered(uint8_t zoneIndex, uint16_t inputBitmask);

    IoService* _io;
    ZoneInfo _zones[MAX_ZONES];
    EventCallback _eventCallback;
    
    volatile uint16_t _virtualInputBitmask;
    uint32_t _virtualInputLastMs[MAX_ZONES];
    SemaphoreHandle_t _zoneMutex;
    portMUX_TYPE _vInputMux;

    static const uint32_t VIRTUAL_INPUT_TIMEOUT_MS = 30000;
};

#endif // SF_ALARM_ZONE_MANAGER_H
