#pragma once

#include <stdint.h>

#include <alloca.h>
#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7FFFFFFF
#define MB_CUR_MAX 4

typedef struct { int quot, rem; } div_t;
typedef struct { long quot, rem; } ldiv_t;
typedef struct { long long quot, rem; } lldiv_t;

// malloc(0) returns NULL. An aligned_alloc block can be passed to free or realloc.
void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void *aligned_alloc(size_t alignment, size_t size);
void free(void *ptr);

[[noreturn]] void exit(int status);
[[noreturn]] void _Exit(int status);
[[noreturn]] void abort(void);
int atexit(void (*handler)(void));
int at_quick_exit(void (*handler)(void));
[[noreturn]] void quick_exit(int status);

// Replace the XXXXXX that end a template, and create the file or directory.
int mkstemp(char *template_path);
char *mkdtemp(char *template_path);
char *mktemp(char *template_path);
int clearenv(void);

// The absolute path with . and .. worked out, into out or a new allocation if NULL.
char *realpath(const char *path, char *out);

// Runs a command line in the shell, returning its exit status or -1.
int system(const char *command);

char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int putenv(char *entry);

int atoi(const char *s);
long atol(const char *s);
long long atoll(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
long long strtoll(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
unsigned long long strtoull(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
long double strtold(const char *s, char **end);

int abs(int x);
long labs(long x);
long long llabs(long long x);
div_t div(int a, int b);
ldiv_t ldiv(long a, long b);
lldiv_t lldiv(long long a, long long b);

int rand(void);
void srand(unsigned seed);
int rand_r(unsigned *state);
long random(void);
void srandom(unsigned seed);
double drand48(void);
long lrand48(void);
void srand48(long seed);

// Multibyte strings are UTF-8.
int mblen(const char *s, size_t len);
int mbtowc(wchar_t *out, const char *s, size_t len);
int wctomb(char *out, wchar_t c);
size_t mbstowcs(wchar_t *dest, const char *src, size_t len);
size_t wcstombs(char *dest, const wchar_t *src, size_t len);

void qsort(void *base, size_t count, size_t size, int (*compare)(const void *, const void *));
void *bsearch(const void *key, const void *base, size_t count, size_t size,
              int (*compare)(const void *, const void *));

// Random numbers good enough for keys, from the kernel. Unlike rand, these need no seed.
uint32_t arc4random(void);
void arc4random_buf(void *buf, size_t len);
uint32_t arc4random_uniform(uint32_t limit);
