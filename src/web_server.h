#ifndef SF_ALARM_WEB_SERVER_H
#define SF_ALARM_WEB_SERVER_H

#include <Arduino.h>

/// Initialize and start the async HTTP web server on port 80.
/// Call after networkInit() and configLoad() in setup().
/// No update function needed — ESPAsyncWebServer runs asynchronously.
class AlarmController;
class ZoneManager;
class IoService;
class NotificationManager;
class MqttService;
class OnvifService;
class PhoneAuthenticator;
class SmsCommandProcessor;
class WhatsappService;
class TelegramService;

void webServerInit(AlarmController* alarm, ZoneManager* zones, IoService* io,
                  NotificationManager* nm, MqttService* mqtt, OnvifService* onvif,
                  PhoneAuthenticator* auth, SmsCommandProcessor* smsProc,
                  WhatsappService* whatsapp, TelegramService* telegram);

#endif // SF_ALARM_WEB_SERVER_H
