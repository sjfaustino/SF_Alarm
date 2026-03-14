#ifndef SF_ALARM_CONTROLLER_H
#define SF_ALARM_CONTROLLER_H

#include <Arduino.h>
#include "alarm_zones.h"

class SystemContext;

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
    ALARM_BUSY        = 6,   // Mutex contention / system syncing
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
// Alarm Event Data
// ---------------------------------------------------------------------------
struct AlarmEventInfo {
    AlarmEvent  event;
    int8_t      zoneId;  // 0-15, or -1 if not applicable
    const char* details; // Context string (e.g. zone name)
};

// ---------------------------------------------------------------------------
// Callback for alarm events (used by SMS notifier)
// ---------------------------------------------------------------------------
typedef void (*AlarmEventCallback)(const AlarmEventInfo& info);

/**
 * @brief Manages the core alarm state machine and security logic.
 */
class AlarmController {
public:
    AlarmController();
    ~AlarmController();

    /// Initialize the alarm controller.
    void init(SystemContext* ctx = nullptr);

    /// Set callback for alarm events.
    void setCallback(AlarmEventCallback cb);

    /// Main update loop.
    void update();

    /// Arm the system (away mode).
    bool armAway(const char* pin);

    /// Arm the system (home mode).
    bool armHome(const char* pin);

    /// Disarm the system.
    bool disarm(const char* pin);

    /// Internal use only: Arm/Disarm without PIN.
    bool armAwayInternal();
    bool armHomeInternal();
    bool disarmInternal();

    /// Mute/silence the siren without disarming.
    bool muteSiren(const char* pin);

    /// Get current state.
    AlarmState getState();
    const char* getStateStr();

    /// Get details.
    uint16_t getActiveAlarmMask();
    uint16_t getDelayRemaining();

    /// PIN management.
    bool setPin(const char* currentPin, const char* newPin);
    void loadPin(const char* pin);
    void copyPin(char* dest, size_t maxLen);
    bool validatePin(const char* pin);

    /// Delay durations.
    void setExitDelay(uint16_t seconds);
    void setEntryDelay(uint16_t seconds);
    uint16_t getExitDelay();
    uint16_t getEntryDelay();

    /// Siren management.
    void setSirenDuration(uint16_t seconds);
    uint16_t getSirenDuration();
    void setSirenOutput(uint8_t channel);
    uint8_t getSirenOutput();

    /// Diagnostics.
    void printStatus();

    /// Broadcast an alert.
    void broadcast(const char* message);

private:
    SystemContext* _ctx;
    char           _alarmPin[16];
    uint16_t       _exitDelaySec;
    uint16_t       _entryDelaySec;
    uint16_t       _sirenDurationSec;
    uint8_t        _sirenOutputChannel;
    
    AlarmState     _currentState;
    AlarmState     _returnState;
    void*          _stateMutex; // SemaphoreHandle_t
    AlarmEventCallback _eventCallback;

    bool           _sirenActive;
    bool           _sirenMuted;
    uint16_t       _activeAlarmMask;
    uint32_t       _delayStartMs;
    uint32_t       _sirenStartMs;
    uint8_t        _triggeringZone;
    uint32_t       _sirenActiveTime;
    uint32_t       _lastSirenUpdateMs;

    uint8_t        _failedAttempts;
    uint32_t       _lockoutStartMs;
    bool           _lockedOut;

    enum PendingArmMode : uint8_t { ARM_PENDING_AWAY = 0, ARM_PENDING_HOME = 1 };
    PendingArmMode _pendingArmMode;

    void fireEvent(AlarmEvent event, int8_t zoneId = -1, const char* details = "");
    bool pinEquals(const char* a, const char* b);
    void sirenOn(int8_t zoneId, const char* name);
    void sirenOff();
};

#endif // SF_ALARM_CONTROLLER_H
