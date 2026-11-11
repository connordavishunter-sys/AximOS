#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define FILE_MAX 2048
#define MAX_PROCESSES 256
#define COMMAND_MAX 64
#define MAX_CPUS 64
#define CLEAR_TO_END_OF_LINE "\x1b[K"

struct entry {
    int pid;
    char state;
    int threads;
    long ticks;
    long previous_ticks;
    long memory_kb;
    int percent_tenths;
    char command[COMMAND_MAX];
};

static struct entry entries[MAX_PROCESSES], previous[MAX_PROCESSES];
static int entry_count, previous_count;
static long cpu_idle[MAX_CPUS], cpu_idle_before[MAX_CPUS];
static long ticks, ticks_before;
static int rate = 100, cpus;
static struct termios original;
static bool on_terminal;

static bool read_text(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return false;

    ssize_t got = read(fd, out, size - 1);

    close(fd);
    if (got < 0)
        return false;
    out[got] = '\0';
    return true;
}

static const char *field(const char *text, const char *name) {
    size_t len = strlen(name);

    for (const char *line = text; line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        if (strncmp(line, name, len) == 0 && line[len] == ':')
            return line + len + 2;
    }
    return "";
}

static void restore_terminal(void) {
    if (on_terminal)
        tcsetattr(STDIN_FILENO, TCSANOW, &original);
    printf("\x1b[?25h");
    fflush(stdout);
}

static void on_signal(int signal) {
    restore_terminal();
    _exit(128 + signal);
}

static void read_system(void) {
    static char text[FILE_MAX];

    memcpy(cpu_idle_before, cpu_idle, sizeof(cpu_idle));
    ticks_before = ticks;
    if (!read_text("/proc/stat", text, sizeof(text)))
        return;

    rate = atoi(field(text, "ticks_per_second"));
    ticks = atol(field(text, "ticks"));
    cpus = 0;
    for (const char *p = strstr(text, "cpu_idle:"); p && cpus < MAX_CPUS; p = strstr(p + 1, "cpu_idle:"))
        cpu_idle[cpus++] = atol(p + 10);
}

static long previous_ticks_of(int pid) {
    for (int i = 0; i < previous_count; i++) {
        if (previous[i].pid == pid)
            return previous[i].ticks;
    }
    return 0;
}

static void read_processes(long elapsed) {
    static char status[FILE_MAX], command[FILE_MAX], path[64];
    DIR *proc = opendir("/proc");
    struct dirent *e;

    memcpy(previous, entries, sizeof(previous));
    previous_count = entry_count;
    entry_count = 0;
    if (!proc)
        return;

    while ((e = readdir(proc)) && entry_count < MAX_PROCESSES) {
        if (e->d_type != DT_DIR)
            continue;

        snprintf(path, sizeof(path), "/proc/%s/status", e->d_name);
        if (!read_text(path, status, sizeof(status)))
            continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", e->d_name);
        if (!read_text(path, command, sizeof(command)))
            command[0] = '\0';
        command[strcspn(command, "\n")] = '\0';

        struct entry *out = &entries[entry_count++];

        out->pid = atoi(field(status, "Pid"));
        out->state = field(status, "State")[0];
        out->threads = atoi(field(status, "Threads"));
        out->ticks = atol(field(status, "CpuTicks"));
        out->memory_kb = atol(field(status, "Memory"));
        out->percent_tenths = elapsed > 0 ? (out->ticks - previous_ticks_of(out->pid)) * 1000 / elapsed : 0;
        snprintf(out->command, sizeof(out->command), "%s", command);
    }
    closedir(proc);
}

static int by_usage(const void *left, const void *right) {
    const struct entry *a = left, *b = right;

    if (a->percent_tenths != b->percent_tenths)
        return b->percent_tenths - a->percent_tenths;
    return a->pid - b->pid;
}

static void draw(int rows, int cols) {
    static char text[FILE_MAX];
    long elapsed = ticks - ticks_before;
    long seconds = ticks / (rate ? rate : 100);

    printf("\x1b[H");
    printf("top - up %ld:%02ld:%02ld, %d processes" CLEAR_TO_END_OF_LINE "\n", seconds / 3600, seconds / 60 % 60, seconds % 60, entry_count);

    printf("CPU:");
    for (int i = 0; i < cpus; i++) {
        long idle = cpu_idle[i] - cpu_idle_before[i];
        long busy = elapsed > 0 ? 100 - idle * 100 / elapsed : 0;

        printf(" %ld%%", busy < 0 ? 0 : busy);
    }
    printf(CLEAR_TO_END_OF_LINE "\n");

    if (read_text("/proc/meminfo", text, sizeof(text))) {
        long total = atol(field(text, "MemTotal")), free_kb = atol(field(text, "MemFree"));

        printf("Mem: ");
        print_size((total - free_kb) * 1024, 0);
        printf(" used of ");
        print_size(total * 1024, 0);
    }
    printf(CLEAR_TO_END_OF_LINE "\n" CLEAR_TO_END_OF_LINE "\n");

    printf("%6s %4s %3s %9s %6s  %s" CLEAR_TO_END_OF_LINE "\n", "PID", "STAT", "THR", "MEM", "CPU%", "COMMAND");

    qsort(entries, entry_count, sizeof(entries[0]), by_usage);

    int shown = 0;

    for (int i = 0; i < entry_count && shown < rows - 6; i++, shown++) {
        printf("%6d %4c %3d ", entries[i].pid, entries[i].state, entries[i].threads);
        print_size(entries[i].memory_kb * 1024, 9);
        printf(" %4d.%d  %.*s" CLEAR_TO_END_OF_LINE "\n", entries[i].percent_tenths / 10, entries[i].percent_tenths % 10,
               cols > 35 ? cols - 35 : 1, entries[i].command);
    }
    printf("\x1b[J");
    fflush(stdout);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: top [-d seconds] [-n count]\n"
                            "Shows what's running and how busy the system is, until q.\n"
                            "  -d  how often to update, by default 1 second\n"
                            "  -n  stop after this many updates\n");

    double delay = 1;
    long limit = 0;
    int option;

    while ((option = getopt(argc, argv, "d:n:")) != -1) {
        if (option == 'd') {
            delay = atof(optarg);
        } else if (option == 'n') {
            limit = atol(optarg);
        } else {
            dprintf(STDERR_FILENO, "usage: top [-d seconds] [-n count]\n");
            return 1;
        }
    }
    if (delay <= 0)
        delay = 1;

    on_terminal = tcgetattr(STDIN_FILENO, &original) == 0 && isatty(STDIN_FILENO);
    if (on_terminal) {
        struct termios raw = original;

        raw.c_lflag &= ~(ICANON | ECHO);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("\x1b[?25l\x1b[H\x1b[2J");

    for (long update = 1; limit == 0 || update <= limit; update++) {
        int rows = 25, cols = 80;

        termsize(&rows, &cols);
        read_system();
        read_processes(ticks - ticks_before);
        draw(rows, cols);

        if (limit && update == limit)
            break;

        struct pollfd wait = {.fd = STDIN_FILENO, .events = POLLIN};
        char key = 0;

        if (poll(&wait, 1, (int)(delay * 1000)) > 0 && read(STDIN_FILENO, &key, 1) == 1 && (key == 'q' || key == 3))
            break;
    }
    restore_terminal();
    return 0;
}
