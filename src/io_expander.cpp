#include "io_expander.h"
#include "config.h"
#include <Wire.h>
#include <PCF8574.h>

// ---------------------------------------------------------------------------
// PCF8574 instances
// ---------------------------------------------------------------------------
static PCF8574 pcfIn1(PCF_INPUT_1_ADDR);    // Inputs 1–8
static PCF8574 pcfIn2(PCF_INPUT_2_ADDR);    // Inputs 9–16
static PCF8574 pcfOut1(PCF_OUTPUT_1_ADDR);   // Outputs 1–8
static PCF8574 pcfOut2(PCF_OUTPUT_2_ADDR);   // Outputs 9–16

static uint16_t currentOutputs = 0x0000;

// Chip health flags
static bool chipOk[4] = { false, false, false, false };
// Chip recovery cooldown (avoid hammering I2C at 50Hz when a chip is down)
static uint32_t chipRetryMs[4] = { 0, 0, 0, 0 };
static const uint32_t CHIP_RETRY_INTERVAL_MS = 5000; // Retry once per 5 seconds

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ioExpanderInit()
{
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_CLOCK_HZ);
    Wire.setTimeOut(100); // Prevent infinite while() loops if the I2C bus is physically jammed/shorted

    // Initialize input chips — set all pins as inputs (write 0xFF)
    chipOk[0] = pcfIn1.begin();
    chipOk[1] = pcfIn2.begin();

    // Initialize output chips — set all pins low (off)
    chipOk[2] = pcfOut1.begin();
    chipOk[3] = pcfOut2.begin();

    if (chipOk[2]) {
        pcfOut1.write8(0x00);
    }
    if (chipOk[3]) {
        pcfOut2.write8(0x00);
    }

    currentOutputs = 0x0000;

    bool allOk = chipOk[0] && chipOk[1] && chipOk[2] && chipOk[3];

    Serial.printf("[IO] Init %s — IN1:%s IN2:%s OUT1:%s OUT2:%s\n",
                  allOk ? "OK" : "PARTIAL",
                  chipOk[0] ? "OK" : "FAIL",
                  chipOk[1] ? "OK" : "FAIL",
                  chipOk[2] ? "OK" : "FAIL",
                  chipOk[3] ? "OK" : "FAIL");

    return allOk;
}

uint16_t ioExpanderReadInputs()
{
    uint8_t low  = 0xFF;
    uint8_t high = 0xFF;

    // Runtime Health Check: Try a dummy transmission to verify chip presence
    if (chipOk[0]) {
        Wire.beginTransmission(PCF_INPUT_1_ADDR);
        if (Wire.endTransmission() != 0) {
            chipOk[0] = false;
            chipRetryMs[0] = 0;
            Serial.println("[IO] ERROR: IN1 chip lost!");
        } else {
            low = pcfIn1.read8();
        }
    } else {
        // Retry with cooldown to avoid hammering I2C at 50Hz
        uint32_t now = millis();
        if (now - chipRetryMs[0] >= CHIP_RETRY_INTERVAL_MS) {
            chipRetryMs[0] = now;
            if (pcfIn1.begin()) {
                chipOk[0] = true;
                Serial.println("[IO] INFO: IN1 chip recovered");
                low = pcfIn1.read8();
            }
        }
    }

    if (chipOk[1]) {
        Wire.beginTransmission(PCF_INPUT_2_ADDR);
        if (Wire.endTransmission() != 0) {
            chipOk[1] = false;
            chipRetryMs[1] = 0;
            Serial.println("[IO] ERROR: IN2 chip lost!");
        } else {
            high = pcfIn2.read8();
        }
    } else {
        uint32_t now = millis();
        if (now - chipRetryMs[1] >= CHIP_RETRY_INTERVAL_MS) {
            chipRetryMs[1] = now;
            if (pcfIn2.begin()) {
                chipOk[1] = true;
                Serial.println("[IO] INFO: IN2 chip recovered");
                high = pcfIn2.read8();
            }
        }
    }

    // Combine into 16-bit value.
    uint16_t raw = ((uint16_t)high << 8) | (uint16_t)low;
    return ~raw & 0xFFFF;
}

void ioExpanderWriteOutputs(uint16_t mask)
{
    currentOutputs = mask;

    if (chipOk[2]) {
        pcfOut1.write8((uint8_t)(mask & 0xFF));
    }
    if (chipOk[3]) {
        pcfOut2.write8((uint8_t)((mask >> 8) & 0xFF));
    }
}

void ioExpanderSetOutput(uint8_t channel, bool state)
{
    if (channel >= 16) return;

    if (state) {
        currentOutputs |= (1 << channel);
    } else {
        currentOutputs &= ~(1 << channel);
    }

    ioExpanderWriteOutputs(currentOutputs);
}

uint16_t ioExpanderGetOutputs()
{
    return currentOutputs;
}

bool ioExpanderChipOk(uint8_t chipIndex)
{
    if (chipIndex >= 4) return false;
    return chipOk[chipIndex];
}
