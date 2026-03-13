#include "io_expander.h"
#include "config.h"
#include <Wire.h>
#include <PCF8574.h>
#include "logging.h"

static const char* TAG = "IO";

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

static SemaphoreHandle_t i2cMutex = nullptr;
#define I2C_LOCK_TIMEOUT pdMS_TO_TICKS(100)

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

bool ioExpanderInit()
{
    if (i2cMutex == nullptr) {
        i2cMutex = xSemaphoreCreateMutex();
    }

    if (xSemaphoreTake(i2cMutex, I2C_LOCK_TIMEOUT) != pdTRUE) return false;

    // --- PHYSICAL BUS RECOVERY (Obsidian Sledgehammer) ---
    // If SDA is held low by a hung slave, we need to toggle SCL until it's released.
    pinMode(I2C_SDA_PIN, INPUT_PULLUP);
    pinMode(I2C_SCL_PIN, OUTPUT);
    digitalWrite(I2C_SCL_PIN, HIGH);

    if (digitalRead(I2C_SDA_PIN) == LOW) {
        LOG_WARN(TAG, "I2C SDA stuck LOW. Attempting bus recovery...");
        for (int i = 0; i < 16; i++) {
            digitalWrite(I2C_SCL_PIN, LOW);
            delayMicroseconds(5);
            digitalWrite(I2C_SCL_PIN, HIGH);
            delayMicroseconds(5);
            if (digitalRead(I2C_SDA_PIN) == HIGH) {
                LOG_INFO(TAG, "I2C bus recovered after %d pulses", i+1);
                break;
            }
        }
    }

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_CLOCK_HZ);
    Wire.setTimeOut(100); 

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

    xSemaphoreGive(i2cMutex);

    currentOutputs = 0x0000;

    bool allOk = chipOk[0] && chipOk[1] && chipOk[2] && chipOk[3];

    LOG_INFO(TAG, "Init %s — IN1:%s IN2:%s OUT1:%s OUT2:%s",
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

    if (i2cMutex == nullptr || xSemaphoreTake(i2cMutex, I2C_LOCK_TIMEOUT) != pdTRUE) {
        return 0; // Skip poll if bus is busy
    }

    // Runtime Health Check: Try a dummy transmission to verify chip presence
    if (chipOk[0]) {
        Wire.beginTransmission(PCF_INPUT_1_ADDR);
        if (Wire.endTransmission() != 0) {
            chipOk[0] = false;
            chipRetryMs[0] = 0;
            LOG_ERROR("IO", "IN1 chip lost!");
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
                LOG_INFO(TAG, "IN1 chip recovered");
                low = pcfIn1.read8();
            }
        }
    }

    if (chipOk[1]) {
        Wire.beginTransmission(PCF_INPUT_2_ADDR);
        if (Wire.endTransmission() != 0) {
            chipOk[1] = false;
            chipRetryMs[1] = 0;
            LOG_ERROR(TAG, "IN2 chip lost!");
        } else {
            high = pcfIn2.read8();
        }
    } else {
        uint32_t now = millis();
        if (now - chipRetryMs[1] >= CHIP_RETRY_INTERVAL_MS) {
            chipRetryMs[1] = now;
            if (pcfIn2.begin()) {
                chipOk[1] = true;
                LOG_INFO(TAG, "IN2 chip recovered");
                high = pcfIn2.read8();
            }
        }
    }

    // --- Output Chip Runtime Health Checks ---
    if (chipOk[2]) {
        Wire.beginTransmission(PCF_OUTPUT_1_ADDR);
        if (Wire.endTransmission() != 0) {
            chipOk[2] = false;
            chipRetryMs[2] = 0;
            LOG_ERROR(TAG, "OUT1 chip lost!");
        }
    } else {
        uint32_t now = millis();
        if (now - chipRetryMs[2] >= CHIP_RETRY_INTERVAL_MS) {
            chipRetryMs[2] = now;
            if (pcfOut1.begin()) {
                chipOk[2] = true;
                pcfOut1.write8((uint8_t)(currentOutputs & 0xFF));
                LOG_INFO(TAG, "OUT1 chip recovered");
            }
        }
    }

    if (chipOk[3]) {
        Wire.beginTransmission(PCF_OUTPUT_2_ADDR);
        if (Wire.endTransmission() != 0) {
            chipOk[3] = false;
            chipRetryMs[3] = 0;
            LOG_ERROR(TAG, "OUT2 chip lost!");
        }
    } else {
        uint32_t now = millis();
        if (now - chipRetryMs[3] >= CHIP_RETRY_INTERVAL_MS) {
            chipRetryMs[3] = now;
            if (pcfOut2.begin()) {
                chipOk[3] = true;
                pcfOut2.write8((uint8_t)((currentOutputs >> 8) & 0xFF));
                LOG_INFO(TAG, "OUT2 chip recovered");
            }
        }
    }

    // Combine into 16-bit value.
    uint16_t raw = ((uint16_t)high << 8) | (uint16_t)low;
    xSemaphoreGive(i2cMutex);
    return ~raw & 0xFFFF;
}

static portMUX_TYPE ioMux = portMUX_INITIALIZER_UNLOCKED;

void ioExpanderWriteOutputs(uint16_t mask)
{
    portENTER_CRITICAL(&ioMux);
    currentOutputs = mask;
    portEXIT_CRITICAL(&ioMux);

    if (i2cMutex == nullptr || xSemaphoreTake(i2cMutex, I2C_LOCK_TIMEOUT) != pdTRUE) return;

    if (chipOk[2]) {
        pcfOut1.write8((uint8_t)(mask & 0xFF));
    }
    if (chipOk[3]) {
        pcfOut2.write8((uint8_t)((mask >> 8) & 0xFF));
    }
    xSemaphoreGive(i2cMutex);
}

void ioExpanderSetOutput(uint8_t channel, bool state)
{
    if (channel >= 16) return;

    portENTER_CRITICAL(&ioMux);
    if (state) {
        currentOutputs |= (1 << channel);
    } else {
        currentOutputs &= ~(1 << channel);
    }
    uint16_t mask = currentOutputs;
    portEXIT_CRITICAL(&ioMux);

    if (i2cMutex == nullptr || xSemaphoreTake(i2cMutex, I2C_LOCK_TIMEOUT) != pdTRUE) return;

    // Call output writer without nested lock since we bypass ioExpanderWriteOutputs update
    if (chipOk[2]) {
        pcfOut1.write8((uint8_t)(mask & 0xFF));
    }
    if (chipOk[3]) {
        pcfOut2.write8((uint8_t)((mask >> 8) & 0xFF));
    }
    xSemaphoreGive(i2cMutex);
}

uint16_t ioExpanderGetOutputs()
{
    portENTER_CRITICAL(&ioMux);
    uint16_t ret = currentOutputs;
    portEXIT_CRITICAL(&ioMux);
    return ret;
}

bool ioExpanderChipOk(uint8_t chipIndex)
{
    if (chipIndex >= 4) return false;
    return chipOk[chipIndex];
}

static uint32_t tamperStartMs = 0;
static bool tamperDebouncing = false;

bool ioExpanderIsTampered()
{
    // If either input chip is dead/disconnected, the I2C line has been tampered with
    bool currentTamper = !chipOk[0] || !chipOk[1];
    
    if (currentTamper) {
        if (!tamperDebouncing) {
            tamperDebouncing = true;
            tamperStartMs = millis();
        } else if (millis() - tamperStartMs >= 500) { // 500ms debounce
            return true;
        }
    } else {
        tamperDebouncing = false;
    }
    return false;
}

TaskHandle_t ioExpanderGetLockOwner()
{
    if (i2cMutex == nullptr) return nullptr;
    return xSemaphoreGetMutexHolder(i2cMutex);
}
