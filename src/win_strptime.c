#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#include "../include/win_strptime.h"

// Windows does not have gmtime_r & localtime_r and completely lacks strptime.
#if defined(_MSC_VER)
static inline struct tm* gmtime_r(const time_t* timer, struct tm* buf) {
    return (gmtime_s(buf, timer) == 0) ? buf : NULL;
}

static inline struct tm* localtime_r(const time_t* timer, struct tm* buf) {
    return (localtime_s(buf, timer) == 0) ? buf : NULL;
}

/**
 * Helper function to check if a year is a leap year
 */
static inline bool is_leap_year(int year) {
    // year is tm_year (years since 1900)
    int actual_year = year + 1900;
    return (actual_year % 4 == 0 && actual_year % 100 != 0) || (actual_year % 400 == 0);
}

/**
 * Helper function to get the number of days in a month
 */
static inline int days_in_month(int month, int year) {
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 0 || month > 11) {
        return 0;
    }
    if (month == 1 && is_leap_year(year)) {  // February in leap year
        return 29;
    }
    return days[month];
}

/**
 * Helper function to validate a date
 */
static inline bool is_valid_date(int day, int month, int year) {
    if (month < 0 || month > 11) {
        return false;
    }
    if (day < 1) {
        return false;
    }
    return day <= days_in_month(month, year);
}

/**
 * Windows implementation of strptime using manual parsing.
 * Supports common format specifiers needed by xtime.
 * 
 * POSIX compliance improvements:
 * - Proper whitespace handling per POSIX spec
 * - Correct field initialization
 * - Better error handling
 * - Date validation (rejects invalid dates like Feb 30)
 * - Support for additional format specifiers
 */
static char* strptime(const char* buf, const char* fmt, struct tm* tm) {
    if (buf == NULL || fmt == NULL || tm == NULL) {
        return NULL;
    }

    const char* s = buf;
    const char* f = fmt;
    bool is_pm = false;
    bool has_ampm = false;
    int century = -1;  // For %C handling
    bool need_date_validation = false;

    // Initialize tm structure to safe defaults (POSIX requirement)
    // Don't zero everything - preserve what caller may have set
    // But ensure fields we might not set have safe values
    if (tm->tm_wday == 0 && tm->tm_yday == 0 && tm->tm_isdst == 0) {
        tm->tm_isdst = -1;  // Unknown DST status
    }

    while (*f != '\0') {
        // Handle whitespace: any whitespace in format matches zero or more in input
        if (isspace((unsigned char)*f)) {
            while (isspace((unsigned char)*f)) f++;
            while (isspace((unsigned char)*s)) s++;
            continue;
        }

        if (*f != '%') {
            // Literal character match (case-sensitive)
            if (*f != *s) {
                return NULL;
            }
            f++;
            s++;
            continue;
        }

        // Format specifier
        f++;  // Skip '%'

        if (*f == '\0') {
            return NULL;  // Trailing % is invalid
        }

        // Handle modifier flags (E and O)
        bool has_modifier = false;
        if (*f == 'E' || *f == 'O') {
            has_modifier = true;
            f++;  // Skip modifier (we'll ignore it for basic implementation)
            if (*f == '\0') {
                return NULL;
            }
        }

        switch (*f) {
            case 'Y': {  // 4-digit year
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) || !isdigit((unsigned char)s[2]) ||
                    !isdigit((unsigned char)s[3])) {
                    return NULL;
                }
                int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + (s[2] - '0') * 10 + (s[3] - '0');
                tm->tm_year = year - 1900;
                s += 4;
                break;
            }

            case 'y': {  // 2-digit year (00-99)
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
                    return NULL;
                }
                int year = (s[0] - '0') * 10 + (s[1] - '0');
                // POSIX: 69-99 -> 1969-1999, 00-68 -> 2000-2068
                if (century >= 0) {
                    tm->tm_year = century * 100 + year - 1900;
                } else {
                    tm->tm_year = (year >= 69) ? year : (year + 100);
                }
                s += 2;
                break;
            }

            case 'C': {  // Century (00-99)
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
                    return NULL;
                }
                century = (s[0] - '0') * 10 + (s[1] - '0');
                s += 2;
                break;
            }

            case 'm': {  // Month (01-12) - zero-padded
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
                    return NULL;
                }
                int month = (s[0] - '0') * 10 + (s[1] - '0');
                if (month < 1 || month > 12) {
                    return NULL;
                }
                tm->tm_mon = month - 1;
                need_date_validation = true;
                s += 2;
                break;
            }

            case 'd': {  // Day of month (01-31) - zero-padded
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
                    return NULL;
                }
                int day = (s[0] - '0') * 10 + (s[1] - '0');
                if (day < 1 || day > 31) {
                    return NULL;
                }
                tm->tm_mday = day;
                need_date_validation = true;
                s += 2;
                break;
            }

            case 'e': {  // Day of month (1-31, space-padded)
                // Skip leading whitespace/zero
                if (*s == ' ' || *s == '0') {
                    s++;
                }
                if (!isdigit((unsigned char)s[0])) {
                    return NULL;
                }
                int day = (s[0] - '0');
                s++;
                if (isdigit((unsigned char)s[0])) {
                    day = day * 10 + (s[0] - '0');
                    s++;
                }
                if (day < 1 || day > 31) {
                    return NULL;
                }
                tm->tm_mday = day;
                need_date_validation = true;
                break;
            }

            case 'H': {  // Hour (00-23) - zero-padded
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
                    return NULL;
                }
                int hour = (s[0] - '0') * 10 + (s[1] - '0');
                if (hour > 23) {
                    return NULL;
                }
                tm->tm_hour = hour;
                s += 2;
                break;
            }

            case 'k': {  // Hour (0-23) - space-padded
                if (*s == ' ') {
                    s++;
                }
                if (!isdigit((unsigned char)s[0])) {
                    return NULL;
                }
                int hour = (s[0] - '0');
                s++;
                if (isdigit((unsigned char)s[0])) {
                    hour = hour * 10 + (s[0] - '0');
                    s++;
                }
                if (hour > 23) {
                    return NULL;
                }
                tm->tm_hour = hour;
                break;
            }

            case 'I': {  // Hour (01-12) - zero-padded
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
                    return NULL;
                }
                int hour = (s[0] - '0') * 10 + (s[1] - '0');
                if (hour < 1 || hour > 12) {
                    return NULL;
                }
                tm->tm_hour = hour;
                s += 2;
                break;
            }

            case 'l': {  // Hour (1-12) - space-padded
                if (*s == ' ') {
                    s++;
                }
                if (!isdigit((unsigned char)s[0])) {
                    return NULL;
                }
                int hour = (s[0] - '0');
                s++;
                if (isdigit((unsigned char)s[0])) {
                    hour = hour * 10 + (s[0] - '0');
                    s++;
                }
                if (hour < 1 || hour > 12) {
                    return NULL;
                }
                tm->tm_hour = hour;
                break;
            }

            case 'M': {  // Minute (00-59) - zero-padded
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
                    return NULL;
                }
                int min = (s[0] - '0') * 10 + (s[1] - '0');
                if (min > 59) {
                    return NULL;
                }
                tm->tm_min = min;
                s += 2;
                break;
            }

            case 'S': {  // Second (00-60, allowing leap second) - zero-padded
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1])) {
                    return NULL;
                }
                int sec = (s[0] - '0') * 10 + (s[1] - '0');
                if (sec > 60) {  // Allow 60 for leap seconds
                    return NULL;
                }
                tm->tm_sec = sec;
                s += 2;
                break;
            }

            case 'p': {  // AM/PM (case-insensitive per POSIX)
                if ((s[0] == 'A' || s[0] == 'a') && (s[1] == 'M' || s[1] == 'm')) {
                    is_pm = false;
                    has_ampm = true;
                    s += 2;
                } else if ((s[0] == 'P' || s[0] == 'p') && (s[1] == 'M' || s[1] == 'm')) {
                    is_pm = true;
                    has_ampm = true;
                    s += 2;
                } else {
                    return NULL;
                }
                break;
            }

            case 'r': {  // 12-hour time with AM/PM (%I:%M:%S %p)
                char* result = strptime(s, "%I:%M:%S %p", tm);
                if (result == NULL) {
                    return NULL;
                }
                s = result;
                has_ampm = true;  // Mark that AM/PM was handled
                break;
            }

            case 'R': {  // Time in HH:MM format
                char* result = strptime(s, "%H:%M", tm);
                if (result == NULL) {
                    return NULL;
                }
                s = result;
                break;
            }

            case 'T': {  // Time in HH:MM:SS format
                char* result = strptime(s, "%H:%M:%S", tm);
                if (result == NULL) {
                    return NULL;
                }
                s = result;
                break;
            }

            case 'D': {  // Date in MM/DD/YY format
                char* result = strptime(s, "%m/%d/%y", tm);
                if (result == NULL) {
                    return NULL;
                }
                s = result;
                break;
            }

            case 'F': {  // Date in YYYY-MM-DD format (ISO 8601)
                char* result = strptime(s, "%Y-%m-%d", tm);
                if (result == NULL) {
                    return NULL;
                }
                s = result;
                break;
            }

            case 'b':    // Abbreviated month name
            case 'h':    // Same as %b
            case 'B': {  // Full month name
                static const char* months[] = {"january",
                                               "february",
                                               "march",
                                               "april",
                                               "may",
                                               "june",
                                               "july",
                                               "august",
                                               "september",
                                               "october",
                                               "november",
                                               "december"};
                static const char* abbr_months[] = {
                  "jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};

                bool found = false;
                // Try full names first, then abbreviations
                for (int i = 0; i < 12; i++) {
                    const char* full = months[i];
                    const char* abbr = abbr_months[i];
                    size_t full_len = strlen(full);
                    size_t abbr_len = 3;

                    // For %B, match full name; for %b/%h, match abbreviation
                    if (*f == 'B') {
                        if (_strnicmp(s, full, full_len) == 0) {
                            tm->tm_mon = i;
                            s += full_len;
                            found = true;
                            break;
                        }
                    } else {
                        if (_strnicmp(s, abbr, abbr_len) == 0) {
                            tm->tm_mon = i;
                            s += abbr_len;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    return NULL;
                }
                need_date_validation = true;
                break;
            }

            case 'a':    // Abbreviated weekday name
            case 'A': {  // Full weekday name
                static const char* weekdays[] = {
                  "sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
                static const char* abbr_weekdays[] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};

                bool found = false;
                for (int i = 0; i < 7; i++) {
                    const char* full = weekdays[i];
                    const char* abbr = abbr_weekdays[i];
                    size_t full_len = strlen(full);
                    size_t abbr_len = 3;

                    if (*f == 'A') {
                        if (_strnicmp(s, full, full_len) == 0) {
                            tm->tm_wday = i;
                            s += full_len;
                            found = true;
                            break;
                        }
                    } else {
                        if (_strnicmp(s, abbr, abbr_len) == 0) {
                            tm->tm_wday = i;
                            s += abbr_len;
                            found = true;
                            break;
                        }
                    }
                }
                if (!found) {
                    return NULL;
                }
                break;
            }

            case 'j': {  // Day of year (001-366) - zero-padded, 3 digits
                if (!isdigit((unsigned char)s[0]) || !isdigit((unsigned char)s[1]) || !isdigit((unsigned char)s[2])) {
                    return NULL;
                }
                int yday = (s[0] - '0') * 100 + (s[1] - '0') * 10 + (s[2] - '0');
                if (yday < 1 || yday > 366) {
                    return NULL;
                }
                tm->tm_yday = yday - 1;
                s += 3;
                break;
            }

            case 'u': {  // Weekday (1-7, Monday=1)
                if (!isdigit((unsigned char)s[0])) {
                    return NULL;
                }
                int wday = s[0] - '0';
                if (wday < 1 || wday > 7) {
                    return NULL;
                }
                tm->tm_wday = (wday == 7) ? 0 : wday;  // Convert to 0-6 (Sunday=0)
                s++;
                break;
            }

            case 'w': {  // Weekday (0-6, Sunday=0)
                if (!isdigit((unsigned char)s[0])) {
                    return NULL;
                }
                int wday = s[0] - '0';
                if (wday > 6) {
                    return NULL;
                }
                tm->tm_wday = wday;
                s++;
                break;
            }

            case 'z': {  // Timezone offset (+hhmm or -hhmm or +hh:mm or -hh:mm)
                // Consume the timezone but don't parse it into tm
                // The caller (xtime_parse) should handle this separately
                if (*s == '+' || *s == '-') {
                    s++;  // Skip sign
                    // Consume hours (2 digits)
                    if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1])) {
                        s += 2;
                    } else {
                        return NULL;
                    }
                    // Optional colon
                    if (*s == ':') {
                        s++;
                    }
                    // Consume minutes (2 digits)
                    if (isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1])) {
                        s += 2;
                    } else {
                        return NULL;
                    }
                } else if (*s == 'Z' || *s == 'z') {
                    s++;  // UTC designator
                } else {
                    return NULL;
                }
                break;
            }

            case 'Z': {  // Timezone name - variable length
                // Skip timezone name/abbreviation
                while (*s != '\0' && !isspace((unsigned char)*s) && *s != '+' && *s != '-' &&
                       isalpha((unsigned char)*s)) {
                    s++;
                }
                if (s == buf) {
                    return NULL;  // No timezone found
                }
                break;
            }

            case 'n':  // Any whitespace
            case 't': {
                // Match one or more whitespace characters
                if (!isspace((unsigned char)*s)) {
                    return NULL;
                }
                while (isspace((unsigned char)*s)) {
                    s++;
                }
                break;
            }

            case '%': {  // Literal '%'
                if (*s != '%') {
                    return NULL;
                }
                s++;
                break;
            }

            default:
                // Unsupported/unknown format specifier
                return NULL;
        }

        f++;
    }

    // Apply AM/PM adjustment if needed (deferred from %I parsing)
    if (has_ampm) {
        if (is_pm && tm->tm_hour < 12) {
            tm->tm_hour += 12;
        } else if (!is_pm && tm->tm_hour == 12) {
            tm->tm_hour = 0;
        }
    }

    // Validate the date if month and day were parsed
    if (need_date_validation && !is_valid_date(tm->tm_mday, tm->tm_mon, tm->tm_year)) {
        return NULL;  // Invalid date (e.g., Feb 30, Apr 31, etc.)
    }

    // Ensure DST flag is set to unknown if not explicitly set
    if (tm->tm_isdst == 0) {
        tm->tm_isdst = -1;
    }

    return (char*)s;
}
#endif
