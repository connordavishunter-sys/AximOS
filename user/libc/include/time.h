#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time_types.h>

#define CLOCKS_PER_SEC 1000000L

typedef long time_t;
typedef long clock_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;  // 0 to 11
    int tm_year; // years since 1900
    int tm_wday; // 0 is Sunday
    int tm_yday; // 0 is 1 January
    int tm_isdst; // whether it's summer time, or for mktime negative if unknown
    long tm_gmtoff; // seconds east of UTC
    const char *tm_zone; // the time zone's abbreviation, such as GMT or BST
};

// Seconds since 1970 in UTC.
time_t time(time_t *out);

// CLOCK_MONOTONIC counts from boot and suits measuring and pacing. CLOCK_REALTIME
// is the date, and can be up to a second from time() since the real-time clock
// only has whole seconds to start from. CLOCK_PROCESS_CPUTIME_ID is time spent
// running in ring 3, in steps of 10 ms.
int clock_gettime(int clock, struct timespec *out);

// The clock can't be set yet, so this fails with EPERM.
int clock_settime(int clock, const struct timespec *value);

// The caller's processor time, in units of CLOCKS_PER_SEC.
clock_t clock(void);

// Sleeps for at least the given time, usually waking within 10 ms after it. A
// signal ends it with EINTR, leaving what was left in remaining, which may be NULL.
int nanosleep(const struct timespec *request, struct timespec *remaining);

// Returns result. Local time is in the zone TZ names, which is a name such as
// Europe/London from /system/share/zoneinfo, a file after a colon, or a POSIX rule
// such as EST5EDT,M3.2.0,M11.1.0. Without TZ, /system/etc/localtime is the zone,
// as the tz command sets it, and without that it's UTC.
struct tm *gmtime_r(const time_t *when, struct tm *result);
struct tm *localtime_r(const time_t *when, struct tm *result);

// These return one shared result, replaced by the next call.
struct tm *gmtime(const time_t *when);
struct tm *localtime(const time_t *when);
char *asctime(const struct tm *t);
char *ctime(const time_t *when);

// Turn a broken-down time into seconds, carrying any fields that are out of range
// and normalising t.
time_t timegm(struct tm *t);
time_t mktime(struct tm *t);

double difftime(time_t end, time_t start);

// The zone's standard and summer time abbreviations, how far west of UTC its
// standard time is, and whether it has summer time. tzset sets them, and the local
// time functions call it, rereading the zone when TZ or its file has changed.
extern char *tzname[2];
extern long timezone;
extern int daylight;
void tzset(void);

int clock_getres(int clock, struct timespec *out);

// out must have room for 26 bytes.
char *asctime_r(const struct tm *t, char *out);
char *ctime_r(const time_t *when, char *out);

// Reads what strftime writes, for %a %A %b %B %d %D %e %F %h %H %I %j %m %M %n %p %R
// %S %t %T %y %Y %%, returning where it stopped, or NULL if the text doesn't match.
char *strptime(const char *s, const char *fmt, struct tm *t);

// Returns the length written, or 0 if it didn't fit in size bytes. Supports
// %a %A %b %B %c %C %d %D %e %F %h %H %I %j %m %M %n %p %r %R %S %t %T %u %w %x %X
// %y %Y %z %Z %%.
size_t strftime(char *out, size_t size, const char *fmt, const struct tm *t);
