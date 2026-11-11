#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "decimal.h"

#define MAX_EXACT_DIGITS 15
#define MAX_EXACT_POWER 22
#define MAX_EXPONENT 100000
#define MANTISSA_BITS 52
#define INTEGER_DIGITS 16 // enough for anything below 2^53
#define MIN_EXPONENT -1074
#define EXPONENT_BIAS 1075
#define EXPONENT_LIMIT 2047

static bool match_word(const char *s, const char *word) {
    return strncasecmp(s, word, strlen(word)) == 0;
}

static bool integer_part(const char *d, uint64_t *value) {
    for (int i = 0; i < POINT - INTEGER_DIGITS; i++) {
        if (d[i])
            return false;
    }
    *value = 0;
    for (int i = POINT - INTEGER_DIGITS; i < POINT; i++)
        *value = *value * 10 + d[i];
    return true;
}

// The exact value goes into a decimal array and is doubled or halved until its
// integer part has all 53 bits of a double, so the fraction left decides rounding.
static double exact(const char *start, int total_digits, int integer_digits, long exponent10) {
    char d[DIGITS];
    bool sticky = false;
    int exponent = 0;
    uint64_t mantissa;

    memset(d, 0, sizeof(d));
    for (int i = 0; start && i < total_digits; start++) {
        if (*start == '.')
            continue;

        int digit = *start - '0';
        long at = POINT - integer_digits + i - exponent10;

        if (at < 0 && digit) {
            errno = ERANGE;
            return HUGE_VAL;
        }
        if (at >= DIGITS)
            sticky |= digit != 0;
        else if (at >= 0)
            d[at] = digit;
        i++;
    }

    while (!integer_part(d, &mantissa) || mantissa >= 1ULL << (MANTISSA_BITS + 1)) {
        sticky |= halve_digits(d);
        exponent++;
    }
    while (mantissa < 1ULL << MANTISSA_BITS && exponent > MIN_EXPONENT) {
        double_digits(d);
        exponent--;
        integer_part(d, &mantissa);
    }

    bool rest = sticky;

    for (int i = POINT + 1; i < DIGITS && !rest; i++)
        rest = d[i] != 0;
    bool inexact = rest || d[POINT];

    if (d[POINT] > 5 || (d[POINT] == 5 && (rest || mantissa & 1)))
        mantissa++;
    if (mantissa == 1ULL << (MANTISSA_BITS + 1)) {
        mantissa >>= 1;
        exponent++;
    }

    int biased = mantissa >> MANTISSA_BITS ? exponent + EXPONENT_BIAS : 0;

    if (biased >= EXPONENT_LIMIT) {
        errno = ERANGE;
        return HUGE_VAL;
    }
    if (biased == 0 && inexact)
        errno = ERANGE;

    uint64_t bits = (mantissa & ((1ULL << MANTISSA_BITS) - 1)) | (uint64_t)biased << MANTISSA_BITS;
    double result;

    memcpy(&result, &bits, sizeof(result));
    return result;
}

double strtod(const char *s, char **end) {
    static const double powers[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9, 1e10, 1e11,
                                    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    const char *p = s;

    while (isspace((unsigned char)*p))
        p++;

    bool negative = *p == '-';
    double sign = negative ? -1.0 : 1.0;

    if (*p == '+' || *p == '-')
        p++;

    if (match_word(p, "inf") || match_word(p, "nan")) {
        bool is_nan = tolower((unsigned char)*p) == 'n';

        p += 3;
        if (!is_nan && match_word(p, "inity"))
            p += 5;
        if (end)
            *end = (char *)p;
        return copysign(is_nan ? NAN : INFINITY, sign);
    }

    const char *start = p;
    int total = 0, integer_digits = 0, significant = 0;
    uint64_t small = 0;
    bool seen_point = false;

    for (;; p++) {
        if (*p == '.' && !seen_point) {
            seen_point = true;
            continue;
        }
        if (!isdigit((unsigned char)*p))
            break;

        total++;
        if (!seen_point)
            integer_digits++;
        if (significant || *p != '0')
            significant++;
        if (significant && significant <= MAX_EXACT_DIGITS)
            small = small * 10 + (*p - '0');
    }

    if (total == 0) {
        if (end)
            *end = (char *)s;
        return 0.0;
    }

    long exponent10 = 0;

    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        bool exponent_negative = *q == '-';

        if (*q == '+' || *q == '-')
            q++;
        if (isdigit((unsigned char)*q)) {
            for (; isdigit((unsigned char)*q); q++) {
                if (exponent10 < MAX_EXPONENT)
                    exponent10 = exponent10 * 10 + (*q - '0');
            }
            if (exponent_negative)
                exponent10 = -exponent10;
            p = q;
        }
    }
    if (end)
        *end = (char *)p;

    if (significant == 0)
        return copysign(0.0, sign);

    // Few digits and a small power are both exact as doubles, and then a single
    // operation on them is correctly rounded.
    long power = exponent10 - (total - integer_digits);

    if (significant <= MAX_EXACT_DIGITS && power >= -MAX_EXACT_POWER && power <= MAX_EXACT_POWER) {
        double value = small;

        return sign * (power < 0 ? value / powers[-power] : value * powers[power]);
    }
    return sign * exact(start, total, integer_digits, exponent10);
}

float strtof(const char *s, char **end) {
    return strtod(s, end);
}

long double strtold(const char *s, char **end) {
    return strtod(s, end);
}
