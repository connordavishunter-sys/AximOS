// Runs the shell on a pseudo-terminal and checks what comes out of the master.
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;
static char seen[8192];
static size_t seen_len;

static void check(const char *name, bool ok) {
    printf("%s %s\n", ok ? "ok" : "FAIL", name);
    if (!ok)
        printf("    seen: %.*s\n", (int)seen_len, seen);
    failures += !ok;
}

// Reads from the master until text has come out, or two seconds pass.
static bool expect(int master, const char *text) {
    for (int tries = 0; tries < 200 && !memmem(seen, seen_len, text, strlen(text)); tries++) {
        struct pollfd p = {.fd = master, .events = POLLIN};

        if (poll(&p, 1, 10) > 0) {
            ssize_t n = read(master, seen + seen_len, sizeof(seen) - seen_len - 1);

            if (n <= 0)
                break;
            seen_len += n;
        }
    }
    return memmem(seen, seen_len, text, strlen(text));
}

static pid_t start(int *master, const char *command) {
    struct winsize size = {.ws_row = 30, .ws_col = 100};
    int slave;

    seen_len = 0;
    if (openpty(master, &slave, NULL, NULL, &size) < 0)
        return -1;

    pid_t child = fork();

    if (child == 0) {
        close(*master);
        login_tty(slave);
        execl("/system/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(slave);
    return child;
}

int main(void) {
    int master, status;
    pid_t child = start(&master, "echo out; read x; echo got $x; echo viatty > /dev/tty; "
                                 "[ -t 0 ] && echo isatty; trap 'echo hup; exit 3' HUP; read y");

    check("openpty", child > 0);
    check("output with newlines as \\r\\n", expect(master, "out\r\n"));
    write(master, "abd\x7f" "c\r", 6);
    check("echo and erase", expect(master, "abd\b \bc\r\n"));
    check("input reaches the program", expect(master, "got abc\r\n"));
    check("/dev/tty is the pseudo-terminal", expect(master, "viatty\r\n"));
    check("isatty", expect(master, "isatty\r\n"));
    close(master);
    check("closing the master hangs up", waitpid(child, &status, 0) == child && WIFEXITED(status)
                                             && (WEXITSTATUS(status) == 3 || WEXITSTATUS(status) == 1));

    child = start(&master, "trap 'echo winch' WINCH; echo ready; sleep 2; echo after");
    check("shell starts", expect(master, "ready"));
    struct winsize size = {.ws_row = 40, .ws_col = 120}, got;
    ioctl(master, TIOCSWINSZ, &size);
    check("size", ioctl(master, TIOCGWINSZ, &got) == 0 && got.ws_row == 40 && got.ws_col == 120);
    write(master, "\x03", 1);
    check("Ctrl+C ends the foreground group", expect(master, "^C"));
    check("Ctrl+C ended it", waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGINT);
    check("nothing ran after Ctrl+C", !memmem(seen, seen_len, "after", 5));

    child = start(&master, "echo bye");
    check("master reads the end once the slave closes", expect(master, "bye") && !expect(master, "never"));
    check("child exits", waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    close(master);

    printf("pty: %d failed\n", failures);
    return failures != 0;
}
