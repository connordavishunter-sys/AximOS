#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FILE_MAX 2048

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

// The number or text after a name in a file of lines like "Name:\tvalue".
static const char *field(const char *text, const char *name) {
    size_t len = strlen(name);

    for (const char *line = text; line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        if (strncmp(line, name, len) == 0 && line[len] == ':')
            return line + len + 2;
    }
    return "";
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: ps\n"
                            "Lists the processes that are running.\n"
                            "  STAT is R for running, S for sleeping and Z for exited\n"
                            "  TIME is the processor time it has used\n");

    DIR *proc = opendir("/proc");
    struct dirent *entry;
    static char status[FILE_MAX], command[FILE_MAX], path[64];
    int rate = 100;

    if (read_text("/proc/stat", status, sizeof(status)))
        rate = atoi(field(status, "ticks_per_second"));
    if (!proc) {
        perror("ps: /proc");
        return 1;
    }

    printf("%6s %6s %4s %3s %9s %8s  %s\n", "PID", "PPID", "STAT", "THR", "MEM", "TIME", "COMMAND");
    while ((entry = readdir(proc))) {
        if (entry->d_type != DT_DIR)
            continue;

        snprintf(path, sizeof(path), "/proc/%s/status", entry->d_name);
        if (!read_text(path, status, sizeof(status)))
            continue;
        snprintf(path, sizeof(path), "/proc/%s/cmdline", entry->d_name);
        if (!read_text(path, command, sizeof(command)))
            command[0] = '\0';
        command[strcspn(command, "\n")] = '\0';

        long ticks = atol(field(status, "CpuTicks"));
        long seconds = ticks / rate;

        printf("%6d %6d %4c %3d ", atoi(field(status, "Pid")), atoi(field(status, "PPid")), field(status, "State")[0],
               atoi(field(status, "Threads")));
        print_size(atol(field(status, "Memory")) * 1024, 9);
        printf(" %02ld:%02ld.%02ld  %s\n", seconds / 60, seconds % 60, (ticks % rate) * 100 / rate, command);
    }
    closedir(proc);
    return 0;
}
