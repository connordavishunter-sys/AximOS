#pragma once

#include <string.h>

int ffs(int value);
void bzero(void *dest, size_t len);
void bcopy(const void *src, void *dest, size_t len);
int bcmp(const void *a, const void *b, size_t len);
char *index(const char *s, int c);
char *rindex(const char *s, int c);
