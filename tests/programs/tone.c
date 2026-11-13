// Plays a 1000 Hz tone, left, and 500 Hz, right, for a second, at the rate given.
#include <audio_types.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    int rate = argc > 1 ? atoi(argv[1]) : 48000, fd = open("/dev/audio", O_WRONLY), delay = -1;
    static short samples[2 * 192000];

    if (fd < 0 || ioctl(fd, AUDIO_SET_RATE, &rate) < 0) {
        printf("tone: can't play at %d\n", rate);
        return 1;
    }
    for (int i = 0; i < rate; i++) {
        samples[2 * i] = (short)(12000 * sin(2 * M_PI * 1000 * i / rate));
        samples[2 * i + 1] = (short)(12000 * sin(2 * M_PI * 500 * i / rate));
    }
    printf("tone: wrote %ld\n", (long)write(fd, samples, rate * 4));
    ioctl(fd, AUDIO_GET_DELAY, &delay);
    printf("tone: delay %d\n", delay);
    close(fd);
    printf("tone: done\n");
    return 0;
}
