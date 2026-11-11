#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fb.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lock.h"
#include "syscall.h"

#define DEFAULT_PATH "/system/bin"
#define EXECL_MAX_ARGS 32

char **environ;
bool __threaded;

// System calls return a negative errno on failure.
static long checked(long result) {
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

struct DIR {
    int fd;
    char *path; // kept for rewinding
    struct dirent entry;
};

pid_t fork(void) {
    __fork_prepare();

    long result = syscall3(SYS_FORK, 0, 0, 0);

    if (result == 0)
        __fork_child();
    else
        __fork_parent();
    return checked(result);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    return checked(syscall3(SYS_EXEC, (long)path, (long)argv, (long)envp));
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, environ);
}

pid_t vfork(void) {
    return fork();
}

int execvp(const char *file, char *const argv[]) {
    if (strchr(file, '/'))
        return execv(file, argv);

    const char *search = getenv("PATH");
    static char path[PATH_MAX];

    if (!search)
        search = DEFAULT_PATH;

    while (*search) {
        size_t len = strcspn(search, ":");

        if (len + strlen(file) + 2 <= PATH_MAX) {
            memcpy(path, search, len);
            path[len] = '/';
            strcpy(&path[len + 1], file);
            execv(path, argv);
            if (errno != ENOENT)
                return -1;
        }
        search += len;
        if (*search)
            search++;
    }
    errno = ENOENT;
    return -1;
}

static int collect_args(char **argv, const char *arg, va_list args) {
    int count = 0;

    for (const char *a = arg; a; a = va_arg(args, const char *)) {
        if (count == EXECL_MAX_ARGS - 1) {
            errno = E2BIG;
            return -1;
        }
        argv[count++] = (char *)a;
    }
    argv[count] = NULL;
    return count;
}

int execl(const char *path, const char *arg, ...) {
    char *argv[EXECL_MAX_ARGS];
    va_list args;

    va_start(args, arg);

    int count = collect_args(argv, arg, args);

    va_end(args);
    return count < 0 ? -1 : execv(path, argv);
}

int execlp(const char *file, const char *arg, ...) {
    char *argv[EXECL_MAX_ARGS];
    va_list args;

    va_start(args, arg);

    int count = collect_args(argv, arg, args);

    va_end(args);
    return count < 0 ? -1 : execvp(file, argv);
}

int execle(const char *path, const char *arg, ...) {
    char *argv[EXECL_MAX_ARGS];
    va_list args;

    va_start(args, arg);

    int count = collect_args(argv, arg, args);
    char **envp = va_arg(args, char **);

    va_end(args);
    return count < 0 ? -1 : execve(path, argv, envp);
}

int access(const char *path, int mode) {
    struct stat st;

    // There are no permissions, so anything that exists can be used.
    (void)mode;
    return stat(path, &st);
}

pid_t wait(int *status) {
    return checked(syscall3(SYS_WAIT, (long)status, -1, 0));
}

pid_t gettid(void) {
    return syscall3(SYS_GETTID, 0, 0, 0);
}

pid_t getpid(void) {
    return syscall3(SYS_GETPID, 0, 0, 0);
}

int open(const char *path, int flags, ...) {
    return checked(syscall3(SYS_OPEN, (long)path, flags, 0));
}

int close(int fd) {
    return checked(syscall3(SYS_CLOSE, fd, 0, 0));
}

ssize_t read(int fd, void *buf, size_t len) {
    return checked(syscall3(SYS_READ, fd, (long)buf, len));
}

ssize_t write(int fd, const void *buf, size_t len) {
    return checked(syscall3(SYS_WRITE, fd, (long)buf, len));
}

long lseek(int fd, long offset, int whence) {
    return checked(syscall3(SYS_LSEEK, fd, offset, whence));
}

int pipe(int fds[2]) {
    return checked(syscall3(SYS_PIPE, (long)fds, 0, 0));
}

int dup(int fd) {
    return checked(syscall3(SYS_DUP, fd, 0, 0));
}

int dup2(int fd, int target) {
    return checked(syscall3(SYS_DUP2, fd, target, 0));
}

int rename(const char *old_path, const char *new_path) {
    return checked(syscall3(SYS_RENAME, (long)old_path, (long)new_path, 0));
}

int unlink(const char *path) {
    return checked(syscall3(SYS_UNLINK, (long)path, 0, 0));
}

int rmdir(const char *path) {
    return checked(syscall3(SYS_RMDIR, (long)path, 0, 0));
}

int mkdir(const char *path, unsigned mode) {
    (void)mode;
    return checked(syscall3(SYS_MKDIR, (long)path, 0, 0));
}

int setpgid(pid_t pid, pid_t pgid) {
    return checked(syscall3(SYS_SETPGID, pid, pgid, 0));
}

int chdir(const char *path) {
    return checked(syscall3(SYS_CHDIR, (long)path, 0, 0));
}

char *getcwd(char *buf, size_t len) {
    return checked(syscall3(SYS_GETCWD, (long)buf, len, 0)) < 0 ? NULL : buf;
}

int stat(const char *path, struct stat *out) {
    return checked(syscall3(SYS_STAT, (long)path, (long)out, 0));
}

int statfs(const char *path, struct statfs *out) {
    return checked(syscall3(SYS_STATFS, (long)path, (long)out, 0));
}

int termsize(int *rows, int *cols) {
    return checked(syscall3(SYS_TERMSIZE, (long)rows, (long)cols, 0));
}

void *mmap(void *addr, size_t len, int prot, int flags, int fd, long offset) {
    long result = syscall6(SYS_MMAP, (long)addr, len, prot, flags, fd, offset);

    return result < 0 ? (checked(result), MAP_FAILED) : (void *)result;
}

int mprotect(void *addr, size_t len, int prot) {
    return checked(syscall3(SYS_MPROTECT, (long)addr, len, prot));
}

int msync(void *addr, size_t len, int flags) {
    (void)addr;
    (void)len;
    (void)flags;
    return 0;
}

int munmap(void *addr, size_t len) {
    return checked(syscall3(SYS_MUNMAP, (long)addr, len, 0));
}

int shm_open(const char *name, int flags, unsigned mode) {
    (void)mode;
    return checked(syscall3(SYS_SHM_OPEN, (long)name, flags, 0));
}

int shm_unlink(const char *name) {
    return checked(syscall3(SYS_SHM_UNLINK, (long)name, 0, 0));
}

int ftruncate(int fd, long size) {
    return checked(syscall3(SYS_FTRUNCATE, fd, size, 0));
}

int meminfo(struct meminfo *info) {
    return checked(syscall3(SYS_MEMINFO, (long)info, 0, 0));
}

int fb_info(int fd, struct fb_info *out) {
    return checked(syscall3(SYS_FB_INFO, fd, (long)out, 0));
}

int fb_set_mode(int fd, int width, int height) {
    return checked(syscall3(SYS_FB_SET_MODE, fd, width, height));
}

int fb_draw(int fd, const struct fb_rect *rect) {
    return checked(syscall3(SYS_FB_DRAW, fd, (long)rect, 0));
}

int reboot(int command) {
    return checked(syscall3(SYS_REBOOT, command, 0, 0));
}

DIR *opendir(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;

    DIR *dir = malloc(sizeof(*dir));
    if (!dir) {
        close(fd);
        return NULL;
    }
    dir->fd = fd;
    dir->path = strdup(path);
    return dir;
}

struct dirent *readdir(DIR *dir) {
    return checked(syscall3(SYS_READDIR, dir->fd, (long)&dir->entry, 0)) == 1 ? &dir->entry : NULL;
}

int closedir(DIR *dir) {
    int result = close(dir->fd);

    free(dir->path);
    free(dir);
    return result;
}

// There's no seeking in a directory, but opening it again starts it over. A
// directory that can't be reopened stays where it was.
void rewinddir(DIR *dir) {
    int fd = dir->path ? open(dir->path, O_RDONLY) : -1;

    if (fd >= 0) {
        close(dir->fd);
        dir->fd = fd;
    }
}

int dirfd(DIR *dir) {
    return dir->fd;
}

int alphasort(const struct dirent **a, const struct dirent **b) {
    return strcmp((*a)->d_name, (*b)->d_name);
}

int scandir(const char *path, struct dirent ***list, int (*filter)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **)) {
    DIR *dir = opendir(path);

    if (!dir)
        return -1;

    struct dirent **found = NULL;
    struct dirent *entry;
    size_t count = 0;

    while ((entry = readdir(dir))) {
        if (filter && !filter(entry))
            continue;

        struct dirent **grown = realloc(found, (count + 1) * sizeof(*found));
        struct dirent *copy = malloc(sizeof(*copy));

        if (grown)
            found = grown;
        if (!grown || !copy) {
            free(copy);
            while (count > 0)
                free(found[--count]);
            free(found);
            closedir(dir);
            errno = ENOMEM;
            return -1;
        }
        *copy = *entry;
        found[count++] = copy;
    }
    closedir(dir);

    if (compare)
        qsort(found, count, sizeof(*found), (int (*)(const void *, const void *))compare);
    *list = found;
    return count;
}

void help_option(int argc, char **argv, const char *text) {
    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        fputs(text, stdout);
        exit(0);
    }
}
