#include "logging.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

// Circular buffer for log messages
struct LogMessage {
    uint32_t timestamp;
    char level[4];
    char tag[8];
    char msg[256]; // Expanded for JSON/Large payloads (Tungsten Aegis)
};

static QueueHandle_t logQueue = NULL;
static const int LOG_QUEUE_SIZE = 16;

static void logSinkTask(void* pvParameters) {
    LogMessage log;
    while (true) {
        if (xQueueReceive(logQueue, &log, portMAX_DELAY) == pdTRUE) {
            Serial.printf("[%9u][%s][%s] %s\n", log.timestamp, log.level, log.tag, log.msg);
        }
    }
}

void logInit() {
    logQueue = xQueueCreate(LOG_QUEUE_SIZE, sizeof(LogMessage));
    if (logQueue != NULL) {
        xTaskCreatePinnedToCore(logSinkTask, "LogSink", 2048, NULL, 1, NULL, 0); // Core 0 (Low Priority)
    }
}

static volatile uint32_t droppedLogs = 0;

void logPrintf(const char* level, const char* tag, const char* fmt, ...) {
    if (logQueue == NULL) return;

    LogMessage log;
    log.timestamp = millis();
    strncpy(log.level, level, sizeof(log.level) - 1);
    log.level[sizeof(log.level) - 1] = '\0';
    strncpy(log.tag, tag, sizeof(log.tag) - 1);
    log.tag[sizeof(log.tag) - 1] = '\0';

    va_list args;
    va_start(args, fmt);
    vsnprintf(log.msg, sizeof(log.msg), fmt, args);
    va_end(args);

    // Strictly non-blocking to protect real-time loops (e.g. Zone Polling).
    if (xQueueSend(logQueue, &log, 0) != pdTRUE) {
        droppedLogs++;
    }
}

uint32_t logGetDroppedCount() {
    return droppedLogs;
}
