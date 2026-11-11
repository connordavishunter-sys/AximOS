#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Local time comes from TZ, which is a zone's name, a path after a colon, or a
// POSIX rule such as "EST5EDT,M3.2.0,M11.1.0". Without it, /system/etc/localtime
// says, and without that it's UTC. Zone files are the usual TZif ones: a table of
// past changes, and a POSIX rule for what comes after it.

#define ZONE_DIR "/system/share/zoneinfo"
#define LOCALTIME "/system/etc/localtime"
#define MAX_TYPES 256
#define MAX_ZONE_FILE (256 * 1024)
#define MAX_ABBREVIATIONS 128
#define SECONDS_PER_DAY 86400

struct local_type {
    int32_t offset; // seconds east of UTC
    bool dst;
    const char *abbreviation;
};

// A date in a POSIX rule: Mm.w.d is day d of week w of month m, with week 5
// meaning the last; Jn is day n counting from 1 and never 29 February; and plain
// n is day n counting from 0. time is when, in the local time then in force.
struct rule_date {
    char kind;
    int month, week, day;
    int32_t time;
};

struct rule {
    struct local_type standard, summer;
    bool has_summer;
    struct rule_date start, end;
};

static struct {
    char key[PATH_MAX + 64]; // what was loaded, to know when it changes
    int64_t *times;
    uint8_t *indexes;
    int count;
    struct local_type types[MAX_TYPES];
    int type_count;
    struct rule rule;
    bool has_rule;
} zone;

char *tzname[2] = {"UTC", "UTC"};
long timezone;
int daylight;

// Abbreviations are kept for good, as struct tm points at them.
static const char *intern(const char *s, size_t len) {
    static char *kept[MAX_ABBREVIATIONS];
    static int count;

    for (int i = 0; i < count; i++) {
        if (strlen(kept[i]) == len && memcmp(kept[i], s, len) == 0)
            return kept[i];
    }
    if (count == MAX_ABBREVIATIONS || !(kept[count] = malloc(len + 1)))
        return "???";
    memcpy(kept[count], s, len);
    kept[count][len] = '\0';
    return kept[count++];
}

static int64_t days_from_civil(int64_t year, int month, int day) {
    year -= month <= 2;

    int64_t era = (year >= 0 ? year : year - 399) / 400;
    int64_t year_of_era = year - era * 400;
    int64_t day_of_year = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;

    return era * 146097 + year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year - 719468;
}

static bool is_leap(int64_t year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static const char *parse_name(const char *s, const char **out) {
    const char *start = s;

    if (*s == '<') {
        while (*++s && *s != '>')
            ;
        if (*s != '>' || s - start < 4)
            return NULL;
        *out = intern(start + 1, s - start - 1);
        return s + 1;
    }
    while ((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z'))
        s++;
    if (s - start < 3)
        return NULL;
    *out = intern(start, s - start);
    return s;
}

// [+-]hh[:mm[:ss]], as seconds. Hours go up to 167 in rule times.
static const char *parse_time(const char *s, int32_t *out) {
    int sign = *s == '-' ? -1 : 1;
    int32_t parts[3] = {0, 0, 0};

    if (*s == '+' || *s == '-')
        s++;
    for (int i = 0; i < 3; i++) {
        if (i > 0 && *s != ':')
            break;
        if (i > 0)
            s++;
        if (*s < '0' || *s > '9')
            return NULL;
        while (*s >= '0' && *s <= '9')
            parts[i] = parts[i] * 10 + (*s++ - '0');
    }
    if (parts[0] > 167 || parts[1] > 59 || parts[2] > 59)
        return NULL;
    *out = sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]);
    return s;
}

static const char *parse_number(const char *s, int low, int high, int *out) {
    int value = 0;

    if (*s < '0' || *s > '9')
        return NULL;
    while (*s >= '0' && *s <= '9')
        value = value * 10 + (*s++ - '0');
    *out = value;
    return value >= low && value <= high ? s : NULL;
}

static const char *parse_date(const char *s, struct rule_date *d) {
    d->kind = *s == 'M' || *s == 'J' ? *s++ : 'D';
    d->time = 2 * 3600;
    if (d->kind == 'M') {
        if (!(s = parse_number(s, 1, 12, &d->month)) || *s++ != '.' || !(s = parse_number(s, 1, 5, &d->week))
            || *s++ != '.' || !(s = parse_number(s, 0, 6, &d->day)))
            return NULL;
    } else if (!(s = parse_number(s, d->kind == 'J', 365, &d->day))) {
        return NULL;
    }
    return *s == '/' ? parse_time(s + 1, &d->time) : s;
}

static bool parse_rule(const char *s, struct rule *r) {
    int32_t offset;

    *r = (struct rule){0};
    if (!(s = parse_name(s, &r->standard.abbreviation)) || !(s = parse_time(s, &offset)))
        return false;
    r->standard.offset = -offset;
    if (!*s)
        return true;

    if (!(s = parse_name(s, &r->summer.abbreviation)))
        return false;
    r->summer.offset = r->standard.offset + 3600;
    r->summer.dst = r->has_summer = true;
    if (*s && *s != ',') {
        if (!(s = parse_time(s, &offset)))
            return false;
        r->summer.offset = -offset;
    }
    if (!*s)
        s = ",M3.2.0,M11.1.0"; // the United States' rules, when none are given
    return *s++ == ',' && (s = parse_date(s, &r->start)) && *s++ == ',' && (s = parse_date(s, &r->end)) && !*s;
}

// The day, counted from 1970, that a rule's date falls on in a year.
static int64_t rule_day(const struct rule_date *d, int64_t year) {
    int64_t first = days_from_civil(year, d->kind == 'M' ? d->month : 1, 1);

    if (d->kind == 'J')
        return first + d->day - 1 + (is_leap(year) && d->day >= 60);
    if (d->kind == 'D')
        return first + d->day;

    static const int month_days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int length = month_days[d->month - 1] + (d->month == 2 && is_leap(year));
    int weekday = (int)(((first + 4) % 7 + 7) % 7); // 1 January 1970 was a Thursday
    int64_t day = (d->day - weekday + 7) % 7 + (d->week - 1) * 7;

    while (day >= length)
        day -= 7;
    return first + day;
}

static const struct local_type *rule_type(const struct rule *r, time_t when) {
    if (!r->has_summer)
        return &r->standard;

    time_t local = when + r->standard.offset;
    struct tm t;
    gmtime_r(&local, &t);

    int64_t year = t.tm_year + 1900LL;
    int64_t start = rule_day(&r->start, year) * SECONDS_PER_DAY + r->start.time - r->standard.offset;
    int64_t end = rule_day(&r->end, year) * SECONDS_PER_DAY + r->end.time - r->summer.offset;

    // In the south, summer time spans the new year.
    bool summer = start < end ? when >= start && when < end : !(when >= end && when < start);
    return summer ? &r->summer : &r->standard;
}

static int64_t big_endian(const unsigned char *p, int size) {
    int64_t value = (int8_t)p[0];

    for (int i = 1; i < size; i++)
        value = value << 8 | p[i];
    return value;
}

struct tzif_counts {
    int64_t is_ut, is_standard, leaps, times, types, chars;
};

static size_t data_size(const struct tzif_counts *c, int time_size) {
    return c->times * (time_size + 1) + c->types * 6 + c->chars + c->leaps * (time_size + 4) + c->is_standard
         + c->is_ut;
}

static bool read_counts(const unsigned char *p, struct tzif_counts *c) {
    *c = (struct tzif_counts){big_endian(p + 20, 4), big_endian(p + 24, 4), big_endian(p + 28, 4),
                              big_endian(p + 32, 4), big_endian(p + 36, 4), big_endian(p + 40, 4)};
    return memcmp(p, "TZif", 4) == 0 && c->times >= 0 && c->times < 100000 && c->types > 0
        && c->types <= MAX_TYPES && c->chars >= 0 && c->chars < 1000 && c->leaps >= 0 && c->leaps < 1000
        && c->is_standard >= 0 && c->is_ut >= 0 && c->is_standard <= c->types && c->is_ut <= c->types;
}

static unsigned char *read_file(const char *path, size_t *len) {
    int fd = open(path, O_RDONLY);
    unsigned char *data = fd >= 0 ? malloc(MAX_ZONE_FILE) : NULL;
    ssize_t n;

    *len = 0;
    while (data && *len < MAX_ZONE_FILE && (n = read(fd, data + *len, MAX_ZONE_FILE - *len)) > 0)
        *len += n;
    if (fd >= 0)
        close(fd);
    return data;
}

// Uses the 64-bit second half of a version 2 or later file, and the rule after it.
static bool load_tzif(const char *path) {
    size_t len;
    unsigned char *data = read_file(path, &len);
    struct tzif_counts c;
    int time_size = 4;
    size_t at = 44;

    if (!data || len < 44 || !read_counts(data, &c)) {
        free(data);
        return false;
    }
    if (data[4] >= '2') {
        at += data_size(&c, 4);
        if (at + 44 > len || !read_counts(data + at, &c)) {
            free(data);
            return false;
        }
        at += 44;
        time_size = 8;
    }
    if (at + data_size(&c, time_size) > len) {
        free(data);
        return false;
    }

    const unsigned char *times = data + at, *indexes = times + c.times * time_size;
    const unsigned char *types = indexes + c.times, *chars = types + c.types * 6;

    zone.times = malloc(c.times * sizeof(int64_t) + 1);
    zone.indexes = malloc(c.times + 1);
    if (!zone.times || !zone.indexes) {
        free(data);
        return false;
    }
    for (int64_t i = 0; i < c.times; i++) {
        zone.times[i] = big_endian(times + i * time_size, time_size);
        zone.indexes[i] = indexes[i] < c.types ? indexes[i] : 0;
    }
    zone.count = c.times;
    for (int64_t i = 0; i < c.types; i++) {
        const unsigned char *t = types + i * 6;
        int index = t[5] < c.chars ? t[5] : 0;
        size_t name_len = strnlen((const char *)chars + index, c.chars - index);

        zone.types[i] = (struct local_type){(int32_t)big_endian(t, 4), t[4], intern((const char *)chars + index, name_len)};
    }
    zone.type_count = c.types;

    // The rule sits between newlines at the end.
    size_t footer = at + data_size(&c, time_size);
    if (time_size == 8 && footer + 1 < len && data[footer] == '\n') {
        char text[128];
        size_t end = footer + 1;

        while (end < len && data[end] != '\n')
            end++;
        if (end < len && end - footer - 1 < sizeof(text)) {
            memcpy(text, data + footer + 1, end - footer - 1);
            text[end - footer - 1] = '\0';
            zone.has_rule = text[0] && parse_rule(text, &zone.rule);
        }
    }
    free(data);
    return true;
}

static void use_utc(void) {
    zone.types[0] = (struct local_type){0, false, "UTC"};
    zone.type_count = 1;
}

// The zone's standard and summer types, as most recently used, for tzname.
static void set_names(void) {
    const struct local_type *standard = &zone.types[0], *summer = NULL;

    if (zone.has_rule) {
        standard = &zone.rule.standard;
        summer = zone.rule.has_summer ? &zone.rule.summer : NULL;
    } else {
        for (int i = 0; i < zone.count; i++) {
            const struct local_type *t = &zone.types[zone.indexes[i]];

            if (t->dst)
                summer = t;
            else
                standard = t;
        }
    }
    tzname[0] = (char *)standard->abbreviation;
    tzname[1] = (char *)(summer ? summer->abbreviation : standard->abbreviation);
    timezone = -standard->offset;
    daylight = summer != NULL;
}

void tzset(void) {
    const char *tz = getenv("TZ");
    char path[PATH_MAX], key[sizeof(zone.key)];
    struct stat st;
    bool is_rule = false;

    if (!tz || !*tz)
        snprintf(path, sizeof(path), "%s", LOCALTIME);
    else if (*tz == ':' || *tz == '/')
        snprintf(path, sizeof(path), tz[*tz == ':'] == '/' ? "%s" : ZONE_DIR "/%s", tz + (*tz == ':'));
    else
        is_rule = snprintf(path, sizeof(path), ZONE_DIR "/%s", tz) >= (int)sizeof(path) || strstr(tz, "..")
               || stat(path, &st) < 0 || !S_ISREG(st.st_mode);

    // Reloading only when something changed keeps localtime cheap.
    bool found = !is_rule && stat(path, &st) == 0;
    snprintf(key, sizeof(key), "%s|%s|%ld|%ld", tz ? tz : "", is_rule ? "" : path, found ? (long)st.st_mtime : -1L,
             found ? (long)st.st_size : -1L);
    if (zone.type_count && strcmp(key, zone.key) == 0)
        return;

    free(zone.times);
    free(zone.indexes);
    zone.times = NULL;
    zone.indexes = NULL;
    zone.count = zone.type_count = 0;
    zone.has_rule = false;
    memcpy(zone.key, key, sizeof(key));

    if (is_rule && parse_rule(tz, &zone.rule)) {
        zone.has_rule = true;
        zone.types[0] = zone.rule.standard;
        zone.type_count = 1;
    } else if (is_rule || !found || !load_tzif(path)) {
        free(zone.times);
        free(zone.indexes);
        zone.times = NULL;
        zone.indexes = NULL;
        zone.count = 0;
        use_utc();
    }
    set_names();
}

static const struct local_type *type_at(time_t when) {
    if (zone.count == 0 || when < zone.times[0]) {
        if (zone.count == 0 && zone.has_rule)
            return rule_type(&zone.rule, when);

        // Before the first change, it's the first standard time.
        for (int i = 0; i < zone.type_count; i++) {
            if (!zone.types[i].dst)
                return &zone.types[i];
        }
        return &zone.types[0];
    }
    if (when >= zone.times[zone.count - 1] && zone.has_rule)
        return rule_type(&zone.rule, when);

    int low = 0, high = zone.count - 1;

    while (low < high) {
        int middle = (low + high + 1) / 2;

        if (zone.times[middle] <= when)
            low = middle;
        else
            high = middle - 1;
    }
    return &zone.types[zone.indexes[low]];
}

struct tm *localtime_r(const time_t *when, struct tm *result) {
    tzset();

    const struct local_type *type = type_at(*when);
    time_t local = *when + type->offset;

    if (!gmtime_r(&local, result))
        return NULL;
    result->tm_isdst = type->dst;
    result->tm_gmtoff = type->offset;
    result->tm_zone = type->abbreviation;
    return result;
}

struct tm *localtime(const time_t *when) {
    static struct tm result;

    return localtime_r(when, &result);
}

// A local time can happen twice when the clocks go back, which tm_isdst can choose
// between, or not at all when they go forward, in which case it's taken as being
// in the time before the change.
time_t mktime(struct tm *t) {
    tzset();

    int wanted_dst = t->tm_isdst;
    time_t local = timegm(t);
    time_t nearby[3] = {local - SECONDS_PER_DAY, local, local + SECONDS_PER_DAY};
    time_t found = local - type_at(local - SECONDS_PER_DAY)->offset;
    bool have = false, have_wanted = false;

    // The earliest that fits, unless a later one has the summer time asked for.
    for (int i = 0; i < 3; i++) {
        time_t candidate = local - type_at(nearby[i])->offset;
        const struct local_type *type = type_at(candidate);
        bool wanted = wanted_dst >= 0 && type->dst == (wanted_dst > 0);

        if (candidate + type->offset != local || (have && (have_wanted || !wanted || candidate == found)))
            continue;
        found = candidate;
        have = true;
        have_wanted = wanted;
    }
    localtime_r(&found, t);
    return found;
}
