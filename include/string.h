#pragma once

#include <stddef.h>

// The compiler may emit calls to these, so they must exist even if unused.
void *memmove(void *dest, const void *src, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *dest, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
char *strrchr(const char *s, int c);
