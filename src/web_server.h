#ifndef SF_ALARM_WEB_SERVER_H
#define SF_ALARM_WEB_SERVER_H

#include <Arduino.h>

/// Initialize and start the async HTTP web server on port 80.
/// Call after networkInit() and configLoad() in setup().
/// No update function needed — ESPAsyncWebServer runs asynchronously.
void webServerInit();

#endif // SF_ALARM_WEB_SERVER_H
