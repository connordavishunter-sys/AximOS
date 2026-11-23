#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// The first program Linux runs. It mounts what the kernel shares through files, runs
// the shell on the console, and collects every orphan. SIGUSR2 turns the computer off
// and SIGTERM restarts it, as busybox's init does, and Ctrl+Alt+Del sends SIGINT.

#define SHELL_PATH "/system/bin/dash"
#define TTYLOGIN_PATH "/system/bin/ttylogin"
#define EXEC_FAILED 127
#define STOP_WAIT_US 2000000 // for programs to finish before they're made to

static char *const env[] = {"PATH=/system/bin", "HOME=/root", "SHELL=" SHELL_PATH, "TERM=linux", "PS1=$PWD# ", NULL};
static volatile sig_atomic_t stop; // how the computer should stop, once asked

static void mount_all(void) {
    static const struct { const char *type, *at; } mounts[] = {
        {"proc", "/proc"}, {"sysfs", "/sys"}, {"devtmpfs", "/dev"}, {"devpts", "/dev/pts"},
        {"tmpfs", "/run"}, {"tmpfs", "/tmp"},
    };

    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        mkdir(mounts[i].at, 0755);
        if (mount(mounts[i].type, mounts[i].at, mounts[i].type, MS_NOSUID, NULL) < 0)
            fprintf(stderr, "init: can't mount %s\n", mounts[i].at);
    }
}

// The terminal the kernel writes its messages to last, which is the one on the
// command line's last console=, so the shell's where those are.
static void open_console(char *path, size_t size) {
    char names[128] = "console";
    FILE *f = fopen("/sys/class/tty/console/active", "r");

    if (f) {
        if (!fgets(names, sizeof(names), f))
            strcpy(names, "console");
        fclose(f);
    }
    names[strcspn(names, "\n")] = '\0';
    snprintf(path, size, "/dev/%s", strrchr(names, ' ') ? strrchr(names, ' ') + 1 : names);
}

// Runs a program in a session of its own, with the console for its terminal, and
// waits for it, collecting any orphans meanwhile.
static int run(const char *path, const char *console) {
    pid_t child = fork();
    int status;

    if (child == 0) {
        int fd = open(console, O_RDWR);

        setsid();
        if (fd >= 0) {
            ioctl(fd, TIOCSCTTY, 0);
            dup2(fd, 0), dup2(fd, 1), dup2(fd, 2);
            if (fd > 2)
                close(fd);
        }
        execve(path, (char *const[]){(char *)path, NULL}, env);
        fprintf(stderr, "init: can't run %s\n", path);
        _exit(EXEC_FAILED);
    }
    while (child > 0 && !stop) {
        pid_t done = wait(&status);

        if (done == child)
            return status;
    }
    return child > 0 ? 0 : EXEC_FAILED << 8;
}

static void asked(int signal) {
    stop = signal;
}

// Asks every program to finish, then makes them, and stops the computer.
static void shut_down(void) {
    printf("init: %s\n", stop == SIGUSR2 ? "turning off" : "restarting");
    kill(-1, SIGTERM);
    usleep(STOP_WAIT_US);
    kill(-1, SIGKILL);
    sync();
    reboot(stop == SIGUSR2 ? RB_POWER_OFF : RB_AUTOBOOT);
}

int main(void) {
    char console[64];
    int sigs[] = {SIGTERM, SIGUSR2, SIGINT};

    if (getpid() != 1) {
        fprintf(stderr, "init: only runs as the first program\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &(struct sigaction){.sa_handler = asked}, NULL); // no SA_RESTART, so wait stops
    reboot(RB_DISABLE_CAD); // Ctrl+Alt+Del comes here as SIGINT
    mount_all();
    sethostname("aximos", 6);
    open_console(console, sizeof(console));

    // ttylogin once it's there, and the shell without it, again each time it ends.
    while (!stop) {
        int status = run(access(TTYLOGIN_PATH, X_OK) == 0 ? TTYLOGIN_PATH : SHELL_PATH, console);

        if (!stop && WIFEXITED(status) && WEXITSTATUS(status) == EXEC_FAILED) {
            fprintf(stderr, "init: nothing to run, so stopping\n");
            stop = SIGUSR2;
        }
    }
    shut_down();
}
