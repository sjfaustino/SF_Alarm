#ifndef ARDUINO_MOCK_H
#define ARDUINO_MOCK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define HIGH 0x1
#define LOW  0x0

#define INPUT 0x01
#define OUTPUT 0x03

void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
uint32_t millis();
void delay(uint32_t ms);

class MockSerial {
public:
    void begin(unsigned long baud) {}
    void print(const char* s) {}
    void println(const char* s) {}
    void printf(const char* format, ...) {}
};

extern MockSerial Serial;

#endif
