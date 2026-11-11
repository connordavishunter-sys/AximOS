#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <limits.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdbool.h>
#include <syslog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mntent.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#include "syscall.h"

#define PAGE_SIZE 4096
#define MAX_OPEN_FILES 64
#define PIPE_SIZE 4096
#define ARG_MAX_BYTES 4096
#define HOST_NAME "aximos"
#define TEMPLATE_SUFFIX 6
#define TEMPLATE_TRIES 1000

static long checked(long result) {
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    return checked(syscall3(SYS_WAIT, (long)status, pid, options));
}

pid_t getppid(void) {
    return syscall3(SYS_GETPPID, 0, 0, 0);
}

pid_t getpgid(pid_t pid) {
    return checked(syscall3(SYS_GETPGID, pid, 0, 0));
}

pid_t getpgrp(void) {
    return getpgid(0);
}

int setpgrp(void) {
    return setpgid(0, 0);
}

pid_t getsid(pid_t pid) {
    return getpgid(pid);
}

int fstat(int fd, struct stat *out) {
    return checked(syscall3(SYS_FSTAT, fd, (long)out, 0));
}

int lstat(const char *path, struct stat *out) {
    return stat(path, out);
}

int chmod(const char *path, mode_t mode) {
    struct stat st;

    (void)mode;
    return stat(path, &st);
}

int fchmod(int fd, mode_t mode) {
    struct stat st;

    (void)mode;
    return fstat(fd, &st);
}

mode_t umask(mode_t mask) {
    static mode_t current = 022;
    mode_t previous = current;

    current = mask & 0777;
    return previous;
}

int chown(const char *path, uid_t owner, gid_t group) {
    (void)owner;
    (void)group;
    return chmod(path, 0);
}

int fchown(int fd, uid_t owner, gid_t group) {
    (void)owner;
    (void)group;
    return fchmod(fd, 0);
}

int lchown(const char *path, uid_t owner, gid_t group) {
    return chown(path, owner, group);
}

int link(const char *path, const char *new_path) {
    (void)path;
    (void)new_path;
    errno = EPERM;
    return -1;
}

int symlink(const char *target, const char *path) {
    return link(target, path);
}

ssize_t readlink(const char *path, char *buf, size_t size) {
    struct stat st;

    (void)buf;
    (void)size;
    if (stat(path, &st) == 0)
        errno = EINVAL;
    return -1;
}

int utime(const char *path, const struct utimbuf *times) {
    (void)times;
    return chmod(path, 0);
}

int utimes(const char *path, const struct timeval times[2]) {
    (void)times;
    return chmod(path, 0);
}

int lutimes(const char *path, const struct timeval times[2]) {
    return utimes(path, times);
}

int futimes(int fd, const struct timeval times[2]) {
    (void)times;
    return fchmod(fd, 0);
}

int utimensat(int directory, const char *path, const struct timespec times[2], int flags) {
    (void)times;
    (void)flags;
    if (directory != AT_FDCWD && path[0] != '/') {
        errno = ENOSYS;
        return -1;
    }
    return chmod(path, 0);
}

int futimens(int fd, const struct timespec times[2]) {
    (void)times;
    return fchmod(fd, 0);
}

int mknod(const char *path, mode_t mode, dev_t device) {
    (void)path;
    (void)mode;
    (void)device;
    errno = EPERM;
    return -1;
}

int mkfifo(const char *path, mode_t mode) {
    return mknod(path, mode | S_IFIFO, 0);
}

int chroot(const char *path) {
    (void)path;
    errno = EPERM;
    return -1;
}

int fchdir(int fd) {
    (void)fd;
    errno = ENOSYS;
    return -1;
}

int clock_settime(int clock, const struct timespec *value) {
    (void)clock;
    (void)value;
    errno = EPERM;
    return -1;
}

int prctl(int option, ...) {
    (void)option;
    errno = EINVAL;
    return -1;
}

pid_t wait4(pid_t pid, int *status, int options, struct rusage *usage) {
    pid_t done = waitpid(pid, status, options);

    if (usage && done >= 0)
        memset(usage, 0, sizeof(*usage));
    return done;
}

pid_t wait3(int *status, int options, struct rusage *usage) {
    return wait4(-1, status, options, usage);
}

int settimeofday(const struct timeval *value, const void *zone) {
    (void)value;
    (void)zone;
    errno = EPERM;
    return -1;
}

int sethostname(const char *name, size_t len) {
    (void)name;
    (void)len;
    errno = EPERM;
    return -1;
}

int creat(const char *path, unsigned mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int fcntl(int fd, int command, ...) {
    va_list args;

    va_start(args, command);

    long value = va_arg(args, long);

    va_end(args);
    return checked(syscall3(SYS_FCNTL, fd, command, value));
}

ssize_t pread(int fd, void *buf, size_t len, long offset) {
    long previous = lseek(fd, 0, SEEK_CUR);

    if (previous < 0 || lseek(fd, offset, SEEK_SET) < 0)
        return -1;

    ssize_t count = read(fd, buf, len);
    int saved = errno;

    lseek(fd, previous, SEEK_SET);
    errno = saved;
    return count;
}

ssize_t pwrite(int fd, const void *buf, size_t len, long offset) {
    long previous = lseek(fd, 0, SEEK_CUR);

    if (previous < 0 || lseek(fd, offset, SEEK_SET) < 0)
        return -1;

    ssize_t count = write(fd, buf, len);
    int saved = errno;

    lseek(fd, previous, SEEK_SET);
    errno = saved;
    return count;
}

int truncate(const char *path, long size) {
    int fd = open(path, O_WRONLY);

    if (fd < 0)
        return -1;

    int result = ftruncate(fd, size);
    int saved = errno;

    close(fd);
    errno = saved;
    return result;
}

static int set_cloexec(int fd, int flags) {
    return flags & O_CLOEXEC ? fcntl(fd, F_SETFD, FD_CLOEXEC) : 0;
}

int pipe2(int fds[2], int flags) {
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) {
        errno = EINVAL;
        return -1;
    }
    if (pipe(fds) < 0)
        return -1;
    for (int i = 0; i < 2; i++) {
        set_cloexec(fds[i], flags);
        if (flags & O_NONBLOCK)
            fcntl(fds[i], F_SETFL, O_NONBLOCK);
    }
    return 0;
}

int dup3(int fd, int target, int flags) {
    if (fd == target || (flags & ~O_CLOEXEC)) {
        errno = EINVAL;
        return -1;
    }
    if (dup2(fd, target) < 0)
        return -1;
    set_cloexec(target, flags);
    return target;
}

int fsync(int fd) {
    return fcntl(fd, F_GETFD) < 0 ? -1 : 0;
}

int fdatasync(int fd) {
    return fsync(fd);
}

void sync(void) {
}

int brk(void *end) {
    if ((void *)syscall3(SYS_BRK, (long)end, 0, 0) == end)
        return 0;
    errno = ENOMEM;
    return -1;
}

void *sbrk(long change) {
    char *old_end = (char *)syscall3(SYS_BRK, 0, 0, 0);

    return change == 0 || brk(old_end + change) == 0 ? old_end : (void *)-1;
}

uid_t getuid(void) {
    return 0;
}

uid_t geteuid(void) {
    return 0;
}

gid_t getgid(void) {
    return 0;
}

gid_t getegid(void) {
    return 0;
}

static int only_root(unsigned id) {
    if (id == 0)
        return 0;
    errno = EPERM;
    return -1;
}

int setuid(uid_t uid) {
    return only_root(uid);
}

int seteuid(uid_t uid) {
    return only_root(uid);
}

int setgid(gid_t gid) {
    return only_root(gid);
}

int setegid(gid_t gid) {
    return only_root(gid);
}

int getgroups(int size, gid_t *groups) {
    (void)size;
    (void)groups;
    return 0;
}

char *getlogin(void) {
    return "root";
}

struct passwd *getpwuid(uid_t uid) {
    static struct passwd root = {"root", "x", 0, 0, "root", "/", "/system/bin/dash"};

    return uid == 0 ? &root : NULL;
}

struct passwd *getpwnam(const char *name) {
    return strcmp(name, "root") == 0 ? getpwuid(0) : NULL;
}

struct group *getgrgid(gid_t gid) {
    static char *members[] = {"root", NULL};
    static struct group root = {"root", "x", 0, members};

    return gid == 0 ? &root : NULL;
}

struct group *getgrnam(const char *name) {
    return strcmp(name, "root") == 0 ? getgrgid(0) : NULL;
}

static bool group_listed;
static bool user_listed;

struct group *getgrent(void) {
    struct group *group = group_listed ? NULL : getgrgid(0);

    group_listed = true;
    return group;
}

void setgrent(void) {
    group_listed = false;
}

void endgrent(void) {
    group_listed = false;
}

struct passwd *getpwent(void) {
    struct passwd *user = user_listed ? NULL : getpwuid(0);

    user_listed = true;
    return user;
}

void setpwent(void) {
    user_listed = false;
}

void endpwent(void) {
    user_listed = false;
}

int initgroups(const char *user, gid_t group) {
    (void)user;
    return only_root(group);
}

int setgroups(size_t count, const gid_t *groups) {
    for (size_t i = 0; i < count; i++)
        if (only_root(groups[i]) < 0)
            return -1;
    return 0;
}

int getgrouplist(const char *user, gid_t group, gid_t *groups, int *count) {
    (void)user;

    int wanted = group == 0 ? 1 : 2;
    bool fits = *count >= wanted;

    if (fits) {
        groups[0] = group;
        if (group != 0)
            groups[1] = 0;
    }
    *count = wanted;
    return fits ? wanted : -1;
}

int gethostname(char *name, size_t len) {
    if (len < sizeof(HOST_NAME)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(name, HOST_NAME);
    return 0;
}

int uname(struct utsname *out) {
    strcpy(out->sysname, "AximOS");
    strcpy(out->nodename, HOST_NAME);
    strcpy(out->release, "0.1");
    strcpy(out->version, "0.1");
    strcpy(out->machine, "x86_64");
    strcpy(out->domainname, "(none)");
    return 0;
}

int getpagesize(void) {
    return PAGE_SIZE;
}

int getdtablesize(void) {
    return MAX_OPEN_FILES;
}

int nice(int change) {
    (void)change;
    return 0;
}

long sysconf(int name) {
    struct meminfo info;

    switch (name) {
    case _SC_ARG_MAX:
        return ARG_MAX_BYTES;
    case _SC_CHILD_MAX:
        return LONG_MAX;
    case _SC_CLK_TCK:
        return CLOCKS_PER_SEC;
    case _SC_OPEN_MAX:
        return MAX_OPEN_FILES;
    case _SC_PAGESIZE:
        return PAGE_SIZE;
    case _SC_NPROCESSORS_CONF:
    case _SC_NPROCESSORS_ONLN:
        return 1; // only one CPU can be in the kernel at a time anyway
    case _SC_PHYS_PAGES:
        return meminfo(&info) == 0 ? (long)(info.total / PAGE_SIZE) : -1;
    case _SC_AVPHYS_PAGES:
        return meminfo(&info) == 0 ? (long)(info.free / PAGE_SIZE) : -1;
    }
    errno = EINVAL;
    return -1;
}

long fpathconf(int fd, int name) {
    (void)fd;
    switch (name) {
    case _PC_LINK_MAX:
        return 1;
    case _PC_NAME_MAX:
        return NAME_MAX;
    case _PC_PATH_MAX:
        return PATH_MAX;
    case _PC_PIPE_BUF:
        return PIPE_SIZE;
    }
    errno = EINVAL;
    return -1;
}

long pathconf(const char *path, int name) {
    (void)path;
    return fpathconf(-1, name);
}

int sysinfo(struct sysinfo *out) {
    struct meminfo info;
    struct timespec now;

    if (meminfo(&info) < 0 || clock_gettime(CLOCK_MONOTONIC, &now) < 0)
        return -1;
    *out = (struct sysinfo){
        .uptime = now.tv_sec,
        .totalram = info.total,
        .freeram = info.free,
        .bufferram = info.cached,
        .mem_unit = 1,
    };
    return 0;
}

int get_nprocs(void) {
    return sysconf(_SC_NPROCESSORS_ONLN);
}

int get_nprocs_conf(void) {
    return sysconf(_SC_NPROCESSORS_CONF);
}

int statvfs(const char *path, struct statvfs *out) {
    struct statfs fs;

    if (statfs(path, &fs) < 0)
        return -1;
    *out = (struct statvfs){
        .f_bsize = fs.f_bsize,
        .f_frsize = fs.f_bsize,
        .f_blocks = fs.f_blocks,
        .f_bfree = fs.f_bfree,
        .f_bavail = fs.f_bfree,
        .f_namemax = NAME_MAX,
    };
    return 0;
}

FILE *setmntent(const char *path, const char *mode) {
    (void)path;
    (void)mode;
    errno = ENOENT;
    return NULL;
}

struct mntent *getmntent(FILE *stream) {
    (void)stream;
    return NULL;
}

struct mntent *getmntent_r(FILE *stream, struct mntent *out, char *buffer, int size) {
    (void)stream;
    (void)out;
    (void)buffer;
    (void)size;
    return NULL;
}

int endmntent(FILE *stream) {
    (void)stream;
    return 1;
}

void openlog(const char *name, int options, int facility) {
    (void)name;
    (void)options;
    (void)facility;
}

void syslog(int priority, const char *format, ...) {
    (void)priority;
    (void)format;
}

void closelog(void) {
}

clock_t times(struct tms *out) {
    if (out)
        *out = (struct tms){.tms_utime = clock()};

    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * CLOCKS_PER_SEC + now.tv_nsec / (1000000000 / CLOCKS_PER_SEC);
}

int getrlimit(int resource, struct rlimit *out) {
    rlim_t value = resource == RLIMIT_NOFILE ? MAX_OPEN_FILES : RLIM_INFINITY;

    *out = (struct rlimit){value, value};
    return 0;
}

int setrlimit(int resource, const struct rlimit *limit) {
    struct rlimit current;

    getrlimit(resource, &current);
    if (limit->rlim_cur <= current.rlim_max && limit->rlim_max <= current.rlim_max)
        return 0;
    errno = EPERM;
    return -1;
}

// Fills in the Xs from a counter that's different in each process, and calls try
// until it stops failing with EEXIST.
static int from_template(char *path, int (*try_path)(const char *)) {
    static const char letters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    static unsigned counter;
    size_t len = strlen(path);

    if (len < TEMPLATE_SUFFIX || strcmp(path + len - TEMPLATE_SUFFIX, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }

    for (int attempt = 0; attempt < TEMPLATE_TRIES; attempt++) {
        unsigned long value = (unsigned long)getpid() * 2654435761ul + time(NULL) + ++counter * 7919ul;

        for (int i = 0; i < TEMPLATE_SUFFIX; i++) {
            path[len - TEMPLATE_SUFFIX + i] = letters[value % (sizeof(letters) - 1)];
            value /= sizeof(letters) - 1;
        }

        int result = try_path(path);

        if (result >= 0 || errno != EEXIST)
            return result;
    }
    errno = EEXIST;
    return -1;
}

static int create_file(const char *path) {
    return open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
}

static int create_directory(const char *path) {
    struct stat st;

    if (stat(path, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    return mkdir(path, 0700);
}

int mkstemp(char *template_path) {
    return from_template(template_path, create_file);
}

// Picks a name without making anything, so someone can get there first.
static int name_is_free(const char *path) {
    struct stat st;

    if (stat(path, &st) == 0) {
        errno = EEXIST;
        return -1;
    }
    return 0;
}

char *mktemp(char *template_path) {
    if (from_template(template_path, name_is_free) < 0)
        template_path[0] = '\0';
    return template_path;
}

char *mkdtemp(char *template_path) {
    return from_template(template_path, create_directory) < 0 ? NULL : template_path;
}

char *realpath(const char *path, char *out) {
    char full[PATH_MAX * 2];
    struct stat st;

    if (!path || !*path) {
        errno = path ? ENOENT : EINVAL;
        return NULL;
    }
    if (*path == '/')
        full[0] = '\0';
    else if (!getcwd(full, PATH_MAX))
        return NULL;
    if (strlen(full) + strlen(path) + 2 > sizeof(full)) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    strcat(strcat(full, "/"), path);

    // Each part is checked as it's reached, so a missing or non-directory one in
    // the middle fails rather than being cancelled out by a later ...
    char result[PATH_MAX];
    size_t len = 0;
    char *state;

    result[0] = '\0';
    for (char *part = strtok_r(full, "/", &state); part; part = strtok_r(NULL, "/", &state)) {
        if (strcmp(part, ".") == 0)
            continue;
        if (strcmp(part, "..") == 0) {
            while (len > 0 && result[--len] != '/')
                ;
            result[len] = '\0';
            continue;
        }
        if (len + strlen(part) + 2 > sizeof(result)) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        result[len++] = '/';
        strcpy(result + len, part);
        len += strlen(part);
        if (stat(result, &st) < 0)
            return NULL;
    }
    if (len == 0)
        strcpy(result, "/");

    if (!out)
        return strdup(result);
    return strcpy(out, result);
}

static void report(const char *fmt, va_list args, bool with_errno) {
    int saved = errno;

    dprintf(STDERR_FILENO, "error: ");
    vdprintf(STDERR_FILENO, fmt, args);
    if (with_errno)
        dprintf(STDERR_FILENO, ": %s", strerror(saved));
    dprintf(STDERR_FILENO, "\n");
}

#define REPORT(with_errno)        \
    va_list args;                 \
    va_start(args, fmt);          \
    report(fmt, args, with_errno); \
    va_end(args)

void err(int status, const char *fmt, ...) {
    REPORT(true);
    exit(status);
}

void errx(int status, const char *fmt, ...) {
    REPORT(false);
    exit(status);
}

void warn(const char *fmt, ...) {
    REPORT(true);
}

void warnx(const char *fmt, ...) {
    REPORT(false);
}
