#include <key_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHELL_PATH "/system/bin/dash"
#define WM_PATH "/system/bin/wm"
#define EXEC_FAILED 127

static char *const env[] = {"PATH=/system/bin:/system/bin/gui", "HOME=/home", "SHELL=" SHELL_PATH, "PS1=$PWD$ ", NULL};

// Runs a program in its own group, with the terminal to go with it, which it waits
// for otherwise, and collects any orphans until it's done.
static int run(const char *path, char *arg) {
    pid_t child = fork();

    if (child == 0) {
        setpgid(0, 0);
        tcsetpgrp(STDIN_FILENO, getpid());
        execve(path, (char *const[]){(char *)path, arg, NULL}, env);
        printf("init: failed to run %s\n", path);
        exit(EXEC_FAILED);
    }
    if (child < 0) {
        printf("init: fork failed\n");
        exit(1);
    }

    int status;
    while (wait(&status) != child)
        ;
    return status;
}

// Starts the desktop, behind its log-in screen, unless Shift is held, and keeps a shell running after it.
int main(void) {
    int modifiers = 0;

    ioctl(STDIN_FILENO, KEYBOARD_GET_MODIFIERS, &modifiers);
    if (modifiers & KEY_MOD_SHIFT)
        printf("init: Shift is held, so starting the shell\n");
    else
        run(WM_PATH, "--login");
    while (run(SHELL_PATH, NULL) != EXEC_FAILED)
        ;
    return 1;
}
