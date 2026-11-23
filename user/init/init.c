#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// The first program Linux runs. It mounts what the kernel shares through files, checks
// the disk it runs from, if it's from one, and starts the services in SERVICES, one of
// which is the desktop, on the screen's first console, and keeps a text login on the
// others and on the serial port, if there's one. SIGUSR2 turns the computer off and
// SIGTERM restarts it, as busybox's init does, and Ctrl+Alt+Del sends SIGINT.
//
// Each service is a file of lines: "command" and what to run, "after" and services
// that start first, and "once" for one that runs to the end before those after it
// start, or "restart" for one that's started again if it stops.

#define SERVICES "/system/etc/services"
#define LOGIN_PATH "/system/bin/ttylogin"
#define SHELL_PATH "/system/bin/dash"
#define MAX_SERVICES 64
#define MAX_TEXT 512
#define STOP_WAIT_US 2000000 // for programs to finish before they're made to
#define TOO_SOON 5           // seconds: one that stops sooner than this...
#define GIVE_UP 5            // ...this many times running isn't started again

struct service {
    char name[64], command[MAX_TEXT], after[MAX_TEXT], console[32];
    bool once, restart, started;
    pid_t pid;
    time_t since;
    int quick_stops;
};

static char *const env[] = {"PATH=/system/bin", "HOME=/root", "SHELL=" SHELL_PATH, "TERM=linux", NULL};
static struct service services[MAX_SERVICES];
static int count;
static volatile sig_atomic_t stop; // how the computer should stop, once asked

static void mount_all(void) {
    static const struct { const char *type, *at; } mounts[] = {
        {"proc", "/proc"}, {"sysfs", "/sys"}, {"devtmpfs", "/dev"}, {"devpts", "/dev/pts"}, {"tmpfs", "/dev/shm"},
        {"tmpfs", "/run"}, {"tmpfs", "/tmp"},
    };

    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); i++) {
        mkdir(mounts[i].at, 0755);
        if (mount(mounts[i].type, mounts[i].at, mounts[i].type, MS_NOSUID, NULL) < 0)
            fprintf(stderr, "init: can't mount %s\n", mounts[i].at);
    }
    // The firmware's variables, there if it started in UEFI, as efiboot changes
    if (access("/sys/firmware/efi/efivars", F_OK) == 0
        && mount("efivarfs", "/sys/firmware/efi/efivars", "efivarfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) < 0)
        fprintf(stderr, "init: can't mount the firmware's variables\n");
}

// Runs a program to the end, on the kernel's console, giving how it exited.
static int run_program(char *const argv[]) {
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        execve(argv[0], argv, env);
        _exit(127);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) ? WEXITSTATUS(status) : 127;
}

// On a disk, the kernel mounts the root read-only, so e2fsck can check it, and fix
// what it safely can, before it's written to. From the initramfs, it's in memory.
static void check_root(void) {
    struct stat st;
    char path[64], line[128], device[80] = "";
    FILE *f;

    if (stat("/", &st) < 0 || major(st.st_dev) == 0)
        return;
    snprintf(path, sizeof(path), "/sys/dev/block/%u:%u/uevent", major(st.st_dev), minor(st.st_dev));
    for (f = fopen(path, "r"); f && fgets(line, sizeof(line), f);) {
        if (!strncmp(line, "DEVNAME=", 8))
            snprintf(device, sizeof(device), "/dev/%.*s", (int)strcspn(line + 8, "\n"), line + 8);
    }
    if (f)
        fclose(f);

    int checked = device[0] ? run_program((char *const[]){"/system/bin/e2fsck", "-p", device, NULL}) : 0;

    if (checked & 2) { // fixed, but the kernel has the old version of what it read
        printf("init: e2fsck fixed %s, so restarting\n", device);
        sync();
        reboot(RB_AUTOBOOT);
    }
    if (checked >= 4)
        fprintf(stderr, "init: %s has errors e2fsck -p couldn't fix, so it's left read-only\n", device);
    else if (mount(NULL, "/", NULL, MS_REMOUNT, NULL) < 0)
        fprintf(stderr, "init: can't make %s writable\n", device);
}

// The first line of a small file, or "" without it.
static void read_line(const char *path, char *out, size_t size) {
    FILE *f = fopen(path, "r");

    out[0] = '\0';
    if (f && fgets(out, size, f))
        out[strcspn(out, "\n")] = '\0';
    if (f)
        fclose(f);
}

static void write_line(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

static void set_up(void) {
    char name[64];

    read_line("/etc/hostname", name, sizeof(name));
    sethostname(name[0] ? name : "aximos", strlen(name[0] ? name : "aximos"));
    write_line("/proc/sys/net/ipv4/ping_group_range", "0 2147483647"); // ping for everyone
}

static void add_service(const char *name, const char *path) {
    FILE *f = fopen(path, "r");
    char line[MAX_TEXT + 16];
    struct service *s = &services[count];

    if (!f || count == MAX_SERVICES)
        return;
    *s = (struct service){0};
    snprintf(s->name, sizeof(s->name), "%s", name);
    while (fgets(line, sizeof(line), f)) {
        char *value = line + strcspn(line, " \t\n");

        value += strspn(value, " \t");
        value[strcspn(value, "\n")] = '\0';
        if (!strncmp(line, "command", 7))
            snprintf(s->command, sizeof(s->command), "%s", value);
        else if (!strncmp(line, "after", 5))
            snprintf(s->after, sizeof(s->after), "%s", value);
        s->once |= !strncmp(line, "once", 4);
        s->restart |= !strncmp(line, "restart", 7);
    }
    fclose(f);
    count += s->command[0] != '\0';
}

// A login on each console there is: the screen's, from the second, as the desktop
// has the first, and the serial port, which is there only if the kernel found one.
static void add_console(const char *tty) {
    char path[64], type[16] = "1";
    struct service *s = &services[count];

    snprintf(path, sizeof(path), "/sys/class/tty/%s/type", tty);
    if (strncmp(tty, "ttyS", 4) == 0)
        read_line(path, type, sizeof(type));
    if (atoi(type) == 0 || count == MAX_SERVICES)
        return;
    *s = (struct service){.restart = true};
    snprintf(s->name, sizeof(s->name), "login on %s", tty);
    snprintf(s->command, sizeof(s->command), "%s", access(LOGIN_PATH, X_OK) == 0 ? LOGIN_PATH : SHELL_PATH);
    snprintf(s->console, sizeof(s->console), "/dev/%s", tty);
    count++;
}

static void read_services(void) {
    DIR *dir = opendir(SERVICES);
    struct dirent *e;
    char path[300];

    while (dir && (e = readdir(dir))) {
        if (e->d_name[0] != '.') {
            snprintf(path, sizeof(path), SERVICES "/%s", e->d_name);
            add_service(e->d_name, path);
        }
    }
    if (dir)
        closedir(dir);
    for (const char *const *tty = (const char *const[]){"tty2", "tty3", "tty4", "tty5", "tty6", "ttyS0", NULL}; *tty; tty++)
        add_console(*tty);
}

// A login runs in a session of its own, with its console as its terminal. Services
// get /dev/null, and write to the kernel's console.
static pid_t start(struct service *s) {
    pid_t pid = fork();

    if (pid == 0) {
        int in = open(s->console[0] ? s->console : "/dev/null", O_RDWR), out = s->console[0] ? in : open("/dev/console", O_WRONLY);
        sigset_t none;

        sigemptyset(&none);
        sigprocmask(SIG_SETMASK, &none, NULL);
        setsid();
        if (s->console[0])
            ioctl(in, TIOCSCTTY, 0);
        dup2(in, 0), dup2(out, 1), dup2(out, 2);
        if (in > 2)
            close(in);
        if (out > 2 && out != in)
            close(out);
        execve("/system/bin/sh", (char *const[]){"sh", "-c", s->command, NULL}, env);
        _exit(127);
    }
    s->pid = pid;
    s->since = time(NULL);
    s->started = true;
    return pid;
}

static struct service *find(const char *name) {
    for (int i = 0; i < count; i++) {
        if (!strcmp(services[i].name, name))
            return &services[i];
    }
    return NULL;
}

// Whether those it comes after have started, and the ones run once have finished.
// Logins come after every service that's run once, as those set the system up.
static bool ready(const struct service *s) {
    char after[MAX_TEXT];

    for (int i = 0; s->console[0] && i < count; i++) {
        if (services[i].once && (!services[i].started || services[i].pid))
            return false;
    }

    snprintf(after, sizeof(after), "%s", s->after);
    for (char *name = strtok(after, " "); name; name = strtok(NULL, " ")) {
        struct service *before = find(name);

        if (before && (!before->started || (before->once && before->pid)))
            return false;
    }
    return true;
}

static struct service *of(pid_t pid) {
    for (int i = 0; i < count; i++) {
        if (services[i].pid == pid)
            return &services[i];
    }
    return NULL;
}

// Starts what's ready, then waits for something to stop, until there's nothing left
// to start and the computer's asked to stop.
static void run(void) {
    while (!stop) {
        for (int i = 0; i < count; i++) {
            if (!services[i].started && ready(&services[i]))
                start(&services[i]);
        }

        int status;
        pid_t pid = wait(&status);
        struct service *s = pid > 0 ? of(pid) : NULL;

        if (pid < 0 && errno == ECHILD)
            pause(); // nothing running, until asked to stop

        if (!s)
            continue; // an orphan, or a signal
        s->pid = 0;
        if (s->once && (!WIFEXITED(status) || WEXITSTATUS(status)))
            fprintf(stderr, "init: %s failed\n", s->name);
        if (!s->restart || stop)
            continue;
        s->quick_stops = time(NULL) - s->since < TOO_SOON ? s->quick_stops + 1 : 0;
        if (s->quick_stops < GIVE_UP)
            start(s);
        else
            fprintf(stderr, "init: %s keeps stopping, so it's left stopped\n", s->name);
    }
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
    int signals[] = {SIGTERM, SIGUSR2, SIGINT};

    if (getpid() != 1) {
        fprintf(stderr, "init: only runs as the first program\n");
        return 1;
    }
    for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
        sigaction(signals[i], &(struct sigaction){.sa_handler = asked}, NULL); // no SA_RESTART, so wait stops
    reboot(RB_DISABLE_CAD); // Ctrl+Alt+Del comes here as SIGINT
    mount_all();
    check_root();
    set_up();
    read_services();
    run();
    shut_down();
}
