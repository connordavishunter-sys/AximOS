// Uses the stack well past what's mapped at the start, and lets the kernel write there.
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int depth(int n) {
    volatile char frame[1024];

    frame[0] = 1;
    return n == 0 ? 0 : depth(n - 1) + frame[0];
}

static int forever(int n) {
    volatile char frame[1024];

    frame[0] = (char)n;
    return forever(n + 1) + frame[0];
}

int main(int argc, char **argv) {
    if (argc > 1)
        return forever(0);

    char big[4 * 1024 * 1024];
    int fds[2];

    // The kernel writes into stack pages nothing has touched yet.
    pipe(fds);
    write(fds[1], "pipe", 4);
    printf("%s read into untouched stack\n", read(fds[0], big + 3 * 1024 * 1024, 4) == 4
           && memcmp(big + 3 * 1024 * 1024, "pipe", 4) == 0 ? "ok" : "FAIL");

    memset(big, 7, sizeof(big));
    printf("%s 4 MB on the stack\n", big[0] == 7 && big[sizeof(big) - 1] == 7 ? "ok" : "FAIL");
    printf("%s 3000 deep recursion\n", depth(3000) == 3000 ? "ok" : "FAIL");

    // A child gets the parent's touched pages and fresh ones of its own.
    pid_t child = fork();

    if (child == 0)
        _exit(big[12345] == 7 && depth(2000) == 2000 ? 0 : 1);

    int status;

    waitpid(child, &status, 0);
    printf("%s fork copies the stack\n", WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "ok" : "FAIL");
    printf("stack: done\n");
    return 0;
}
