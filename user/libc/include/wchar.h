#pragma once

#include <stddef.h>
#include <stdint.h>

#define WEOF ((wint_t)-1)

typedef unsigned wint_t;

// Holds a partly converted character, for when a sequence is split across calls.
typedef struct {
    unsigned value;
    int remaining;
} mbstate_t;

// Multibyte strings are UTF-8.
size_t mbrtowc(wchar_t *out, const char *s, size_t len, mbstate_t *state);
size_t wcrtomb(char *out, wchar_t c, mbstate_t *state);
size_t mbrlen(const char *s, size_t len, mbstate_t *state);
int mbsinit(const mbstate_t *state);
wint_t btowc(int c);
int wctob(wint_t c);
int wcwidth(wchar_t c);

size_t wcslen(const wchar_t *s);
wchar_t *wcscpy(wchar_t *dest, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dest, const wchar_t *src);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsstr(const wchar_t *s, const wchar_t *needle);
wchar_t *wmemcpy(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dest, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *dest, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
