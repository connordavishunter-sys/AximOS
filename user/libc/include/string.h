#pragma once

#include <stddef.h>

void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
void *memset(void *dest, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);

size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
char *stpcpy(char *dest, const char *src);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t n);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
void *memrchr(const void *s, int c, size_t n);
int strcmp(const char *a, const char *b);

// Compares with runs of digits ordered by value, so file9 comes before file10.
int strverscmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strtok(char *s, const char *delimiters);
char *strtok_r(char *s, const char *delimiters, char **state);
char *strsep(char **state, const char *delimiters);
char *strerror(int error);
int strerror_r(int error, char *out, size_t size);
void *memccpy(void *dest, const void *src, int c, size_t n);
void *mempcpy(void *dest, const void *src, size_t n);
void *memmem(const void *haystack, size_t haystack_len, const void *needle, size_t needle_len);
char *stpncpy(char *dest, const char *src, size_t n);
char *strchrnul(const char *s, int c);
char *strcasestr(const char *s, const char *needle);
size_t strlcpy(char *dest, const char *src, size_t size);
size_t strlcat(char *dest, const char *src, size_t size);

// There's only the C locale, so these go by byte value.
int strcoll(const char *a, const char *b);
size_t strxfrm(char *dest, const char *src, size_t n);
