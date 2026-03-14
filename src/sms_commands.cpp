#include "sms_commands.h"
#include "logging.h"

static const char* TAG = "SMSC";

#include "sms_gateway.h"
#include "whatsapp_client.h"
#include "mqtt_client.h"
#include <ArduinoJson.h>
#include "alarm_controller.h"
#include "alarm_zones.h"
#include "config.h"
#include <string.h>
#include <ctype.h>
#include "config_manager.h"

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static char     phoneNumbers[MAX_PHONE_NUMBERS][MAX_PHONE_LEN];
static int      phoneCount = 0;

// Custom alarm text per zone (GA09: #X#text)
static char     alarmTexts[MAX_ZONES][80];

// Periodic report interval in minutes (GA09: %#Txx)
static uint16_t reportIntervalMin = DEFAULT_REPORT_INTERVAL_MIN;
static SemaphoreHandle_t smsStateMutex = NULL;

static SemaphoreHandle_t htmlMutex = NULL;
#define MAX_SMS_TEXT_LEN 160
static char htmlBuffer[MAX_SMS_TEXT_LEN * 6 + 1];

// Custom recovery text (GA09: #0#text)
static char recoveryText[80] = "SF_Alarm: All zones restored to normal.";

static WorkingMode currentMode = MODE_SMS;
static uint32_t lastReportMs = 0;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isAuthorized(const char* sender)
{
    // Security: Require meaningful length to prevent spoofing/collisions
    int senderLen = strnlen(sender, MAX_PHONE_LEN + 10);
    const int MIN_MATCH_DIGITS = 10; // Stepped up from 9 for better entropy

    bool authorized = false;
    if (smsStateMutex && xSemaphoreTake(smsStateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        for (int i = 0; i < phoneCount; i++) {
            // Exact match (Safe)
            if (strcmp(sender, phoneNumbers[i]) == 0) { authorized = true; break; }

            // Partial match for varying country code formats (+351 vs 00351 vs local)
            int storedLen = strlen(phoneNumbers[i]);
            if (senderLen >= MIN_MATCH_DIGITS && storedLen >= MIN_MATCH_DIGITS) {
                // Strict length delta check (prevent prepending spam to match suffix)
                int lenDiff = (senderLen > storedLen) ? (senderLen - storedLen) : (storedLen - senderLen);
                if (lenDiff <= 5) {
                    if (strcmp(sender + senderLen - MIN_MATCH_DIGITS,
                               phoneNumbers[i] + storedLen - MIN_MATCH_DIGITS) == 0) {
                        authorized = true; 
                        break;
                    }
                }
            }
        }
        xSemaphoreGive(smsStateMutex);
    }
    return authorized;
}

static void sendReply(const char* sender, const char* message)
{
    // Fix: Queue Starvation DoS vulnerability offload.
    // Instead of synchronously triggering smsGatewaySend (which halts the task for 10s),
    // push the reply into the async queue so core processing can continue immediately.
    alarmQueueReply(sender, message);
}



static void trimStr(char* str)
{
    // Trim leading whitespace
    unsigned char* start = (unsigned char*)str;
    while (*start && isspace(*start)) start++;
    if ((char*)start != str) memmove(str, start, strlen((char*)start) + 1);

    // Trim trailing whitespace
    char* end = str + strlen(str) - 1;
    while(end >= str && isspace((unsigned char)*end)) end--;
    *(end + 1) = '\0';
}

/// Encodes unsafe HTML characters to prevent XSS without losing data
static void encodeHtml(char* str, size_t maxLen)
{
    if (str == nullptr || maxLen == 0 || htmlMutex == NULL) return;
    
    if (xSemaphoreTake(htmlMutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        char* src = str;
        char* dst = htmlBuffer;
        
        size_t srcLen = strlen(str);
        if (srcLen > MAX_SMS_TEXT_LEN) srcLen = MAX_SMS_TEXT_LEN;

        for (size_t i = 0; i < srcLen; i++) {
            char c = *src++;
            if (c == '<')      { strcpy(dst, "&lt;"); dst += 4; }
            else if (c == '>') { strcpy(dst, "&gt;"); dst += 4; }
            else if (c == '"') { strcpy(dst, "&quot;"); dst += 6; }
            else if (c == '\''){ strcpy(dst, "&apos;"); dst += 6; }
            else if (c == '&') { strcpy(dst, "&amp;"); dst += 5; }
            else { *dst++ = c; }
        }
        *dst = '\0';
        
        strncpy(str, htmlBuffer, maxLen - 1);
        str[maxLen - 1] = '\0';
        
        xSemaphoreGive(htmlMutex);
    } else {
        // Omega Suture: If the static buffer mutex timed out, use an emergency in-place scrubber.
        // This is stack-safe (no allocation), avoids logic crashes from error strings,
        // and never returns unsanitized content. It replaces dangerous chars with '?'.
        LOG_ERROR(TAG, "HTML encoder timeout! Using emergency in-place scrubber.");
        char* p = str;
        while (*p) {
            if (*p == '<' || *p == '>' || *p == '"' || *p == '\'' || *p == '&') {
                *p = '?';
            }
            p++;
        }
    }
}

// ---------------------------------------------------------------------------
// Individual Parsers (Extracted from GA09 monolith)
// ---------------------------------------------------------------------------

/// Parse: #NN#phone#  — where NN is 01-16
static bool parseSetPhone(const char* body, const char* sender)
{
    if (body[0] != '#' || !isdigit(body[1]) || !isdigit(body[2]) || body[3] != '#') return false;

    int slot = (body[1] - '0') * 10 + (body[2] - '0');
    if (slot < 1 || slot > MAX_PHONE_NUMBERS) return false;

    // Extract phone number between #NN# and trailing #
    const char* start = body + 4;
    const char* end = strchr(start, '#');
    
    char phone[MAX_PHONE_LEN];
    if (end) {
        int len = end - start;
        if (len >= MAX_PHONE_LEN) len = MAX_PHONE_LEN - 1;
        if (len < 0) return false; // Guard against negative math
        strncpy(phone, start, len);
        phone[len] = '\0';
    } else {
        // Strict Boundary Hardening: Reject if trailing # is missing to prevent walk-off
        return false;
    }

    if (strlen(phone) == 0) {
        // Special case: empty phone = remove? Some versions use this.
        // For now, GA09 usually expects a number.
        return false;
    }

    smsCmdSetPhone(slot - 1, phone);
    
    char reply[64];
    snprintf(reply, sizeof(reply), "SF_Alarm: Phone %d set to %s", slot, phone);
    sendReply(sender, reply);
    return true;
}

/// Parse: @#phone1#phone2#...  — set multiple phones at once
static bool parseSetMultiplePhones(const char* body, const char* sender)
{
    // Format: @#600123456#600654321#
    if (body[0] != '@' || body[1] != '#') return false;

    smsCmdClearPhones();

    char temp[160];
    strncpy(temp, body + 2, sizeof(temp)-1);
    temp[sizeof(temp)-1] = '\0';

    char* token = strtok(temp, "#");
    while (token) {
        smsCmdAddPhone(token);
        token = strtok(NULL, "#");
    }

    sendReply(sender, "SF_Alarm: Multiple phone numbers updated");
    return true;
}

/// Parse: #N#text  — where N is 1-16 (alarm) or 0 (recovery)
static bool parseSetAlarmText(const char* body, const char* sender)
{
    if (body[0] != '#' || !isdigit(body[1]) || body[2] != '#') return false;

    int zone = body[1] - '0';
    const char* text = body + 3;

    if (zone == 0) {
        smsCmdSetRecoveryText(text);
        sendReply(sender, "SF_Alarm: Recovery text updated");
    } else if (zone >= 1 && zone <= 9) { // GA09 restriction: single digit
        smsCmdSetAlarmText(zone - 1, text);
        char reply[64];
        snprintf(reply, sizeof(reply), "SF_Alarm: Zone %d alert text updated", zone);
        sendReply(sender, reply);
    } else {
        return false;
    }
    return true;
}

/// Parse: *NCxxx  — set zones to NC (Normal Closed)
static bool parseSetNC(const char* body, const char* sender)
{
    // Format: *NC123 or *NC1,2,3 or *NCALL
    const char* args = NULL;
    if (strncasecmp(body, "*NC", 3) == 0) args = body + 3;
    else if (strncasecmp(body, "**NC", 4) == 0) args = body + 4;
    
    if (!args) return false;

    // Reset all to NO first
    for (int i = 0; i < MAX_ZONES; i++) {
        ZoneConfig* cfg = zonesGetConfig(i);
        if (cfg) cfg->wiring = ZONE_NO;
    }

    if (strcmp(args, "0") == 0) {
        sendReply(sender, "SF_Alarm: All zones set to NO");
        return true;
    }

    if (strcasecmp(args, "ALL") == 0) {
        for (int i = 0; i < MAX_ZONES; i++) {
            ZoneConfig* cfg = zonesGetConfig(i);
            if (cfg) cfg->wiring = ZONE_NC;
        }
        sendReply(sender, "SF_Alarm: All zones set to NC");
        return true;
    }

    if (strchr(args, ',') != NULL) {
        char argsCopy[64];
        strncpy(argsCopy, args, sizeof(argsCopy) - 1);
        argsCopy[sizeof(argsCopy) - 1] = '\0';
        char* token = strtok(argsCopy, ",");
        while (token) {
            int z = atoi(token);
            if (z >= 1 && z <= MAX_ZONES) {
                ZoneConfig* cfg = zonesGetConfig(z - 1);
                if (cfg) cfg->wiring = ZONE_NC;
            }
            token = strtok(NULL, ",");
        }
    } else {
        for (int i = 0; args[i]; i++) {
            if (isdigit(args[i])) {
                int z = args[i] - '0';
                if (z >= 1 && z <= 9) {
                    ZoneConfig* cfg = zonesGetConfig(z - 1);
                    if (cfg) cfg->wiring = ZONE_NC;
                }
            }
        }
    }

    sendReply(sender, "SF_Alarm: NC zones updated");
    return true;
}

/// Parse: STATUS?  — query system status
static bool parseStatus(const char* body, const char* sender)
{
    char buf[160];
    uint16_t triggered = zonesGetTriggeredMask();
    int trigCount = 0;
    for (int i = 0; i < 16; i++) {
        if (triggered & (1 << i)) trigCount++;
    }

    snprintf(buf, sizeof(buf),
             "SF_Alarm [%s] ZonesTrig:%d | Mask:%04X | Clear:%s | Phones:%d",
             alarmGetStateStr(),
             trigCount,
             alarmGetActiveAlarmMask(),
             zonesAllClear() ? "YES" : "NO",
             smsCmdGetPhoneCount());

    sendReply(sender, buf);
    return true;
}

/// Parse: ARM / ARM HOME / DISARM / DISARM <pin>
static bool parseArmDisarm(const char* body, const char* sender)
{
    // The incoming 'body' is already trimmed of leading/trailing spaces.
    // Let's find the command by skipping spaces manually.
    const char* p = body;
    char cmd1[16] = {0};
    char cmd2[16] = {0};
    
    // Read first word
    int i = 0;
    while (*p && !isspace(*p) && i < 15) {
        cmd1[i++] = toupper((unsigned char)*p);
        p++;
    }
    // skip spaces
    while (*p && isspace(*p)) p++;
    
    // If cmd1 is ARM, we might have HOME as cmd2
    if (strcmp(cmd1, "ARM") == 0) {
        const char* rollback = p;
        i = 0;
        while (*p && !isspace(*p) && i < 15) {
            cmd2[i++] = toupper((unsigned char)*p);
            p++;
        }
        if (strcmp(cmd2, "HOME") == 0) {
            // skip spaces
            while (*p && isspace(*p)) p++;
            if (alarmArmHome(p)) {
                sendReply(sender, "SF_Alarm: Arming HOME. Exit delay started.");
            } else {
                sendReply(sender, "SF_Alarm: ARM HOME failed. Check PIN/zones.");
            }
            return true;
        } else {
            // It was just ARM <pin>
            p = rollback; // The second word is actually the pin
            if (alarmArmAway(p)) {
                sendReply(sender, "SF_Alarm: Arming AWAY. Exit delay started.");
            } else {
                sendReply(sender, "SF_Alarm: ARM failed. Check PIN/zones.");
            }
            return true;
        }
    } else if (strcmp(cmd1, "DISARM") == 0) {
        if (alarmDisarm(p)) {
            sendReply(sender, "SF_Alarm: System DISARMED.");
        } else {
            sendReply(sender, "SF_Alarm: DISARM failed. Invalid PIN.");
        }
        return true;
    }

    return false;
}

/// Parse: MUTE
static bool parseMute(const char* body, const char* sender)
{
    const char* pin = body + 4;
    while (*pin && isspace(*pin)) pin++;

    if (alarmMuteSiren(pin)) {
        sendReply(sender, "SF_Alarm: Siren MUTED.");
    } else {
        sendReply(sender, "SF_Alarm: MUTE failed. Invalid PIN.");
    }
    return true;
}

/// Parse: BYPASS n / UNBYPASS n
static bool parseBypass(const char* body, const char* sender)
{
    char upper[32];
    strncpy(upper, body, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (int i = 0; upper[i]; i++) upper[i] = toupper((unsigned char)upper[i]);

    if (strncmp(upper, "BYPASS ", 7) == 0) {
        int zone = atoi(upper + 7);
        if (zone >= 1 && zone <= MAX_ZONES) {
            zonesSetBypassed(zone - 1, true);
            char reply[64];
            snprintf(reply, sizeof(reply), "SF_Alarm: Zone %d BYPASSED", zone);
            sendReply(sender, reply);
            return true;
        }
    }

    if (strncmp(upper, "UNBYPASS ", 9) == 0) {
        int zone = atoi(upper + 9);
        if (zone >= 1 && zone <= MAX_ZONES) {
            zonesSetBypassed(zone - 1, false);
            char reply[64];
            snprintf(reply, sizeof(reply), "SF_Alarm: Zone %d restored", zone);
            sendReply(sender, reply);
            return true;
        }
    }

    return false;
}

/// Parse: HELP
static bool parseHelp(const char* body, const char* sender)
{
    sendReply(sender, 
              "SF_Alarm Control:\n"
              "ARM [pin] | ARM HOME [alias] | DISARM [pin]\n"
              "STATUS | @#STATUS? | MUTE\n"
              "BYPASS n | UNBYPASS n");
    
    sendReply(sender,
              "SF_Alarm Config:\n"
              "#01#phone# (Add phone)\n"
              "#N#text (Alarm text)\n"
              "#0#text (Recovery text)\n"
              "*NCxyz (NC zones)\n"
              "%#Mx (Mode: 1.SMS 2.Call 3.Both)");

    sendReply(sender,
              "SF_Alarm Integrations:\n"
              "%#Wx (Alert: 1.SMS 2.WA 3.Both)\n"
              "#WA#ph#key# (WhatsApp setup)\n"
              "#MQTT#srv#port#usr#pass# (Broker)");
    return true;
}

/// Parse: %#Txx  — set report timer in minutes
static bool parseReportTimer(const char* body, const char* sender)
{
    int minutes = atoi(body + 3);
    if (minutes < 0) minutes = 0;
    if (minutes > MAX_REPORT_INTERVAL_MIN) minutes = MAX_REPORT_INTERVAL_MIN;

    smsCmdSetReportInterval(minutes);

    char reply[80];
    if (minutes == 0) {
        snprintf(reply, sizeof(reply), "SF_Alarm: Periodic status report DISABLED");
    } else {
        snprintf(reply, sizeof(reply), "SF_Alarm: Periodic status report set to %d minutes", minutes);
    }
    sendReply(sender, reply);
    return true;
}

/// Parse: @#ARMXXXXXXXX  — enable/disable zones via binary string
static bool parseArmInputs(const char* body, const char* sender)
{
    const char* bits = NULL;
    if (strncasecmp(body, "@@#ARM", 6) == 0) bits = body + 6;
    else if (strncasecmp(body, "@#ARM", 5) == 0) bits = body + 5;
    
    if (bits == NULL) return false;
    int len = strlen(bits);
    if (len == 0) return false;

    for (int i = 0; i < len && i < MAX_ZONES; i++) {
        int bitIdx = len - 1 - i;
        bool enabled = (bits[bitIdx] == '1');
        ZoneConfig* cfg = zonesGetConfig(i);
        if (cfg) cfg->enabled = enabled;
    }

    configMarkDirty(CFG_ZONES);
    sendReply(sender, "SF_Alarm: Zone enable/disable configuration updated");
    return true;
}

/// Parse: &...  — voice call numbers (unsupported)
static bool parseCallNumbers(const char* body, const char* sender)
{
    sendReply(sender, "SF_Alarm: Voice call alerts not supported by hardware. Use SMS alerts (#01#).");
    return true;
}

/// Parse: %#Wx  — set alert channel (1:SMS, 2:WA, 3:Both)
static bool parseAlertChannel(const char* body, const char* sender)
{
    int m = body[3] - '0';
    if (m < 1 || m > 3) return false;

    whatsappSetConfig(whatsappGetPhone(), whatsappGetApiKey(), (WhatsAppMode)m);

    char reply[80];
    const char* modeStrs[] = {"", "SMS ONLY", "WHATSAPP ONLY", "SMS & WHATSAPP"};
    snprintf(reply, sizeof(reply), "SF_Alarm: Alert channel set to %s", modeStrs[m]);
    sendReply(sender, reply);
    return true;
}

/// Parse: #WA#phone#apikey#  — set WhatsApp credentials
static bool parseSetWhatsApp(const char* body, const char* sender)
{
    const char* phoneStart = body + 4;
    const char* phoneEnd = strchr(phoneStart, '#');
    if (!phoneEnd) return false;

    char phone[32];
    int phoneLen = phoneEnd - phoneStart;
    if (phoneLen >= (int)sizeof(phone)) phoneLen = sizeof(phone) - 1;
    strncpy(phone, phoneStart, phoneLen);
    phone[phoneLen] = '\0';

    const char* keyStart = phoneEnd + 1;
    const char* keyEnd = strchr(keyStart, '#');
    char key[32];
    if (keyEnd) {
        int keyLen = keyEnd - keyStart;
        if (keyLen >= (int)sizeof(key)) keyLen = sizeof(key) - 1;
        strncpy(key, keyStart, keyLen);
        key[keyLen] = '\0';
    } else {
        strncpy(key, keyStart, sizeof(key) - 1);
        key[sizeof(key) - 1] = '\0';
    }

    whatsappSetConfig(phone, key, whatsappGetMode());
    sendReply(sender, "SF_Alarm: WhatsApp configuration updated");
    return true;
}

/// Parse: #MQTT#server#port#user#pass# — set MQTT credentials
static bool parseSetMQTT(const char* body, const char* sender)
{
    char temp[160];
    strncpy(temp, body, sizeof(temp)-1);
    temp[sizeof(temp)-1] = '\0';

    char* parts[7]; 
    int count = 0;
    char* token = strtok(temp, "#");
    while (token && count < 7) {
        parts[count++] = token;
        token = strtok(NULL, "#");
    }

    if (count < 2) return false; 

    const char* server = parts[1];
    uint16_t port = (count > 2) ? atoi(parts[2]) : 1883;
    const char* user = (count > 3) ? parts[3] : "";
    const char* pass = (count > 4) ? parts[4] : "";
    const char* clientId = (count > 5) ? parts[5] : "SF_Alarm";

    mqttSetConfig(server, port, user, pass, clientId);
    sendReply(sender, "SF_Alarm: MQTT configuration updated");
    return true;
}

/// Parse: %#Mx  — set working mode (1:SMS, 2:Call, 3:Both)
static bool parseWorkingMode(const char* body, const char* sender)
{
    int m = body[3] - '0';
    if (m < 1 || m > 3) return false;

    smsCmdSetWorkingMode((WorkingMode)m);

    char reply[80];
    const char* modeStrs[] = {"", "SMS ONLY", "CALL ONLY", "SMS & CALL"};
    snprintf(reply, sizeof(reply), "SF_Alarm: Working mode set to %s", modeStrs[m]);
    sendReply(sender, reply);
    return true;
}

// ---------------------------------------------------------------------------
// Table-Driven Dispatcher (The Iron Citadel)
// ---------------------------------------------------------------------------

typedef bool (*ParserFunc)(const char* body, const char* sender);

struct CommandEntry {
    const char* pattern;
    ParserFunc parser;
    bool modifiesConfig;
};

static const CommandEntry COMMAND_TABLE[] = {
    {"#01#",      parseSetPhone,          true},
    {"@#",        parseSetMultiplePhones, true},
    {"@@#",       parseSetMultiplePhones, true},
    {"#WA#",      parseSetWhatsApp,       true},
    {"#MQTT#",    parseSetMQTT,           true},
    {"#",         parseSetAlarmText,      true},
    {"*NC",       parseSetNC,             true},
    {"**NC",      parseSetNC,             true},
    {"%#T",       parseReportTimer,       true},
    {"@#ARM",     parseArmInputs,         true},
    {"@@#ARM",    parseArmInputs,         true},
    {"%#M",       parseWorkingMode,       true},
    {"%#W",       parseAlertChannel,       true},
    {"&",         parseCallNumbers,       false},
    {"ARM HOME",  parseArmDisarm,         false},
    {"ARM",       parseArmDisarm,         false},
    {"DISARM",    parseArmDisarm,         false},
    {"MUTE",      parseMute,              false},
    {"BYPASS",    parseBypass,            false},
    {"UNBYPASS",  parseBypass,            false},
    {"STATUS",    parseStatus,            false},
    {"@#STATUS?", parseStatus,            false},
    {"@@#STATUS?",parseStatus,            false},
    {"HELP",      parseHelp,              false}
};
static const int COMMAND_TABLE_SIZE = sizeof(COMMAND_TABLE) / sizeof(CommandEntry);

void smsCmdProcess(const char* sender, const char* body)
{
    // Logic Bomb Prevention: Inhibit remote commands during critical hardware failure
    if (alarmGetState() == ALARM_TRIGGERED) {
        LOG_WARN(TAG, "SMS Command ignored while in PANIC state.");
        return;
    }

    char upperBody[32];
    strncpy(upperBody, body, sizeof(upperBody)-1);
    upperBody[sizeof(upperBody)-1] = '\0';
    for(int i=0; upperBody[i]; i++) upperBody[i] = toupper((unsigned char)upperBody[i]);

    bool hasPIN = (strstr(upperBody, "ARM") != NULL || strcasestr(upperBody, "DISARM") != NULL);
    if (hasPIN) {
        LOG_INFO(TAG, "SMS from %s: \"[ACTION WITH PIN]\"", sender);
    } else {
        LOG_INFO(TAG, "SMS from %s: \"%s\"", sender, body);
    }

    char trimmed[160];
    strncpy(trimmed, body, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';
    trimStr(trimmed);
    if (strlen(trimmed) == 0) return;

    bool authorized = isAuthorized(sender);
    bool isFirstPhoneReg = (trimmed[0] == '#' && isdigit(trimmed[1]) && phoneCount == 0);

    if (!authorized && !isFirstPhoneReg) {
        LOG_WARN(TAG, "SECURITY: Unauthorized SMS attempt from %s", sender);
        return;
    }

    bool handled = false;
    bool configChanged = false;

    char upperTrimmed[160];
    strncpy(upperTrimmed, trimmed, sizeof(upperTrimmed)-1);
    upperTrimmed[sizeof(upperTrimmed)-1] = '\0';
    for (int i = 0; upperTrimmed[i]; i++) upperTrimmed[i] = toupper((unsigned char)upperTrimmed[i]);

    for (int i = 0; i < COMMAND_TABLE_SIZE; i++) {
        const CommandEntry& entry = COMMAND_TABLE[i];
        bool match = false;
        
        if (entry.pattern[0] == '#' || entry.pattern[0] == '@' || entry.pattern[0] == '*' || entry.pattern[0] == '%' || entry.pattern[0] == '&') {
            match = (strncasecmp(trimmed, entry.pattern, strlen(entry.pattern)) == 0);
        } else {
            match = (strncmp(upperTrimmed, entry.pattern, strlen(entry.pattern)) == 0);
        }

        if (match) {
            if (entry.parser(trimmed, sender)) {
                handled = true;
                if (entry.modifiesConfig) configChanged = true;
                break;
            }
        }
    }

    if (handled) {
        if (configChanged) {
            configSave();
            LOG_INFO(TAG, "Configuration updated via SMS command");
        }
        return;
    }

    LOG_WARN(TAG, "Unknown command from %s: \"%s\"", sender, trimmed);
    sendReply(sender, "SF_Alarm: Unknown command. Send HELP for options.");
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void smsCmdInit()
{
    if (htmlMutex == NULL) {
        htmlMutex = xSemaphoreCreateMutex();
    }
    if (smsStateMutex == NULL) {
        smsStateMutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    phoneCount = 0;
    memset(phoneNumbers, 0, sizeof(phoneNumbers));

    for (int i = 0; i < MAX_ZONES; i++) {
        snprintf(alarmTexts[i], sizeof(alarmTexts[i]),
                 "ALARM Zone %d triggered!", i + 1);
    }
    xSemaphoreGive(smsStateMutex);

    LOG_INFO(TAG, "SMS command processor initialized (GA09 compatible)");
}

int smsCmdAddPhone(const char* phone)
{
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    if (phoneCount >= MAX_PHONE_NUMBERS) {
        xSemaphoreGive(smsStateMutex);
        LOG_WARN(TAG, "Phone list full");
        return -1;
    }

    for (int i = 0; i < phoneCount; i++) {
        if (strcmp(phoneNumbers[i], phone) == 0) {
            xSemaphoreGive(smsStateMutex);
            return i;
        }
    }

    strncpy(phoneNumbers[phoneCount], phone, MAX_PHONE_LEN - 1);
    phoneNumbers[phoneCount][MAX_PHONE_LEN - 1] = '\0';
    int slot = phoneCount;
    phoneCount++;
    xSemaphoreGive(smsStateMutex);

    LOG_INFO(TAG, "Added phone [%d]: %s", slot + 1, phone);
    return slot;
}

bool smsCmdSetPhone(int slot, const char* phone)
{
    if (slot < 0 || slot >= MAX_PHONE_NUMBERS) return false;

    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    strncpy(phoneNumbers[slot], phone, MAX_PHONE_LEN - 1);
    phoneNumbers[slot][MAX_PHONE_LEN - 1] = '\0';
    if (slot >= phoneCount) phoneCount = slot + 1;
    xSemaphoreGive(smsStateMutex);

    LOG_INFO(TAG, "Phone [%02d] = %s", slot + 1, phone);
    return true;
}

bool smsCmdRemovePhone(const char* phone)
{
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    for (int i = 0; i < phoneCount; i++) {
        if (strcmp(phoneNumbers[i], phone) == 0) {
            for (int j = i; j < phoneCount - 1; j++) {
                strncpy(phoneNumbers[j], phoneNumbers[j + 1], MAX_PHONE_LEN - 1);
                phoneNumbers[j][MAX_PHONE_LEN - 1] = '\0';
            }
            phoneCount--;
            memset(phoneNumbers[phoneCount], 0, MAX_PHONE_LEN);
            xSemaphoreGive(smsStateMutex);
            LOG_INFO(TAG, "Removed phone: %s (%d remaining)", phone, phoneCount);
            return true;
        }
    }
    xSemaphoreGive(smsStateMutex);
    return false;
}

int smsCmdGetPhoneCount()
{
    return phoneCount;
}

const char* smsCmdGetPhone(int index)
{
    if (index < 0 || index >= MAX_PHONE_NUMBERS) return "";
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    const char* p = phoneNumbers[index];
    xSemaphoreGive(smsStateMutex);
    return p;
}

void smsCmdClearPhones()
{
    phoneCount = 0;
    memset(phoneNumbers, 0, sizeof(phoneNumbers));
    LOG_INFO(TAG, "All phone numbers cleared");
}

void smsCmdSendAlert(const char* message)
{
    LOG_INFO(TAG, "Broadcasting alert to %d number(s): %s", phoneCount, message);
    for (int i = 0; i < phoneCount; i++) {
        if (strlen(phoneNumbers[i]) > 0) {
            smsGatewaySend(phoneNumbers[i], message);
        }
    }
}

const char* smsCmdGetAlarmText(int zoneIndex)
{
    if (zoneIndex < 0 || zoneIndex >= MAX_ZONES) return "";
    return alarmTexts[zoneIndex];
}

void smsCmdSetAlarmText(int zoneIndex, const char* text)
{
    if (zoneIndex < 0 || zoneIndex >= MAX_ZONES) return;
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    strncpy(alarmTexts[zoneIndex], text, sizeof(alarmTexts[zoneIndex]) - 1);
    alarmTexts[zoneIndex][sizeof(alarmTexts[zoneIndex]) - 1] = '\0';
    encodeHtml(alarmTexts[zoneIndex], sizeof(alarmTexts[zoneIndex]));
    xSemaphoreGive(smsStateMutex);
    LOG_INFO(TAG, "Zone %d alarm text: \"%s\"", zoneIndex + 1, alarmTexts[zoneIndex]);
}

uint16_t smsCmdGetReportInterval()
{
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    uint16_t r = reportIntervalMin;
    xSemaphoreGive(smsStateMutex);
    return r;
}

void smsCmdSetReportInterval(uint16_t minutes)
{
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    reportIntervalMin = minutes;
    xSemaphoreGive(smsStateMutex);
    LOG_INFO(TAG, "Report interval set to %d minutes", minutes);
}

const char* smsCmdGetRecoveryText()
{
    return recoveryText;
}

void smsCmdSetRecoveryText(const char* text)
{
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    strncpy(recoveryText, text, sizeof(recoveryText) - 1);
    recoveryText[sizeof(recoveryText) - 1] = '\0';
    encodeHtml(recoveryText, sizeof(recoveryText));
    xSemaphoreGive(smsStateMutex);
    LOG_INFO(TAG, "Recovery text: \"%s\"", recoveryText);
}

WorkingMode smsCmdGetWorkingMode()
{
    return currentMode;
}

void smsCmdSetWorkingMode(WorkingMode mode)
{
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    currentMode = mode;
    xSemaphoreGive(smsStateMutex);
    LOG_INFO(TAG, "Working mode: %d", (int)mode);
}

void smsCmdUpdate()
{
    uint32_t now = millis();
    xSemaphoreTake(smsStateMutex, portMAX_DELAY);
    uint16_t interval = reportIntervalMin;
    xSemaphoreGive(smsStateMutex);

    if (interval > 0) {
        if (lastReportMs == 0) lastReportMs = now;
        if (now - lastReportMs >= (uint32_t)interval * 60 * 1000) {
            lastReportMs = now;
            char buf[160];
            uint16_t triggered = zonesGetTriggeredMask();
            int trigCount = 0;
            for (int j = 0; j < 16; j++) {
                if (triggered & (1 << j)) trigCount++;
            }

            snprintf(buf, sizeof(buf),
                     "SF_Alarm PERIODIC: [%s] Trig:%d Mask:%04X | Clear:%s",
                     alarmGetStateStr(),
                     trigCount,
                     alarmGetActiveAlarmMask(),
                     zonesAllClear() ? "YES" : "NO");
            alarmBroadcast(buf);
        }
    } else {
        lastReportMs = 0;
    }
}
