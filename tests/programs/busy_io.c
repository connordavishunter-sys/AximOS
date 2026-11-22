// Keeps the disk busy writing and reading back a big file, a megabyte at a time and
// 4 KB at a time as cp does, while timing small things other programs do: looking
// at a file, reading /proc and /tmp, and starting a program. None should wait long.
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BIG (1024 * 1024)
#define FILE_SIZE (16 * BIG)
#define ROUNDS 40
#define LIMIT_MS 100

static char buffer[BIG];

static long now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static void churn(const char *path, int chunk) {
    for (;;) {
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);

        for (int done = 0; done < FILE_SIZE; done += chunk)
            write(fd, buffer, chunk);
        close(fd);
        fd = open(path, O_RDONLY);
        while (read(fd, buffer, chunk) > 0)
            ;
        close(fd);
    }
}

static void read_small(const char *path) {
    char line[256];
    int fd = open(path, O_RDONLY);

    read(fd, line, sizeof(line));
    close(fd);
}

static void run_echo(void) {
    pid_t pid = fork();

    if (!pid) {
        int null = open("/dev/null", O_WRONLY);

        dup2(null, 1);
        execl("/system/bin/echo", "echo", "hi", (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

int main(void) {
    const char *names[] = {"stat a file", "read /proc/uptime", "read /tmp", "start a program"};
    long worst[4] = {0};
    pid_t churners[2];
    struct stat st;

    memset(buffer, 'x', sizeof(buffer));
    int fd = open("/tmp/note", O_WRONLY | O_CREAT, 0644);
    write(fd, "note\n", 5);
    close(fd);

    for (int i = 0; i < 2; i++)
        if (!(churners[i] = fork()))
            churn(i ? "/home/big2" : "/home/big1", i ? 4096 : BIG);
    usleep(500000);

    long start = now_ms();
    for (int round = 0; round < ROUNDS; round++) {
        for (int op = 0; op < 4; op++) {
            long t = now_ms();

            if (op == 0)
                stat("/system/bin/ls", &st);
            else if (op == 1)
                read_small("/proc/uptime");
            else if (op == 2)
                read_small("/tmp/note");
            else
                run_echo();
            t = now_ms() - t;
            worst[op] = t > worst[op] ? t : worst[op];
        }
        usleep(20000);
    }
    long total = now_ms() - start;

    for (int i = 0; i < 2; i++) {
        kill(churners[i], SIGKILL);
        waitpid(churners[i], NULL, 0);
    }
    for (int op = 0; op < 4; op++)
        printf("%s %s, at worst %ld ms\n", worst[op] <= LIMIT_MS ? "ok" : "FAIL", names[op], worst[op]);
    printf("busy_io: done in %ld ms\n", total);
    unlink("/home/big1");
    unlink("/home/big2");
    return 0;
}
