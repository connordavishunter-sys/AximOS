// Linked against a library that isn't installed, so the dynamic linker stops it.
#include <stdio.h>

extern int missing_value;

int main(void) {
    printf("should not run %d\n", missing_value);
    return 0;
}
