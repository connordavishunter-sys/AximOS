#pragma once

#include <stddef.h>

// Converts between UTF-8, UTF-16 and UTF-32 either way round, ASCII, ISO-8859-1
// and Windows-1252. Names are matched ignoring case, and //TRANSLIT or //IGNORE
// after them is allowed but changes nothing.
typedef struct iconv_state *iconv_t;

iconv_t iconv_open(const char *to, const char *from);
size_t iconv(iconv_t cd, char **in, size_t *in_left, char **out, size_t *out_left);
int iconv_close(iconv_t cd);
