#include "sms_command_processor.h"
#include "phone_authenticator.h"
#include "alarm_controller.h"
#include "zone_manager.h"
#include "io_service.h"
#include "notification_manager.h"
#include "mqtt_client.h"
#include "onvif_client.h"
#include "whatsapp_client.h"
#include "telegram_client.h"
#include "system_state.h"
#include "config_manager.h"
#include "logging.h"
#include <string.h>
#include <ctype.h>

static const char* TAG = "SMSP";

SmsCommandProcessor::SmsCommandProcessor() 
    : _alarm(nullptr), _zones(nullptr), _io(nullptr), _nm(nullptr), 
      _mqtt(nullptr), _onvif(nullptr), _wa(nullptr), _tg(nullptr), _auth(nullptr),
      _reportIntervalMin(DEFAULT_REPORT_INTERVAL_MIN), _currentMode(MODE_SMS),
      _lastReportMs(0), _stateMutex(NULL) {
    memset(_alarmTexts, 0, sizeof(_alarmTexts));
    strcpy(_recoveryText, "SF_Alarm: All zones restored to normal.");
}

SmsCommandProcessor::~SmsCommandProcessor() {
    if (_stateMutex) vSemaphoreDelete(_stateMutex);
}

void SmsCommandProcessor::init(AlarmController* alarm, ZoneManager* zones, IoService* io,
                             NotificationManager* nm, MqttService* mqtt, OnvifService* onvif,
                             WhatsappService* wa, TelegramService* tg, PhoneAuthenticator* auth) {
    _alarm = alarm;
    _zones = zones;
    _io = io;
    _nm = nm;
    _mqtt = mqtt;
    _onvif = onvif;
    _wa = wa;
    _tg = tg;
    _auth = auth;

    if (!_stateMutex) _stateMutex = xSemaphoreCreateMutex();
}

void SmsCommandProcessor::process(const char* sender, const char* body) {
    if (!sender || !body) return;
    if (!_auth->isAuthorized(sender)) {
        LOG_WARN(TAG, "Unauthorized SMS from %s", sender);
        return;
    }

    char cleanBody[256];
    strncpy(cleanBody, body, sizeof(cleanBody)-1);
    cleanBody[sizeof(cleanBody)-1] = '\0';
    
    char* p = cleanBody;
    while (*p && isspace(*p)) p++;
    char* end = p + strlen(p) - 1;
    while (end > p && isspace(*end)) { *end = '\0'; end--; }

    if (strlen(p) == 0) return;

    LOG_INFO(TAG, "Processing SMS from %s: %s", sender, p);

    if (parseArmDisarm(p, sender)) return;
    if (parseStatus(p, sender)) return;
    if (parseHelp(p, sender)) return;
    if (parseMute(p, sender)) return;
    if (parseSetPhone(p, sender)) return;
    if (parseSetMultiplePhones(p, sender)) return;
    if (parseSetAlarmText(p, sender)) return;
    if (parseSetNC(p, sender)) return;
    if (parseBypass(p, sender)) return;
    if (parseReportTimer(p, sender)) return;
    if (parseArmInputs(p, sender)) return;
    if (parseCallNumbers(p, sender)) return;
    if (parseAlertChannel(p, sender)) return;
    if (parseSetWhatsApp(p, sender)) return;
    if (parseSetTelegram(p, sender)) return;
    if (parseSetMQTT(p, sender)) return;
    if (parseWorkingMode(p, sender)) return;

    LOG_WARN(TAG, "Unknown command in SMS: %s", p);
}

void SmsCommandProcessor::sendReply(const char* sender, const char* message) {
    _nm->queueReply(sender, message);
}

bool SmsCommandProcessor::tokenize(const char* body, int fieldCount, char* fields[], int fieldSizes[]) {
    const char* start = body;
    if (*start == '#') start++;
    while (*start && *start != '#') start++;
    if (*start == '#') start++;

    for (int i = 0; i < fieldCount; i++) {
        const char* end = strchr(start, '#');
        if (!end) {
            strncpy(fields[i], start, fieldSizes[i] - 1);
            fields[i][fieldSizes[i]-1] = '\0';
            return (i == fieldCount - 1);
        }
        int len = end - start;
        if (len >= fieldSizes[i]) len = fieldSizes[i] - 1;
        strncpy(fields[i], start, len);
        fields[i][len] = '\0';
        start = end + 1;
    }
    return true;
}

bool SmsCommandProcessor::parseArmDisarm(const char* body, const char* sender) {
    char cmd1[16] = {0};
    const char* p = body;
    int i = 0;
    while (*p && !isspace(*p) && i < 15) { cmd1[i++] = toupper((unsigned char)*p); p++; }
    while (*p && isspace(*p)) p++;

    if (strcmp(cmd1, "ARM") == 0) {
        char cmd2[16] = {0};
        const char* rollback = p;
        i = 0;
        while (*p && !isspace(*p) && i < 15) { cmd2[i++] = toupper((unsigned char)*p); p++; }
        if (strcmp(cmd2, "HOME") == 0) {
            while (*p && isspace(*p)) p++;
            if (_alarm->armHome(p)) sendReply(sender, "SF_Alarm: Arming HOME. Exit delay started.");
            else sendReply(sender, "SF_Alarm: ARM HOME failed. Check PIN/zones.");
            return true;
        } else {
            if (_alarm->armAway(rollback)) sendReply(sender, "SF_Alarm: Arming AWAY. Exit delay started.");
            else sendReply(sender, "SF_Alarm: ARM failed. Check PIN/zones.");
            return true;
        }
    } else if (strcmp(cmd1, "DISARM") == 0) {
        if (_alarm->disarm(p)) sendReply(sender, "SF_Alarm: System DISARMED.");
        else sendReply(sender, "SF_Alarm: DISARM failed. Invalid PIN.");
        return true;
    }
    return false;
}

bool SmsCommandProcessor::parseStatus(const char* body, const char* sender) {
    if (strcasecmp(body, "@#STATUS?") != 0 && strcasecmp(body, "STATUS") != 0 && strcasecmp(body, "STATUS?") != 0) return false;
    
    SystemSnapshot snap;
    StateManager::capture(_alarm, _zones, _io, _nm, _mqtt, _onvif, snap);

    char buf[160];
    int trigCount = 0;
    for (int i = 0; i < 16; i++) { if (snap.zones[i].isTriggered) trigCount++; }

    snprintf(buf, sizeof(buf),
             "SF_Alarm [%s] ZonesTrig:%d | Mask:%04X | Clear:%s | Phones:%d",
             snap.alarmStateStr, trigCount, snap.activeAlarmMask,
             _zones->areAllClear() ? "YES" : "NO", _auth->getPhoneCount());

    sendReply(sender, buf);
    return true;
}

bool SmsCommandProcessor::parseHelp(const char* body, const char* sender) {
    if (strcasecmp(body, "HELP") != 0) return false;
    sendReply(sender, "SF_Alarm: ARM [pin] | DISARM [pin] | STATUS | MUTE | BYPASS n | config: #01#phone# | #N#text");
    return true;
}

bool SmsCommandProcessor::parseMute(const char* body, const char* sender) {
    if (strncasecmp(body, "MUTE", 4) != 0) return false;
    const char* pin = body + 4;
    while (*pin && isspace(*pin)) pin++;
    if (_alarm->muteSiren(pin)) sendReply(sender, "SF_Alarm: Siren MUTED.");
    else sendReply(sender, "SF_Alarm: MUTE failed. Invalid PIN.");
    return true;
}

bool SmsCommandProcessor::parseSetPhone(const char* body, const char* sender) {
    if (body[0] != '#' || !isdigit(body[1]) || !isdigit(body[2]) || body[3] != '#') return false;
    int slot = (body[1] - '0') * 10 + (body[2] - '0');
    if (slot < 1 || slot > MAX_PHONE_NUMBERS) return false;
    const char* start = body + 4;
    const char* end = strchr(start, '#');
    if (!end) return false;
    char phone[MAX_PHONE_LEN];
    int len = end - start;
    if (len >= MAX_PHONE_LEN) len = MAX_PHONE_LEN - 1;
    strncpy(phone, start, len);
    phone[len] = '\0';
    _auth->setPhone(slot - 1, phone);
    configMarkDirty(CFG_ROUTER);
    char reply[64];
    snprintf(reply, sizeof(reply), "SF_Alarm: Phone %d set to %s", slot, phone);
    sendReply(sender, reply);
    return true;
}

bool SmsCommandProcessor::parseSetMultiplePhones(const char* body, const char* sender) {
    if (body[0] != '@' || body[1] != '#') return false;
    _auth->clearPhones();
    char temp[160];
    strncpy(temp, body + 2, sizeof(temp)-1);
    temp[sizeof(temp)-1] = '\0';
    char* saveptr;
    char* token = strtok_r(temp, "#", &saveptr);
    while (token) {
        _auth->addPhone(token);
        token = strtok_r(NULL, "#", &saveptr);
    }
    configMarkDirty(CFG_ROUTER);
    sendReply(sender, "SF_Alarm: Multiple phone numbers updated");
    return true;
}

bool SmsCommandProcessor::parseSetAlarmText(const char* body, const char* sender) {
    if (body[0] != '#' || !isdigit(body[1]) || body[2] != '#') return false;
    int zone = body[1] - '0';
    const char* text = body + 3;
    if (zone == 0) { setRecoveryText(text); sendReply(sender, "SF_Alarm: Recovery text updated"); }
    else if (zone >= 1 && zone <= 9) {
        setAlarmText(zone - 1, text);
        char reply[64];
        snprintf(reply, sizeof(reply), "SF_Alarm: Zone %d alert text updated", zone);
        sendReply(sender, reply);
    } else return false;
    configMarkDirty(CFG_ZONES);
    return true;
}

bool SmsCommandProcessor::parseSetNC(const char* body, const char* sender) {
    const char* args = NULL;
    if (strncasecmp(body, "*NC", 3) == 0) args = body + 3;
    else if (strncasecmp(body, "**NC", 4) == 0) args = body + 4;
    if (!args) return false;

    for (int i = 0; i < MAX_ZONES; i++) {
        ZoneConfig* cfg = _zones->getConfig(i);
        if (cfg) cfg->wiring = ZONE_NO;
    }

    if (strcmp(args, "0") == 0) { sendReply(sender, "SF_Alarm: All zones set to NO"); return true; }
    if (strcasecmp(args, "ALL") == 0) {
        for (int i = 0; i < MAX_ZONES; i++) {
            ZoneConfig* cfg = _zones->getConfig(i);
            if (cfg) cfg->wiring = ZONE_NC;
        }
        sendReply(sender, "SF_Alarm: All zones set to NC");
        return true;
    }

    if (strchr(args, ',') != NULL) {
        char argsCopy[64];
        strncpy(argsCopy, args, sizeof(argsCopy) - 1);
        char* saveptr;
        char* token = strtok_r(argsCopy, ",", &saveptr);
        while (token) {
            int z = atoi(token);
            if (z >= 1 && z <= MAX_ZONES) {
                ZoneConfig* cfg = _zones->getConfig(z - 1);
                if (cfg) cfg->wiring = ZONE_NC;
            }
            token = strtok_r(NULL, ",", &saveptr);
        }
    } else {
        for (int i = 0; args[i]; i++) {
            if (isdigit(args[i])) {
                int z = args[i] - '0';
                if (z >= 1 && z <= 9) {
                    ZoneConfig* cfg = _zones->getConfig(z - 1);
                    if (cfg) cfg->wiring = ZONE_NC;
                }
            }
        }
    }
    configMarkDirty(CFG_ZONES);
    sendReply(sender, "SF_Alarm: NC zones updated");
    return true;
}

bool SmsCommandProcessor::parseBypass(const char* body, const char* sender) {
    if (strncasecmp(body, "BYPASS ", 7) == 0) {
        int zone = atoi(body + 7);
        if (zone >= 1 && zone <= MAX_ZONES) {
            _zones->setBypassed(zone - 1, true);
            sendReply(sender, "SF_Alarm: Zone bypassed");
            return true;
        }
    } else if (strncasecmp(body, "UNBYPASS ", 9) == 0) {
        int zone = atoi(body + 9);
        if (zone >= 1 && zone <= MAX_ZONES) {
            _zones->setBypassed(zone - 1, false);
            sendReply(sender, "SF_Alarm: Zone restored");
            return true;
        }
    }
    return false;
}

bool SmsCommandProcessor::parseReportTimer(const char* body, const char* sender) {
    if (strncmp(body, "%#T", 3) != 0) return false;
    int minutes = atoi(body + 3);
    setReportInterval((uint16_t)minutes);
    sendReply(sender, "SF_Alarm: Report interval updated");
    return true;
}

bool SmsCommandProcessor::parseArmInputs(const char* body, const char* sender) {
    const char* bits = NULL;
    if (strncasecmp(body, "@@#ARM", 6) == 0) bits = body + 6;
    else if (strncasecmp(body, "@#ARM", 5) == 0) bits = body + 5;
    if (!bits) return false;

    int len = strlen(bits);
    for (int i = 0; i < len && i < MAX_ZONES; i++) {
        int bitIdx = len - 1 - i;
        ZoneConfig* cfg = _zones->getConfig(i);
        if (cfg) cfg->enabled = (bits[bitIdx] == '1');
    }
    configMarkDirty(CFG_ZONES);
    sendReply(sender, "SF_Alarm: Zone mask updated");
    return true;
}

bool SmsCommandProcessor::parseCallNumbers(const char* body, const char* sender) {
    if (body[0] != '&') return false;
    sendReply(sender, "SF_Alarm: Voice calls not supported. Use SMS.");
    return true;
}

bool SmsCommandProcessor::parseAlertChannel(const char* body, const char* sender) {
    if (strncmp(body, "%#W", 3) != 0) return false;
    int m = body[3] - '0';
    _nm->setChannels((uint8_t)m);
    configMarkDirty(CFG_ALERTS);
    sendReply(sender, "SF_Alarm: Channels updated");
    return true;
}

bool SmsCommandProcessor::parseSetWhatsApp(const char* body, const char* sender) {
    if (strncmp(body, "#WA#", 4) != 0) return false;
    char phone[32], key[64];
    char* fields[] = {phone, key};
    int sizes[] = {sizeof(phone), sizeof(key)};
    if (!tokenize(body, 2, fields, sizes)) return false;
    _wa->setConfig(phone, key);
    configSaveWhatsapp();
    sendReply(sender, "SF_Alarm: WA config updated");
    return true;
}

bool SmsCommandProcessor::parseSetTelegram(const char* body, const char* sender) {
    if (strncmp(body, "#TG#", 4) != 0) return false;
    char tok[80], chat[32];
    char* fields[] = {tok, chat};
    int sizes[] = {sizeof(tok), sizeof(chat)};
    if (!tokenize(body, 2, fields, sizes)) return false;
    _tg->setConfig(tok, chat);
    configSaveTelegram();
    sendReply(sender, "SF_Alarm: TG config updated");
    return true;
}

bool SmsCommandProcessor::parseSetMQTT(const char* body, const char* sender) {
    if (strncmp(body, "#MQTT#", 6) != 0) return false;
    char temp[160];
    strncpy(temp, body, 159);
    char* saved[7]; int count = 0; char* saveptr;
    char* t = strtok_r(temp, "#", &saveptr);
    while (t && count < 7) { saved[count++] = t; t = strtok_r(NULL, "#", &saveptr); }
    if (count < 2) return false;
    _mqtt->setConfig(saved[1], (count > 2)?atoi(saved[2]):1883, (count > 3)?saved[3]:"", (count > 4)?saved[4]:"", (count > 5)?saved[5]:"SF");
    sendReply(sender, "SF_Alarm: MQTT config updated");
    return true;
}

bool SmsCommandProcessor::parseWorkingMode(const char* body, const char* sender) {
    if (strncmp(body, "%#M", 3) != 0) return false;
    int m = body[3] - '0';
    if (m < 1 || m > 3) return false;
    setWorkingMode((WorkingMode)m);
    configMarkDirty(CFG_ROUTER);
    sendReply(sender, "SF_Alarm: Mode updated");
    return true;
}

const char* SmsCommandProcessor::getAlarmText(int zoneIndex) const {
    if (zoneIndex < 0 || zoneIndex >= MAX_ZONES) return nullptr;
    return _alarmTexts[zoneIndex];
}

void SmsCommandProcessor::setAlarmText(int zoneIndex, const char* text) {
    if (zoneIndex < 0 || zoneIndex >= MAX_ZONES || !text) return;
    if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(_alarmTexts[zoneIndex], text, 79);
        _alarmTexts[zoneIndex][79] = '\0';
        xSemaphoreGive(_stateMutex);
    }
}

const char* SmsCommandProcessor::getRecoveryText() const { return _recoveryText; }
void SmsCommandProcessor::setRecoveryText(const char* text) {
    if (!text) return;
    if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(_recoveryText, text, 79);
        _recoveryText[79] = '\0';
        xSemaphoreGive(_stateMutex);
    }
}

uint16_t SmsCommandProcessor::getReportInterval() const { return _reportIntervalMin; }
void SmsCommandProcessor::setReportInterval(uint16_t minutes) {
    if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _reportIntervalMin = minutes;
        xSemaphoreGive(_stateMutex);
    }
}

WorkingMode SmsCommandProcessor::getWorkingMode() const { return _currentMode; }
void SmsCommandProcessor::setWorkingMode(WorkingMode mode) {
    if (xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        _currentMode = mode;
        xSemaphoreGive(_stateMutex);
    }
}

void SmsCommandProcessor::update() {
    uint32_t now = millis();
    uint16_t interval = getReportInterval();
    if (interval > 0 && (now - _lastReportMs >= (uint32_t)interval * 60000)) {
        _lastReportMs = now;
        SystemSnapshot snap;
        StateManager::capture(_alarm, _zones, _io, _nm, _mqtt, _onvif, snap);
        char buf[128];
        snprintf(buf, sizeof(buf), "SF_Alarm Periodic: State=%s ZonesClear=%s", 
                 snap.alarmStateStr, _zones->areAllClear()?"YES":"NO");
        for (int i = 0; i < _auth->getPhoneCount(); i++) {
            _nm->queueReply(_auth->getPhone(i), buf);
        }
    }
}
