#include <stdint.h>
#include <string.h>

void *memcpy(void *dest, const void *src, size_t n) {
    void *d = dest;

    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dest;
}

void *memset(void *dest, int value, size_t n) {
    void *d = dest;

    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(value) : "memory");
    return dest;
}

size_t strlen(const char *s) {
    size_t len = 0;

    while (s[len])
        len++;
    return len;
}

int strcmp(const char *a, const char *b) {
    for (; *a && *a == *b; a++, b++)
        ;
    return (uint8_t)*a - (uint8_t)*b;
}

char *strrchr(const char *s, int c) {
    const char *found = NULL;

    for (;; s++) {
        if (*s == (char)c)
            found = s;
        if (*s == '\0')
            return (char *)found;
    }
}

int memcmp(const void *a, const void *b, size_t n) {
    const uint8_t *x = a, *y = b;

    for (; n--; x++, y++) {
        if (*x != *y)
            return *x - *y;
    }
    return 0;
}

void *memmove(void *dest, const void *src, size_t n) {
    uint8_t *d = dest;
    const uint8_t *s = src;

    if (d < s) {
        for (size_t i = 0; i < n; i++)
            d[i] = s[i];
    } else {
        for (size_t i = n; i > 0; i--)
            d[i - 1] = s[i - 1];
    }
    return dest;
}
