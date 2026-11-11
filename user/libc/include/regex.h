#pragma once

#include <stddef.h>

// Flags for regcomp.
#define REG_EXTENDED 1
#define REG_ICASE 2
#define REG_NEWLINE 4
#define REG_NOSUB 8

// Flags for regexec. With REG_STARTEND, pmatch[0] gives the range to search, which
// may hold NUL bytes.
#define REG_NOTBOL 1
#define REG_NOTEOL 2
#define REG_STARTEND 4

// Errors.
#define REG_NOMATCH 1
#define REG_BADPAT 2
#define REG_ECOLLATE 3
#define REG_ECTYPE 4
#define REG_EESCAPE 5
#define REG_ESUBREG 6
#define REG_EBRACK 7
#define REG_EPAREN 8
#define REG_EBRACE 9
#define REG_BADBR 10
#define REG_ERANGE 11
#define REG_ESPACE 12
#define REG_BADRPT 13
#define REG_EEND 14
#define REG_ESIZE 15

#define RE_DUP_MAX 0x7FFF

typedef int regoff_t;

typedef struct {
    size_t re_nsub;
    void *compiled;
} regex_t;

typedef struct {
    regoff_t rm_so;
    regoff_t rm_eo;
} regmatch_t;

// The usual POSIX functions, on bytes, plus GNU's \w \W \s \S \b \B \< \> \` \' and,
// in basic expressions, \+ \? \|. The longest of the leftmost matches wins. A
// pattern with back references can take exponential time; others can't.
int regcomp(regex_t *regex, const char *pattern, int flags);
int regexec(const regex_t *regex, const char *text, size_t nmatch, regmatch_t *matches, int flags);
size_t regerror(int code, const regex_t *regex, char *out, size_t size);
void regfree(regex_t *regex);
