#include <key_event.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHELL_PATH "/system/bin/dash"
#define TTYLOGIN_PATH "/system/bin/ttylogin"
#define WM_PATH "/system/bin/wm"
#define EXEC_FAILED 127

// The apps are in /system/bin/gui and its folders, as the start menu shows them.
static char *const env[] = {"PATH=/system/bin:/system/bin/gui:/system/bin/gui/Accessories:/system/bin/gui/Games:"
                            "/system/bin/gui/Multimedia:/system/bin/gui/System:/system/bin/gui/Demos",
                            "HOME=/home", "SHELL=" SHELL_PATH, "PS1=$PWD$ ", NULL};

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

// The kernel's word that the power button was pressed.
static void power_button(int signal) {
    (void)signal;
    reboot(RB_POWER_OFF);
}

// Starts the desktop, behind its log-in screen, unless Shift is held, and keeps a
// shell running after it, behind ttylogin, which asks for the same account.
// Started from the install disc, which can't be written to, there's no account to
// make, so the desktop starts with the installer and the shell after it.
int main(void) {
    int modifiers = 0;
    struct statfs fs;
    bool disc = statfs("/", &fs) == 0 && !strcmp(fs.f_type, "iso9660");

    signal(SIGPWR, power_button);
    ioctl(STDIN_FILENO, KEYBOARD_GET_MODIFIERS, &modifiers);
    if (modifiers & KEY_MOD_SHIFT)
        printf("init: Shift is held, so starting the shell\n");
    else
        run(WM_PATH, disc ? "install" : "--login");
    while (disc && run(SHELL_PATH, NULL) != EXEC_FAILED)
        ;
    while (run(TTYLOGIN_PATH, NULL) != EXEC_FAILED)
        ;
    while (run(SHELL_PATH, NULL) != EXEC_FAILED) // without ttylogin
        ;
    return 1;
}
