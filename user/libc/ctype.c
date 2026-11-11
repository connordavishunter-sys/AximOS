#include <ctype.h>

// Only ASCII: there are no locales.
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return isupper(c) || islower(c); }
int isalnum(int c) { return isalpha(c) || isdigit(c); }
int isblank(int c) { return c == ' ' || c == '\t'; }
int iscntrl(int c) { return (c >= 0 && c < ' ') || c == 0x7F; }
int isgraph(int c) { return c > ' ' && c < 0x7F; }
int isprint(int c) { return c >= ' ' && c < 0x7F; }
int ispunct(int c) { return isgraph(c) && !isalnum(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isxdigit(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
int tolower(int c) { return isupper(c) ? c - 'A' + 'a' : c; }
int toupper(int c) { return islower(c) ? c - 'a' + 'A' : c; }

int isascii(int c) {
    return c >= 0 && c < 0x80;
}

int toascii(int c) {
    return c & 0x7F;
}
