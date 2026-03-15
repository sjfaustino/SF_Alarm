#ifndef SF_ALARM_PHONE_AUTHENTICATOR_H
#define SF_ALARM_PHONE_AUTHENTICATOR_H

#include <Arduino.h>
#include "config.h"

/**
 * @brief Manages a list of authorized phone numbers and their matching logic.
 */
class PhoneAuthenticator {
public:
    PhoneAuthenticator();
    ~PhoneAuthenticator();

    /**
     * @brief Initialize with a mutex for thread-safe access.
     */
    void init();

    /**
     * @brief Check if a phone number is authorized.
     */
    bool isAuthorized(const char* sender) const;

    /**
     * @brief Add a phone number to the list.
     */
    int addPhone(const char* phone);

    /**
     * @brief Set a phone number in a specific slot.
     */
    bool setPhone(int slot, const char* phone);

    /**
     * @brief Remove a phone number from the list.
     */
    bool removePhone(const char* phone);

    /**
     * @brief Get the count of configured phone numbers.
     */
    int getPhoneCount() const;

    /**
     * @brief Get a phone number by index.
     */
    const char* getPhone(int index) const;

    /**
     * @brief Clear all phone numbers.
     */
    void clearPhones();

private:
    char _phoneNumbers[MAX_PHONE_NUMBERS][MAX_PHONE_LEN];
    int _phoneCount;
    mutable SemaphoreHandle_t _mutex;
};

#endif // SF_ALARM_PHONE_AUTHENTICATOR_H
