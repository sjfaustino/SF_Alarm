#ifndef SF_ALARM_SERIAL_CLI_H
#define SF_ALARM_SERIAL_CLI_H

#include <Arduino.h>
#include <SimpleCLI.h>

class AlarmController;
class ZoneManager;
class IoService;
class SmsService;
class WhatsappService;
class SmsCommandProcessor;
class PhoneAuthenticator;

/**
 * @brief SerialCLI: Provides a command-line interface over the serial port.
 * Refactored to eliminate singleton patterns and use a poll-driven dispatcher.
 */
class SerialCLI {
public:
    SerialCLI();
    ~SerialCLI();

    /**
     * @brief Initialize the CLI with required services.
     */
    void init(AlarmController* alarm, ZoneManager* zones, IoService* io, 
              SmsService* sms, WhatsappService* whatsapp,
              SmsCommandProcessor* smsProc, PhoneAuthenticator* auth);

    /**
     * @brief Process incoming serial characters. Call from loop() or task.
     */
    void update();

private:
    void registerCommands();
    void printPrompt();
    bool requirePin(Command& command);
    
    // Command Handlers
    void handleCommand(Command command);
    void cmdArm(Command& cmd);
    void cmdDisarm(Command& cmd);
    void cmdMute(Command& cmd);
    void cmdStatus(Command& cmd);
    void cmdZones(Command& cmd);
    void cmdInputs(Command& cmd);
    void cmdOutputs(Command& cmd);
    void cmdNetwork(Command& cmd);
    void cmdTime(Command& cmd);
    void cmdConfig(Command& cmd);
    void cmdReboot(Command& cmd);
    void cmdFactory(Command& cmd);
    void cmdZoneUpdate(Command& cmd);
    void cmdPhone(Command& cmd);
    void cmdTest(Command& cmd);
    void cmdGsm(Command& cmd);
    void cmdSmsBox(Command& cmd);
    void cmdWifi(Command& cmd);
    void cmdRouter(Command& cmd);
    void cmdSetPin(Command& cmd);

    // Dependencies
    AlarmController* _alarm;
    ZoneManager*     _zones;
    IoService*       _io;
    SmsService*      _sms;
    WhatsappService* _whatsapp;
    SmsCommandProcessor* _smsProc;
    PhoneAuthenticator* _auth;

    // State
    SimpleCLI _cli;
    bool _inputMonitorActive;
    uint16_t _lastMonitorInputs;
};

#endif // SF_ALARM_SERIAL_CLI_H
