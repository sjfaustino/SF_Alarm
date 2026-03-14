#ifndef SF_ALARM_STRING_UTILS_H
#define SF_ALARM_STRING_UTILS_H

#include <Arduino.h>

/// Extract a substring between two markers from a body safely.
inline String extractBetween(const String& body, const char* before, const char* after, int startPos = 0)
{
    int s = body.indexOf(before, startPos);
    if (s < 0) return "";
    s += strlen(before);
    int e = body.indexOf(after, s);
    if (e < 0) return "";
    return body.substring(s, e);
}

#endif // SF_ALARM_STRING_UTILS_H
