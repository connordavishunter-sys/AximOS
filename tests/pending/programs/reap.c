// A child's last thread, not its first, exits while closing /dev/audio waits for
// what it wrote to play, as the player's does when logging out stops it, and the
// parent asks after it all the while.
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static void *play(void *unused) {
    static char silence[48000]; // about a quarter of a second
    int fd = open("/dev/audio", O_WRONLY);

    (void)unused;
    if (fd < 0)
        exit(2);
    for (int i = 0; i < 4; i++)
        write(fd, silence, sizeof(silence));
    exit(0); // the first thread's gone, so closing it waits in this one
}

int main(void) {
    for (int round = 0; round < 5; round++) {
        pid_t child = fork();

        if (child == 0) {
            pthread_t t;

            pthread_create(&t, NULL, play, NULL);
            pthread_exit(NULL);
        }

        int status = 0, asked = 0;

        while (waitpid(child, &status, WNOHANG) == 0) {
            nanosleep(&(struct timespec){0, 200000}, NULL);
            asked++;
        }
        printf("reap: round %d collected status %d after asking %d times\n", round, WEXITSTATUS(status), asked);
    }
    printf("reap: done\n");
    return 0;
}
