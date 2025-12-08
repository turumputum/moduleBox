// ***************************************************************************
// TITLE
//
// PROJECT
//     scheduler
// ***************************************************************************

#define _POSIX_C_SOURCE 200809L
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "schedule_parser.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdbool.h>

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------


// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------


// Token type enumeration
typedef enum {
    TOKEN_TYPE_DAY_NAME,
    TOKEN_TYPE_DATE,
    TOKEN_TYPE_TIME
} token_type_t;

// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------


// Parse day of week string to enum
int parse_day_of_week(const char *day_str) {
    if (strcasecmp(day_str, "Sun") == 0 || strcasecmp(day_str, "Sunday") == 0)
        return DAY_SUNDAY;
    if (strcasecmp(day_str, "Mon") == 0 || strcasecmp(day_str, "Monday") == 0)
        return DAY_MONDAY;
    if (strcasecmp(day_str, "Tue") == 0 || strcasecmp(day_str, "Tuesday") == 0)
        return DAY_TUESDAY;
    if (strcasecmp(day_str, "Wed") == 0 || strcasecmp(day_str, "Wednesday") == 0)
        return DAY_WEDNESDAY;
    if (strcasecmp(day_str, "Thu") == 0 || strcasecmp(day_str, "Thursday") == 0)
        return DAY_THURSDAY;
    if (strcasecmp(day_str, "Fri") == 0 || strcasecmp(day_str, "Friday") == 0)
        return DAY_FRIDAY;
    if (strcasecmp(day_str, "Sat") == 0 || strcasecmp(day_str, "Saturday") == 0)
        return DAY_SATURDAY;
    return -1;
}

// Trim whitespace from both ends of a string
static void trim(char *str) {
    char *start = str;
    char *end;
    
    // Trim leading whitespace
    while (isspace((unsigned char)*start)) {
        start++;
    }
    
    if (*start == 0) {
        *str = 0;
        return;
    }
    
    // Trim trailing whitespace
    end = start + strlen(start) - 1;
    while (end > start && isspace((unsigned char)*end)) {
        end--;
    }
    
    end[1] = '\0';
    
    // Move trimmed string to start
    memmove(str, start, end - start + 2);
}


// Detect token type
token_type_t detect_token_type(const char *token) {
    if (!token || strlen(token) == 0) {
        return TOKEN_TYPE_TIME; // Default
    }
    
    // Check if it's a day name (starts with letter)
    if (isalpha((unsigned)token[0])) {
        return TOKEN_TYPE_DAY_NAME;
    }
    
    // Check if it's a date (starts with digit and contains '-')
    if (isdigit((unsigned)token[0]) && strchr(token, '-') != NULL) {
        return TOKEN_TYPE_DATE;
    }
    
    // Otherwise it's a time pattern
    return TOKEN_TYPE_TIME;
}

// Parse day name token
int parse_day_name_token(const char *token, schedule_time_t *time) {
    char day_token[64];
    strncpy(day_token, token, sizeof(day_token) - 1);
    day_token[sizeof(day_token) - 1] = '\0';
    trim(day_token);
    
    int day = parse_day_of_week(day_token);
    if (day >= 0) {
        time->day_of_week_mask |= (1 << day);
        return 0;
    }
    return -1;
}

// Parse date token (DD-MM or DD-MM-YYYY)
int parse_date_token(const char *token, schedule_time_t *time) {
    char *day_part = NULL;
    char *month_part = NULL;
    char *year_part = NULL;
    char date_part[64];
    strncpy(date_part, token, sizeof(date_part) - 1);
    date_part[sizeof(date_part) - 1] = '\0';
    
    // Split by dash to get day, month, and year parts
    // Date token must always contain at least one dash
    char *dash1 = strchr(date_part, '-');
    if (!dash1) {
        return -1;  // Invalid: date token must contain at least one dash
    }
    
    // First dash found: split day and month/year
    *dash1 = '\0';
    day_part = date_part;
    month_part = dash1 + 1;
    
    // Check for second dash (year)
    char *dash2 = strchr(month_part, '-');
    if (dash2) {
        *dash2 = '\0';
        year_part = dash2 + 1;
    }
    
    // Process day part: asterisk prefix means interval flag
    if (day_part) {
        if (day_part[0] == '*' && day_part[1] != '\0') {
            if (isdigit((unsigned char)day_part[1])) {
                // Day interval: *N means every N days
                time->day_interval = 1;  // Set interval flag
                time->day = atoi(day_part + 1);
            } else {
                // Just * means any day (wildcard)
                time->day_interval = 0;  // No interval
                time->day = ANY_DAY;
            }
        } else if (day_part[0] == '*') {
            // Just * means any day (wildcard)
            time->day_interval = 0;  // No interval
            time->day = ANY_DAY;
        } else {
            // Specific day value
            time->day_interval = 0;  // No interval
            time->day = atoi(day_part);
        }
    } else {
        time->day_interval = 0;  // No interval
        time->day = ANY_DAY;
    }
    
    // Process month part: asterisk prefix means interval flag
    if (month_part) {
        if (month_part[0] == '*' && month_part[1] != '\0') {
            if (isdigit((unsigned char)month_part[1])) {
                // Month interval: *N means every N months
                time->month_interval = 1;  // Set interval flag
                time->month = atoi(month_part + 1);
            } else {
                // Just * means any month (wildcard)
                time->month_interval = 0;  // No interval
                time->month = ANY_MONTH;
            }
        } else if (month_part[0] == '*') {
            // Just * means any month (wildcard)
            time->month_interval = 0;  // No interval
            time->month = ANY_MONTH;
        } else {
            // Specific month value
            time->month_interval = 0;  // No interval
            time->month = atoi(month_part);
        }
    } else {
        time->month_interval = 0;  // No interval
        time->month = ANY_MONTH;
    }

    // Process year part: asterisk prefix means interval flag
    if (year_part) {
        if (year_part[0] == '*' && year_part[1] != '\0') {
            if (isdigit((unsigned char)year_part[1])) {
                // Year interval: *N means every N years
                time->year_interval = 1;  // Set interval flag
                int year = atoi(year_part + 1);
                time->year = year >= 2000 ? year - 2000 : year;
            } else {
                // Just * means any year (wildcard)
                time->year_interval = 0;  // No interval
                time->year = ANY_YEAR;
            }
        } else if (year_part[0] == '*') {
            // Just * means any year (wildcard)
            time->year_interval = 0;  // No interval
            time->year = ANY_YEAR;
        } else {
            // Specific year value
            time->year_interval = 0;  // No interval
            int year = atoi(year_part);
            time->year = year >= 2000 ? year - 2000 : year;
        }
    } else {
        time->year_interval = 0;  // No interval
        time->year = ANY_YEAR;
    }
    
    return 0;
}

// Parse time token (various formats)
int parse_time_token(const char *token, schedule_time_t *time) {
    char *hour_part = NULL;
    char *minute_part = NULL;
    char *second_part = NULL;
    char time_part[64];
    strncpy(time_part, token, sizeof(time_part) - 1);
    time_part[sizeof(time_part) - 1] = '\0';
    
    // Split by colon to get hours, minutes, and seconds parts
    char *colon1 = strchr(time_part, ':');

    if (!colon1) 
    {
        // Colon not found: process both hours and minutes
        hour_part = NULL;
        minute_part = time_part;
        second_part = NULL;
    }
    else
    {
        // First colon found: process hours and minutes/seconds
        *colon1 = '\0';
        hour_part = time_part;
        minute_part = colon1 + 1;
        
        // Check for second colon (seconds)
        char *colon2 = strchr(minute_part, ':');
        if (colon2) {
            *colon2 = '\0';
            second_part = colon2 + 1;
        } else {
            second_part = NULL;
        }
    }

    // Process hours part: asterisk prefix means interval flag
    if (hour_part)
    {
        if (hour_part[0] == '*' && hour_part[1] != '\0')
        {
            if (isdigit((unsigned char)hour_part[1])) {
                // Hour interval: *N means every N hours
                time->hour_interval = 1;  // Set interval flag
                time->hour = atoi(hour_part + 1);
            } else {
                // Just * means any hour (wildcard)
                time->hour_interval = 0;  // No interval
                time->hour = ANY_HOUR;
            }
        } else if (hour_part[0] == '*') {
            // Just * means any hour (wildcard)
            time->hour_interval = 0;  // No interval
            time->hour = ANY_HOUR;
        } else {
            // Specific hour value
            time->hour_interval = 0;  // No interval
            time->hour = atoi(hour_part);
        }
    }
    else
    {
        time->hour_interval = 0;  // No interval
        time->hour = ANY_HOUR;
    }
    
    // Process minutes part: asterisk prefix means interval flag
    if (minute_part && minute_part[0] != '\0') {
        if (minute_part[0] == '*' && minute_part[1] != '\0') {
            if (isdigit((unsigned char)minute_part[1])) {
                // Minute interval: *N means every N minutes
                time->minute_interval = 1;  // Set interval flag
                time->minute = atoi(minute_part + 1);
            } else {
                // Just * means any minute (wildcard)
                time->minute_interval = 0;  // No interval
                time->minute = ANY_MINUTE;
            }
        } else if (minute_part[0] == '*') {
            // Just * means any minute (wildcard)
            time->minute_interval = 0;  // No interval
            time->minute = ANY_MINUTE;
        } else {
            // Specific minute value
            time->minute_interval = 0;  // No interval
            time->minute = atoi(minute_part);
        }
    } else {
        time->minute_interval = 0;  // No interval
        time->minute = ANY_MINUTE;
    }
    
    // Process seconds part: asterisk prefix means interval flag
    if (second_part && second_part[0] != '\0') {
        if (second_part[0] == '*' && second_part[1] != '\0') {
            if (isdigit((unsigned char)second_part[1])) {
                // Second interval: *N means every N seconds
                time->second_interval = 1;  // Set interval flag
                time->second = atoi(second_part + 1);
            } else {
                // Just * means any second (wildcard)
                time->second_interval = 0;  // No interval
                time->second = ANY_SECOND;
            }
        } else if (second_part[0] == '*') {
            // Just * means any second (wildcard)
            time->second_interval = 0;  // No interval
            time->second = ANY_SECOND;
        } else {
            // Specific second value
            time->second_interval = 0;  // No interval
            time->second = atoi(second_part);
        }
    } else {
        time->second_interval = 0;  // No interval
        time->second = 0;
    }
    
    return 0;
}

// Parse schedule time string
int parse_schedule_time(const char *time_str, schedule_time_t *time) {
    char *str = strdup(time_str);
    char *saveptr;
    
    // Initialize time structure
    memset(time, 0, sizeof(schedule_time_t));
    time->hour = ANY_HOUR;
    time->minute = ANY_MINUTE;
    time->second = ANY_SECOND;
    time->day = ANY_DAY;
    time->month = ANY_MONTH;
    time->year = ANY_YEAR;
    time->day_of_week_mask = 0;
    time->hour_interval = 0;
    time->minute_interval = 0;
    time->second_interval = 0;
    time->day_interval = 0;
    time->month_interval = 0;
    time->year_interval = 0;
    
    trim(str);
    
    // Split by spaces to get tokens
    char *tokens[10];
    int token_count = 0;
    char *token = strtok_r(str, " ", &saveptr);
    while (token && token_count < 10) {
        tokens[token_count++] = token;
        token = strtok_r(NULL, " ", &saveptr);
    }
    
    if (token_count == 0) {
        free(str);
        return -1;
    }
    
    // Single loop: check each token type and parse accordingly
    bool found_time = false;
    for (int i = 0; i < token_count; i++) {
        token_type_t type = detect_token_type(tokens[i]);
        
        switch (type) {
            case TOKEN_TYPE_DAY_NAME:
                // Parse day name token
                if (parse_day_name_token(tokens[i], time) != 0) {
                    // Not a valid day name, treat as time
                    if (parse_time_token(tokens[i], time) == 0) {
                        found_time = true;
                    }
                }
                break;
                
            case TOKEN_TYPE_DATE:
                // Parse date token
                if (parse_date_token(tokens[i], time) != 0) {
                    free(str);
                    return -1;
                }
                break;
                
            case TOKEN_TYPE_TIME:
                // Parse time token (must be the last token)
                if (parse_time_token(tokens[i], time) == 0) {
                    found_time = true;
                } else {
                    free(str);
                    return -1;
                }
                break;
        }
        
        // Time token should be the last one, stop after finding it
        if (found_time) {
            break;
        }
    }
    
    // Must have at least a time token
    if (!found_time) {
        free(str);
        return -1;
    }
    
    free(str);
    return 0;
}

// Check if a given date/time matches a schedule
int matches_schedule_time(const schedule_time_t *schedule, int year, int month, int day, 
                         int hour, int minute, int second, int day_of_week) {
    // Check day of week mask (if set)
    if (schedule->day_of_week_mask != 0) {
        if ((schedule->day_of_week_mask & (1 << day_of_week)) == 0) {
            return 0;  // Day of week doesn't match
        }
    }

    // Check year (stored as offset from 2000)
    if (schedule->year_interval > 0) {
        // Year interval: every N years (stored year is the interval value)
        unsigned interval_years = schedule->year;
        // Check if year matches the interval pattern starting from 2000
        if (year < 2000 || (year - 2000) % interval_years != 0) {
            return 0;  // Year doesn't match interval
        }
    } else if (schedule->year != ANY_YEAR) {
        // Stored year is offset from 2000
        unsigned stored_year = schedule->year + 2000;
        if (year != (int)stored_year) {
            return 0;  // Year doesn't match
        }
    }
    
    // Check month
    if (schedule->month_interval > 0) {
        // Month interval: every N months (simplified - assumes starting from month 1)
        if ((month - 1) % schedule->month != 0) {
            return 0;  // Month doesn't match interval
        }
    } else if (schedule->month != ANY_MONTH) {
        if (month != schedule->month) {
            return 0;  // Month doesn't match
        }
    }
    
    // Check day
    if (schedule->day_interval > 0) {
        // Day interval: every N days (simplified - assumes starting from day 1)
        if ((day - 1) % schedule->day != 0) {
            return 0;  // Day doesn't match interval
        }
    } else {
        // No interval - check for exact match or wildcard
        if (schedule->day != ANY_DAY) {
            if (day != (int)schedule->day) {
                return 0;  // Day doesn't match

            }
        }
        // If schedule->day == ANY_DAY, any day matches (no check needed)
    }
    
    // Check hour
    if (schedule->hour_interval > 0) {
        // Hour interval: every N hours
        if (hour % schedule->hour != 0) {
            return 0;  // Hour doesn't match interval
        }
    } else if (schedule->hour != ANY_HOUR) {
        if (hour != schedule->hour) {
            return 0;  // Hour doesn't match
        }
    }
    
    // Check minute
    if (schedule->minute_interval > 0) {
        // Minute interval: every N minutes
        if (minute % schedule->minute != 0) {
            return 0;  // Minute doesn't match interval
        }
    } else if (schedule->minute != ANY_MINUTE) {
        if (minute != schedule->minute) {
            return 0;  // Minute doesn't match
        }
    }
    
    // Check second
    if (schedule->second_interval > 0) {
        // Second interval: every N seconds
        if (second % schedule->second != 0) {
            return 0;  // Second doesn't match interval
        }
    } else if (schedule->second != ANY_SECOND) {
        if (second != schedule->second) {
            return 0;  // Second doesn't match
        }
    }
    
    return 1;  // All checks passed
}

