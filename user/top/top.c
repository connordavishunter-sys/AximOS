#include <aximos.h>
#include <dirent.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define MAX_PROCESSES 1024
#define COMMAND_MAX 64
#define CLEAR_TO_END_OF_LINE "\x1b[K"

struct entry {
    int pid, threads, percent_tenths;
    char state;
    long ticks;
    uint64_t memory;
    char command[COMMAND_MAX];
};

static struct entry entries[MAX_PROCESSES], previous[MAX_PROCESSES];
static int entry_count, previous_count;
static struct cpu_times now, before;
static struct termios original;
static bool on_terminal;

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

static long previous_ticks_of(int pid) {
    for (int i = 0; i < previous_count; i++) {
        if (previous[i].pid == pid)
            return previous[i].ticks;
    }
    return 0;
}

static void read_processes(long elapsed) {
    DIR *proc = opendir("/proc");
    struct dirent *e;
    struct process p;

    memcpy(previous, entries, sizeof(previous));
    previous_count = entry_count;
    entry_count = 0;
    while (proc && (e = readdir(proc)) && entry_count < MAX_PROCESSES) {
        if (!atoi(e->d_name) || !process_read(atoi(e->d_name), &p))
            continue;

        struct entry *out = &entries[entry_count++];

        *out = (struct entry){p.pid, p.threads, 0, p.state, p.ticks, p.memory, ""};
        out->percent_tenths = elapsed > 0 ? (p.ticks - previous_ticks_of(p.pid)) * 1000 / elapsed : 0;
        snprintf(out->command, sizeof(out->command), "%s", p.command);
    }
    if (proc)
        closedir(proc);
}

static int by_usage(const void *left, const void *right) {
    const struct entry *a = left, *b = right;

    if (a->percent_tenths != b->percent_tenths)
        return b->percent_tenths - a->percent_tenths;
    return a->pid - b->pid;
}

static void draw(int rows, int cols) {
    long elapsed = now.ticks - before.ticks, seconds = now.ticks / sysconf(_SC_CLK_TCK);
    struct memory m;

    printf("\x1b[H");
    printf("top - up %ld:%02ld:%02ld, %d processes" CLEAR_TO_END_OF_LINE "\n", seconds / 3600, seconds / 60 % 60, seconds % 60, entry_count);

    printf("CPU:");
    for (int i = 0; i < now.count; i++) {
        long idle = now.idle[i] - before.idle[i];
        long busy = elapsed > 0 ? 100 - idle * 100 / elapsed : 0;

        printf(" %ld%%", busy < 0 ? 0 : busy);
    }
    printf(CLEAR_TO_END_OF_LINE "\n");

    if (memory_read(&m)) {
        printf("Mem: ");
        print_size(m.total - m.available, 0);
        printf(" used of ");
        print_size(m.total, 0);
    }
    printf(CLEAR_TO_END_OF_LINE "\n" CLEAR_TO_END_OF_LINE "\n");

    printf("%6s %4s %3s %9s %6s  %s" CLEAR_TO_END_OF_LINE "\n", "PID", "STAT", "THR", "MEM", "CPU%", "COMMAND");

    qsort(entries, entry_count, sizeof(entries[0]), by_usage);

    int shown = 0;

    for (int i = 0; i < entry_count && shown < rows - 6; i++, shown++) {
        printf("%6d %4c %3d ", entries[i].pid, entries[i].state, entries[i].threads);
        print_size(entries[i].memory, 9);
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
        before = now;
        cpu_times(&now);
        read_processes(now.ticks - before.ticks);
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
