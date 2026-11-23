#include <aximos.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool all_files, summary_only;
static int status;

// Bytes on the disk, in whole sectors as the file's blocks say.
static uint64_t space(const struct stat *st) {
    return (uint64_t)st->st_blocks * 512;
}

static void report(uint64_t bytes, const char *path) {
    print_size(bytes, 10);
    printf("  %s\n", path);
}

static uint64_t visit(const char *path, int depth) {
    struct stat st;

    if (stat(path, &st) < 0) {
        dprintf(STDERR_FILENO, "du: %s: %s\n", path, strerror(errno));
        status = 1;
        return 0;
    }

    uint64_t total = space(&st);

    if (!S_ISDIR(st.st_mode)) {
        if (depth == 0 || (all_files && !summary_only))
            report(total, path);
        return total;
    }

    DIR *dir = opendir(path);
    struct dirent *entry;

    if (!dir) {
        dprintf(STDERR_FILENO, "du: %s: %s\n", path, strerror(errno));
        status = 1;
        return total;
    }
    while ((entry = readdir(dir))) {
        char *child;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0
            || asprintf(&child, "%s/%s", path, entry->d_name) < 0)
            continue;
        total += visit(child, depth + 1);
        free(child);
    }
    closedir(dir);

    if (!summary_only || depth == 0)
        report(total, path);
    return total;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: du [-as] [path...]\n"
                            "Shows how much disk space directories use.\n"
                            "  -a  list files too\n"
                            "  -s  only the total for each path\n");

    int option;

    while ((option = getopt(argc, argv, "as")) != -1) {
        if (option == 'a') {
            all_files = true;
        } else if (option == 's') {
            summary_only = true;
        } else {
            dprintf(STDERR_FILENO, "usage: du [-as] [path...]\n");
            return 1;
        }
    }

    if (optind == argc)
        visit(".", 0);
    for (int i = optind; i < argc; i++)
        visit(argv[i], 0);
    return status;
}
