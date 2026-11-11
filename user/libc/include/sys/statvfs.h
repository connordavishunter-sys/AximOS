#pragma once

struct statvfs {
    unsigned long f_bsize;
    unsigned long f_frsize;
    unsigned long f_blocks;
    unsigned long f_bfree;
    unsigned long f_bavail;
    unsigned long f_files; // always 0
    unsigned long f_ffree; // always 0
    unsigned long f_favail; // always 0
    unsigned long f_fsid; // always 0
    unsigned long f_flag; // always 0
    unsigned long f_namemax;
};

int statvfs(const char *path, struct statvfs *out);
