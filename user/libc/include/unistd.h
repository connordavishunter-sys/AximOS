#pragma once

// What portable code checks for before using clock_gettime, gmtime_r and the like.
#define _POSIX_VERSION 200809L

#include <fs_types.h>
#include <stddef.h>
#include <sys/types.h>
#include <syscall_numbers.h>

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

extern char **environ;

// Goes through arguments like -a -b value, returning each option's letter, then
// -1 when they run out. A leading : in options makes a missing value return :.
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *options);

// Returns the seconds still left if a signal ended the sleep.
unsigned sleep(unsigned seconds);
int usleep(unsigned microseconds);

[[noreturn]] void _exit(int status);
pid_t fork(void);

// The same as fork, so the child gets its own copy of memory.
pid_t vfork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
int execv(const char *path, char *const argv[]);
int execvp(const char *file, char *const argv[]);
int execl(const char *path, const char *arg, ...);
int execlp(const char *file, const char *arg, ...);
int execle(const char *path, const char *arg, ...);
pid_t getpid(void);
pid_t gettid(void);
ssize_t read(int fd, void *buf, size_t len);
ssize_t write(int fd, const void *buf, size_t len);
int close(int fd);
long lseek(int fd, long offset, int whence);
int ftruncate(int fd, long size);
int pipe(int fds[2]);
int dup(int fd);
int dup2(int fd, int target);
int chdir(const char *path);
int unlink(const char *path);
int rmdir(const char *path);
int access(const char *path, int mode);
int setpgid(pid_t pid, pid_t pgid);
pid_t getppid(void);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
int setpgrp(void);
pid_t getsid(pid_t pid); // there are no sessions, so this is the process group
pid_t setsid(void);
pid_t tcgetpgrp(int fd);

// Sets the group Ctrl+C ends. 0 means none, leaving Ctrl+C an ordinary key.
int tcsetpgrp(int fd, pid_t pgid);

// Whether fd is the console, as a terminal.
int isatty(int fd);
char *ttyname(int fd);
int ttyname_r(int fd, char *out, size_t size);
char *ctermid(char *out);

// Seconds until SIGALRM, or 0 to cancel. Returns what was left on the previous one.
unsigned alarm(unsigned seconds);
int pause(void);

ssize_t pread(int fd, void *buf, size_t len, long offset);
ssize_t pwrite(int fd, const void *buf, size_t len, long offset);
int truncate(const char *path, long size);
int pipe2(int fds[2], int flags);
int dup3(int fd, int target, int flags);

// Writes go straight to the disk, so these only check their argument.
int fsync(int fd);
int fdatasync(int fd);
void sync(void);

// malloc gets its memory the same way, and works around whatever these do.
int brk(void *end);
void *sbrk(long change);

// There's one user, root, and these never fail.
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid); // fails with EPERM for anyone but root, as do the rest
int seteuid(uid_t uid);
int setgid(gid_t gid);
int setegid(gid_t gid);
int getgroups(int size, gid_t *groups);
char *getlogin(void);

// There's no ownership, so these only check the file exists.
int chown(const char *path, uid_t owner, gid_t group);
int fchown(int fd, uid_t owner, gid_t group);
int lchown(const char *path, uid_t owner, gid_t group);

// FAT32 can't store links, so these fail: EPERM to make one, EINVAL to read one.
int link(const char *path, const char *new_path);
int symlink(const char *target, const char *path);
ssize_t readlink(const char *path, char *buf, size_t size);

int gethostname(char *name, size_t len);
int getpagesize(void);
int getdtablesize(void);
int nice(int change);

#define _SC_ARG_MAX 0
#define _SC_CHILD_MAX 1
#define _SC_CLK_TCK 2
#define _SC_OPEN_MAX 4
#define _SC_PAGESIZE 30
#define _SC_PAGE_SIZE _SC_PAGESIZE
#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PHYS_PAGES 85
#define _SC_AVPHYS_PAGES 86

#define _PC_LINK_MAX 0
#define _PC_NAME_MAX 3
#define _PC_PATH_MAX 4
#define _PC_PIPE_BUF 5

long sysconf(int name);
long pathconf(const char *path, int name);
long fpathconf(int fd, int name);

// Returns buf, or NULL if the path doesn't fit in len bytes.
char *getcwd(char *buf, size_t len);

// Not POSIX: if the first argument is -h, prints the text and exits.
void help_option(int argc, char **argv, const char *text);

int getentropy(void *buf, size_t len);

// Not POSIX: the size of the text screen.
int termsize(int *rows, int *cols);

// Not POSIX: how memory is being used.
int meminfo(struct meminfo *info);

// There's no way to change these yet, so they fail with EPERM or ENOSYS.
int chroot(const char *path);
int fchdir(int fd);
int sethostname(const char *name, size_t len);
