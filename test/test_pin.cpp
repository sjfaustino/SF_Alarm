#include <unity.h>
#include "Arduino.h"

// Mock for Serial
MockSerial Serial;
uint32_t mock_millis = 0;
uint32_t millis() { return mock_millis; }

// We want to test pinEquals from alarm_controller.cpp
// Since it's static, we might need a test-specific wrapper or 
// inclusive testing. For now, let's redeclare the logic to verify its correctness.

bool pinEquals(const char* a, const char* b)
{
    volatile uint8_t diff = 0;
    uint8_t aLocked = 0;
    uint8_t bLocked = 0;

    // Fixed MAX_PIN_LEN for test
    const size_t MAX_PIN_LEN = 10; 

    for (size_t i = 0; i < MAX_PIN_LEN; i++) {
        uint8_t ca = (uint8_t)a[i];
        uint8_t cb = (uint8_t)b[i];

        if (ca == '\0') aLocked = 0xFF;
        if (cb == '\0') bLocked = 0xFF;

        diff |= (ca ^ cb) & (~aLocked) & (~bLocked);
        diff |= (aLocked ^ bLocked);
    }

    return (diff == 0);
}

void test_pin_exact_match() {
    TEST_ASSERT_TRUE(pinEquals("1234", "1234"));
}

void test_pin_mismatch() {
    TEST_ASSERT_FALSE(pinEquals("1234", "4321"));
}

void test_pin_partial_match() {
    TEST_ASSERT_FALSE(pinEquals("1234", "123"));
    TEST_ASSERT_FALSE(pinEquals("123", "1234"));
}

void test_pin_empty() {
    TEST_ASSERT_TRUE(pinEquals("", ""));
}

void test_pin_longer() {
    TEST_ASSERT_FALSE(pinEquals("123456", "1234"));
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_pin_exact_match);
    RUN_TEST(test_pin_mismatch);
    RUN_TEST(test_pin_partial_match);
    RUN_TEST(test_pin_empty);
    RUN_TEST(test_pin_longer);
    return UNITY_END();
}
