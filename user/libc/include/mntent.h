#pragma once

#include <stdio.h>

#define MNTOPT_NOAUTO "noauto"
#define MNTOPT_DEFAULTS "defaults"

struct mntent {
    char *mnt_fsname;
    char *mnt_dir;
    char *mnt_type;
    char *mnt_opts;
    int mnt_freq;
    int mnt_passno;
};

// There's no mount table to read, so setmntent fails with ENOENT.
FILE *setmntent(const char *path, const char *mode);
struct mntent *getmntent(FILE *stream);
struct mntent *getmntent_r(FILE *stream, struct mntent *out, char *buffer, int size);
int endmntent(FILE *stream);
