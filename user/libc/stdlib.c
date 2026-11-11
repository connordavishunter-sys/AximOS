#include <stdint.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int digit_value(int c) {
    if (isdigit(c))
        return c - '0';
    return isalpha(c) ? tolower(c) - 'a' + 10 : INT_MAX;
}

// Everything strto*l shares. Gives the magnitude, saturated, and leaves the sign
// and whether it overflowed for the caller to apply.
static uint64_t parse_integer(const char *s, char **end, int base, bool *negative, bool *overflow) {
    const char *p = s;
    uint64_t value = 0;
    bool any = false;

    *negative = *overflow = false;
    if (base < 0 || base == 1 || base > 36) {
        errno = EINVAL;
        if (end)
            *end = (char *)s;
        return 0;
    }

    while (isspace((unsigned char)*p))
        p++;
    if (*p == '+' || *p == '-')
        *negative = *p++ == '-';

    // The prefix only counts when a digit follows it, or "0x" reads as a lone 0.
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X') &&
        isxdigit((unsigned char)p[2])) {
        p += 2;
        base = 16;
    } else if (base == 0) {
        base = *p == '0' ? 8 : 10;
    }

    for (; digit_value((unsigned char)*p) < base; p++) {
        any = true;
        if (__builtin_mul_overflow(value, (uint64_t)base, &value) ||
            __builtin_add_overflow(value, (uint64_t)digit_value((unsigned char)*p), &value))
            *overflow = true;
    }

    if (end)
        *end = (char *)(any ? p : s);
    return *overflow ? UINT64_MAX : value;
}

long long strtoll(const char *s, char **end, int base) {
    bool negative, overflow;
    uint64_t value = parse_integer(s, end, base, &negative, &overflow);

    if (overflow || value > (uint64_t)LLONG_MAX + negative) {
        errno = ERANGE;
        return negative ? LLONG_MIN : LLONG_MAX;
    }
    return negative ? -value : value;
}

unsigned long long strtoull(const char *s, char **end, int base) {
    bool negative, overflow;
    uint64_t value = parse_integer(s, end, base, &negative, &overflow);

    if (overflow) {
        errno = ERANGE;
        return ULLONG_MAX;
    }
    return negative ? -value : value;
}

long strtol(const char *s, char **end, int base) { return strtoll(s, end, base); }
unsigned long strtoul(const char *s, char **end, int base) { return strtoull(s, end, base); }
intmax_t strtoimax(const char *s, char **end, int base) { return strtoll(s, end, base); }
uintmax_t strtoumax(const char *s, char **end, int base) { return strtoull(s, end, base); }

int atoi(const char *s) { return strtol(s, NULL, 10); }
long atol(const char *s) { return strtol(s, NULL, 10); }
long long atoll(const char *s) { return strtoll(s, NULL, 10); }
double atof(const char *s) { return strtod(s, NULL); }

int abs(int x) { return x < 0 ? -x : x; }
long labs(long x) { return x < 0 ? -x : x; }
long long llabs(long long x) { return x < 0 ? -x : x; }
div_t div(int a, int b) { return (div_t){a / b, a % b}; }
ldiv_t ldiv(long a, long b) { return (ldiv_t){a / b, a % b}; }
lldiv_t lldiv(long long a, long long b) { return (lldiv_t){a / b, a % b}; }

// A 64-bit linear congruential generator, whose high bits are its best ones.
static uint64_t rand_state = 1;

int rand(void) {
    rand_state = rand_state * 6364136223846793005ULL + 1;
    return rand_state >> 33;
}

void srand(unsigned seed) {
    rand_state = seed;
}

int rand_r(unsigned *state) {
    *state = *state * 1103515245u + 12345;
    return *state >> 1;
}

long random(void) {
    return rand();
}

void srandom(unsigned seed) {
    srand(seed);
}

#define RAND48_MULTIPLIER 0x5DEECE66DULL
#define RAND48_INCREMENT 0xB
#define RAND48_MASK ((1ULL << 48) - 1)

static uint64_t rand48_state = 0x1234ABCD330EULL;

static uint64_t next_rand48(void) {
    rand48_state = (rand48_state * RAND48_MULTIPLIER + RAND48_INCREMENT) & RAND48_MASK;
    return rand48_state;
}

double drand48(void) {
    return (double)next_rand48() / (double)(RAND48_MASK + 1);
}

long lrand48(void) {
    return next_rand48() >> 17;
}

void srand48(long seed) {
    rand48_state = ((uint64_t)seed << 16 | 0x330E) & RAND48_MASK;
}

static void swap_bytes(char *a, char *b, size_t size) {
    while (size--) {
        char t = *a;

        *a++ = *b;
        *b++ = t;
    }
}

static void sift_down(char *base, size_t root, size_t count, size_t size,
                      int (*compare)(const void *, const void *)) {
    for (;;) {
        size_t child = root * 2 + 1;

        if (child >= count)
            return;
        if (child + 1 < count && compare(base + child * size, base + (child + 1) * size) < 0)
            child++;
        if (compare(base + root * size, base + child * size) >= 0)
            return;
        swap_bytes(base + root * size, base + child * size, size);
        root = child;
    }
}

// Heapsort: no recursion, no allocation, and no bad inputs. It isn't stable.
void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *)) {
    for (size_t i = count / 2; i-- > 0;)
        sift_down(base, i, count, size, compare);
    for (size_t i = count; i-- > 1;) {
        swap_bytes(base, (char *)base + i * size, size);
        sift_down(base, 0, i, size, compare);
    }
}

void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *)) {
    const char *low = base;

    while (count > 0) {
        const char *middle = low + count / 2 * size;
        int order = compare(key, middle);

        if (order == 0)
            return (void *)middle;
        if (order > 0) {
            low = middle + size;
            count--;
        }
        count /= 2;
    }
    return NULL;
}
