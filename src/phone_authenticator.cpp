#include "phone_authenticator.h"
#include <string.h>
#include <ctype.h>

PhoneAuthenticator::PhoneAuthenticator() : _phoneCount(0), _mutex(NULL) {
    memset(_phoneNumbers, 0, sizeof(_phoneNumbers));
}

PhoneAuthenticator::~PhoneAuthenticator() {
    if (_mutex) vSemaphoreDelete(_mutex);
}

void PhoneAuthenticator::init() {
    if (!_mutex) _mutex = xSemaphoreCreateMutex();
}

bool PhoneAuthenticator::isAuthorized(const char* sender) const {
    if (!sender) return false;

    // Security: Require meaningful length to prevent spoofing/collisions
    int senderLen = strnlen(sender, MAX_PHONE_LEN + 10);
    const int MIN_MATCH_DIGITS = 10; 

    bool authorized = false;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < _phoneCount; i++) {
            // Exact match (Safe)
            if (strcmp(sender, _phoneNumbers[i]) == 0) { authorized = true; break; }

            // Partial match for varying country code formats (+351 vs 00351 vs local)
            int storedLen = strlen(_phoneNumbers[i]);
            if (senderLen >= MIN_MATCH_DIGITS && storedLen >= MIN_MATCH_DIGITS) {
                // Suffix matching for mobile flexibility
                if (strcmp(sender + senderLen - MIN_MATCH_DIGITS,
                           _phoneNumbers[i] + storedLen - MIN_MATCH_DIGITS) == 0) {
                    authorized = true; 
                    break;
                }
            }
        }
        xSemaphoreGive(_mutex);
    }
    return authorized;
}

int PhoneAuthenticator::addPhone(const char* phone) {
    if (!phone || strlen(phone) == 0) return -1;
    
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        // Prevent duplicates
        for (int i = 0; i < _phoneCount; i++) {
            if (strcmp(phone, _phoneNumbers[i]) == 0) {
                xSemaphoreGive(_mutex);
                return i;
            }
        }
        
        if (_phoneCount < MAX_PHONE_NUMBERS) {
            strncpy(_phoneNumbers[_phoneCount], phone, MAX_PHONE_LEN - 1);
            _phoneNumbers[_phoneCount][MAX_PHONE_LEN - 1] = '\0';
            int idx = _phoneCount;
            _phoneCount++;
            xSemaphoreGive(_mutex);
            return idx;
        }
        xSemaphoreGive(_mutex);
    }
    return -1;
}

bool PhoneAuthenticator::setPhone(int slot, const char* phone) {
    if (slot < 0 || slot >= MAX_PHONE_NUMBERS || !phone) return false;
    
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        strncpy(_phoneNumbers[slot], phone, MAX_PHONE_LEN - 1);
        _phoneNumbers[slot][MAX_PHONE_LEN - 1] = '\0';
        if (slot >= _phoneCount) _phoneCount = slot + 1;
        
        // Compact phone count if needed? GA09 typically preserves slots.
        xSemaphoreGive(_mutex);
        return true;
    }
    return false;
}

bool PhoneAuthenticator::removePhone(const char* phone) {
    if (!phone) return false;
    
    bool found = false;
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        for (int i = 0; i < _phoneCount; i++) {
            if (strcmp(phone, _phoneNumbers[i]) == 0) {
                // Shift remainder
                for (int j = i; j < _phoneCount - 1; j++) {
                    strcpy(_phoneNumbers[j], _phoneNumbers[j+1]);
                }
                memset(_phoneNumbers[_phoneCount - 1], 0, MAX_PHONE_LEN);
                _phoneCount--;
                found = true;
                break;
            }
        }
        xSemaphoreGive(_mutex);
    }
    return found;
}

int PhoneAuthenticator::getPhoneCount() const {
    return _phoneCount;
}

const char* PhoneAuthenticator::getPhone(int index) const {
    if (index < 0 || index >= _phoneCount) return nullptr;
    return _phoneNumbers[index];
}

void PhoneAuthenticator::clearPhones() {
    if (_mutex && xSemaphoreTake(_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        memset(_phoneNumbers, 0, sizeof(_phoneNumbers));
        _phoneCount = 0;
        xSemaphoreGive(_mutex);
    }
}
