#include <unity.h>
#include "Arduino.h"

// Mock for RTC memory
static uint32_t mock_rtc_delay = 0;
static bool mock_rtc_valid = true;

uint32_t rtcGetDelayRemaining() { return mock_rtc_delay; }
void rtcSetDelayRemaining(uint32_t s) { mock_rtc_delay = s; }
bool rtcIsValid() { return mock_rtc_valid; }

// Simplified state logic for test
enum TestState { DISARMED, ENTRY_DELAY, TRIGGERED };
static TestState currentState = DISARMED;

void simulate_boot(TestState savedNvsState) {
    bool warmBoot = rtcIsValid();
    uint32_t remaining = rtcGetDelayRemaining();

    if (savedNvsState == ENTRY_DELAY) {
        if (warmBoot && remaining > 0) {
            currentState = ENTRY_DELAY;
            // Delay resumes
        } else {
            // Cold boot interruption
            currentState = TRIGGERED;
        }
    } else {
        currentState = savedNvsState;
    }
}

void test_chronos_warm_boot_resumption() {
    mock_rtc_valid = true;
    mock_rtc_delay = 15;
    currentState = DISARMED;

    simulate_boot(ENTRY_DELAY);
    TEST_ASSERT_EQUAL(ENTRY_DELAY, currentState);
    TEST_ASSERT_EQUAL(15, mock_rtc_delay);
}

void test_chronos_cold_boot_interruption() {
    mock_rtc_valid = false; // Simulated cold boot (magic bytes invalid)
    mock_rtc_delay = 0;
    currentState = DISARMED;

    simulate_boot(ENTRY_DELAY);
    TEST_ASSERT_EQUAL(TRIGGERED, currentState);
}

void test_chronos_normal_boot() {
    mock_rtc_valid = false;
    mock_rtc_delay = 0;
    currentState = DISARMED;

    simulate_boot(DISARMED);
    TEST_ASSERT_EQUAL(DISARMED, currentState);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_chronos_warm_boot_resumption);
    RUN_TEST(test_chronos_cold_boot_interruption);
    RUN_TEST(test_chronos_normal_boot);
    return UNITY_END();
}
