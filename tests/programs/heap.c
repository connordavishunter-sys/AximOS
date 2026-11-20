// Checks malloc and friends: zero-byte requests, and a long run of random allocations,
// resizes and frees whose contents must all survive.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 2000
#define ROUNDS 200000

static unsigned seed = 1;

static unsigned next(void) {
    seed = seed * 1103515245 + 12345;
    return seed >> 8;
}

int main(void) {
    void *a = malloc(0), *b = malloc(0), *c = calloc(0, 8);

    printf("%s malloc(0) and calloc(0, n) give blocks of their own\n", a && b && c && a != b ? "ok" : "FAIL");
    free(a), free(b), free(c);

    static unsigned char *block[SLOTS];
    static size_t size[SLOTS];
    bool intact = true;

    for (int round = 0; round < ROUNDS && intact; round++) {
        int i = next() % SLOTS;

        // Each block is filled with its slot's number, which is checked before it goes.
        for (size_t j = 0; j < size[i]; j++)
            intact &= block[i][j] == (unsigned char)i;
        if (!intact)
            break;
        size_t want = next() % 4 ? next() % 256 : next() % 70000;

        if (next() % 3 == 0 && block[i]) {
            unsigned char *moved = realloc(block[i], want ? want : 1);

            if (!moved) {
                intact = false;
                break;
            }
            // What it held, as far as the new size, comes with it.
            for (size_t j = 0; j < size[i] && j < want; j++)
                intact &= moved[j] == (unsigned char)i;
            block[i] = moved;
        } else {
            free(block[i]);
            block[i] = malloc(want);
        }
        if (!block[i]) {
            intact = false;
            break;
        }
        size[i] = want;
        memset(block[i], i, size[i]);
        intact &= ((uintptr_t)block[i] & 15) == 0;
    }
    printf("%s %d random allocations keep what's in them, 16-byte aligned\n", intact ? "ok" : "FAIL", ROUNDS);
    for (int i = 0; i < SLOTS; i++)
        free(block[i]);
    printf("heap: done\n");
    return 0;
}
