#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

static bool recursive;

static const char *base_name(const char *path) {
    const char *end = path + strlen(path);

    while (end > path + 1 && end[-1] == '/')
        end--;

    const char *start = end;

    while (start > path && start[-1] != '/')
        start--;
    return start;
}

static int fail(const char *path) {
    dprintf(STDERR_FILENO, "cp: %s: %s\n", path, strerror(errno));
    return 1;
}

static int copy_file(const char *source, const char *target) {
    static char buffer[BUFFER_SIZE];
    int in = open(source, O_RDONLY);

    if (in < 0)
        return fail(source);

    int out = open(target, O_WRONLY | O_CREAT | O_TRUNC);

    if (out < 0) {
        close(in);
        return fail(target);
    }

    int status = 0;
    ssize_t count;

    while ((count = read(in, buffer, BUFFER_SIZE)) > 0) {
        if (write(out, buffer, count) != count) {
            status = fail(target);
            break;
        }
    }
    if (count < 0)
        status = fail(source);
    close(in);
    close(out);
    return status;
}

static int copy_path(const char *source, const char *target);

static int copy_directory(const char *source, const char *target) {
    struct stat st;

    if (stat(target, &st) < 0 && mkdir(target, 0) < 0)
        return fail(target);

    DIR *dir = opendir(source);
    struct dirent *entry;
    int status = 0;

    if (!dir)
        return fail(source);
    while ((entry = readdir(dir))) {
        char *from, *to;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (asprintf(&from, "%s/%s", source, entry->d_name) < 0 || asprintf(&to, "%s/%s", target, entry->d_name) < 0)
            return fail(source);
        status |= copy_path(from, to);
        free(from);
        free(to);
    }
    closedir(dir);
    return status;
}

static int copy_path(const char *source, const char *target) {
    struct stat st;

    if (stat(source, &st) < 0)
        return fail(source);
    if (!S_ISDIR(st.st_mode))
        return copy_file(source, target);
    if (!recursive) {
        dprintf(STDERR_FILENO, "cp: %s: Is a directory, which needs -r\n", source);
        return 1;
    }
    return copy_directory(source, target);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: cp [-r] source... target\n"
                            "Copies files. A target that is a directory keeps the sources' names.\n"
                            "  -r  copy directories and what's in them\n");

    int option;

    while ((option = getopt(argc, argv, "r")) != -1) {
        if (option != 'r') {
            dprintf(STDERR_FILENO, "usage: cp [-r] source... target\n");
            return 1;
        }
        recursive = true;
    }
    if (argc - optind < 2) {
        dprintf(STDERR_FILENO, "usage: cp [-r] source... target\n");
        return 1;
    }

    const char *target = argv[argc - 1];
    struct stat st;
    bool into_directory = stat(target, &st) == 0 && S_ISDIR(st.st_mode);

    if (argc - optind > 2 && !into_directory) {
        dprintf(STDERR_FILENO, "cp: %s: not a directory\n", target);
        return 1;
    }

    int status = 0;

    for (int i = optind; i < argc - 1; i++) {
        char *to;

        if (!into_directory) {
            status |= copy_path(argv[i], target);
            continue;
        }
        if (asprintf(&to, "%s/%s", target, base_name(argv[i])) < 0)
            return fail(target);
        status |= copy_path(argv[i], to);
        free(to);
    }
    return status;
}
