#ifndef SF_ALARM_IO_EXPANDER_H
#define SF_ALARM_IO_EXPANDER_H

#include <Arduino.h>
#include <stdint.h>

/// Initialize the I2C bus and all 4 PCF8574 expander chips.
/// Returns true if all chips responded on the bus.
bool ioExpanderInit();

/// Read all 16 digital inputs as a bitmask.
/// Bit 0 = input 1, bit 15 = input 16.
/// Inputs are active-low (opto-isolated dry contact, shorted = triggered).
/// The returned value is inverted so that 1 = triggered, 0 = normal.
uint16_t ioExpanderReadInputs();

/// Write all 16 outputs at once from a bitmask.
/// Bit 0 = output 1, bit 15 = output 16.
/// 1 = ON (MOSFET conducts), 0 = OFF.
void ioExpanderWriteOutputs(uint16_t mask);

/// Set a single output channel (0–15) to the given state.
void ioExpanderSetOutput(uint8_t channel, bool state);

/// Get the current output state bitmask.
uint16_t ioExpanderGetOutputs();

/// Check if a specific PCF8574 chip is responding.
/// chipIndex: 0=input1, 1=input2, 2=output1, 3=output2
bool ioExpanderChipOk(uint8_t chipIndex);

/// Check if the I2C input bus has been physically compromised (wires cut/shorted)
bool ioExpanderIsTampered();

#endif // SF_ALARM_IO_EXPANDER_H
