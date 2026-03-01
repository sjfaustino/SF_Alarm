#include "sms_commands.h"
#include "sms_gateway.h"
#include "alarm_controller.h"
#include "alarm_zones.h"
#include "config.h"
#include <string.h>
#include <ctype.h>

// ---------------------------------------------------------------------------
// Module State
// ---------------------------------------------------------------------------
static char phoneNumbers[MAX_PHONE_NUMBERS][MAX_PHONE_LEN];
static int  phoneCount = 0;

// Custom alarm text per zone (GA09: #X#text)
static char alarmTexts[MAX_ZONES][80];

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static bool isAuthorized(const char* sender)
{
    if (phoneCount == 0) return false;

    for (int i = 0; i < phoneCount; i++) {
        // Compare last 10 digits for flexibility with country code formatting
        int senderLen = strlen(sender);
        int storedLen = strlen(phoneNumbers[i]);

        if (senderLen >= 10 && storedLen >= 10) {
            if (strcmp(sender + senderLen - 10,
                       phoneNumbers[i] + storedLen - 10) == 0) {
                return true;
            }
        }
        if (strcmp(sender, phoneNumbers[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void sendReply(const char* sender, const char* message)
{
    smsGatewaySend(sender, message);
}

static void trimStr(char* str)
{
    // Trim leading whitespace
    char* start = str;
    while (*start && isspace(*start)) start++;
    if (start != str) memmove(str, start, strlen(start) + 1);

    // Trim trailing whitespace
    int len = strlen(str);
    while (len > 0 && isspace(str[len - 1])) {
        str[len - 1] = '\0';
        len--;
    }
}

// ---------------------------------------------------------------------------
// GA09-style Command Parsers
// ---------------------------------------------------------------------------

/// Parse: #NN#phone_number#  — set phone slot NN (01–05)
static bool parseSetPhone(const char* body, const char* sender)
{
    // Format: #01#1234567890#
    if (body[0] != '#') return false;
    if (!isdigit(body[1]) || !isdigit(body[2])) return false;
    if (body[3] != '#') return false;

    int slot = (body[1] - '0') * 10 + (body[2] - '0');
    if (slot < 1 || slot > MAX_PHONE_NUMBERS) return false;

    // Extract phone number (everything between #NN# and trailing #)
    const char* numStart = body + 4;
    const char* numEnd = strchr(numStart, '#');

    char phone[MAX_PHONE_LEN];
    if (numEnd) {
        int len = numEnd - numStart;
        if (len <= 0 || len >= MAX_PHONE_LEN) return false;
        strncpy(phone, numStart, len);
        phone[len] = '\0';
    } else {
        // No trailing #, take rest of string
        strncpy(phone, numStart, MAX_PHONE_LEN - 1);
        phone[MAX_PHONE_LEN - 1] = '\0';
    }

    trimStr(phone);
    if (strlen(phone) == 0) return false;

    smsCmdSetPhone(slot - 1, phone);

    char reply[80];
    snprintf(reply, sizeof(reply), "SF_Alarm: Phone %02d set to %s", slot, phone);
    sendReply(sender, reply);
    return true;
}

/// Parse: @#num1#num2#...  — set multiple phone numbers at once
static bool parseSetMultiplePhones(const char* body, const char* sender)
{
    // Format: @#1234567890#0987654321#...
    if (body[0] != '@' || body[1] != '#') return false;

    smsCmdClearPhones();

    const char* ptr = body + 2;  // Skip @#
    int slot = 0;
    char phone[MAX_PHONE_LEN];

    while (*ptr && slot < MAX_PHONE_NUMBERS) {
        const char* end = strchr(ptr, '#');
        int len;
        if (end) {
            len = end - ptr;
        } else {
            len = strlen(ptr);
        }

        if (len > 0 && len < MAX_PHONE_LEN) {
            strncpy(phone, ptr, len);
            phone[len] = '\0';
            trimStr(phone);

            // Check if this is actually the STATUS? command
            if (strcmp(phone, "STATUS?") == 0) {
                return false;  // Let the status handler deal with it
            }

            if (strlen(phone) > 0) {
                smsCmdSetPhone(slot, phone);
                slot++;
            }
        }

        if (end) {
            ptr = end + 1;
        } else {
            break;
        }
    }

    char reply[80];
    snprintf(reply, sizeof(reply), "SF_Alarm: %d phone number(s) configured", slot);
    sendReply(sender, reply);
    return true;
}

/// Parse: #N#Alarm text  — set alarm text for zone N (1–16)
static bool parseSetAlarmText(const char* body, const char* sender)
{
    // Format: #1#Front door opened  or  #12#Garage sensor
    if (body[0] != '#') return false;

    int zone = 0;
    int idx = 1;

    // Parse zone number (1 or 2 digits)
    if (isdigit(body[idx])) {
        zone = body[idx] - '0';
        idx++;
        if (isdigit(body[idx])) {
            zone = zone * 10 + (body[idx] - '0');
            idx++;
        }
    } else {
        return false;
    }

    if (body[idx] != '#') return false;
    idx++;

    // Check zone is valid (1–16) and this isn't a phone number command
    if (zone < 1 || zone > MAX_ZONES) return false;

    // If we already matched #NN# as a phone command (01-05), don't match here
    // Phone command has body[1] and body[2] as digits and slot 01-05
    // Zone command has zone 1-16 but phone uses 01-05
    // Differentiate: phone commands have the NUMBER after #NN# looking like a phone
    // Alarm text: #N#text where text doesn't start with digits only
    // Actually, the GA09 differentiates by slot range:
    // Slots 01-06 are phones, but zones also start at 1. 
    // The GA09 only has 8 zones and we dont overlap. With 16 zones we can
    // differentiate: if body starts with #0 its a phone command.
    // Zones 1-16: #1# to #16#
    // Phones 01-05: #01# to #05#
    // Only overlap for zones 1-5 with phone 01-05. For GA09, phone commands
    // always have leading zero. So #01# = phone, #1# = zone text.
    if (body[1] == '0') return false;  // Leading zero = phone number command

    const char* text = body + idx;
    smsCmdSetAlarmText(zone - 1, text);

    char reply[80];
    snprintf(reply, sizeof(reply), "SF_Alarm: Zone %d alarm text updated", zone);
    sendReply(sender, reply);
    return true;
}

/// Parse: *NCxyz  — set NC/NO configuration for zones
static bool parseSetNC(const char* body, const char* sender)
{
    // Formats:
    //   *NC0      — all zones NO
    //   *NCALL    — all zones NC
    //   *NC246    — set zones 2,4,6 as NC, rest as NO
    //   *NC1234   — zones 1-4 as NC (single digits only for zones 1-9)
    //   NOTE: For zones 10-16 we extend: *NCa=10, b=11... or just *NC10,12,16
    if (body[0] != '*') return false;

    char upper[64];
    strncpy(upper, body, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (int i = 0; upper[i]; i++) upper[i] = toupper(upper[i]);

    if (strncmp(upper + 1, "NC", 2) != 0) return false;

    const char* args = upper + 3;

    // First, set all zones to NO
    for (int i = 0; i < MAX_ZONES; i++) {
        ZoneConfig* cfg = zonesGetConfig(i);
        if (cfg) cfg->wiring = ZONE_NO;
    }

    if (strcmp(args, "0") == 0) {
        // All NO — already done
        sendReply(sender, "SF_Alarm: All zones set to NO");
        return true;
    }

    if (strcmp(args, "ALL") == 0) {
        for (int i = 0; i < MAX_ZONES; i++) {
            ZoneConfig* cfg = zonesGetConfig(i);
            if (cfg) cfg->wiring = ZONE_NC;
        }
        sendReply(sender, "SF_Alarm: All zones set to NC");
        return true;
    }

    // Parse comma-separated or concatenated zone numbers
    // Support both: *NC246 (single digits) and *NC2,4,6,10,12 (comma-separated)
    if (strchr(args, ',') != NULL) {
        // Comma-separated format for zones > 9
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
        // GA09 original: single-digit concatenation (zones 1-9)
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

/// Parse: @#STATUS?  — query system status
static bool parseStatus(const char* body, const char* sender)
{
    char upper[32];
    strncpy(upper, body, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (int i = 0; upper[i]; i++) upper[i] = toupper(upper[i]);

    if (strcmp(upper, "@#STATUS?") != 0 && strcmp(upper, "STATUS") != 0) {
        return false;
    }

    char buf[160];
    uint16_t triggered = zonesGetTriggeredMask();
    int trigCount = 0;
    for (int i = 0; i < 16; i++) {
        if (triggered & (1 << i)) trigCount++;
    }

    snprintf(buf, sizeof(buf),
             "SF_Alarm [%s] Zones:%d triggered | Clear:%s | Phones:%d",
             alarmGetStateStr(),
             trigCount,
             zonesAllClear() ? "YES" : "NO",
             smsCmdGetPhoneCount());

    sendReply(sender, buf);
    return true;
}

/// Parse: ARM / ARM HOME / DISARM / DISARM <pin>
static bool parseArmDisarm(const char* body, const char* sender)
{
    char upper[64];
    strncpy(upper, body, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (int i = 0; upper[i]; i++) upper[i] = toupper(upper[i]);
    trimStr(upper);

    // ARM HOME <pin>
    if (strncmp(upper, "ARM HOME", 8) == 0) {
        const char* pin = upper + 8;
        while (*pin && isspace(*pin)) pin++;
        if (alarmArmHome(pin)) {
            sendReply(sender, "SF_Alarm: Arming HOME. Exit delay started.");
        } else {
            sendReply(sender, "SF_Alarm: ARM HOME failed. Check PIN/zones.");
        }
        return true;
    }

    // ARM <pin>
    if (strncmp(upper, "ARM", 3) == 0 && (upper[3] == '\0' || isspace(upper[3]))) {
        const char* pin = upper + 3;
        while (*pin && isspace(*pin)) pin++;
        if (alarmArmAway(pin)) {
            sendReply(sender, "SF_Alarm: Arming AWAY. Exit delay started.");
        } else {
            sendReply(sender, "SF_Alarm: ARM failed. Check PIN/zones.");
        }
        return true;
    }

    // DISARM <pin>
    if (strncmp(upper, "DISARM", 6) == 0) {
        // Use original body for PIN (case-sensitive)
        const char* pin = body + 6;
        while (*pin && isspace(*pin)) pin++;
        if (alarmDisarm(pin)) {
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
    char upper[16];
    strncpy(upper, body, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (int i = 0; upper[i]; i++) upper[i] = toupper(upper[i]);
    trimStr(upper);

    if (strcmp(upper, "MUTE") == 0) {
        alarmMuteSiren();
        sendReply(sender, "SF_Alarm: Siren MUTED.");
        return true;
    }
    return false;
}

/// Parse: BYPASS n / UNBYPASS n
static bool parseBypass(const char* body, const char* sender)
{
    char upper[32];
    strncpy(upper, body, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (int i = 0; upper[i]; i++) upper[i] = toupper(upper[i]);
    trimStr(upper);

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
    char upper[16];
    strncpy(upper, body, sizeof(upper) - 1);
    upper[sizeof(upper) - 1] = '\0';
    for (int i = 0; upper[i]; i++) upper[i] = toupper(upper[i]);
    trimStr(upper);

    if (strcmp(upper, "HELP") == 0) {
        sendReply(sender,
                  "SF_Alarm Cmds: "
                  "#01#phone# | #N#text | *NCxyz | "
                  "ARM/DISARM [pin] | STATUS | @#STATUS? | "
                  "MUTE | BYPASS/UNBYPASS n | HELP");
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void smsCmdInit()
{
    phoneCount = 0;
    memset(phoneNumbers, 0, sizeof(phoneNumbers));

    // Set default alarm texts
    for (int i = 0; i < MAX_ZONES; i++) {
        snprintf(alarmTexts[i], sizeof(alarmTexts[i]),
                 "ALARM Zone %d triggered!", i + 1);
    }

    Serial.println("[CMD] SMS command processor initialized (GA09 compatible)");
}

void smsCmdProcess(const char* sender, const char* body)
{
    Serial.printf("[CMD] SMS from %s: \"%s\"\n", sender, body);

    // Make a trimmed copy
    char trimmed[160];
    strncpy(trimmed, body, sizeof(trimmed) - 1);
    trimmed[sizeof(trimmed) - 1] = '\0';
    trimStr(trimmed);

    if (strlen(trimmed) == 0) return;

    // Check authorization (phone config commands from first phone are always allowed
    // if no phones are set yet)
    bool authorized = isAuthorized(sender);
    bool isConfigCmd = (trimmed[0] == '#' || trimmed[0] == '@' || trimmed[0] == '*');

    if (!authorized) {
        // Allow first phone registration if no numbers configured
        if (phoneCount == 0 && trimmed[0] == '#' && isdigit(trimmed[1])) {
            Serial.println("[CMD] First phone registration allowed");
            // Will be processed below
        } else {
            Serial.printf("[CMD] Unauthorized sender: %s\n", sender);
            return;
        }
    }

    // Try each parser in order
    if (parseSetPhone(trimmed, sender)) return;       // #01#phone#
    if (parseSetMultiplePhones(trimmed, sender)) return; // @#num1#num2#
    if (parseSetAlarmText(trimmed, sender)) return;    // #N#text
    if (parseSetNC(trimmed, sender)) return;           // *NCxyz
    if (parseStatus(trimmed, sender)) return;          // @#STATUS? or STATUS
    if (parseArmDisarm(trimmed, sender)) return;       // ARM/DISARM
    if (parseMute(trimmed, sender)) return;            // MUTE
    if (parseBypass(trimmed, sender)) return;          // BYPASS/UNBYPASS
    if (parseHelp(trimmed, sender)) return;            // HELP

    Serial.printf("[CMD] Unknown command: \"%s\"\n", trimmed);
    sendReply(sender, "SF_Alarm: Unknown command. Send HELP for options.");
}

int smsCmdAddPhone(const char* phone)
{
    if (phoneCount >= MAX_PHONE_NUMBERS) {
        Serial.println("[CMD] Phone list full");
        return -1;
    }

    for (int i = 0; i < phoneCount; i++) {
        if (strcmp(phoneNumbers[i], phone) == 0) return i;
    }

    strncpy(phoneNumbers[phoneCount], phone, MAX_PHONE_LEN - 1);
    phoneNumbers[phoneCount][MAX_PHONE_LEN - 1] = '\0';
    int slot = phoneCount;
    phoneCount++;
    Serial.printf("[CMD] Added phone [%d]: %s\n", slot + 1, phone);
    return slot;
}

bool smsCmdSetPhone(int slot, const char* phone)
{
    if (slot < 0 || slot >= MAX_PHONE_NUMBERS) return false;

    strncpy(phoneNumbers[slot], phone, MAX_PHONE_LEN - 1);
    phoneNumbers[slot][MAX_PHONE_LEN - 1] = '\0';

    if (slot >= phoneCount) phoneCount = slot + 1;

    Serial.printf("[CMD] Phone [%02d] = %s\n", slot + 1, phone);
    return true;
}

bool smsCmdRemovePhone(const char* phone)
{
    for (int i = 0; i < phoneCount; i++) {
        if (strcmp(phoneNumbers[i], phone) == 0) {
            for (int j = i; j < phoneCount - 1; j++) {
                strncpy(phoneNumbers[j], phoneNumbers[j + 1], MAX_PHONE_LEN);
            }
            phoneCount--;
            memset(phoneNumbers[phoneCount], 0, MAX_PHONE_LEN);
            Serial.printf("[CMD] Removed phone: %s (%d remaining)\n", phone, phoneCount);
            return true;
        }
    }
    return false;
}

int smsCmdGetPhoneCount()
{
    return phoneCount;
}

const char* smsCmdGetPhone(int index)
{
    if (index < 0 || index >= MAX_PHONE_NUMBERS) return "";
    return phoneNumbers[index];
}

void smsCmdClearPhones()
{
    phoneCount = 0;
    memset(phoneNumbers, 0, sizeof(phoneNumbers));
    Serial.println("[CMD] All phone numbers cleared");
}

void smsCmdSendAlert(const char* message)
{
    Serial.printf("[CMD] Broadcasting alert to %d number(s): %s\n",
                  phoneCount, message);

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
    strncpy(alarmTexts[zoneIndex], text, sizeof(alarmTexts[zoneIndex]) - 1);
    alarmTexts[zoneIndex][sizeof(alarmTexts[zoneIndex]) - 1] = '\0';
    Serial.printf("[CMD] Zone %d alarm text: \"%s\"\n", zoneIndex + 1, text);
}
