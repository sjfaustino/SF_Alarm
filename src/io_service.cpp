#include "io_service.h"
#include "config.h"
#include "logging.h"
#include "system_health.h"
#include <Wire.h>

static const char* TAG = "IO";

#define I2C_LOCK_TIMEOUT_READ  pdMS_TO_TICKS(10)
#define I2C_LOCK_TIMEOUT_WRITE pdMS_TO_TICKS(500)
#define CHIP_RETRY_INTERVAL_MS 5000

portMUX_TYPE IoService::_ioMux = portMUX_INITIALIZER_UNLOCKED;

IoService::IoService() 
    : _busMutex(nullptr), 
      _pcfIn1(PCF_INPUT_1_ADDR), 
      _pcfIn2(PCF_INPUT_2_ADDR),
      _pcfOut1(PCF_OUTPUT_1_ADDR),
      _pcfOut2(PCF_OUTPUT_2_ADDR),
      _currentOutputs(0),
      _tamperStartMs(0),
      _tamperDebouncing(false)
{
    memset(_chipOk, 0, sizeof(_chipOk));
    memset(_chipRetryMs, 0, sizeof(_chipRetryMs));
}

IoService::~IoService() {}

bool IoService::init(void* busMutex) {
    _busMutex = busMutex;
    SemaphoreHandle_t mutex = getMutex();
    if (mutex == nullptr) return false;
    if (xSemaphoreTake(mutex, I2C_LOCK_TIMEOUT_WRITE) != pdTRUE) return false;

    // --- PHYSICAL BUS RECOVERY ---
    pinMode(I2C_SDA_PIN, INPUT_PULLUP);
    pinMode(I2C_SCL_PIN, OUTPUT);
    digitalWrite(I2C_SCL_PIN, HIGH);

    if (digitalRead(I2C_SDA_PIN) == LOW) {
        LOG_WARN(TAG, "I2C SDA stuck LOW. Attempting bus recovery...");
        for (int i = 0; i < 20; i++) {
            digitalWrite(I2C_SCL_PIN, LOW);
            delayMicroseconds(5);
            digitalWrite(I2C_SCL_PIN, HIGH);
            delayMicroseconds(5);
            if (digitalRead(I2C_SDA_PIN) == HIGH) {
                LOG_INFO(TAG, "I2C bus released after %d pulses", i+1);
                break;
            }
        }
    }

    // Issue deterministic STOP condition
    pinMode(I2C_SDA_PIN, OUTPUT);
    digitalWrite(I2C_SCL_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SDA_PIN, LOW);
    delayMicroseconds(5);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(5);
    digitalWrite(I2C_SDA_PIN, HIGH);
    delayMicroseconds(5);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_CLOCK_HZ);
    Wire.setTimeOut(100);

    _chipOk[0] = _pcfIn1.begin();
    _chipOk[1] = _pcfIn2.begin();
    _chipOk[2] = _pcfOut1.begin();
    _chipOk[3] = _pcfOut2.begin();

    if (_chipOk[2]) _pcfOut1.write8(0x00);
    if (_chipOk[3]) _pcfOut2.write8(0x00);

    xSemaphoreGive(mutex);
    _currentOutputs = 0x0000;

    bool allOk = _chipOk[0] && _chipOk[1] && _chipOk[2] && _chipOk[3];
    LOG_INFO(TAG, "Init %s", allOk ? "OK" : "PARTIAL");
    if (allOk) sysHealthReport(HB_BIT_I2C);
    return allOk;
}

bool IoService::readInputs(uint16_t* mask) {
    if (mask == nullptr) return false;
    uint8_t low = 0xFF, high = 0xFF;

    SemaphoreHandle_t mutex = getMutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, I2C_LOCK_TIMEOUT_READ) != pdTRUE) return false;

    bool chipsDied = false;
    auto checkAndRead = [&](int idx, PCF8574& pcf, uint8_t& val) {
        if (_chipOk[idx]) {
            Wire.beginTransmission(pcf.getAddress());
            if (Wire.endTransmission() != 0) {
                _chipOk[idx] = false;
                _chipRetryMs[idx] = 0;
                chipsDied = true;
                LOG_ERROR(TAG, "Chip %d lost!", idx);
            } else {
                val = pcf.read8();
            }
        } else {
            uint32_t now = millis();
            if (now - _chipRetryMs[idx] >= CHIP_RETRY_INTERVAL_MS) {
                _chipRetryMs[idx] = now;
                if (pcf.begin()) {
                    _chipOk[idx] = true;
                    LOG_INFO(TAG, "Chip %d recovered", idx);
                    val = pcf.read8();
                } else chipsDied = true;
            } else chipsDied = true;
        }
    };

    checkAndRead(0, _pcfIn1, low);
    checkAndRead(1, _pcfIn2, high);

    // Also health check output chips
    for (int i = 2; i < 4; i++) {
        PCF8574& pcf = (i == 2) ? _pcfOut1 : _pcfOut2;
        if (_chipOk[i]) {
            Wire.beginTransmission(pcf.getAddress());
            if (Wire.endTransmission() != 0) {
                _chipOk[i] = false;
                _chipRetryMs[i] = 0;
                chipsDied = true;
                LOG_ERROR(TAG, "Output chip %d lost!", i);
            }
        } else {
            uint32_t now = millis();
            if (now - _chipRetryMs[i] >= CHIP_RETRY_INTERVAL_MS) {
                _chipRetryMs[i] = now;
                if (pcf.begin()) {
                    _chipOk[i] = true;
                    uint8_t outVal = (i == 2) ? (_currentOutputs & 0xFF) : ((_currentOutputs >> 8) & 0xFF);
                    pcf.write8(outVal);
                    LOG_INFO(TAG, "Output chip %d recovered", i);
                }
            }
        }
    }

    if (chipsDied) busRecover();

    *mask = ~(((uint16_t)high << 8) | (uint16_t)low) & 0xFFFF;
    xSemaphoreGive(mutex);
    sysHealthReport(HB_BIT_I2C);
    return true;
}

void IoService::setOutput(uint8_t channel, bool state) {
    if (channel >= 16) return;

    portENTER_CRITICAL(&_ioMux);
    if (state) _currentOutputs |= (1 << channel);
    else _currentOutputs &= ~(1 << channel);
    uint16_t mask = _currentOutputs;
    portEXIT_CRITICAL(&_ioMux);

    SemaphoreHandle_t mutex = getMutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, I2C_LOCK_TIMEOUT_WRITE) != pdTRUE) return;

    if (_chipOk[2]) _pcfOut1.write8((uint8_t)(mask & 0xFF));
    if (_chipOk[3]) _pcfOut2.write8((uint8_t)((mask >> 8) & 0xFF));
    xSemaphoreGive(mutex);
    sysHealthReport(HB_BIT_I2C);
}

void IoService::writeOutputs(uint16_t mask) {
    portENTER_CRITICAL(&_ioMux);
    _currentOutputs = mask;
    portEXIT_CRITICAL(&_ioMux);

    SemaphoreHandle_t mutex = getMutex();
    if (mutex == nullptr || xSemaphoreTake(mutex, I2C_LOCK_TIMEOUT_WRITE) != pdTRUE) return;

    if (_chipOk[2]) _pcfOut1.write8((uint8_t)(mask & 0xFF));
    if (_chipOk[3]) _pcfOut2.write8((uint8_t)((mask >> 8) & 0xFF));
    xSemaphoreGive(mutex);
    sysHealthReport(HB_BIT_I2C);
}

uint16_t IoService::getOutputs() {
    portENTER_CRITICAL(&_ioMux);
    uint16_t ret = _currentOutputs;
    portEXIT_CRITICAL(&_ioMux);
    return ret;
}

bool IoService::isChipOk(uint8_t chipIndex) {
    if (chipIndex >= 4) return false;
    return _chipOk[chipIndex];
}

bool IoService::isTampered() {
    bool currentTamper = !_chipOk[0] || !_chipOk[1];
    if (currentTamper) {
        if (!_tamperDebouncing) {
            _tamperDebouncing = true;
            _tamperStartMs = millis();
        } else if (millis() - _tamperStartMs >= 500) return true;
    } else _tamperDebouncing = false;
    return false;
}

TaskHandle_t IoService::getLockOwner() {
    SemaphoreHandle_t mutex = getMutex();
    if (mutex == nullptr) return nullptr;
    return xSemaphoreGetMutexHolder(mutex);
}

void IoService::busRecover() {
    LOG_WARN(TAG, "I2C bus recovery initiated...");
    Wire.end();
    pinMode(I2C_SDA_PIN, INPUT_PULLUP);
    pinMode(I2C_SCL_PIN, OUTPUT);
    digitalWrite(I2C_SCL_PIN, HIGH);

    if (digitalRead(I2C_SDA_PIN) == LOW) {
        for (int i = 0; i < 20; i++) {
            digitalWrite(I2C_SCL_PIN, LOW);
            delayMicroseconds(10);
            digitalWrite(I2C_SCL_PIN, HIGH);
            delayMicroseconds(10);
            if (digitalRead(I2C_SDA_PIN) == HIGH) break;
        }
    }
    // Force STOP condition
    pinMode(I2C_SDA_PIN, OUTPUT);
    digitalWrite(I2C_SDA_PIN, LOW);
    digitalWrite(I2C_SCL_PIN, HIGH);
    delayMicroseconds(10);
    digitalWrite(I2C_SDA_PIN, HIGH);
    
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN, I2C_CLOCK_HZ);
    Wire.setTimeOut(100);
}

SemaphoreHandle_t IoService::getMutex() {
    return (SemaphoreHandle_t)_busMutex;
}
