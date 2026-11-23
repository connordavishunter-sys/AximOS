#include <aximos.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <unistd.h>

// Options that are flags to mount, and the rest are passed on to the filesystem.
static const struct { const char *name; unsigned long flag; bool clears; } flags[] = {
    {"ro", MS_RDONLY, false},       {"rw", MS_RDONLY, true},     {"nosuid", MS_NOSUID, false},
    {"nodev", MS_NODEV, false},     {"noexec", MS_NOEXEC, false}, {"remount", MS_REMOUNT, false},
    {"noatime", MS_NOATIME, false}, {"bind", MS_BIND, false},    {"defaults", 0, false},
};

static unsigned long parse(char *options, char *data, size_t size) {
    unsigned long result = 0;

    data[0] = '\0';
    for (char *o = strtok(options, ","); o; o = strtok(NULL, ",")) {
        size_t i = 0;

        while (i < sizeof(flags) / sizeof(flags[0]) && strcmp(flags[i].name, o))
            i++;
        if (i < sizeof(flags) / sizeof(flags[0]))
            result = flags[i].clears ? result & ~flags[i].flag : result | flags[i].flag;
        else
            snprintf(data + strlen(data), size - strlen(data), "%s%s", data[0] ? "," : "", o);
    }
    return result;
}

// Without a type, each filesystem the kernel knows that needs a device is tried.
static int mount_any(const char *device, const char *dir, unsigned long flags, const char *data) {
    FILE *f = fopen("/proc/filesystems", "r");
    char line[64];
    int saved = ENODEV;

    while (f && fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "nodev", 5))
            continue;
        line[strcspn(line, "\n")] = '\0';
        if (mount(device, dir, line + strspn(line, "\t "), flags, data) == 0) {
            fclose(f);
            return 0;
        }
        saved = errno == EINVAL ? saved : errno;
    }
    if (f)
        fclose(f);
    errno = saved;
    return -1;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: mount [-t type] [-o options] device folder\n"
                            "Mounts a filesystem on a folder, or with nothing given, lists what's mounted.\n"
                            "  -t  its type, such as ext4 or vfat; without it each is tried\n"
                            "  -o  options separated by commas: ro, rw, nosuid, nodev, noexec, noatime,\n"
                            "      remount, bind, and any the filesystem has\n");

    char *type = NULL, *options = NULL, data[256];
    int option;

    while ((option = getopt(argc, argv, "t:o:")) != -1) {
        if (option == 't')
            type = optarg;
        else if (option == 'o')
            options = optarg;
        else
            return 1;
    }
    if (optind == argc) {
        FILE *f = fopen("/proc/mounts", "r");
        int c;

        while (f && (c = getc(f)) != EOF)
            putchar(c);
        return f ? 0 : 1;
    }
    if (argc - optind != 2) {
        fprintf(stderr, "usage: mount [-t type] [-o options] device folder\n");
        return 1;
    }

    unsigned long mount_flags = options ? parse(options, data, sizeof(data)) : (data[0] = '\0', 0);
    const char *device = argv[optind], *dir = argv[optind + 1];

    if ((type ? mount(device, dir, type, mount_flags, data) : mount_any(device, dir, mount_flags, data)) < 0) {
        fprintf(stderr, "mount: %s on %s: %s\n", device, dir, strerror(errno));
        return 1;
    }
    return 0;
}
