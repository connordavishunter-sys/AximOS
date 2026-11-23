#include <aximos.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char usage[] = "usage: rm [-rf] file...\n";
static bool recursive, force;

static int complain(const char *path) {
    if (force && errno == ENOENT)
        return 0;
    dprintf(STDERR_FILENO, "rm: %s: %s\n", path, strerror(errno));
    return 1;
}

static int remove_path(const char *path) {
    struct stat st;

    if (stat(path, &st) < 0)
        return complain(path);
    if (!S_ISDIR(st.st_mode))
        return unlink(path) < 0 ? complain(path) : 0;
    if (!recursive) {
        errno = EISDIR;
        return complain(path);
    }

    DIR *dir = opendir(path);
    struct dirent *entry;
    int status = 0;

    if (!dir)
        return complain(path);
    while ((entry = readdir(dir))) {
        char child[PATH_MAX];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
        status |= remove_path(child);
    }
    closedir(dir);
    return status | (rmdir(path) < 0 ? complain(path) : 0);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: rm [-rf] file...\n"
                            "Removes files. -r removes directories and everything in them,\n"
                            "and -f says nothing about files that aren't there.\n");

    int option;

    while ((option = getopt(argc, argv, "rRf")) != -1) {
        if (option == 'f')
            force = true;
        else if (option == 'r' || option == 'R')
            recursive = true;
        else {
            dprintf(STDERR_FILENO, "%s", usage);
            return 1;
        }
    }
    if (optind == argc && !force) {
        dprintf(STDERR_FILENO, "%s", usage);
        return 1;
    }

    int status = 0;

    for (int i = optind; i < argc; i++)
        status |= remove_path(argv[i]);
    return status;
}
