#include <errno.h>
#include <iconv.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define NOTHING UINT32_MAX // a byte order mark, which isn't passed on

enum charset { UTF8, ASCII, LATIN1, CP1252, UTF16BE, UTF16LE, UTF16, UTF32BE, UTF32LE };

struct iconv_state {
    enum charset to, from;
};

static const struct {
    const char *name;
    enum charset charset;
} names[] = {
    {"UTF-8", UTF8},          {"UTF8", UTF8},           {"US-ASCII", ASCII},      {"ASCII", ASCII},
    {"ANSI_X3.4-1968", ASCII}, {"ISO-8859-1", LATIN1},  {"ISO8859-1", LATIN1},    {"LATIN1", LATIN1},
    {"WINDOWS-1252", CP1252}, {"CP1252", CP1252},       {"UTF-16", UTF16},        {"UTF-16BE", UTF16BE},
    {"UTF-16LE", UTF16LE},    {"UCS-2", UTF16BE},       {"UCS-2BE", UTF16BE},     {"UCS-2LE", UTF16LE},
    {"UTF-32", UTF32BE},      {"UTF-32BE", UTF32BE},    {"UTF-32LE", UTF32LE},    {"UCS-4", UTF32BE},
    {"UCS-4BE", UTF32BE},     {"UCS-4LE", UTF32LE},
};

// What Windows-1252 has in 0x80 to 0x9F, where ISO-8859-1 has control characters.
static const uint16_t cp1252[32] = {
    0x20AC, 0x81,   0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x8D,   0x017D, 0x8F,
    0x90,   0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x9D,   0x017E, 0x0178,
};

static int charset_of(const char *name) {
    size_t len = strcspn(name, "/");

    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (strlen(names[i].name) == len && strncasecmp(names[i].name, name, len) == 0)
            return names[i].charset;
    return -1;
}

iconv_t iconv_open(const char *to, const char *from) {
    int t = charset_of(to), f = charset_of(from);
    iconv_t cd;

    if (t < 0 || f < 0) {
        errno = EINVAL;
        return (iconv_t)-1;
    }
    if (!(cd = malloc(sizeof(*cd))))
        return (iconv_t)-1;
    cd->to = t == UTF16 ? UTF16BE : t;
    cd->from = f;
    return cd;
}

int iconv_close(iconv_t cd) {
    free(cd);
    return 0;
}

// The next character, how many bytes it took, 0 if they're cut short, or -1 if they're
// not a character.
static int decode(iconv_t cd, const unsigned char *s, size_t len, uint32_t *c) {
    switch (cd->from) {
    case ASCII:
        *c = s[0];
        return s[0] < 0x80 ? 1 : -1;
    case LATIN1:
        *c = s[0];
        return 1;
    case CP1252:
        *c = s[0] >= 0x80 && s[0] < 0xA0 ? cp1252[s[0] - 0x80] : s[0];
        return 1;
    case UTF16:
        if (len >= 2 && (s[0] << 8 | s[1]) == 0xFEFF) // a byte order mark says which way round
            cd->from = UTF16BE;
        else if (len >= 2 && (s[0] << 8 | s[1]) == 0xFFFE)
            cd->from = UTF16LE;
        else if (len >= 2)
            return cd->from = UTF16BE, decode(cd, s, len, c);
        else
            return 0;
        *c = NOTHING;
        return 2;
    case UTF16BE:
    case UTF16LE: {
        bool be = cd->from == UTF16BE;

        if (len < 2)
            return 0;
        *c = be ? s[0] << 8 | s[1] : s[1] << 8 | s[0];
        if (*c >= 0xDC00 && *c < 0xE000)
            return -1;
        if (*c < 0xD800 || *c >= 0xDC00)
            return 2;
        if (len < 4)
            return 0;

        uint32_t low = be ? s[2] << 8 | s[3] : s[3] << 8 | s[2];

        if (low < 0xDC00 || low >= 0xE000)
            return -1;
        *c = 0x10000 + ((*c - 0xD800) << 10) + (low - 0xDC00);
        return 4;
    }
    case UTF32BE:
    case UTF32LE:
        if (len < 4)
            return 0;
        *c = cd->from == UTF32BE ? (uint32_t)s[0] << 24 | s[1] << 16 | s[2] << 8 | s[3]
                                 : (uint32_t)s[3] << 24 | s[2] << 16 | s[1] << 8 | s[0];
        return *c < 0x110000 && (*c < 0xD800 || *c >= 0xE000) ? 4 : -1;
    default: { // UTF-8
        int n = s[0] < 0x80 ? 1 : s[0] >= 0xC2 && s[0] < 0xE0 ? 2 : s[0] >= 0xE0 && s[0] < 0xF0 ? 3
              : s[0] >= 0xF0 && s[0] < 0xF5 ? 4 : -1;
        uint32_t min[] = {0, 0, 0x80, 0x800, 0x10000};

        if (n < 0)
            return -1;
        *c = n == 1 ? s[0] : s[0] & (0x7F >> n);
        for (int i = 1; i < n; i++) {
            if ((size_t)i >= len)
                return 0;
            if ((s[i] & 0xC0) != 0x80)
                return -1;
            *c = *c << 6 | (s[i] & 0x3F);
        }
        return *c < min[n] || *c >= 0x110000 || (*c >= 0xD800 && *c < 0xE000) ? -1 : n;
    }
    }
}

// Writes a character, returning how many bytes it took, 0 if there isn't room, or -1
// if the character set doesn't have it.
static int encode(enum charset to, uint32_t c, unsigned char *s, size_t room) {
    switch (to) {
    case ASCII:
    case LATIN1:
        if (c >= (to == ASCII ? 0x80u : 0x100u))
            return -1;
        if (!room)
            return 0;
        s[0] = c;
        return 1;
    case CP1252: {
        int b = c < 0x80 || (c >= 0xA0 && c < 0x100) ? (int)c : -1;

        for (int i = 0; i < 32 && b < 0; i++)
            if (cp1252[i] == c)
                b = 0x80 + i;
        if (b < 0)
            return -1;
        if (!room)
            return 0;
        s[0] = b;
        return 1;
    }
    case UTF16BE:
    case UTF16LE: {
        uint16_t units[2] = {c, 0};
        int n = 1;

        if (c >= 0x10000) {
            units[0] = 0xD800 + ((c - 0x10000) >> 10), units[1] = 0xDC00 + ((c - 0x10000) & 0x3FF);
            n = 2;
        }
        if (room < (size_t)n * 2)
            return 0;
        for (int i = 0; i < n; i++) {
            s[2 * i + (to == UTF16LE)] = units[i] >> 8;
            s[2 * i + (to != UTF16LE)] = units[i];
        }
        return 2 * n;
    }
    case UTF32BE:
    case UTF32LE:
        if (room < 4)
            return 0;
        for (int i = 0; i < 4; i++)
            s[to == UTF32BE ? i : 3 - i] = c >> (24 - 8 * i);
        return 4;
    default: { // UTF-8
        int n = c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;

        if (room < (size_t)n)
            return 0;
        if (n == 1) {
            s[0] = c;
            return 1;
        }
        s[0] = (0xF00 >> n) | (c >> (6 * (n - 1)));
        for (int i = 1; i < n; i++)
            s[i] = 0x80 | ((c >> (6 * (n - 1 - i))) & 0x3F);
        return n;
    }
    }
}

size_t iconv(iconv_t cd, char **in, size_t *in_left, char **out, size_t *out_left) {
    if (!in || !*in) // resetting the state, of which there's only UTF-16's byte order
        return 0;
    while (*in_left) {
        uint32_t c;
        int took = decode(cd, (const unsigned char *)*in, *in_left, &c), wrote = 0;

        if (took <= 0) {
            errno = took ? EILSEQ : EINVAL;
            return (size_t)-1;
        }
        if (c != NOTHING) {
            if (!out || !*out || (wrote = encode(cd->to, c, (unsigned char *)*out, *out_left)) == 0) {
                errno = E2BIG;
                return (size_t)-1;
            }
            if (wrote < 0) {
                errno = EILSEQ;
                return (size_t)-1;
            }
        }
        *in += took, *in_left -= took;
        *out += wrote, *out_left -= wrote;
    }
    return 0;
}
