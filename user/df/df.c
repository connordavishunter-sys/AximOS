#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/statfs.h>

#define COLUMN_WIDTH 10

static void show(const char *path, const char *mount_point) {
    struct statfs fs;

    if (statfs(path, &fs) < 0) {
        dprintf(STDERR_FILENO, "df: %s: %s\n", path, strerror(errno));
        return;
    }

    uint64_t total = fs.f_blocks * fs.f_bsize;
    uint64_t free = fs.f_bfree * fs.f_bsize;
    uint64_t used = total - free;
    int percent = total ? (int)(used * 100 / total) : 0;

    // tmpfs is in memory, with no device.
    static char device[32];
    strcpy(device, strcmp(fs.f_type, "tmpfs") ? "/dev/" : "");
    strcat(device, fs.f_device);

    printf("%-12s %-8s ", device, fs.f_type);
    print_size(total, COLUMN_WIDTH);
    printf(" ");
    print_size(used, COLUMN_WIDTH);
    printf(" ");
    print_size(free, COLUMN_WIDTH);
    printf(" %3d%%  %s\n", percent, mount_point);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: df\n"
                            "Shows the size and free space of each mounted filesystem.\n");

    printf("%-12s %-8s %10s %10s %10s %4s  %s\n", "Filesystem", "Type", "Size", "Used",
           "Available", "Use%", "Mounted on");

    show("/", "/");
    show("/tmp", "/tmp");

    DIR *dev = opendir("/dev");
    if (!dev) {
        dprintf(STDERR_FILENO, "df: can't open /dev\n");
        return 1;
    }

    static char path[PATH_MAX];
    struct dirent *entry;
    while ((entry = readdir(dev))) {
        if (entry->d_type != DT_DIR)
            continue;

        strcpy(path, "/dev/");
        strcpy(&path[5], entry->d_name);
        show(path, path);
    }
    closedir(dev);
    return 0;
}
