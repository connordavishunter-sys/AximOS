// Prints the terminal's size, and with -w again each time it changes.
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static volatile sig_atomic_t changed;

static void show_size(void) {
    struct winsize size;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0)
        printf("rows %d cols %d\n", size.ws_row, size.ws_col);
    fflush(stdout);
}

static void on_winch(int signal) {
    (void)signal;
    changed = 1;
}

int main(int argc, char **argv) {
    bool watch = argc > 1 && strcmp(argv[1], "-w") == 0;

    signal(SIGWINCH, on_winch);
    show_size();
    while (watch) {
        pause();
        if (changed) {
            changed = 0;
            show_size();
        }
    }
    return 0;
}
