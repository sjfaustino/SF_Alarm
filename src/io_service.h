#ifndef SF_ALARM_IO_SERVICE_H
#define SF_ALARM_IO_SERVICE_H

#include <Arduino.h>
#include <stdint.h>
#include <PCF8574.h>


/**
 * IoService: Class-based Hardware Abstraction Layer (HAL) for PCF8574 expanders.
 * Manages I2C bus health, recovery, and thread-safe I/O operations.
 */
class IoService {
public:
    IoService();
    ~IoService();

    bool init(void* busMutex);

    /// Read all 16 digital inputs as a bitmask.
    /// Returns true on success, false if the bus is busy or a chip is lost.
    bool readInputs(uint16_t* mask);

    /// Set a single output channel (0–15) to the given state.
    void setOutput(uint8_t channel, bool state);

    /// Write all 16 outputs at once from a bitmask.
    void writeOutputs(uint16_t mask);

    /// Get the current output state bitmask.
    uint16_t getOutputs();

    /// Check if a specific PCF8574 chip is responding.
    bool isChipOk(uint8_t chipIndex);

    /// Check if the I2C input bus has been physically compromised.
    bool isTampered();

    /// Get the handle of the task currently holding the I2C mutex.
    TaskHandle_t getLockOwner();

private:
    void busRecover();
    SemaphoreHandle_t getMutex();

    void* _busMutex;
    PCF8574 _pcfIn1;
    PCF8574 _pcfIn2;
    PCF8574 _pcfOut1;
    PCF8574 _pcfOut2;

    uint16_t _currentOutputs;
    bool _chipOk[4];
    uint32_t _chipRetryMs[4];
    
    uint32_t _tamperStartMs;
    bool _tamperDebouncing;

    static portMUX_TYPE _ioMux;
};

#endif // SF_ALARM_IO_SERVICE_H
