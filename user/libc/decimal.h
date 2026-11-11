#pragma once

// Decimal numbers with exact digits, shared by printf and strtod. Digits sit in
// an array as plain 0 to 9, with the decimal point before POINT.
#define POINT 320
#define FRACTION_DIGITS 1080
#define DIGITS (POINT + FRACTION_DIGITS)

static inline void double_digits(char *d) {
    int carry = 0;

    for (int i = DIGITS - 1; i >= 0; i--) {
        int value = d[i] * 2 + carry;

        d[i] = value % 10;
        carry = value / 10;
    }
}

// Returns whether a bit fell off the end.
static inline bool halve_digits(char *d) {
    int remainder = 0;

    for (int i = 0; i < DIGITS; i++) {
        int value = d[i] + remainder * 10;

        d[i] = value / 2;
        remainder = value % 2;
    }
    return remainder;
}
