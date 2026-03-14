#ifndef SF_ALARM_STRING_UTILS_H
#define SF_ALARM_STRING_UTILS_H

#include <Arduino.h>

/// Extract a substring between two markers from a body safely.
/// Added maxLen guard to prevent "logical memory bombs" (Obsidian Aegis).
inline String extractBetween(const String& body, const char* before, const char* after, int startPos = 0, size_t maxLen = 1024)
{
    int s = body.indexOf(before, startPos);
    if (s < 0) return "";
    s += strlen(before);
    int e = body.indexOf(after, s);
    if (e < 0) return "";
    
    // Guard against oversized extraction causing heap detonation
    if ((size_t)(e - s) > maxLen) return "";

    return body.substring(s, e);
}

/// Explicitly overwrite a String object's underlying buffer with zeros.  
/// Prevents sensitive data (PINs, cookies) from lingering in heap fragments.
inline void scrubString(String& s)
{
    if (s.length() > 0) {
        char* buf = (char*)s.c_str();
        memset(buf, 0, s.length());
        s = ""; 
    }
}

/// Explicitly overwrite a fixed-length buffer with zeros.
inline void scrubBuffer(void* buf, size_t len)
{
    if (buf && len > 0) {
        memset(buf, 0, len);
    }
}

#endif // SF_ALARM_STRING_UTILS_H
