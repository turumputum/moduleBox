// ***************************************************************************
// TITLE
//
// PROJECT
//     scheduler
// ***************************************************************************

#ifndef SCHEDULE_PARSER_H
#define SCHEDULE_PARSER_H

// ---------------------------------------------------------------------------
// ------------------------------- DEFINITIONS -------------------------------
// -----|-------------------|-------------------------------------------------

#define HOUR_BITS       5
#define MINUTE_BITS     6
#define SECOND_BITS     6

#define DAY_BITS        5
#define MONTH_BITS      4
#define YEAR_BITS       7

#define ANY_HOUR        ((1 << HOUR_BITS) - 1)
#define ANY_MINUTE      ((1 << MINUTE_BITS) - 1)
#define ANY_SECOND      ((1 << SECOND_BITS) - 1)
#define ANY_DAY         0
#define ANY_MONTH       0
#define ANY_YEAR        0 


#define MAX_LINE_LENGTH 512
#define MAX_KEY_LENGTH 256
#define MAX_VALUE_LENGTH 256
#define MAX_COMMAND_LENGTH 32
#define MAX_SCHEDULE_ENTRIES 10
#define MAX_DAYS 7

// ---------------------------------------------------------------------------
// ---------------------------------- TYPES ----------------------------------
// -|-----------------------|-------------------------------------------------

// Day of week enumeration
typedef enum {
    DAY_SUNDAY = 0,
    DAY_MONDAY,
    DAY_TUESDAY,
    DAY_WEDNESDAY,
    DAY_THURSDAY,
    DAY_FRIDAY,
    DAY_SATURDAY
} day_of_week_t;

// Schedule time structure
typedef struct {
    // Time components
    unsigned hour_interval  : 1;            // 0 means no interval
    unsigned hour           : HOUR_BITS;    // ANY_HOUR means wildcard (*), 0-23
    unsigned minute_interval: 1;            // 0 means no interval
    unsigned minute         : MINUTE_BITS;  // ANY_MINUTE means wildcard (*), 0-59
    unsigned second_interval: 1;            // 0 means no interval
    unsigned second         : SECOND_BITS;  // ANY_SECOND means wildcard (*), 0-59

    // Date components
    unsigned day_interval   : 1;            // 0 means no interval
    unsigned day            : DAY_BITS;    // ANY_DAY means wildcard (*)
    unsigned month_interval : 1;            // 0 means no interval
    unsigned month          : MONTH_BITS;  // ANY_MONTH means wildcard (*)
    unsigned year_interval  : 1;            // 0 means no interval
    unsigned year           : YEAR_BITS;   // ANY_YEAR means wildcard (*)

    // Day of week (bitmask: 0x01=Sun, 0x02=Mon, ..., 0x40=Sat)
    unsigned day_of_week_mask;  // 0 means any day
   
} schedule_time_t;

// Schedule entry structure
typedef struct {
    schedule_time_t time;
    char command[MAX_COMMAND_LENGTH];
} schedule_entry_t;


// ---------------------------------------------------------------------------
// -------------------------------- FUNCTIONS --------------------------------
// -----------------|---------------------------(|------------------|---------

// Parse schedule time string
int parse_schedule_time(const char *time_str, schedule_time_t *time);

// Comparison function
int matches_schedule_time(const schedule_time_t *schedule, int year, int month, int day, 
                         int hour, int minute, int second, int day_of_week);

#endif // SCHEDULE_PARSER_H

