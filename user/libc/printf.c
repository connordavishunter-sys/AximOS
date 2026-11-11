#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "decimal.h"

#define BUFFER_SIZE 256

// Text goes to a string if there is one, and otherwise through a small buffer
// to a stream if there is one, and otherwise to a file descriptor.
struct output {
    char *string;
    size_t room;
    FILE *stream;
    int fd;
    char buffer[BUFFER_SIZE];
    size_t used;
    int total;
    bool failed;
};

static void flush(struct output *out) {
    if (out->stream) {
        if (fwrite(out->buffer, 1, out->used, out->stream) != out->used)
            out->failed = true;
    } else {
        size_t done = 0;

        while (done < out->used) {
            ssize_t wrote = write(out->fd, out->buffer + done, out->used - done);

            if (wrote <= 0) {
                out->failed = true;
                break;
            }
            done += wrote;
        }
    }
    out->used = 0;
}

static void put(struct output *out, char c) {
    out->total++;
    if (out->string) {
        if (out->room > 1) {
            *out->string++ = c;
            out->room--;
        }
        return;
    }
    if (out->used == BUFFER_SIZE)
        flush(out);
    out->buffer[out->used++] = c;
}

static void pad(struct output *out, char c, int count) {
    for (; count > 0; count--)
        put(out, c);
}

// A precision of -1 means no limit on the length.
static void put_string(struct output *out, const char *s, int width, bool left, int precision) {
    int len = 0;

    while (s[len] && (precision < 0 || len < precision))
        len++;
    if (!left)
        pad(out, ' ', width - len);
    for (int i = 0; i < len; i++)
        put(out, s[i]);
    if (left)
        pad(out, ' ', width - len);
}

static void put_number(struct output *out, uint64_t value, unsigned base, bool upper,
                       bool negative, const char *prefix, int width, bool left, bool zero,
                       int precision) {
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char text[64];
    int len = 0;

    while (value) {
        text[len++] = digits[value % base];
        value /= base;
    }
    // A precision of 0 prints nothing at all for 0, and giving one turns off the 0 flag.
    if (len == 0 && precision != 0)
        text[len++] = '0';
    if (precision >= 0)
        zero = false;

    int zeros = precision > len ? precision - len : 0;

    int prefix_len = negative;
    for (const char *p = prefix; *p; p++)
        prefix_len++;

    int padding = width - len - prefix_len - zeros;

    if (!left && !zero)
        pad(out, ' ', padding);
    if (negative)
        put(out, '-');
    for (const char *p = prefix; *p; p++)
        put(out, *p);
    if (!left && zero)
        pad(out, '0', padding);
    pad(out, '0', zeros);
    while (len > 0)
        put(out, text[--len]);
    if (left)
        pad(out, ' ', padding);
}

// Doubles are written by working out their exact decimal digits, one doubling or
// halving at a time. Slow for extreme values, but it rounds correctly every time.
#define MAX_PRECISION 1000
#define DOUBLE_MANTISSA_BITS 52
#define DOUBLE_EXPONENT_MASK 0x7FF
#define DOUBLE_EXPONENT_BIAS 1075

// Rounds to the digits before index end, with exact ties going to even.
static void round_digits(char *d, int end) {
    if (end >= DIGITS)
        return;

    bool up = d[end] > 5;

    if (d[end] == 5) {
        for (int i = end + 1; i < DIGITS && !up; i++)
            up = d[i] != 0;
        if (!up)
            up = end > 0 && d[end - 1] % 2 == 1;
    }

    for (int i = end; i < DIGITS; i++)
        d[i] = 0;

    for (int i = end - 1; up && i >= 0; i--) {
        d[i]++;
        up = d[i] == 10;
        if (up)
            d[i] = 0;
    }
}

static int first_nonzero(const char *d) {
    for (int i = 0; i < DIGITS; i++) {
        if (d[i])
            return i;
    }
    return -1;
}

// These write text and return the new end of it.
static char *write_fixed(char *text, const char *d, int precision, bool strip) {
    int first = first_nonzero(d);
    int start = first >= 0 && first < POINT ? first : POINT - 1;

    for (int i = start; i < POINT; i++)
        *text++ = '0' + d[i];

    char *point = text;

    *text++ = '.';
    for (int i = 0; i < precision; i++)
        *text++ = '0' + (POINT + i < DIGITS ? d[POINT + i] : 0);

    if (strip) {
        while (text > point + 1 && text[-1] == '0')
            text--;
    }
    return text == point + 1 ? point : text;
}

static char *write_exponent(char *text, const char *d, int precision, int first, bool upper, bool strip) {
    int exponent = first >= 0 ? POINT - 1 - first : 0;
    int lead = first >= 0 ? first : POINT - 1;

    *text++ = '0' + d[lead];

    char *point = text;

    *text++ = '.';
    for (int i = 0; i < precision; i++)
        *text++ = '0' + (lead + 1 + i < DIGITS ? d[lead + 1 + i] : 0);

    if (strip) {
        while (text > point + 1 && text[-1] == '0')
            text--;
    }
    if (text == point + 1)
        text = point;

    *text++ = upper ? 'E' : 'e';
    *text++ = exponent < 0 ? '-' : '+';

    int magnitude = exponent < 0 ? -exponent : exponent;

    if (magnitude >= 100)
        *text++ = '0' + magnitude / 100;
    *text++ = '0' + magnitude / 10 % 10;
    *text++ = '0' + magnitude % 10;
    return text;
}

static void put_double(struct output *out, double value, char conv, int precision, int width,
                       bool left, bool zero, const char *positive) {
    static const char *const specials[] = {"inf", "INF", "nan", "NAN"};
    bool upper = conv == 'F' || conv == 'E' || conv == 'G';
    char lower_conv = upper ? conv - 'A' + 'a' : conv;
    char digits[DIGITS];
    char text[DIGITS + 16];
    char *end = text;
    uint64_t bits;

    __builtin_memcpy(&bits, &value, sizeof(bits));

    bool negative = bits >> 63;
    int biased = bits >> DOUBLE_MANTISSA_BITS & DOUBLE_EXPONENT_MASK;
    uint64_t mantissa = bits & ((1ULL << DOUBLE_MANTISSA_BITS) - 1);
    const char *sign = negative ? "-" : positive;

    if (biased == DOUBLE_EXPONENT_MASK) {
        const char *word = specials[(mantissa ? 2 : 0) + upper];

        if (!left)
            pad(out, ' ', width - 3 - (int)(*sign != 0));
        put_string(out, sign, 0, false, -1);
        put_string(out, word, 0, false, -1);
        if (left)
            pad(out, ' ', width - 3 - (int)(*sign != 0));
        return;
    }

    if (precision > MAX_PRECISION)
        precision = MAX_PRECISION;

    __builtin_memset(digits, 0, sizeof(digits));
    if (biased)
        mantissa |= 1ULL << DOUBLE_MANTISSA_BITS;

    for (int i = POINT - 1; mantissa; i--) {
        digits[i] = mantissa % 10;
        mantissa /= 10;
    }

    int exponent = (biased ? biased : 1) - DOUBLE_EXPONENT_BIAS;

    for (; exponent > 0; exponent--)
        double_digits(digits);
    for (; exponent < 0; exponent++)
        halve_digits(digits);

    if (lower_conv == 'f') {
        int places = precision < 0 ? 6 : precision;

        round_digits(digits, POINT + places);
        end = write_fixed(text, digits, places, false);
    } else if (lower_conv == 'e') {
        int places = precision < 0 ? 6 : precision;
        int first = first_nonzero(digits);

        if (first >= 0)
            round_digits(digits, first + 1 + places);
        end = write_exponent(text, digits, places, first_nonzero(digits), upper, false);
    } else {
        // %g picks between them by the exponent, once rounded to its digits.
        int significant = precision < 0 ? 6 : precision == 0 ? 1 : precision;
        int first = first_nonzero(digits);

        if (first >= 0)
            round_digits(digits, first + significant);

        first = first_nonzero(digits);

        int exp10 = first >= 0 ? POINT - 1 - first : 0;

        if (exp10 < -4 || exp10 >= significant)
            end = write_exponent(text, digits, significant - 1, first, upper, true);
        else
            end = write_fixed(text, digits, significant - 1 - exp10, true);
    }

    int padding = width - (int)(end - text) - (int)(*sign != 0);

    if (!left && !zero)
        pad(out, ' ', padding);
    put_string(out, sign, 0, false, -1);
    if (!left && zero)
        pad(out, '0', padding);
    for (char *p = text; p < end; p++)
        put(out, *p);
    if (left)
        pad(out, ' ', padding);
}

// Reads an argument of the size given by the length modifier.
static int64_t next_signed(va_list *args, int length) {
    switch (length) {
    case -2:
        return (signed char)va_arg(*args, int);
    case -1:
        return (short)va_arg(*args, int);
    case 0:
        return va_arg(*args, int);
    default:
        return va_arg(*args, long);
    }
}

static uint64_t next_unsigned(va_list *args, int length) {
    switch (length) {
    case -2:
        return (unsigned char)va_arg(*args, unsigned);
    case -1:
        return (unsigned short)va_arg(*args, unsigned);
    case 0:
        return va_arg(*args, unsigned);
    default:
        return va_arg(*args, unsigned long);
    }
}

static void format(struct output *out, const char *fmt, va_list incoming) {
    va_list args;

    // A va_list parameter has decayed to a pointer, which can't be passed on by
    // address the way the helpers above expect.
    va_copy(args, incoming);

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(out, *fmt);
            continue;
        }
        fmt++;

        bool left = false, zero = false, plus = false, space = false, alternate = false;
        for (;; fmt++) {
            if (*fmt == '-')
                left = true;
            else if (*fmt == '0')
                zero = true;
            else if (*fmt == '+')
                plus = true;
            else if (*fmt == ' ')
                space = true;
            else if (*fmt == '#')
                alternate = true;
            else
                break;
        }

        int width = 0;
        if (*fmt == '*') {
            width = va_arg(args, int);
            fmt++;
        }
        for (; *fmt >= '0' && *fmt <= '9'; fmt++)
            width = width * 10 + (*fmt - '0');
        if (width < 0) {
            left = true;
            width = -width;
        }

        int precision = -1;
        if (*fmt == '.') {
            precision = 0;
            fmt++;
            if (*fmt == '*') {
                precision = va_arg(args, int);
                fmt++;
            }
            for (; *fmt >= '0' && *fmt <= '9'; fmt++)
                precision = precision * 10 + (*fmt - '0');
        }

        // Negative lengths are narrower than int, positive ones are 64-bit.
        int length = 0;
        bool long_double = false;

        for (;; fmt++) {
            if (*fmt == 'h')
                length--;
            else if (*fmt == 'l' || *fmt == 'z' || *fmt == 'j' || *fmt == 't')
                length++;
            else if (*fmt == 'L')
                long_double = true;
            else
                break;
        }

        const char *positive = plus ? "+" : space ? " " : "";

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t value = next_signed(&args, length);
            uint64_t magnitude = value < 0 ? -(uint64_t)value : (uint64_t)value;
            put_number(out, magnitude, 10, false, value < 0, value < 0 ? "" : positive, width, left, zero, precision);
            break;
        }
        case 'u':
            put_number(out, next_unsigned(&args, length), 10, false, false, "", width, left, zero, precision);
            break;
        case 'o': {
            uint64_t value = next_unsigned(&args, length);
            put_number(out, value, 8, false, false, alternate && value ? "0" : "", width, left, zero, precision);
            break;
        }
        case 'x':
        case 'X': {
            uint64_t value = next_unsigned(&args, length);
            const char *prefix = !alternate || !value ? "" : *fmt == 'X' ? "0X" : "0x";
            put_number(out, value, 16, *fmt == 'X', false, prefix, width, left, zero, precision);
            break;
        }
        case 'p':
            put_number(out, (uint64_t)va_arg(args, void *), 16, false, false, "0x", width, left, zero, -1);
            break;
        case 'c':
            if (!left)
                pad(out, ' ', width - 1);
            put(out, va_arg(args, int));
            if (left)
                pad(out, ' ', width - 1);
            break;
        case 's': {
            const char *s = va_arg(args, const char *);
            put_string(out, s ? s : "(null)", width, left, precision);
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
            put_double(out, long_double ? (double)va_arg(args, long double) : va_arg(args, double), *fmt, precision, width, left, zero, positive);
            break;
        case '%':
            put(out, '%');
            break;
        case '\0':
            fmt--;
            break;
        default:
            put(out, '%');
            put(out, *fmt);
        }
    }

    va_end(args);
    if (out->string && out->room > 0)
        *out->string = '\0';
    if (!out->string)
        flush(out);
}

static int finish(const struct output *out) {
    return out->failed ? -1 : out->total;
}

int vfprintf(FILE *stream, const char *fmt, va_list args) {
    struct output out = {.stream = stream};

    // A whole printf stays together, whatever other threads are printing.
    flockfile(stream);
    format(&out, fmt, args);

    int result = finish(&out);

    funlockfile(stream);
    return result;
}

int vdprintf(int fd, const char *fmt, va_list args) {
    struct output out = {.fd = fd};

    format(&out, fmt, args);
    return finish(&out);
}

int vsnprintf(char *dest, size_t size, const char *fmt, va_list args) {
    // A string that isn't really there still needs telling apart from no string.
    static char nowhere;
    struct output out = {.string = size ? dest : &nowhere, .room = size};

    format(&out, fmt, args);
    return out.total;
}

int vprintf(const char *fmt, va_list args) {
    return vfprintf(stdout, fmt, args);
}

int vsprintf(char *dest, const char *fmt, va_list args) {
    return vsnprintf(dest, SIZE_MAX, fmt, args);
}

int vasprintf(char **out, const char *fmt, va_list args) {
    va_list again;

    va_copy(again, args);

    int len = vsnprintf(NULL, 0, fmt, again);

    va_end(again);
    *out = malloc(len + 1);
    return *out ? vsnprintf(*out, len + 1, fmt, args) : -1;
}

#define VARIADIC(function, ...)                 \
    int total;                                  \
    va_list args;                               \
    va_start(args, fmt);                        \
    total = function(__VA_ARGS__, fmt, args);   \
    va_end(args);                               \
    return total

int fprintf(FILE *stream, const char *fmt, ...) { VARIADIC(vfprintf, stream); }
int dprintf(int fd, const char *fmt, ...) { VARIADIC(vdprintf, fd); }
int sprintf(char *dest, const char *fmt, ...) { VARIADIC(vsprintf, dest); }
int snprintf(char *dest, size_t size, const char *fmt, ...) { VARIADIC(vsnprintf, dest, size); }
int asprintf(char **out, const char *fmt, ...) { VARIADIC(vasprintf, out); }

int printf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int total = vfprintf(stdout, fmt, args);
    va_end(args);
    return total;
}

void print_size(uint64_t bytes, int width) {
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    uint64_t tenths = 0;
    int unit = 0;

    while (bytes >= 1024 && unit < 4) {
        tenths = bytes % 1024 * 10 / 1024;
        bytes /= 1024;
        unit++;
    }

    // Below these widths there's nothing to pad, and a negative one would pad.
    if (unit)
        printf("%*lu.%lu %s", width > 6 ? width - 6 : 0, bytes, tenths, units[unit]);
    else
        printf("%*lu %-3s", width > 4 ? width - 4 : 0, bytes, units[unit]);
}
