#pragma once

#include <stdint.h>
#include <time_types.h>

// Shared by the kernel and libc.

#define PATH_MAX 1024
#define NAME_MAX 255

#define DT_UNKNOWN 0
#define DT_FIFO 1
#define DT_CHR 2
#define DT_DIR 4
#define DT_BLK 6
#define DT_REG 8
#define DT_LNK 10
#define DT_SOCK 12

struct dirent {
    uint32_t d_size;
    uint8_t d_type;
    char d_name[NAME_MAX + 1];
};

#define S_IFMT 0170000
#define S_IFIFO 0010000
#define S_IFBLK 0060000
#define S_IFLNK 0120000
#define S_IFCHR 0020000
#define S_IFDIR 0040000
#define S_IFREG 0100000
#define S_IFSOCK 0140000
#define S_ISFIFO(mode) (((mode) & S_IFMT) == S_IFIFO)
#define S_ISCHR(mode) (((mode) & S_IFMT) == S_IFCHR)
#define S_ISDIR(mode) (((mode) & S_IFMT) == S_IFDIR)
#define S_ISREG(mode) (((mode) & S_IFMT) == S_IFREG)
#define S_ISLNK(mode) (((mode) & S_IFMT) == S_IFLNK)
#define S_ISBLK(mode) (((mode) & S_IFMT) == S_IFBLK)
#define S_ISSOCK(mode) (((mode) & S_IFMT) == S_IFSOCK)

#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IRWXU 0700
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXG 0070
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXO 0007
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

// There are no owners or permissions, so everything belongs to root and reads as
// rwxrwxrwx, and there are no hard links.
struct stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    uint64_t st_rdev; // always 0
    struct timespec st_atim;
    struct timespec st_mtim; // since 1970 in UTC, or 0 if unknown
    struct timespec st_ctim;
};

#define st_atime st_atim.tv_sec
#define st_mtime st_mtim.tv_sec
#define st_ctime st_ctim.tv_sec

#define POLLIN 0x001
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020

struct pollfd {
    int fd;
    short events;
    short revents;
};

// Sizes in bytes. kernel counts as used, and includes cached, which could be given back.
struct meminfo {
    uint64_t total;
    uint64_t free;
    uint64_t kernel;
    uint64_t cached;
};

struct statfs {
    uint64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    char f_type[16];
    char f_device[16];
};
