#ifndef SF_ALARM_CONFIG_UTILS_H
#define SF_ALARM_CONFIG_UTILS_H

#include <Arduino.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include "config_keys.h"

/**
 * @brief Thread-safe configuration utilities
 */
class ConfigUtils {
public:
    static void init();
    static bool lock(uint32_t timeoutMs = 1000);
    static void unlock();

    /**
     * @brief Scrub potential format string vulnerabilities
     */
    static void scrubFmt(char* str);

    /**
     * @brief Scrub a String object from heap
     */
    static void scrubString(String& str);

    /**
     * @brief Helper to wrap Preferences lifecycle with locking
     */
    class Session {
    public:
        Session(bool readOnly = true);
        ~Session();
        Preferences& p() { return _p; }
        bool isValid() const { return _valid; }
    private:
        Preferences _p;
        bool _valid;
    };
};

#endif // SF_ALARM_CONFIG_UTILS_H
