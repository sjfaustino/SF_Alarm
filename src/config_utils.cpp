#include "config_utils.h"
#include "logging.h"
#include "string_utils.h"

static SemaphoreHandle_t configMutex = NULL;
static const char* TAG = "CFG_UTIL";

void ConfigUtils::init() {
    if (configMutex == NULL) {
        configMutex = xSemaphoreCreateRecursiveMutex();
    }
}

bool ConfigUtils::lock(uint32_t timeoutMs) {
    if (configMutex == NULL) return false;
    return xSemaphoreTakeRecursive(configMutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void ConfigUtils::unlock() {
    if (configMutex != NULL) {
        xSemaphoreGiveRecursive(configMutex);
    }
}

void ConfigUtils::scrubFmt(char* str) {
    if (!str) return;
    while (*str) {
        if (*str == '%') *str = '_';
        str++;
    }
}

void ConfigUtils::scrubString(String& str) {
    volatile char* p = (volatile char*)str.c_str();
    size_t len = str.length();
    for (size_t i = 0; i < len; i++) p[i] = 0;
    str = "";
}

ConfigUtils::Session::Session(bool readOnly) : _valid(false) {
    if (ConfigUtils::lock()) {
        if (_p.begin(NVS_NAMESPACE, readOnly)) {
            _valid = true;
        } else {
            ConfigUtils::unlock();
        }
    }
}

ConfigUtils::Session::~Session() {
    if (_valid) {
        _p.end();
        ConfigUtils::unlock();
    }
}
