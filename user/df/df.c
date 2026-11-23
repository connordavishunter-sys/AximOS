#include <aximos.h>
#include <mntent.h>
#include <stdio.h>
#include <string.h>
#include <sys/statvfs.h>

#define COLUMN_WIDTH 10

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: df\n"
                            "Shows the size and free space of each mounted filesystem.\n");

    FILE *mounts = setmntent("/proc/mounts", "r");
    struct mntent *m;

    if (!mounts) {
        perror("df: /proc/mounts");
        return 1;
    }
    printf("%-12s %-8s %10s %10s %10s %4s  %s\n", "Filesystem", "Type", "Size", "Used", "Available", "Use%",
           "Mounted on");
    while ((m = getmntent(mounts))) {
        struct statvfs fs;

        // Those like /proc have no size, and devtmpfs only holds device files.
        if (statvfs(m->mnt_dir, &fs) < 0 || !fs.f_blocks || !strcmp(m->mnt_type, "devtmpfs"))
            continue;

        uint64_t total = (uint64_t)fs.f_blocks * fs.f_frsize, free = (uint64_t)fs.f_bavail * fs.f_frsize;
        uint64_t used = total - (uint64_t)fs.f_bfree * fs.f_frsize;

        printf("%-12s %-8s ", m->mnt_fsname, m->mnt_type);
        print_size(total, COLUMN_WIDTH);
        printf(" ");
        print_size(used, COLUMN_WIDTH);
        printf(" ");
        print_size(free, COLUMN_WIDTH);
        printf(" %3d%%  %s\n", (int)(total ? used * 100 / total : 0), m->mnt_dir);
    }
    endmntent(mounts);
    return 0;
}
