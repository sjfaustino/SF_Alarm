#ifndef SF_ALARM_HTML_UTILS_H
#define SF_ALARM_HTML_UTILS_H

#include <Arduino.h>

/**
 * HtmlUtils: Robust, lightweight HTML parsing for embedded systems.
 * Replaces brittle "Butter Knife Surgery" (manual String::indexOf chains)
 * with tag-aware attribute extraction.
 */
namespace HtmlUtils {

/// Find the start and end of a specific HTML tag.
/// Returns true if found, sets start and end positions.
inline bool findTag(const String& html, const char* tagName, int& tagStart, int& tagEnd, int startPos = 0) {
    String searchStr = String("<") + tagName;
    tagStart = html.indexOf(searchStr, startPos);
    if (tagStart < 0) return false;

    // Check if it's a real tag (followed by space, newline, or >)
    char next = html.charAt(tagStart + searchStr.length());
    if (next != ' ' && next != '>' && next != '\n' && next != '\r' && next != '\t') {
        // Just a prefix match (e.g. <input-group instead of <input)
        return findTag(html, tagName, tagStart, tagEnd, tagStart + 1);
    }

    tagEnd = html.indexOf('>', tagStart);
    if (tagEnd < 0) return false;

    return true;
}

/// Extract an attribute value from an isolated HTML tag string.
/// Handles double quotes, single quotes, and unquoted values.
inline String getAttribute(const String& tag, const char* attrName) {
    // Search for attrName=
    String attrSearch = String(attrName) + "=";
    int attrPos = tag.indexOf(attrSearch);
    if (attrPos < 0) {
        String lowerTag = tag;
        lowerTag.toLowerCase();
        String lowerAttr = attrSearch;
        lowerAttr.toLowerCase();
        attrPos = lowerTag.indexOf(lowerAttr);
        if (attrPos < 0) return "";
    }

    int valStart = attrPos + attrSearch.length();
    if (valStart >= (int)tag.length()) return "";

    char quote = tag.charAt(valStart);
    int actualStart = valStart;
    int actualEnd = -1;

    if (quote == '"' || quote == '\'') {
        actualStart++;
        actualEnd = tag.indexOf(quote, actualStart);
    } else {
        // No quotes (e.g., value=123)
        // Find first space or end of tag
        for (int i = actualStart; i < (int)tag.length(); i++) {
            char c = tag.charAt(i);
            if (isspace(c) || c == '>' || c == '/') {
                actualEnd = i;
                break;
            }
        }
    }

    if (actualEnd < 0 || actualEnd <= actualStart) return "";
    return tag.substring(actualStart, actualEnd);
}

} // namespace HtmlUtils

#endif // SF_ALARM_HTML_UTILS_H
