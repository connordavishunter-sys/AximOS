#include <strings.h>
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include "syscall.h"

#define SECONDS_PER_DAY 86400
#define NS_PER_SECOND 1000000000L
#define NS_PER_US 1000L
#define US_PER_SECOND 1000000U

time_t time(time_t *out) {
    time_t now = syscall3(SYS_TIME, 0, 0, 0);

    if (out)
        *out = now;
    return now;
}

static long checked(long result) {
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

int clock_gettime(int clock, struct timespec *out) {
    return checked(syscall3(SYS_CLOCK_GETTIME, clock, (long)out, 0));
}

int gettimeofday(struct timeval *out, void *zone) {
    struct timespec now;

    (void)zone;
    if (clock_gettime(CLOCK_REALTIME, &now) < 0)
        return -1;
    out->tv_sec = now.tv_sec;
    out->tv_usec = now.tv_nsec / NS_PER_US;
    return 0;
}

clock_t clock(void) {
    struct timespec used;

    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &used) < 0)
        return (clock_t)-1;
    return used.tv_sec * CLOCKS_PER_SEC + used.tv_nsec / (NS_PER_SECOND / CLOCKS_PER_SEC);
}

int nanosleep(const struct timespec *request, struct timespec *remaining) {
    return checked(syscall3(SYS_NANOSLEEP, (long)request, (long)remaining, 0));
}

unsigned sleep(unsigned seconds) {
    struct timespec request = {seconds, 0}, remaining = {0, 0};

    if (nanosleep(&request, &remaining) == 0)
        return 0;
    return remaining.tv_sec + (remaining.tv_nsec > 0);
}

int usleep(unsigned microseconds) {
    struct timespec request = {microseconds / US_PER_SECOND,
                               (long)(microseconds % US_PER_SECOND) * NS_PER_US};

    return nanosleep(&request, NULL);
}

static bool is_leap_year(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int days_before_month(int year, int month) {
    static const short days[] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};

    return days[month] + (month > 1 && is_leap_year(year));
}

// Howard Hinnant's civil calendar algorithm, which treats March as the start of
// the year so that leap days come last.
struct tm *gmtime_r(const time_t *when, struct tm *t) {
    int64_t days = *when / SECONDS_PER_DAY;
    int64_t rest = *when % SECONDS_PER_DAY;

    if (rest < 0) {
        rest += SECONDS_PER_DAY;
        days--;
    }

    // 1 January 1970 was a Thursday.
    t->tm_wday = ((days + 4) % 7 + 7) % 7;

    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int day_of_era = days - era * 146097;
    int year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    int day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    int shifted_month = (5 * day_of_year + 2) / 153;
    int month = shifted_month < 10 ? shifted_month + 2 : shifted_month - 10;
    int year = year_of_era + era * 400 + (month <= 1 ? 1 : 0);

    t->tm_mday = day_of_year - (153 * shifted_month + 2) / 5 + 1;
    t->tm_mon = month;
    t->tm_year = year - 1900;
    t->tm_yday = days_before_month(year, month) + t->tm_mday - 1;
    t->tm_hour = rest / 3600;
    t->tm_min = rest % 3600 / 60;
    t->tm_sec = rest % 60;
    return t;
}

struct tm *gmtime(const time_t *when) {
    static struct tm result;

    return gmtime_r(when, &result);
}

// The only time zone is UTC.
struct tm *localtime_r(const time_t *when, struct tm *result) {
    return gmtime_r(when, result);
}

struct tm *localtime(const time_t *when) {
    return gmtime(when);
}

// The other direction of gmtime_r, again by Howard Hinnant. Fields out of range
// carry over, and t ends up normalised.
time_t timegm(struct tm *t) {
    int64_t months = t->tm_year + (int64_t)1900;
    int64_t month = t->tm_mon % 12;

    months += t->tm_mon / 12;
    if (month < 0) {
        month += 12;
        months--;
    }

    int64_t year = month < 2 ? months - 1 : months;
    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t year_of_era = year - era * 400;
    int64_t day_of_year = (153 * (month < 2 ? month + 10 : month - 2) + 2) / 5;
    int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    int64_t days = era * 146097 + day_of_era - 719468 + t->tm_mday - 1;
    time_t when = days * SECONDS_PER_DAY + t->tm_hour * (int64_t)3600 + t->tm_min * (int64_t)60 + t->tm_sec;

    gmtime_r(&when, t);
    return when;
}

time_t mktime(struct tm *t) {
    return timegm(t);
}

double difftime(time_t end, time_t start) {
    return (double)(end - start);
}

static const char *const day_names[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                        "Thursday", "Friday", "Saturday"};
static const char *const month_names[] = {"January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December"};

// Formats that are other formats put together.
static const char *composite(char c) {
    switch (c) {
    case 'c': return "%a %b %e %H:%M:%S %Y";
    case 'D': case 'x': return "%m/%d/%y";
    case 'F': return "%Y-%m-%d";
    case 'r': return "%I:%M:%S %p";
    case 'R': return "%H:%M";
    case 'T': case 'X': return "%H:%M:%S";
    default: return NULL;
    }
}

size_t strftime(char *out, size_t size, const char *fmt, const struct tm *t) {
    size_t used = 0;

    for (; *fmt; fmt++) {
        char text[64];
        const char *inner;
        int hour12 = t->tm_hour % 12 ? t->tm_hour % 12 : 12;

        text[0] = '\0';
        if (*fmt != '%') {
            text[0] = *fmt;
            text[1] = '\0';
        } else if (*++fmt == '\0') {
            break;
        } else if ((inner = composite(*fmt))) {
            strftime(text, sizeof(text), inner, t);
        } else {
            switch (*fmt) {
            case 'a': snprintf(text, sizeof(text), "%.3s", day_names[t->tm_wday]); break;
            case 'A': strcpy(text, day_names[t->tm_wday]); break;
            case 'b': case 'h': snprintf(text, sizeof(text), "%.3s", month_names[t->tm_mon]); break;
            case 'B': strcpy(text, month_names[t->tm_mon]); break;
            case 'C': snprintf(text, sizeof(text), "%02d", (t->tm_year + 1900) / 100); break;
            case 'd': snprintf(text, sizeof(text), "%02d", t->tm_mday); break;
            case 'e': snprintf(text, sizeof(text), "%2d", t->tm_mday); break;
            case 'H': snprintf(text, sizeof(text), "%02d", t->tm_hour); break;
            case 'I': snprintf(text, sizeof(text), "%02d", hour12); break;
            case 'j': snprintf(text, sizeof(text), "%03d", t->tm_yday + 1); break;
            case 'm': snprintf(text, sizeof(text), "%02d", t->tm_mon + 1); break;
            case 'M': snprintf(text, sizeof(text), "%02d", t->tm_min); break;
            case 'n': strcpy(text, "\n"); break;
            case 'p': strcpy(text, t->tm_hour < 12 ? "AM" : "PM"); break;
            case 'S': snprintf(text, sizeof(text), "%02d", t->tm_sec); break;
            case 't': strcpy(text, "\t"); break;
            case 'u': snprintf(text, sizeof(text), "%d", t->tm_wday ? t->tm_wday : 7); break;
            case 'w': snprintf(text, sizeof(text), "%d", t->tm_wday); break;
            case 'y': snprintf(text, sizeof(text), "%02d", ((t->tm_year % 100) + 100) % 100); break;
            case 'Y': snprintf(text, sizeof(text), "%d", t->tm_year + 1900); break;
            case 'z': strcpy(text, "+0000"); break;
            case 'Z': strcpy(text, "UTC"); break;
            case '%': strcpy(text, "%"); break;
            default: snprintf(text, sizeof(text), "%%%c", *fmt); break;
            }
        }

        size_t len = strlen(text);

        // The result is all or nothing, and has to fit with its terminator.
        if (used + len >= size)
            return 0;
        memcpy(out + used, text, len);
        used += len;
    }
    out[used] = '\0';
    return used;
}

char *asctime(const struct tm *t) {
    static char text[64];

    strftime(text, sizeof(text), "%a %b %e %H:%M:%S %Y\n", t);
    return text;
}

char *ctime(const time_t *when) {
    return asctime(gmtime(when));
}

char *tzname[2] = {"UTC", "UTC"};
long timezone;
int daylight;

void tzset(void) {
}

int clock_getres(int clock, struct timespec *out) {
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC && clock != CLOCK_PROCESS_CPUTIME_ID) {
        errno = EINVAL;
        return -1;
    }
    if (out)
        *out = (struct timespec){0, clock == CLOCK_PROCESS_CPUTIME_ID ? 10000000 : 1};
    return 0;
}

char *asctime_r(const struct tm *t, char *out) {
    return strftime(out, 26, "%a %b %e %H:%M:%S %Y\n", t) ? out : NULL;
}

char *ctime_r(const time_t *when, char *out) {
    struct tm t;

    return asctime_r(gmtime_r(when, &t), out);
}

// Reads up to max digits as a number between low and high.
static const char *parse_number(const char *s, int max, int low, int high, int *out) {
    int value = 0;
    int digits = 0;

    while (*s == ' ')
        s++;
    while (digits < max && *s >= '0' && *s <= '9') {
        value = value * 10 + (*s++ - '0');
        digits++;
    }
    if (digits == 0 || value < low || value > high)
        return NULL;
    *out = value;
    return s;
}

// Matches a name from a list of 12 or 7, long or as its first three letters.
static const char *parse_name(const char *s, const char *const *names, int count, int *out) {
    for (int i = 0; i < count; i++) {
        size_t len = strlen(names[i]);

        if (strncasecmp(s, names[i], len) == 0) {
            *out = i;
            return s + len;
        }
        if (strncasecmp(s, names[i], 3) == 0) {
            *out = i;
            return s + 3;
        }
    }
    return NULL;
}

char *strptime(const char *s, const char *fmt, struct tm *t) {
    static const char *const days[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
    static const char *const months[] = {"January", "February", "March", "April", "May", "June", "July",
                                         "August", "September", "October", "November", "December"};
    int value = 0;

    for (; *fmt && s; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*s))
                s++;
            continue;
        }
        if (*fmt != '%') {
            s = *s == *fmt ? s + 1 : NULL;
            continue;
        }

        switch (*++fmt) {
        case 'a': case 'A':
            s = parse_name(s, days, 7, &t->tm_wday);
            break;
        case 'b': case 'B': case 'h':
            s = parse_name(s, months, 12, &t->tm_mon);
            break;
        case 'd': case 'e':
            s = parse_number(s, 2, 1, 31, &t->tm_mday);
            break;
        case 'H':
            s = parse_number(s, 2, 0, 23, &t->tm_hour);
            break;
        case 'I':
            s = parse_number(s, 2, 1, 12, &value);
            t->tm_hour = (t->tm_hour >= 12 ? 12 : 0) + value % 12;
            break;
        case 'j':
            s = parse_number(s, 3, 1, 366, &value);
            t->tm_yday = value - 1;
            break;
        case 'm':
            s = parse_number(s, 2, 1, 12, &value);
            t->tm_mon = value - 1;
            break;
        case 'M':
            s = parse_number(s, 2, 0, 59, &t->tm_min);
            break;
        case 'p':
            if (strncasecmp(s, "AM", 2) == 0)
                t->tm_hour %= 12;
            else if (strncasecmp(s, "PM", 2) == 0)
                t->tm_hour = t->tm_hour % 12 + 12;
            else
                return NULL;
            s += 2;
            break;
        case 'S':
            s = parse_number(s, 2, 0, 60, &t->tm_sec);
            break;
        case 'y':
            s = parse_number(s, 2, 0, 99, &value);
            t->tm_year = value < 69 ? value + 100 : value;
            break;
        case 'Y':
            s = parse_number(s, 4, 0, 9999, &value);
            t->tm_year = value - 1900;
            break;
        case 'n': case 't':
            while (isspace((unsigned char)*s))
                s++;
            break;
        case 'D':
            s = strptime(s, "%m/%d/%y", t);
            break;
        case 'F':
            s = strptime(s, "%Y-%m-%d", t);
            break;
        case 'R':
            s = strptime(s, "%H:%M", t);
            break;
        case 'T':
            s = strptime(s, "%H:%M:%S", t);
            break;
        case '%':
            s = *s == '%' ? s + 1 : NULL;
            break;
        default:
            return NULL;
        }
    }
    return (char *)s;
}
