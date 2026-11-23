#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

static bool is_directory(const char *path) {
    struct stat st;

    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

// Moving into a directory keeps the source's name.
static bool resolve_target(const char *source, const char *target, char *out) {
    if (!is_directory(target)) {
        if (strlen(target) >= PATH_MAX)
            return false;
        strcpy(out, target);
        return true;
    }

    size_t len = strlen(source);
    while (len > 1 && source[len - 1] == '/')
        len--;

    size_t start = len;
    while (start > 0 && source[start - 1] != '/')
        start--;

    size_t name_len = len - start;
    if (strlen(target) + name_len + 2 > PATH_MAX)
        return false;

    strcpy(out, target);
    size_t used = strlen(out);
    out[used++] = '/';
    memcpy(&out[used], &source[start], name_len);
    out[used + name_len] = '\0';
    return true;
}

// Moves a file to another filesystem. The source only goes once the copy is
// complete, and a partial copy is cleaned up.
static bool copy_then_remove(const char *source, const char *target) {
    static char buffer[BUFFER_SIZE];
    int in = open(source, O_RDONLY);
    if (in < 0)
        return false;

    int out = open(target, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        return false;
    }

    bool ok = true;
    ssize_t count;
    while ((count = read(in, buffer, BUFFER_SIZE)) > 0) {
        if (write(out, buffer, count) != count) {
            ok = false;
            break;
        }
    }
    close(in);
    close(out);

    if (!ok || count < 0) {
        unlink(target);
        return false;
    }
    return unlink(source) == 0;
}

static int move(const char *source, const char *target_arg) {
    static char target[PATH_MAX];
    struct stat st;

    if (stat(source, &st) < 0) {
        dprintf(STDERR_FILENO, "mv: %s: %s\n", source, strerror(errno));
        return 1;
    }
    if (!resolve_target(source, target_arg, target)) {
        dprintf(STDERR_FILENO, "mv: %s: path too long\n", target_arg);
        return 1;
    }

    if (rename(source, target) == 0)
        return 0;

    // Only another filesystem is worth copying to, and only for a file.
    bool can_copy = errno == EXDEV && !S_ISDIR(st.st_mode);

    if (!can_copy || !copy_then_remove(source, target)) {
        dprintf(STDERR_FILENO, "mv: %s to %s: %s\n", source, target, strerror(errno));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: mv source... target\n"
                            "Moves files and directories. A target that is a directory keeps the sources' names.\n");

    if (argc < 3) {
        dprintf(STDERR_FILENO, "usage: mv source... target\n");
        return 1;
    }

    const char *target = argv[argc - 1];
    if (argc > 3 && !is_directory(target)) {
        dprintf(STDERR_FILENO, "mv: %s: not a directory\n", target);
        return 1;
    }

    int status = 0;
    for (int i = 1; i < argc - 1; i++)
        status |= move(argv[i], target);
    return status;
}
