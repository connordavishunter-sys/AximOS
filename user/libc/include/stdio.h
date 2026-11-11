#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <syscall_numbers.h>

#define EOF (-1)
#define BUFSIZ 4096
#define L_tmpnam 32
#define TMP_MAX 238328
#define P_tmpdir "/tmp"
#define FILENAME_MAX 1024
#define FOPEN_MAX 64

typedef long fpos_t;

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

// stdout and stderr start unbuffered, with each printf written whole at the end of
// the call, so output can't be lost to a crash or be held back from a prompt.
// Use setvbuf for speed. Files opened with fopen are fully buffered, and exit
// flushes them, but being killed by a signal doesn't.
typedef struct FILE FILE;
extern FILE *stdin, *stdout, *stderr;

// Modes are r, w or a, with any of + b x.
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
int fflush(FILE *stream); // NULL flushes every stream
int setvbuf(FILE *stream, char *buffer, int mode, size_t size);
void setbuf(FILE *stream, char *buffer);
void setlinebuf(FILE *stream);
int fileno(FILE *stream);

size_t fread(void *dest, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *src, size_t size, size_t count, FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
int ungetc(int c, FILE *stream);
char *fgets(char *dest, int size, FILE *stream);
ssize_t getline(char **line, size_t *size, FILE *stream);
ssize_t getdelim(char **line, size_t *size, int delimiter, FILE *stream);
int fputc(int c, FILE *stream);
int putc(int c, FILE *stream);
int putchar(int c);
int fputs(const char *s, FILE *stream);
int puts(const char *s);

// Each stream has a lock, so a printf comes out whole between threads. These group
// several calls under it, and the _unlocked ones are the same as the plain ones.
void flockfile(FILE *stream);
void funlockfile(FILE *stream);
int ftrylockfile(FILE *stream);
#define fputs_unlocked fputs
#define fgets_unlocked fgets
#define fread_unlocked fread
#define fwrite_unlocked fwrite
#define fflush_unlocked fflush
#define ferror_unlocked ferror
#define feof_unlocked feof
#define clearerr_unlocked clearerr
#define fileno_unlocked fileno
int getc_unlocked(FILE *stream);
int getchar_unlocked(void);
int putc_unlocked(int c, FILE *stream);
int putchar_unlocked(int c);

// A temporary file, removed when closed or at exit, and a name for making one.
FILE *tmpfile(void);
char *tmpnam(char *out);

// Runs a command line in the shell with a pipe to its output, for mode "r", or its
// input, for "w". pclose waits for it and returns its status.
FILE *popen(const char *command, const char *mode);
int pclose(FILE *stream);

int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
int fseeko(FILE *stream, off_t offset, int whence);
off_t ftello(FILE *stream);
int fgetpos(FILE *stream, fpos_t *out);
int fsetpos(FILE *stream, const fpos_t *position);
void rewind(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);

// Supports %d %i %u %x %X %o %c %s %p %f %F %e %E %g %G %%, the flags - 0 + space
// and # (not on floats), a field width or *, a precision, and the length modifiers
// hh h l ll z j t L, though long double only has the precision of double.
__attribute__((format(printf, 1, 2)))
int printf(const char *fmt, ...);

__attribute__((format(printf, 2, 3)))
int fprintf(FILE *stream, const char *fmt, ...);

__attribute__((format(printf, 2, 3)))
int dprintf(int fd, const char *fmt, ...);

__attribute__((format(printf, 2, 3)))
int sprintf(char *dest, const char *fmt, ...);

__attribute__((format(printf, 3, 4)))
int snprintf(char *dest, size_t size, const char *fmt, ...);

// Allocates the result, to be freed by the caller.
__attribute__((format(printf, 2, 3)))
int asprintf(char **out, const char *fmt, ...);

int vprintf(const char *fmt, va_list args);
int vfprintf(FILE *stream, const char *fmt, va_list args);
int vdprintf(int fd, const char *fmt, va_list args);
int vsprintf(char *dest, const char *fmt, va_list args);
int vsnprintf(char *dest, size_t size, const char *fmt, va_list args);
int vasprintf(char **out, const char *fmt, va_list args);

// Supports %d %i %u %x %X %o %c %s %[ %p %n %f %F %e %E %g %G %%, assignment
// suppression with *, a width, and the length modifiers hh h l ll z j t L.
__attribute__((format(scanf, 1, 2)))
int scanf(const char *fmt, ...);

__attribute__((format(scanf, 2, 3)))
int fscanf(FILE *stream, const char *fmt, ...);

__attribute__((format(scanf, 2, 3)))
int sscanf(const char *s, const char *fmt, ...);

int vscanf(const char *fmt, va_list args);
int vfscanf(FILE *stream, const char *fmt, va_list args);
int vsscanf(const char *s, const char *fmt, va_list args);

// Prints a size with its unit, like "1.5 MiB", rounded down and right-aligned so
// that units line up in a column. A width of 10 fits every value below 10000 TiB.
void print_size(uint64_t bytes, int width);

// Writes "s: " and then the message for errno to stderr.
void perror(const char *s);

// Moves a file or directory on the same filesystem, replacing a file at new_path.
int rename(const char *old_path, const char *new_path);

// Deletes a file or an empty directory.
int remove(const char *path);
