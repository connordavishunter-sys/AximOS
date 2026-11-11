#include <strings.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

void *memcpy(void *dest, const void *src, size_t n) {
    void *d = dest;

    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dest;
}

void *memmove(void *dest, const void *src, size_t n) {
    char *d = dest;
    const char *s = src;

    if (d < s)
        return memcpy(dest, src, n);
    while (n--)
        d[n] = s[n];
    return dest;
}

void *memset(void *dest, int value, size_t n) {
    void *d = dest;

    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(value) : "memory");
    return dest;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;

    for (; n--; x++, y++) {
        if (*x != *y)
            return *x - *y;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;

    while (s[len])
        len++;
    return len;
}

char *strcpy(char *dest, const char *src) {
    return memcpy(dest, src, strlen(src) + 1);
}

int strcmp(const char *a, const char *b) {
    return strncmp(a, b, (size_t)-1);
}

int strncmp(const char *a, const char *b, size_t n) {
    for (; n > 0; a++, b++, n--) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (*a == '\0')
            break;
    }
    return 0;
}

static int lower(int c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (; n > 0; a++, b++, n--) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);

        if (x != y)
            return x - y;
        if (x == '\0')
            break;
    }
    return 0;
}

void *memrchr(const void *s, int c, size_t n) {
    const unsigned char *bytes = s;

    while (n--)
        if (bytes[n] == (unsigned char)c)
            return (void *)&bytes[n];
    return NULL;
}

int strverscmp(const char *a, const char *b) {
    while (*a && *a == *b && !(*a >= '0' && *a <= '9')) {
        a++;
        b++;
    }
    if (*a < '0' || *a > '9' || *b < '0' || *b > '9') {
        if (*a != *b || !*a)
            return (unsigned char)*a - (unsigned char)*b;
    }

    // Both are at digits. Those with leading zeros sort as fractions, before the rest.
    bool a_fraction = *a == '0';
    bool b_fraction = *b == '0';

    if (a_fraction != b_fraction)
        return a_fraction ? -1 : 1;
    if (a_fraction) {
        bool past_zeros = false;

        while (*a && *a == *b && *a >= '0' && *a <= '9') {
            past_zeros |= *a != '0';
            a++;
            b++;
        }
        if ((*a < '0' || *a > '9') && (*b < '0' || *b > '9'))
            return *a || *b ? strverscmp(a, b) : 0;
        // More zeros sort first, but past them the shorter fraction does.
        if (*a < '0' || *a > '9')
            return past_zeros ? -1 : 1;
        if (*b < '0' || *b > '9')
            return past_zeros ? 1 : -1;
        return *a - *b;
    }

    size_t a_digits = 0;
    size_t b_digits = 0;

    while (a[a_digits] >= '0' && a[a_digits] <= '9')
        a_digits++;
    while (b[b_digits] >= '0' && b[b_digits] <= '9')
        b_digits++;
    if (a_digits != b_digits)
        return a_digits < b_digits ? -1 : 1;

    int order = strncmp(a, b, a_digits);

    return order ? order : strverscmp(a + a_digits, b + b_digits);
}

int strcasecmp(const char *a, const char *b) {
    return strncasecmp(a, b, (size_t)-1);
}

void *memchr(const void *s, int c, size_t n) {
    const unsigned char *p = s;

    for (; n--; p++) {
        if (*p == (unsigned char)c)
            return (void *)p;
    }
    return NULL;
}

size_t strnlen(const char *s, size_t max) {
    size_t len = 0;

    while (len < max && s[len])
        len++;
    return len;
}

// Pads the rest of dest with zeros, and leaves it unterminated if src doesn't fit.
char *strncpy(char *dest, const char *src, size_t n) {
    size_t len = strnlen(src, n);

    memcpy(dest, src, len);
    memset(dest + len, 0, n - len);
    return dest;
}

char *stpcpy(char *dest, const char *src) {
    size_t len = strlen(src);

    memcpy(dest, src, len + 1);
    return dest + len;
}

char *strcat(char *dest, const char *src) {
    strcpy(dest + strlen(dest), src);
    return dest;
}

char *strncat(char *dest, const char *src, size_t n) {
    char *end = dest + strlen(dest);
    size_t len = strnlen(src, n);

    memcpy(end, src, len);
    end[len] = '\0';
    return dest;
}

char *strdup(const char *s) {
    return strndup(s, (size_t)-1);
}

char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char *copy = malloc(len + 1);

    if (copy) {
        memcpy(copy, s, len);
        copy[len] = '\0';
    }
    return copy;
}

char *strstr(const char *haystack, const char *needle) {
    size_t len = strlen(needle);

    for (;; haystack++) {
        if (strncmp(haystack, needle, len) == 0)
            return (char *)haystack;
        if (*haystack == '\0')
            return NULL;
    }
}

size_t strspn(const char *s, const char *accept) {
    size_t len = 0;

    while (s[len] && strchr(accept, s[len]))
        len++;
    return len;
}

size_t strcspn(const char *s, const char *reject) {
    size_t len = 0;

    while (s[len] && !strchr(reject, s[len]))
        len++;
    return len;
}

char *strpbrk(const char *s, const char *accept) {
    s += strcspn(s, accept);
    return *s ? (char *)s : NULL;
}

char *strtok_r(char *s, const char *delimiters, char **state) {
    if (!s)
        s = *state;

    s += strspn(s, delimiters);
    if (*s == '\0') {
        *state = s;
        return NULL;
    }

    char *end = s + strcspn(s, delimiters);

    *state = *end ? end + 1 : end;
    *end = '\0';
    return s;
}

char *strtok(char *s, const char *delimiters) {
    static char *state;

    return strtok_r(s, delimiters, &state);
}

// Unlike strtok, this returns empty tokens between adjacent delimiters.
char *strsep(char **state, const char *delimiters) {
    char *s = *state;

    if (s) {
        char *end = s + strcspn(s, delimiters);

        *state = *end ? end + 1 : NULL;
        *end = '\0';
    }
    return s;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
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

char *strerror(int error) {
    switch (error) {
    case 0: return "No error";
    case EPERM: return "Operation not permitted";
    case ENOENT: return "No such file or directory";
    case ESRCH: return "No such process";
    case EINTR: return "Interrupted system call";
    case EIO: return "Input/output error";
    case ENXIO: return "No such device or address";
    case E2BIG: return "Argument list too long";
    case ENOEXEC: return "Exec format error";
    case EBADF: return "Bad file descriptor";
    case ECHILD: return "No child processes";
    case ENOMEM: return "Out of memory";
    case EACCES: return "Permission denied";
    case EFAULT: return "Bad address";
    case EBUSY: return "Device or resource busy";
    case EEXIST: return "File exists";
    case EXDEV: return "Invalid cross-device link";
    case EAGAIN: return "Resource temporarily unavailable";
    case EDOM: return "Numerical argument out of domain";
    case ENODEV: return "No such device";
    case ENFILE: return "Too many open files in system";
    case ENOTTY: return "Inappropriate ioctl for device";
    case EMLINK: return "Too many links";
    case ELOOP: return "Too many levels of symbolic links";
    case EOVERFLOW: return "Value too large for defined data type";
    case EILSEQ: return "Invalid or incomplete multibyte or wide character";
    case ETIMEDOUT: return "Connection timed out";
    case EDEADLK: return "Resource deadlock avoided";
    case ENOTSOCK: return "Socket operation on non-socket";
    case EDESTADDRREQ: return "Destination address required";
    case EMSGSIZE: return "Message too long";
    case EPROTOTYPE: return "Protocol wrong type for socket";
    case ENOPROTOOPT: return "Protocol not available";
    case EPROTONOSUPPORT: return "Protocol not supported";
    case ESOCKTNOSUPPORT: return "Socket type not supported";
    case EAFNOSUPPORT: return "Address family not supported by protocol";
    case EADDRINUSE: return "Address already in use";
    case EADDRNOTAVAIL: return "Cannot assign requested address";
    case ECONNABORTED: return "Software caused connection abort";
    case ECONNRESET: return "Connection reset by peer";
    case ENOBUFS: return "No buffer space available";
    case EISCONN: return "Transport endpoint is already connected";
    case ENOTCONN: return "Transport endpoint is not connected";
    case ECONNREFUSED: return "Connection refused";
    case EALREADY: return "Operation already in progress";
    case EINPROGRESS: return "Operation now in progress";
    case ENOTDIR: return "Not a directory";
    case EISDIR: return "Is a directory";
    case EINVAL: return "Invalid argument";
    case EMFILE: return "Too many open files";
    case EFBIG: return "File too large";
    case ENOSPC: return "No space left on device";
    case ESPIPE: return "Illegal seek";
    case EROFS: return "Read-only file system";
    case EPIPE: return "Broken pipe";
    case ERANGE: return "Result too large";
    case ENAMETOOLONG: return "File name too long";
    case ENOSYS: return "Function not implemented";
    case ENOTSUP: return "Operation not supported";
    case ENOTEMPTY: return "Directory not empty";
    default: return "Unknown error";
    }
}

int strerror_r(int error, char *out, size_t size) {
    return strlcpy(out, strerror(error), size) < size ? 0 : ERANGE;
}

void *memccpy(void *dest, const void *src, int c, size_t n) {
    char *d = dest;
    const char *s = src;

    for (size_t i = 0; i < n; i++) {
        d[i] = s[i];
        if ((unsigned char)s[i] == (unsigned char)c)
            return d + i + 1;
    }
    return NULL;
}

void *mempcpy(void *dest, const void *src, size_t n) {
    return (char *)memcpy(dest, src, n) + n;
}

void *memmem(const void *haystack, size_t haystack_len, const void *needle, size_t needle_len) {
    const char *h = haystack;

    if (needle_len == 0)
        return (void *)h;
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(h + i, needle, needle_len) == 0)
            return (void *)(h + i);
    }
    return NULL;
}

char *stpncpy(char *dest, const char *src, size_t n) {
    size_t len = strnlen(src, n);

    memcpy(dest, src, len);
    memset(dest + len, 0, n - len);
    return dest + len;
}

char *strchrnul(const char *s, int c) {
    char *found = strchr(s, c);

    return found ? found : (char *)s + strlen(s);
}

char *strcasestr(const char *s, const char *needle) {
    size_t len = strlen(needle);

    for (;; s++) {
        if (strncasecmp(s, needle, len) == 0)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

size_t strlcpy(char *dest, const char *src, size_t size) {
    size_t len = strlen(src);

    if (size > 0) {
        size_t copied = len < size - 1 ? len : size - 1;

        memcpy(dest, src, copied);
        dest[copied] = '\0';
    }
    return len;
}

size_t strlcat(char *dest, const char *src, size_t size) {
    size_t used = strnlen(dest, size);

    return used == size ? size + strlen(src) : used + strlcpy(dest + used, src, size - used);
}

int strcoll(const char *a, const char *b) {
    return strcmp(a, b);
}

size_t strxfrm(char *dest, const char *src, size_t n) {
    return n ? strlcpy(dest, src, n) : strlen(src);
}

int ffs(int value) {
    return __builtin_ffs(value);
}

void bzero(void *dest, size_t len) {
    memset(dest, 0, len);
}

void bcopy(const void *src, void *dest, size_t len) {
    memmove(dest, src, len);
}

int bcmp(const void *a, const void *b, size_t len) {
    return memcmp(a, b, len);
}

char *index(const char *s, int c) {
    return strchr(s, c);
}

char *rindex(const char *s, int c) {
    return strrchr(s, c);
}
