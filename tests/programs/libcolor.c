// Needed by libshape.so, so the program gets it second-hand.
#include <string.h>

const char *shape_hook(void);

const char *color_name(void) {
    return strcmp(shape_hook(), "program") == 0 ? "red, hook from the program" : "red, hook from libshape";
}
