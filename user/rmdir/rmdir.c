#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool report(const char *path) {
    dprintf(STDERR_FILENO, "rmdir: %s: %s\n", path, strerror(errno));
    return false;
}

static bool remove_tree(const char *path);

// Finds the next thing in a directory, and returns its path or NULL with none left.
// The directory is closed again before anything is removed, so that deep trees can't
// run out of file descriptors.
static char *next_entry(const char *path, bool *is_dir, bool *ok) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    char *child = NULL;

    if (!dir) {
        *ok = report(path);
        return NULL;
    }
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        *is_dir = entry->d_type == DT_DIR;
        if (asprintf(&child, "%s/%s", path, entry->d_name) < 0) {
            *ok = report(path);
            child = NULL;
        }
        break;
    }
    closedir(dir);
    return child;
}

static bool remove_contents(const char *path) {
    bool ok = true;
    bool is_dir = false;
    char *child;

    while (ok && (child = next_entry(path, &is_dir, &ok))) {
        if (is_dir)
            ok = remove_tree(child);
        else if (unlink(child) < 0)
            ok = report(child);
        free(child);
    }
    return ok;
}

// rmdir goes first, as it refuses mount points before it looks at their contents.
// Emptying them first would wipe a whole drive and then fail.
static bool remove_tree(const char *path) {
    if (rmdir(path) == 0)
        return true;
    if (errno != ENOTEMPTY)
        return report(path);
    return remove_contents(path) && (rmdir(path) == 0 || report(path));
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: rmdir [-f] directory...\n"
                            "Removes empty directories.\n"
                            "  -f  also remove everything inside them\n");

    bool force = false;
    int option;

    while ((option = getopt(argc, argv, "f")) != -1) {
        if (option != 'f') {
            dprintf(STDERR_FILENO, "usage: rmdir [-f] directory...\n");
            return 1;
        }
        force = true;
    }
    if (optind == argc) {
        dprintf(STDERR_FILENO, "usage: rmdir [-f] directory...\n");
        return 1;
    }

    int status = 0;
    for (int i = optind; i < argc; i++) {
        bool ok = force ? remove_tree(argv[i]) : rmdir(argv[i]) == 0 || report(argv[i]);

        if (!ok)
            status = 1;
    }
    return status;
}
