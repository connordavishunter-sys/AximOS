#pragma once

// These print the program's name, a message and, except for the x versions, the
// message for errno. err and errx then exit with the given status.
[[noreturn]] __attribute__((format(printf, 2, 3))) void err(int status, const char *fmt, ...);
[[noreturn]] __attribute__((format(printf, 2, 3))) void errx(int status, const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void warn(const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void warnx(const char *fmt, ...);
