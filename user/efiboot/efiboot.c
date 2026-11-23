#include <aximos.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Shows and changes the firmware's boot entries, its Boot#### variables, through
// Linux's efivarfs: which there are, the order they're tried in, and the one to
// try next, once. An entry starts a loader from a GPT partition, found by its
// PARTUUID, so it works whatever the drive is called.

#define VARS "/sys/firmware/efi/efivars"
#define GLOBAL "8be4df61-93ca-11d2-aa0d-00e098032b8c"
#define ATTRIBUTES 7 // kept through restarts, and seen by the firmware and the OS
#define MAX_VAR 4096
#define MAX_ENTRIES 256

static int fail(const char *what) {
    dprintf(STDERR_FILENO, "efiboot: %s: %s\n", what, strerror(errno));
    return 1;
}

// A variable's value, without the attributes efivarfs puts before it, or -1.
static int get(const char *name, uint8_t *out, size_t size) {
    char path[128];
    uint8_t buf[MAX_VAR + 4];

    snprintf(path, sizeof(path), VARS "/%s-" GLOBAL, name);

    int fd = open(path, O_RDONLY), n = fd < 0 ? -1 : read(fd, buf, sizeof(buf));

    if (fd >= 0)
        close(fd);
    if (n < 4 || (size_t)(n - 4) > size)
        return -1;
    memcpy(out, buf + 4, n - 4);
    return n - 4;
}

// efivarfs makes its files immutable, so they're not removed by accident.
static void make_mutable(int fd) {
    int flags;

    if (ioctl(fd, FS_IOC_GETFLAGS, &flags) == 0 && flags & FS_IMMUTABLE_FL) {
        flags &= ~FS_IMMUTABLE_FL;
        ioctl(fd, FS_IOC_SETFLAGS, &flags);
    }
}

static bool set(const char *name, const void *value, size_t size) {
    char path[128];
    uint8_t buf[MAX_VAR + 4];
    uint32_t attributes = ATTRIBUTES;

    snprintf(path, sizeof(path), VARS "/%s-" GLOBAL, name);

    int fd = open(path, O_WRONLY | O_CREAT, 0644);

    if (fd < 0)
        return false;
    make_mutable(fd);
    memcpy(buf, &attributes, 4);
    memcpy(buf + 4, value, size);

    bool ok = write(fd, buf, size + 4) == (ssize_t)(size + 4); // in one write, as efivarfs needs

    close(fd);
    return ok;
}

static bool unset(const char *name) {
    char path[128];

    snprintf(path, sizeof(path), VARS "/%s-" GLOBAL, name);

    int fd = open(path, O_RDONLY);

    if (fd >= 0) {
        make_mutable(fd);
        close(fd);
    }
    return unlink(path) == 0 || errno == ENOENT;
}

// The GUID as it's written on the drive, the first three parts little-endian.
static bool parse_guid(const char *text, uint8_t out[16]) {
    static const int order[16] = {3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15};
    int at = 0;

    for (int i = 0; i < 16; i++, at += 2) {
        unsigned byte;

        at += text[at] == '-';
        if (sscanf(text + at, "%2x", &byte) != 1)
            return false;
        out[order[i]] = byte;
    }
    return text[at] == '\0';
}

static void show_guid(const uint8_t g[16], char out[37]) {
    snprintf(out, 37, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", g[3], g[2], g[1], g[0],
             g[5], g[4], g[7], g[6], g[8], g[9], g[10], g[11], g[12], g[13], g[14], g[15]);
}

// The partition a PARTUUID is, as udev links it, or a partition's PARTUUID.
static bool partition_of(const char *uuid, char *name, size_t size) {
    char path[128], link[128];
    ssize_t n;

    snprintf(path, sizeof(path), "/dev/disk/by-partuuid/%s", uuid);
    if ((n = readlink(path, link, sizeof(link) - 1)) < 0)
        return false;
    link[n] = '\0';
    snprintf(name, size, "%s", strrchr(link, '/') ? strrchr(link, '/') + 1 : link);
    return true;
}

static bool uuid_of(const char *partition, char *uuid, size_t size) {
    DIR *dir = opendir("/dev/disk/by-partuuid");
    struct dirent *e;
    char name[64];
    bool found = false;

    while (dir && !found && (e = readdir(dir))) {
        if (e->d_name[0] != '.' && partition_of(e->d_name, name, sizeof(name)) && !strcmp(name, partition)) {
            snprintf(uuid, size, "%s", e->d_name);
            found = true;
        }
    }
    if (dir)
        closedir(dir);
    return found;
}

static uint64_t sys_number(const char *partition, const char *what) {
    char path[128];
    unsigned long long n = 0;

    snprintf(path, sizeof(path), "/sys/class/block/%s/%s", partition, what);

    FILE *f = fopen(path, "r");

    if (f) {
        if (fscanf(f, "%llu", &n) != 1)
            n = 0;
        fclose(f);
    }
    return n;
}

// UTF-16 to ASCII, as far as the first NUL, giving how many bytes it took.
static size_t from_utf16(const uint8_t *in, size_t size, char *out, size_t room) {
    size_t i = 0, n = 0;

    for (; i + 1 < size && (in[i] || in[i + 1]); i += 2) {
        if (n + 1 < room)
            out[n++] = in[i + 1] || in[i] > 0x7E ? '?' : in[i];
    }
    out[n] = '\0';
    return i + 2;
}

static size_t to_utf16(const char *in, uint8_t *out) {
    size_t n = 0;

    for (; *in; in++, n += 2)
        out[n] = *in, out[n + 1] = 0;
    out[n] = out[n + 1] = 0;
    return n + 2;
}

// The order, as numbers, from BootOrder.
static int boot_order(uint16_t *order) {
    int n = get("BootOrder", (uint8_t *)order, MAX_ENTRIES * 2);

    return n < 0 ? 0 : n / 2;
}

// Where an entry's loader is: a partition, by name if it's here, and the file on it.
static void describe(const uint8_t *path, size_t size, char *out, size_t room) {
    char where[64] = "", file[256] = "";

    for (size_t at = 0; at + 4 <= size;) {
        uint8_t type = path[at], subtype = path[at + 1];
        uint16_t len = path[at + 2] | path[at + 3] << 8;

        if (type == 0x7F || len < 4 || at + len > size)
            break;
        if (type == 4 && subtype == 1 && len >= 42) { // a hard drive's partition
            char uuid[37];

            show_guid(path + at + 24, uuid);
            if (!partition_of(uuid, where, sizeof(where)))
                snprintf(where, sizeof(where), "%s", uuid);
        } else if (type == 4 && subtype == 4) {
            from_utf16(path + at + 4, len - 4, file, sizeof(file));
        }
        at += len;
    }
    snprintf(out, room, "%s%s%s", where[0] ? where : file[0] ? "" : "firmware's own", where[0] && file[0] ? " " : "",
             file);
}

static int list(void) {
    uint16_t order[MAX_ENTRIES], current, next;
    int n = boot_order(order);

    if (access(VARS, F_OK)) {
        dprintf(STDERR_FILENO, "efiboot: the firmware's variables aren't here, as it didn't start in UEFI\n");
        return 1;
    }
    if (get("BootCurrent", (uint8_t *)&current, 2) == 2)
        printf("Started from %04X\n", current);
    if (get("BootNext", (uint8_t *)&next, 2) == 2)
        printf("Next time    %04X\n", next);
    printf("Order       ");
    for (int i = 0; i < n; i++)
        printf(" %04X", order[i]);
    printf("\n");

    DIR *dir = opendir(VARS);
    struct dirent *e;

    while (dir && (e = readdir(dir))) {
        uint8_t entry[MAX_VAR];
        char name[16], title[128], where[320];
        unsigned number;
        int size;

        if (strlen(e->d_name) != 8 + 1 + 36 || sscanf(e->d_name, "Boot%4X-", &number) != 1
            || strcmp(e->d_name + 9, GLOBAL))
            continue;
        snprintf(name, sizeof(name), "Boot%04X", number);
        if ((size = get(name, entry, sizeof(entry))) < 6)
            continue;

        uint16_t path_size = entry[4] | entry[5] << 8;
        size_t title_size = from_utf16(entry + 6, size - 6, title, sizeof(title));

        size_t left = (size_t)size - 6 > title_size ? size - 6 - title_size : 0;

        describe(entry + 6 + title_size, path_size <= left ? path_size : 0, where, sizeof(where));
        printf("%04X %c %-30s %s\n", number, entry[0] & 1 ? '*' : ' ', title, where);
    }
    if (dir)
        closedir(dir);
    return 0;
}

static bool number(const char *text, uint16_t *out) {
    char *end;
    unsigned long n = strtoul(text, &end, 16);

    *out = n;
    if (*end || !*text || n > 0xFFFF) {
        dprintf(STDERR_FILENO, "efiboot: %s isn't an entry's number\n", text);
        return false;
    }
    return true;
}

static bool exists(uint16_t n) {
    char name[16];
    uint8_t buf[MAX_VAR];

    snprintf(name, sizeof(name), "Boot%04X", n);
    return get(name, buf, sizeof(buf)) >= 0;
}

static int set_order(uint16_t *order, int n) {
    return set("BootOrder", order, n * 2) ? 0 : fail("BootOrder");
}

// An entry for a loader on a partition, first in the order.
static int add(const char *partition, const char *loader, const char *title) {
    uint8_t entry[MAX_VAR], guid[16];
    char uuid[37], name[16];
    uint64_t start = sys_number(partition, "start"), sectors = sys_number(partition, "size");
    uint32_t part = sys_number(partition, "partition");
    uint16_t order[MAX_ENTRIES + 1], n = 0;
    int count = boot_order(order);

    if (!part || !uuid_of(partition, uuid, sizeof(uuid)) || !parse_guid(uuid, guid)) {
        dprintf(STDERR_FILENO, "efiboot: %s isn't a GPT partition\n", partition);
        return 1;
    }
    if (strlen(title) > 100 || strlen(loader) > 200) {
        dprintf(STDERR_FILENO, "efiboot: the name or loader's path is too long\n");
        return 1;
    }
    while (exists(n) && n < 0xFFFF)
        n++;

    uint32_t active = 1;
    size_t at = 6;

    memcpy(entry, &active, 4);
    at += to_utf16(title, entry + at);

    size_t path_at = at;
    uint8_t *hd = entry + at;

    // The partition, by its number, where it is on the drive, and its GUID.
    memcpy(hd, (uint8_t[]){4, 1, 42, 0}, 4);
    memcpy(hd + 4, &part, 4);
    memcpy(hd + 8, &start, 8);
    memcpy(hd + 16, &sectors, 8);
    memcpy(hd + 24, guid, 16);
    hd[40] = 2; // GPT
    hd[41] = 2; // and a GUID for its signature
    at += 42;

    // Then the loader's file, with backslashes, as the firmware has paths.
    uint8_t *file = entry + at;
    size_t len = to_utf16(loader, file + 4) + 4;

    for (size_t i = 4; i < len; i += 2)
        file[i] = file[i] == '/' ? '\\' : file[i];
    memcpy(file, (uint8_t[]){4, 4, len & 0xFF, len >> 8}, 4);
    at += len;
    memcpy(entry + at, (uint8_t[]){0x7F, 0xFF, 4, 0}, 4);
    at += 4;

    uint16_t path_size = at - path_at;

    memcpy(entry + 4, &path_size, 2);
    snprintf(name, sizeof(name), "Boot%04X", n);
    if (!set(name, entry, at))
        return fail(name);
    memmove(order + 1, order, count * 2);
    order[0] = n;
    if (set_order(order, count + 1))
        return 1;
    printf("efiboot: added %04X\n", n);
    return 0;
}

// Removes an entry, and it from the order.
static int remove_entry(const char *text) {
    uint16_t n, order[MAX_ENTRIES];
    int count = boot_order(order), kept = 0;
    char name[16];

    if (!number(text, &n))
        return 1;
    snprintf(name, sizeof(name), "Boot%04X", n);
    if (!exists(n)) {
        dprintf(STDERR_FILENO, "efiboot: there's no entry %04X\n", n);
        return 1;
    }
    if (!unset(name))
        return fail(name);
    for (int i = 0; i < count; i++) {
        if (order[i] != n)
            order[kept++] = order[i];
    }
    return kept == count ? 0 : set_order(order, kept);
}

static int order(char **numbers, int n) {
    uint16_t order[MAX_ENTRIES];

    if (n > MAX_ENTRIES)
        n = MAX_ENTRIES;
    for (int i = 0; i < n; i++) {
        if (!number(numbers[i], &order[i]))
            return 1;
        if (!exists(order[i])) {
            dprintf(STDERR_FILENO, "efiboot: there's no entry %04X\n", order[i]);
            return 1;
        }
    }
    return set_order(order, n);
}

static int next(const char *text) {
    uint16_t n;

    if (!number(text, &n))
        return 1;
    if (!exists(n)) {
        dprintf(STDERR_FILENO, "efiboot: there's no entry %04X\n", n);
        return 1;
    }
    return set("BootNext", &n, 2) ? 0 : fail("BootNext");
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: efiboot [list]\n"
                            "       efiboot add PARTITION LOADER NAME\n"
                            "       efiboot remove NUMBER\n"
                            "       efiboot order NUMBER...\n"
                            "       efiboot next NUMBER\n"
                            "Shows the firmware's boot entries, * for those it tries, and the order it tries\n"
                            "them in. add makes one that starts LOADER, a path on PARTITION, like\n"
                            "\\EFI\\BOOT\\BOOTX64.EFI, and puts it first. next has the firmware start one,\n"
                            "the next time only. Numbers are in hex, as list shows them.\n");

    if (argc == 1 || !strcmp(argv[1], "list"))
        return list();
    if (!strcmp(argv[1], "add") && argc == 5)
        return add(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "remove") && argc == 3)
        return remove_entry(argv[2]);
    if (!strcmp(argv[1], "order") && argc >= 3)
        return order(argv + 2, argc - 2);
    if (!strcmp(argv[1], "next") && argc == 3)
        return next(argv[2]);
    dprintf(STDERR_FILENO, "efiboot: try efiboot --help\n");
    return 1;
}
