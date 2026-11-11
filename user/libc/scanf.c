#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUMBER_SIZE 512

// Reads from a string if there is one, otherwise from a stream.
struct input {
    const char *string;
    FILE *stream;
    int count;
};

static int next(struct input *in) {
    int c;

    if (in->string)
        c = *in->string ? (unsigned char)*in->string++ : EOF;
    else
        c = fgetc(in->stream);
    if (c != EOF)
        in->count++;
    return c;
}

static void back(struct input *in, int c) {
    if (c == EOF)
        return;
    in->count--;
    if (in->string)
        in->string--;
    else
        ungetc(c, in->stream);
}

static void skip_space(struct input *in) {
    int c;

    do
        c = next(in);
    while (c != EOF && isspace(c));
    back(in, c);
}

// Takes the next character if it's accepted and the width allows it.
static bool take(struct input *in, char *text, int *len, int width, const char *accept) {
    if (*len >= width)
        return false;

    int c = next(in);

    if (c != EOF && c != '\0' && strchr(accept, tolower(c))) {
        text[(*len)++] = c;
        return true;
    }
    back(in, c);
    return false;
}

// Collects the text of an integer, returning its base or 0 if there's no number.
static int read_integer(struct input *in, char *text, int width, int base) {
    int len = 0;
    bool digits = false;

    take(in, text, &len, width, "+-");
    if ((base == 0 || base == 16) && take(in, text, &len, width, "0")) {
        digits = true;
        base = take(in, text, &len, width, "x") ? (digits = false, 16) : base == 16 ? 16 : 8;
    }
    if (base == 0)
        base = 10;

    const char *allowed = base == 16 ? "0123456789abcdef" : base == 8 ? "01234567" : "0123456789";

    while (take(in, text, &len, width, allowed))
        digits = true;
    text[len] = '\0';
    return digits ? base : 0;
}

// Collects the text of a floating point number, returning whether there is one.
static bool read_float(struct input *in, char *text, int width) {
    int len = 0;
    int digits = 0;

    take(in, text, &len, width, "+-");

    // inf, infinity and nan are all letters that a number can't start with.
    if (take(in, text, &len, width, "in")) {
        while (take(in, text, &len, width, "aitnfy"))
            ;
        text[len] = '\0';
        return true;
    }

    while (take(in, text, &len, width, "0123456789"))
        digits++;
    if (take(in, text, &len, width, ".")) {
        while (take(in, text, &len, width, "0123456789"))
            digits++;
    }
    if (digits && take(in, text, &len, width, "e")) {
        take(in, text, &len, width, "+-");
        while (take(in, text, &len, width, "0123456789"))
            ;
    }
    text[len] = '\0';
    return digits > 0;
}

static void store_integer(void *dest, int length, uint64_t value) {
    switch (length) {
    case -2: *(signed char *)dest = value; break;
    case -1: *(short *)dest = value; break;
    case 0: *(int *)dest = value; break;
    default: *(long *)dest = value; break;
    }
}

// Reads a %[ set from just after the bracket, returning what follows it.
static const char *parse_set(const char *fmt, bool *set) {
    bool invert = *fmt == '^';

    if (invert)
        fmt++;
    memset(set, invert, 256);

    // A ] straight away is part of the set.
    for (bool first = true; *fmt && (first || *fmt != ']'); first = false, fmt++) {
        if (fmt[1] == '-' && fmt[2] && fmt[2] != ']') {
            for (int c = (unsigned char)fmt[0]; c <= (unsigned char)fmt[2]; c++)
                set[c] = !invert;
            fmt += 2;
        } else {
            set[(unsigned char)*fmt] = !invert;
        }
    }
    return *fmt ? fmt : fmt - 1;
}

static int scan(struct input *in, const char *fmt, va_list incoming) {
    va_list args;
    int assigned = 0;
    bool any_input = false;

    va_copy(args, incoming);
    for (; *fmt; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            skip_space(in);
            continue;
        }
        if (*fmt != '%' || fmt[1] == '%') {
            if (*fmt == '%') {
                fmt++;
                skip_space(in);
            }

            int c = next(in);

            if (c != (unsigned char)*fmt) {
                back(in, c);
                if (c == EOF && !any_input)
                    assigned = EOF;
                break;
            }
            any_input = true;
            continue;
        }
        fmt++;

        bool suppress = *fmt == '*';

        if (suppress)
            fmt++;

        int width = 0;

        for (; isdigit((unsigned char)*fmt); fmt++)
            width = width * 10 + (*fmt - '0');

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

        char conv = *fmt;

        if (conv == 'n') {
            if (!suppress)
                store_integer(va_arg(args, void *), length, in->count);
            continue;
        }
        if (conv != 'c' && conv != '[')
            skip_space(in);

        // Running out of input before anything matched is the only way to get EOF.
        int peek = next(in);

        back(in, peek);
        if (peek == EOF) {
            if (!any_input)
                assigned = EOF;
            break;
        }

        void *dest = suppress ? NULL : va_arg(args, void *);
        char text[NUMBER_SIZE];
        int limit = width && width < NUMBER_SIZE ? width : NUMBER_SIZE - 1;
        bool matched = true;

        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': {
            int base = conv == 'i' ? 0 : conv == 'o' ? 8 : conv == 'x' || conv == 'X' || conv == 'p' ? 16 : 10;

            base = read_integer(in, text, limit, base);
            matched = base != 0;
            if (matched && dest) {
                if (conv == 'p')
                    *(void **)dest = (void *)strtoull(text, NULL, base);
                else
                    store_integer(dest, length, conv == 'd' || conv == 'i' ? (uint64_t)strtoll(text, NULL, base)
                                                                           : strtoull(text, NULL, base));
            }
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
            matched = read_float(in, text, limit);
            if (matched && dest) {
                if (long_double)
                    *(long double *)dest = strtod(text, NULL);
                else if (length > 0)
                    *(double *)dest = strtod(text, NULL);
                else
                    *(float *)dest = strtof(text, NULL);
            }
            break;
        case 'c': {
            char *out = dest;

            for (int i = 0; i < (width ? width : 1); i++) {
                int c = next(in);

                if (c == EOF) {
                    matched = i > 0;
                    break;
                }
                if (out)
                    *out++ = c;
            }
            break;
        }
        case 's': case '[': {
            bool set[256];
            char *out = dest;
            int len = 0;

            if (conv == '[') {
                fmt = parse_set(fmt + 1, set);
            } else {
                for (int c = 0; c < 256; c++)
                    set[c] = !isspace(c);
            }

            while (!width || len < width) {
                int c = next(in);

                if (c == EOF || !set[c]) {
                    back(in, c);
                    break;
                }
                if (out)
                    *out++ = c;
                len++;
            }
            if (out)
                *out = '\0';
            matched = len > 0;
            break;
        }
        default:
            matched = false;
        }

        if (!matched)
            break;
        any_input = true;
        if (dest)
            assigned++;
    }
    va_end(args);
    return assigned;
}

int vsscanf(const char *s, const char *fmt, va_list args) {
    struct input in = {.string = s};

    return scan(&in, fmt, args);
}

int vfscanf(FILE *stream, const char *fmt, va_list args) {
    struct input in = {.stream = stream};

    return scan(&in, fmt, args);
}

int vscanf(const char *fmt, va_list args) {
    return vfscanf(stdin, fmt, args);
}

int sscanf(const char *s, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);

    int result = vsscanf(s, fmt, args);

    va_end(args);
    return result;
}

int fscanf(FILE *stream, const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);

    int result = vfscanf(stream, fmt, args);

    va_end(args);
    return result;
}

int scanf(const char *fmt, ...) {
    va_list args;

    va_start(args, fmt);

    int result = vscanf(fmt, args);

    va_end(args);
    return result;
}
