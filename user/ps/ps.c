#include <aximos.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: ps\n"
                            "Lists the processes that are running.\n"
                            "  STAT is R for running, S for sleeping, D for waiting on a device and Z for exited\n"
                            "  TIME is the processor time it has used\n");

    DIR *proc = opendir("/proc");
    struct dirent *entry;
    struct process p;
    long rate = sysconf(_SC_CLK_TCK);

    if (!proc) {
        perror("ps: /proc");
        return 1;
    }
    printf("%6s %6s %4s %3s %9s %8s  %s\n", "PID", "PPID", "STAT", "THR", "MEM", "TIME", "COMMAND");
    while ((entry = readdir(proc))) {
        if (!atoi(entry->d_name) || !process_read(atoi(entry->d_name), &p))
            continue;

        long seconds = p.ticks / rate;

        printf("%6d %6d %4c %3d ", p.pid, p.ppid, p.state, p.threads);
        print_size(p.memory, 9);
        printf(" %02ld:%02ld.%02ld  %s\n", seconds / 60, seconds % 60, p.ticks % rate * 100 / rate, p.command);
    }
    closedir(proc);
    return 0;
}
