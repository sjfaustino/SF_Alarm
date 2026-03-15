#ifndef SF_ALARM_SMS_COMMAND_PROCESSOR_H
#define SF_ALARM_SMS_COMMAND_PROCESSOR_H

#include <Arduino.h>
#include "config.h"
enum WorkingMode {
    MODE_SMS  = 1,
    MODE_CALL = 2,
    MODE_BOTH = 3
};

class AlarmController;
class ZoneManager;
class IoService;
class NotificationManager;
class MqttService;
class OnvifService;
class WhatsappService;
class TelegramService;
class PhoneAuthenticator;

/**
 * @brief Handles parsing and execution of SMS commands.
 */
class SmsCommandProcessor {
public:
    SmsCommandProcessor();
    ~SmsCommandProcessor();

    void init(AlarmController* alarm, ZoneManager* zones, IoService* io,
              NotificationManager* nm, MqttService* mqtt, OnvifService* onvif,
              WhatsappService* wa, TelegramService* tg, PhoneAuthenticator* auth);

    /**
     * @brief Process a received SMS message.
     */
    void process(const char* sender, const char* body);

    /**
     * @brief Get/Set custom alarm text for zones.
     */
    const char* getAlarmText(int zoneIndex) const;
    void setAlarmText(int zoneIndex, const char* text);

    /**
     * @brief Get/Set recovery text.
     */
    const char* getRecoveryText() const;
    void setRecoveryText(const char* text);

    /**
     * @brief Get/Set report interval.
     */
    uint16_t getReportInterval() const;
    void setReportInterval(uint16_t minutes);

    /**
     * @brief Get/Set working mode.
     */
    WorkingMode getWorkingMode() const;
    void setWorkingMode(WorkingMode mode);

    /**
     * @brief Update loop for periodic reports.
     */
    void update();

private:
    // Dependencies
    AlarmController* _alarm;
    ZoneManager*     _zones;
    IoService*       _io;
    NotificationManager* _nm;
    MqttService*     _mqtt;
    OnvifService*    _onvif;
    WhatsappService* _wa;
    TelegramService* _tg;
    PhoneAuthenticator* _auth;

    // State
    char _alarmTexts[MAX_ZONES][80];
    char _recoveryText[80];
    uint16_t _reportIntervalMin;
    WorkingMode _currentMode;
    uint32_t _lastReportMs;
    SemaphoreHandle_t _stateMutex;

    // Command Parsers
    bool parseSetPhone(const char* body, const char* sender);
    bool parseSetMultiplePhones(const char* body, const char* sender);
    bool parseSetAlarmText(const char* body, const char* sender);
    bool parseSetNC(const char* body, const char* sender);
    bool parseStatus(const char* body, const char* sender);
    bool parseArmDisarm(const char* body, const char* sender);
    bool parseMute(const char* body, const char* sender);
    bool parseBypass(const char* body, const char* sender);
    bool parseHelp(const char* body, const char* sender);
    bool parseReportTimer(const char* body, const char* sender);
    bool parseArmInputs(const char* body, const char* sender);
    bool parseCallNumbers(const char* body, const char* sender);
    bool parseAlertChannel(const char* body, const char* sender);
    bool parseSetWhatsApp(const char* body, const char* sender);
    bool parseSetTelegram(const char* body, const char* sender);
    bool parseSetMQTT(const char* body, const char* sender);
    bool parseWorkingMode(const char* body, const char* sender);

    // Helpers
    void sendReply(const char* sender, const char* message);
    bool tokenize(const char* body, int fieldCount, char* fields[], int fieldSizes[]);
};

#endif // SF_ALARM_SMS_COMMAND_PROCESSOR_H
