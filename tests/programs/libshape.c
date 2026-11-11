// A library linked into libs.c, with data, a constructor and a destructor.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *color_name(void);

int shape_count = 3;
int shape_ready;
const char shape_title[] = "shapes";

__attribute__((constructor)) static void start(void) {
    shape_ready = shape_count;
}

__attribute__((destructor)) static void stop(void) {
    printf("libshape destructor ran\n");
}

// The program has one of these too, and the program's wins.
const char *shape_hook(void) {
    return "libshape";
}

char *describe(int sides) {
    char *text = malloc(64);

    snprintf(text, 64, "%d sides, %s", sides, color_name());
    return text;
}
