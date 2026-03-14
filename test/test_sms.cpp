#include <unity.h>
#include <string.h>
#include "Arduino.h"

// We want to test the parsing logic from sms_commands.cpp
// Since the functions are static, we redeclare/mock the logic here to verify the STRTOK_R safety.

// Mock storage for phones
static char phones[5][20];
static int phoneCount = 0;

void mockClearPhones() { phoneCount = 0; }
void mockAddPhone(const char* p) {
    if (phoneCount < 5) {
        strncpy(phones[phoneCount++], p, 19);
    }
}

bool parseSetMultiplePhones_Mock(const char* body) {
    if (body[0] != '@' || body[1] != '#') return false;
    mockClearPhones();

    char temp[160];
    strncpy(temp, body + 2, sizeof(temp)-1);
    temp[sizeof(temp)-1] = '\0';

    char* saveptr;
    char* token = strtok_r(temp, "#", &saveptr);
    while (token) {
        mockAddPhone(token);
        token = strtok_r(NULL, "#", &saveptr);
    }
    return true;
}

void test_sms_multiple_phones_parsing() {
    TEST_ASSERT_TRUE(parseSetMultiplePhones_Mock("@#600111222#600333444#"));
    TEST_ASSERT_EQUAL(2, phoneCount);
    TEST_ASSERT_EQUAL_STRING("600111222", phones[0]);
    TEST_ASSERT_EQUAL_STRING("600333444", phones[1]);
}

void test_sms_parsing_with_empty_tokens() {
    TEST_ASSERT_TRUE(parseSetMultiplePhones_Mock("@##600999000##"));
    TEST_ASSERT_EQUAL(1, phoneCount); // strtok_r collapses delimiters
    TEST_ASSERT_EQUAL_STRING("600999000", phones[0]);
}

void test_sms_reentrancy_simulation() {
    // This is hard to test in a single thread, but we can verify saveptr context integrity
    char msg1[] = "@#111#222#";
    char msg2[] = "@#AAA#BBB#";
    char* ptr1;
    char* ptr2;

    char* t1 = strtok_r(msg1 + 2, "#", &ptr1);
    char* t2 = strtok_r(msg2 + 2, "#", &ptr2);

    TEST_ASSERT_EQUAL_STRING("111", t1);
    TEST_ASSERT_EQUAL_STRING("AAA", t2);

    t1 = strtok_r(NULL, "#", &ptr1);
    TEST_ASSERT_EQUAL_STRING("222", t1);
    
    t2 = strtok_r(NULL, "#", &ptr2);
    TEST_ASSERT_EQUAL_STRING("BBB", t2);
}

int main(int argc, char **argv) {
    UNITY_BEGIN();
    RUN_TEST(test_sms_multiple_phones_parsing);
    RUN_TEST(test_sms_parsing_with_empty_tokens);
    RUN_TEST(test_sms_reentrancy_simulation);
    return UNITY_END();
}
