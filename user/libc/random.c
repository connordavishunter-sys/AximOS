#include <errno.h>
#include <stdlib.h>
#include <sys/random.h>
#include "syscall.h"

ssize_t getrandom(void *buf, size_t len, unsigned flags) {
    long result = syscall3(SYS_GETRANDOM, (long)buf, len, flags);

    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

int getentropy(void *buf, size_t len) {
    if (len > 256) {
        errno = EIO;
        return -1;
    }

    size_t done = 0;

    while (done < len) {
        ssize_t got = getrandom((char *)buf + done, len - done, 0);

        if (got < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += got;
    }
    return 0;
}

// Asking the kernel every time leaves nothing behind for a fork to share.
void arc4random_buf(void *buf, size_t len) {
    size_t done = 0;

    while (done < len) {
        ssize_t got = getrandom((char *)buf + done, len - done, 0);

        if (got > 0)
            done += got;
    }
}

uint32_t arc4random(void) {
    uint32_t value;

    arc4random_buf(&value, sizeof(value));
    return value;
}

// Throws away the values that would favour the low end of the range.
uint32_t arc4random_uniform(uint32_t limit) {
    if (limit < 2)
        return 0;

    uint32_t floor = -limit % limit;
    uint32_t value;

    do
        value = arc4random();
    while (value < floor);
    return value % limit;
}
