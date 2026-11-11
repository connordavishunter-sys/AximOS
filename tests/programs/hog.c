// Takes memory a megabyte at a time until there's none left, then gives it back.
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define CHUNK (1024 * 1024)

int main(void) {
    int megabytes = 0;
    void *chunk;

    while ((chunk = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED) {
        memset(chunk, 1, CHUNK);
        megabytes++;
    }
    printf("hog: %d MB\n", megabytes);
    return 0;
}
