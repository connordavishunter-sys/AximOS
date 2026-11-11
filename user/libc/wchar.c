#include <errno.h>
#include <locale.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

#define MAX_CODE_POINT 0x10FFFF
#define INVALID ((size_t)-1)
#define INCOMPLETE ((size_t)-2)

char *setlocale(int category, const char *name) {
    (void)category;
    if (!name || !*name || strcmp(name, "C") == 0 || strcmp(name, "POSIX") == 0)
        return "C";
    return NULL;
}

struct lconv *localeconv(void) {
    static struct lconv conventions = {
        ".", "", "", "", "", "", "", "", "", "", 127, 127, 127, 127, 127, 127, 127, 127,
    };

    return &conventions;
}

// The smallest value each length can stand for, to catch overlong forms.
static unsigned min_for_length(int len) {
    return len == 2 ? 0x80 : len == 3 ? 0x800 : 0x10000;
}

size_t mbrtowc(wchar_t *out, const char *s, size_t len, mbstate_t *state) {
    static mbstate_t shared;

    if (!state)
        state = &shared;
    if (!s) {
        *state = (mbstate_t){0};
        return 0;
    }
    if (len == 0)
        return INCOMPLETE;

    size_t used = 0;

    if (state->remaining == 0) {
        unsigned char first = s[used++];
        int extra = first < 0x80 ? 0 : (first & 0xE0) == 0xC0 ? 1 : (first & 0xF0) == 0xE0 ? 2
                  : (first & 0xF8) == 0xF0                    ? 3 : -1;

        if (extra < 0) {
            errno = EILSEQ;
            return INVALID;
        }
        if (extra == 0) {
            if (out)
                *out = first;
            return first ? 1 : 0;
        }
        state->remaining = extra;
        state->value = first & (0x3F >> extra);
        state->value |= (unsigned)(extra + 1) << 24; // remembers the total length
    }

    while (used < len && state->remaining > 0) {
        unsigned char next = s[used++];

        if ((next & 0xC0) != 0x80) {
            *state = (mbstate_t){0};
            errno = EILSEQ;
            return INVALID;
        }
        state->value = (state->value & 0xFF000000) | ((state->value & 0xFFFFFF) << 6) | (next & 0x3F);
        state->remaining--;
    }
    if (state->remaining > 0)
        return INCOMPLETE;

    int total = state->value >> 24;
    unsigned value = state->value & 0xFFFFFF;

    *state = (mbstate_t){0};
    if (value < min_for_length(total) || value > MAX_CODE_POINT || (value >= 0xD800 && value < 0xE000)) {
        errno = EILSEQ;
        return INVALID;
    }
    if (out)
        *out = value;
    return used;
}

size_t wcrtomb(char *out, wchar_t c, mbstate_t *state) {
    char scratch[4];
    unsigned value = c;

    (void)state;
    if (!out) {
        out = scratch;
        value = 0;
    }
    if (value > MAX_CODE_POINT || (value >= 0xD800 && value < 0xE000)) {
        errno = EILSEQ;
        return INVALID;
    }
    if (value < 0x80) {
        out[0] = value;
        return 1;
    }

    int len = value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
    static const unsigned char lead[] = {0, 0, 0xC0, 0xE0, 0xF0};

    for (int i = len - 1; i > 0; i--) {
        out[i] = 0x80 | (value & 0x3F);
        value >>= 6;
    }
    out[0] = lead[len] | value;
    return len;
}

size_t mbrlen(const char *s, size_t len, mbstate_t *state) {
    return mbrtowc(NULL, s, len, state);
}

int mbsinit(const mbstate_t *state) {
    return !state || state->remaining == 0;
}

wint_t btowc(int c) {
    return c >= 0 && c < 0x80 ? (wint_t)c : WEOF;
}

int wctob(wint_t c) {
    return c < 0x80 ? (int)c : -1;
}

int wcwidth(wchar_t c) {
    if (c == 0)
        return 0;
    return (unsigned)c < 0x20 || c == 0x7F ? -1 : 1;
}

int mblen(const char *s, size_t len) {
    return mbtowc(NULL, s, len);
}

int mbtowc(wchar_t *out, const char *s, size_t len) {
    mbstate_t state = {0};

    if (!s)
        return 0;

    size_t result = mbrtowc(out, s, len, &state);

    return result == INVALID || result == INCOMPLETE ? -1 : (int)result;
}

int wctomb(char *out, wchar_t c) {
    return out ? (int)wcrtomb(out, c, NULL) : 0;
}

size_t mbstowcs(wchar_t *dest, const char *src, size_t len) {
    mbstate_t state = {0};
    size_t count = 0;

    while (!dest || count < len) {
        wchar_t c;
        size_t used = mbrtowc(&c, src, MB_CUR_MAX, &state);

        if (used == INVALID || used == INCOMPLETE)
            return INVALID;
        if (used == 0) {
            if (dest)
                dest[count] = 0;
            return count;
        }
        if (dest)
            dest[count] = c;
        src += used;
        count++;
    }
    return count;
}

size_t wcstombs(char *dest, const wchar_t *src, size_t len) {
    size_t count = 0;

    for (;; src++) {
        char bytes[4];
        size_t used = wcrtomb(bytes, *src, NULL);

        if (used == INVALID)
            return INVALID;
        if (*src == 0) {
            if (dest && count < len)
                dest[count] = 0;
            return count;
        }
        if (dest && count + used > len)
            return count;
        if (dest)
            memcpy(dest + count, bytes, used);
        count += used;
    }
}

size_t wcslen(const wchar_t *s) {
    size_t len = 0;

    while (s[len])
        len++;
    return len;
}

wchar_t *wcscpy(wchar_t *dest, const wchar_t *src) {
    return wmemcpy(dest, src, wcslen(src) + 1);
}

wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n) {
    size_t i = 0;

    for (; i < n && src[i]; i++)
        dest[i] = src[i];
    for (; i < n; i++)
        dest[i] = 0;
    return dest;
}

wchar_t *wcscat(wchar_t *dest, const wchar_t *src) {
    wcscpy(dest + wcslen(dest), src);
    return dest;
}

int wcscmp(const wchar_t *a, const wchar_t *b) {
    while (*a && *a == *b)
        a++, b++;
    return *a < *b ? -1 : *a > *b;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (; n > 0; a++, b++, n--) {
        if (*a != *b)
            return *a < *b ? -1 : 1;
        if (!*a)
            break;
    }
    return 0;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c) {
    for (;; s++) {
        if (*s == c)
            return (wchar_t *)s;
        if (!*s)
            return NULL;
    }
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c) {
    const wchar_t *found = NULL;

    for (;; s++) {
        if (*s == c)
            found = s;
        if (!*s)
            return (wchar_t *)found;
    }
}

wchar_t *wcsstr(const wchar_t *s, const wchar_t *needle) {
    size_t len = wcslen(needle);

    for (; ; s++) {
        if (wcsncmp(s, needle, len) == 0)
            return (wchar_t *)s;
        if (!*s)
            return NULL;
    }
}

wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n) {
    return memcpy(dest, src, n * sizeof(wchar_t));
}

wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n) {
    return memmove(dest, src, n * sizeof(wchar_t));
}

wchar_t *wmemset(wchar_t *dest, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++)
        dest[i] = c;
    return dest;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c)
            return (wchar_t *)&s[i];
    }
    return NULL;
}

int iswalpha(wint_t c) {
    return (c | 0x20) - 'a' < 26 || c >= 0x80;
}

int iswdigit(wint_t c) {
    return c - '0' < 10;
}

int iswalnum(wint_t c) {
    return iswalpha(c) || iswdigit(c);
}

int iswblank(wint_t c) {
    return c == ' ' || c == '\t';
}

int iswcntrl(wint_t c) {
    return c < 0x20 || c == 0x7F;
}

int iswgraph(wint_t c) {
    return c > 0x20 && c != 0x7F;
}

int iswlower(wint_t c) {
    return c - 'a' < 26;
}

int iswprint(wint_t c) {
    return c >= 0x20 && c != 0x7F;
}

int iswpunct(wint_t c) {
    return iswgraph(c) && !iswalnum(c);
}

int iswspace(wint_t c) {
    return c == ' ' || c - '\t' < 5;
}

int iswupper(wint_t c) {
    return c - 'A' < 26;
}

int iswxdigit(wint_t c) {
    return iswdigit(c) || (c | 0x20) - 'a' < 6;
}

wint_t towlower(wint_t c) {
    return iswupper(c) ? c + 0x20 : c;
}

wint_t towupper(wint_t c) {
    return iswlower(c) ? c - 0x20 : c;
}
