#include <aximos.h>
#include <blkid/blkid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

// Lists drives, partitions them with GPT, adds partitions in their free space, and
// makes filesystems on them: FAT32 of its own, as the firmware boots from, and ext4
// through mkfs.ext4. A drive is erased only if nothing has it mounted. Adding to
// one only writes its partition table and space no partition has, so may be done
// while it's in use.

#define SECTOR 512
#define MAX_DISKS 16
#define MAX_PARTITIONS 16
#define GPT_ENTRIES 128
#define GPT_ENTRY_SIZE 128
#define GPT_TABLE_SECTORS (GPT_ENTRIES * GPT_ENTRY_SIZE / SECTOR)
#define ALIGN 2048 // partitions start on whole MiB
#define CHUNK (64 * 1024)
#define FAT_RESERVED 32
#define FAT_MIN_CLUSTERS 65525

struct disk {
    char name[32];
    uint64_t sectors;
    bool usable; // a disk that can be written to
};

struct partition {
    char name[40];
    uint64_t first, count;
};

static const uint8_t esp_type[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                     0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
static const uint8_t linux_type[16] = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                       0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};
static const uint8_t data_type[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                      0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
static bool yes;

static int fail(const char *what) {
    dprintf(STDERR_FILENO, "disk: %s: %s\n", what, strerror(errno));
    return 1;
}

// A number from a file in /sys, or 0.
static uint64_t sys_number(const char *path) {
    FILE *f = fopen(path, "r");
    unsigned long long n = 0;

    if (f) {
        if (fscanf(f, "%llu", &n) != 1)
            n = 0;
        fclose(f);
    }
    return n;
}

// The drives Linux has, apart from those in memory, such as loop devices.
static int disks(struct disk *out) {
    DIR *dir = opendir("/sys/block");
    struct dirent *e;
    char path[300];
    int n = 0;

    while (dir && n < MAX_DISKS && (e = readdir(dir))) {
        if (e->d_name[0] == '.' || !strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3)
            || !strncmp(e->d_name, "zram", 4))
            continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        snprintf(path, sizeof(path), "/sys/block/%s/size", e->d_name);
        out[n].sectors = sys_number(path);
        snprintf(path, sizeof(path), "/sys/block/%s/ro", e->d_name);
        out[n].usable = out[n].sectors && !sys_number(path);
        n++;
    }
    if (dir)
        closedir(dir);
    return n;
}

static bool transfer(int fd, uint64_t lba, void *buf, size_t sectors, bool write_it) {
    uint8_t *at = buf;
    size_t left = sectors * SECTOR;

    if (lseek(fd, lba * SECTOR, SEEK_SET) < 0)
        return false;
    while (left) {
        ssize_t n = write_it ? write(fd, at, left) : read(fd, at, left);

        if (n <= 0)
            return false;
        at += n;
        left -= n;
    }
    return true;
}

static bool put(int fd, uint64_t lba, const void *buf, size_t sectors) {
    return transfer(fd, lba, (void *)buf, sectors, true);
}

static bool zero(int fd, uint64_t lba, uint64_t sectors) {
    static uint8_t zeros[CHUNK];

    for (uint64_t n; sectors; sectors -= n, lba += n) {
        n = sectors < CHUNK / SECTOR ? sectors : CHUNK / SECTOR;
        if (!put(fd, lba, zeros, n))
            return false;
    }
    return true;
}

static uint32_t crc32(const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t crc = 0xFFFFFFFF;

    while (len--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++)
            crc = crc >> 1 ^ (0xEDB88320 & -(crc & 1));
    }
    return ~crc;
}

static void random_guid(uint8_t guid[16]) {
    int fd = open("/dev/urandom", O_RDONLY);

    if (fd < 0 || read(fd, guid, 16) != 16)
        memset(guid, 0x5A, 16);
    if (fd >= 0)
        close(fd);
    guid[7] = (guid[7] & 0x0F) | 0x40;
    guid[8] = (guid[8] & 0x3F) | 0x80;
}

// The partitions the kernel found on a drive, in /sys/block/<drive>/<partition>.
static int partitions(const char *drive, struct partition *out) {
    char path[300];
    struct dirent *e;
    int n = 0;

    snprintf(path, sizeof(path), "/sys/block/%s", drive);

    DIR *dir = opendir(path);

    while (dir && n < MAX_PARTITIONS && (e = readdir(dir))) {
        snprintf(path, sizeof(path), "/sys/block/%s/%s/partition", drive, e->d_name);
        if (e->d_name[0] == '.' || access(path, F_OK))
            continue;
        snprintf(out[n].name, sizeof(out[n].name), "%s", e->d_name);
        snprintf(path, sizeof(path), "/sys/block/%s/%s/start", drive, e->d_name);
        out[n].first = sys_number(path);
        snprintf(path, sizeof(path), "/sys/block/%s/%s/size", drive, e->d_name);
        out[n].count = sys_number(path);
        n++;
    }
    if (dir)
        closedir(dir);
    return n;
}

// What udev found on a device, which anyone can read, though only root can read
// the device itself: lines like "E:ID_FS_TYPE=ext4" in /run/udev/data/b8:1.
static void udev_found(const char *name, char *type, char *label, size_t size) {
    char path[300], line[300], number[32] = "";
    FILE *f;

    snprintf(path, sizeof(path), "/sys/class/block/%s/dev", name);
    if ((f = fopen(path, "r"))) {
        if (fgets(number, sizeof(number), f))
            number[strcspn(number, "\n")] = '\0';
        fclose(f);
    }
    snprintf(path, sizeof(path), "/run/udev/data/b%s", number);
    if (!number[0] || !(f = fopen(path, "r")))
        return;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (!strncmp(line, "E:ID_FS_TYPE=", 13))
            snprintf(type, size, "%s", line + 13);
        else if (!strncmp(line, "E:ID_FS_LABEL=", 14))
            snprintf(label, size, "%s", line + 14);
    }
    fclose(f);
}

// What's on a drive or partition, as its type and label, as "ext4 AXIMOS".
static void label_of(const char *name, char *out, size_t size) {
    char path[64], type[32] = "", label[32] = "";
    const char *found;

    snprintf(path, sizeof(path), "/dev/%s", name);

    blkid_probe probe = blkid_new_probe_from_filename(path);

    if (probe && blkid_do_safeprobe(probe) == 0) {
        if (blkid_probe_lookup_value(probe, "TYPE", &found, NULL) == 0)
            snprintf(type, sizeof(type), "%s", found);
        if (blkid_probe_lookup_value(probe, "LABEL", &found, NULL) == 0)
            snprintf(label, sizeof(label), "%s", found);
    } else if (!probe) {
        udev_found(name, type, label, sizeof(type));
    }
    snprintf(out, size, "%s%s%s", type[0] ? type : "unformatted", label[0] ? " " : "", label);
    if (probe)
        blkid_free_probe(probe);
}

static void show_size(uint64_t sectors) {
    print_size(sectors * SECTOR, 9);
}

static int by_start(const void *a, const void *b) {
    const struct partition *x = a, *y = b;

    return (x->first > y->first) - (x->first < y->first);
}

// The stretches of a GPT drive no partition has, from its first usable sector, of a
// MiB or more, each starting on a whole MiB. They're added to the end of the
// partitions, which are sorted.
static int add_gaps(uint64_t sectors, uint64_t usable, struct partition *p, int n, int room) {
    uint64_t at = (usable + ALIGN - 1) / ALIGN * ALIGN, end = sectors - 1 - GPT_TABLE_SECTORS; // past the last usable sector
    int used = n;

    qsort(p, n, sizeof(*p), by_start);
    for (int i = 0; i <= used && n < room; i++) {
        uint64_t until = i < used ? p[i].first : end;

        if (until >= at + ALIGN)
            p[n++] = (struct partition){.first = at, .count = until - at};
        if (i < used && p[i].first + p[i].count > at)
            at = (p[i].first + p[i].count + ALIGN - 1) / ALIGN * ALIGN;
    }
    return n;
}

static int list(void) {
    struct disk d[MAX_DISKS];
    int count = disks(d);

    for (int i = 0; i < count; i++) {
        struct partition p[MAX_PARTITIONS * 2];
        char label[64];
        int n = partitions(d[i].name, p);

        if (n) {
            n = add_gaps(d[i].sectors, ALIGN, p, n, MAX_PARTITIONS * 2);
            qsort(p, n, sizeof(*p), by_start);
        }

        printf("%-10s", d[i].name);
        show_size(d[i].sectors);
        printf("  %s\n", d[i].usable ? "disk" : "disc or read-only");
        if (!d[i].usable)
            continue;
        if (n == 0) {
            label_of(d[i].name, label, sizeof(label));
            if (strcmp(label, "unformatted"))
                printf("  (whole)   %s\n", label);
        }
        for (int j = 0; j < n; j++) {
            if (p[j].name[0])
                label_of(p[j].name, label, sizeof(label));
            printf("  %-10s", p[j].name[0] ? p[j].name : "free");
            show_size(p[j].count);
            printf("%s%s\n", p[j].name[0] ? "  " : "", p[j].name[0] ? label : "");
        }
    }
    return 0;
}

static bool confirm(const char *name) {
    char answer[8];

    if (yes)
        return true;
    printf("This erases everything on %s. Go on? [y/N] ", name);
    fflush(stdout);
    return fgets(answer, sizeof(answer), stdin) && (answer[0] == 'y' || answer[0] == 'Y');
}

// Opens a drive or partition by name, for writing, which it must be possible to.
// Opening it exclusively fails while anything has it mounted.
static int open_drive(const char *name, struct disk *out, bool exclusive) {
    char path[64];
    int fd;

    snprintf(path, sizeof(path), "/sys/class/block/%s/size", name);
    *out = (struct disk){.sectors = sys_number(path)};
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(path, sizeof(path), "/dev/%s", name);
    if (!out->sectors) {
        dprintf(STDERR_FILENO, "disk: no drive or partition called %s\n", name);
        return -1;
    }
    if ((fd = open(path, O_RDWR | O_CLOEXEC | (exclusive ? O_EXCL : 0))) < 0 && errno == EBUSY)
        dprintf(STDERR_FILENO, "disk: %s is in use, as something on it is mounted\n", name);
    else if (fd < 0)
        fail(path);
    return fd;
}

static void put_header(uint8_t *h, uint64_t self, uint64_t other, uint64_t entries, uint64_t sectors, uint64_t first,
                       const uint8_t guid[16], uint32_t table_crc) {
    uint32_t revision = 0x10000, header_size = 92, count = GPT_ENTRIES, size = GPT_ENTRY_SIZE, crc;
    uint64_t last = sectors - 2 - GPT_TABLE_SECTORS;

    memset(h, 0, SECTOR);
    memcpy(h, "EFI PART", 8);
    memcpy(h + 8, &revision, 4);
    memcpy(h + 12, &header_size, 4);
    memcpy(h + 24, &self, 8);
    memcpy(h + 32, &other, 8);
    memcpy(h + 40, &first, 8);
    memcpy(h + 48, &last, 8);
    memcpy(h + 56, guid, 16);
    memcpy(h + 72, &entries, 8);
    memcpy(h + 80, &count, 4);
    memcpy(h + 84, &size, 4);
    memcpy(h + 88, &table_crc, 4);
    crc = crc32(h, header_size);
    memcpy(h + 16, &crc, 4);
}

// The header and its entries at the start, and copies at the end. Partitions start
// from first, which disk has right after the entries, though others have 2048.
static bool put_gpt(int fd, uint64_t sectors, uint64_t first, const uint8_t *table, const uint8_t guid[16]) {
    uint8_t header[SECTOR];
    uint32_t table_crc = crc32(table, GPT_TABLE_SECTORS * SECTOR);
    bool ok;

    put_header(header, 1, sectors - 1, 2, sectors, first, guid, table_crc);
    ok = put(fd, 1, header, 1) && put(fd, 2, table, GPT_TABLE_SECTORS);
    put_header(header, sectors - 1, 1, sectors - 1 - GPT_TABLE_SECTORS, sectors, first, guid, table_crc);
    ok = ok && put(fd, sectors - 1 - GPT_TABLE_SECTORS, table, GPT_TABLE_SECTORS) && put(fd, sectors - 1, header, 1);
    return ok && fsync(fd) == 0;
}

// One partition for each size in MiB, and the rest of the drive for "rest" or
// when there are none. The first is the EFI system partition, which the firmware
// boots from.
static int partition(const char *name, char **sizes, int size_count) {
    static uint8_t table[GPT_TABLE_SECTORS * SECTOR];
    uint8_t mbr[SECTOR] = {0}, guid[16];
    struct disk d;
    uint64_t next = ALIGN;
    int fd, n = size_count ? size_count : 1;

    if (n > GPT_ENTRIES)
        n = GPT_ENTRIES;
    memset(table, 0, sizeof(table));
    if ((fd = open_drive(name, &d, true)) < 0)
        return 1;

    uint64_t last = d.sectors - 2 - GPT_TABLE_SECTORS;

    for (int i = 0; i < n; i++) {
        bool rest = !size_count || !strcmp(sizes[i], "rest");
        char *end;
        uint64_t mib = rest ? 0 : strtoull(sizes[i], &end, 10);

        if (!rest && (*end || mib == 0)) {
            dprintf(STDERR_FILENO, "disk: %s isn't a size in MiB\n", sizes[i]);
            close(fd);
            return 1;
        }

        uint64_t count = rest ? last + 1 - next : mib * 2048;
        uint64_t final = next + count - 1;
        uint8_t *e = &table[i * GPT_ENTRY_SIZE];

        if (next > last || final > last || (rest && i != n - 1)) {
            dprintf(STDERR_FILENO, "disk: the partitions don't fit on %s\n", name);
            close(fd);
            return 1;
        }
        memcpy(e, i == 0 ? esp_type : data_type, 16);
        random_guid(e + 16);
        memcpy(e + 32, &next, 8);
        memcpy(e + 40, &final, 8);

        const char *title = i == 0 ? "EFI system" : "AximOS data";

        for (int c = 0; title[c]; c++)
            e[56 + 2 * c] = title[c]; // in UTF-16
        next = (final + ALIGN) / ALIGN * ALIGN;
    }
    if (!confirm(name)) {
        close(fd);
        return 1;
    }

    // An MBR saying the whole disk is taken, for what doesn't know GPT.
    uint32_t one = 1, covered = d.sectors - 1 > UINT32_MAX ? UINT32_MAX : d.sectors - 1;

    memcpy(mbr + 446, (uint8_t[]){0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF}, 8);
    memcpy(mbr + 446 + 8, &one, 4);
    memcpy(mbr + 446 + 12, &covered, 4);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    random_guid(guid);
    // What was before the first partition goes, as a disc image's would still be found.
    bool ok = zero(fd, 2 + GPT_TABLE_SECTORS, ALIGN - 2 - GPT_TABLE_SECTORS) && put(fd, 0, mbr, 1)
        && put_gpt(fd, d.sectors, 2 + GPT_TABLE_SECTORS, table, guid);
    if (ok && ioctl(fd, BLKRRPART) < 0)
        dprintf(STDERR_FILENO, "disk: Linux will see the new partitions once %s is restarted\n", name);
    close(fd);
    if (!ok)
        return fail(name);
    printf("disk: partitioned %s\n", name);
    return 0;
}

// Reads a drive's GPT, if it's laid out as disk would lay it out, as any tool does:
// 128 entries after the header, and copies at the drive's end.
static bool get_gpt(int fd, uint64_t sectors, uint8_t *table, uint8_t guid[16], uint64_t *first) {
    uint8_t header[SECTOR], expect[SECTOR];

    if (!transfer(fd, 1, header, 1, false) || memcmp(header, "EFI PART", 8)
        || !transfer(fd, 2, table, GPT_TABLE_SECTORS, false))
        return false;
    memcpy(guid, header + 56, 16);
    memcpy(first, header + 40, 8);
    put_header(expect, 1, sectors - 1, 2, sectors, *first, guid, crc32(table, GPT_TABLE_SECTORS * SECTOR));
    return !memcmp(header, expect, 92);
}

// Adds a partition to a GPT drive, in the first free stretch it fits, or for "rest"
// the largest. An ESP, or one for Linux.
static int add(const char *name, const char *size, const char *type) {
    static uint8_t table[GPT_TABLE_SECTORS * SECTOR];
    struct partition p[GPT_ENTRIES + MAX_PARTITIONS];
    uint8_t guid[16];
    struct disk d;
    uint64_t usable;
    bool esp = !strcmp(type, "esp"), rest = !strcmp(size, "rest");
    char *end;
    uint64_t want = rest ? 0 : strtoull(size, &end, 10) * 2048;
    int fd, n = 0, slot = -1, best = -1;

    if (!esp && strcmp(type, "linux")) {
        dprintf(STDERR_FILENO, "disk: it adds esp or linux partitions, not %s\n", type);
        return 1;
    }
    if (!rest && (*end || want == 0)) {
        dprintf(STDERR_FILENO, "disk: %s isn't a size in MiB\n", size);
        return 1;
    }
    if ((fd = open_drive(name, &d, false)) < 0)
        return 1;
    if (!get_gpt(fd, d.sectors, table, guid, &usable)) {
        dprintf(STDERR_FILENO, "disk: %s has no GPT partition table disk can add to\n", name);
        close(fd);
        return 1;
    }
    for (int i = 0; i < GPT_ENTRIES; i++) {
        uint8_t *e = &table[i * GPT_ENTRY_SIZE];
        uint64_t first, final;

        if (!memcmp(e, (uint8_t[16]){0}, 16)) {
            slot = slot < 0 ? i : slot;
            continue;
        }
        memcpy(&first, e + 32, 8);
        memcpy(&final, e + 40, 8);
        p[n++] = (struct partition){.first = first, .count = final + 1 - first};
    }

    int used = n;

    n = add_gaps(d.sectors, usable, p, n, GPT_ENTRIES + MAX_PARTITIONS);
    for (int i = used; i < n; i++) {
        if (rest ? best < 0 || p[i].count > p[best].count : best < 0 && p[i].count >= want)
            best = i;
    }
    if (slot < 0 || best < 0) {
        dprintf(STDERR_FILENO, "disk: %s has no free space that big\n", name);
        close(fd);
        return 1;
    }

    uint64_t first = p[best].first, final = first + (rest ? p[best].count : want) - 1;
    uint8_t *e = &table[slot * GPT_ENTRY_SIZE];
    const char *title = esp ? "EFI system" : "AximOS";

    memcpy(e, esp ? esp_type : linux_type, 16);
    random_guid(e + 16);
    memcpy(e + 32, &first, 8);
    memcpy(e + 40, &final, 8);
    for (int c = 0; title[c]; c++)
        e[56 + 2 * c] = title[c];

    // Linux is told of it by itself, as it won't read the table again while in use.
    struct blkpg_partition part = {.start = first * SECTOR, .length = (final + 1 - first) * SECTOR, .pno = slot + 1};
    bool ok = put_gpt(fd, d.sectors, usable, table, guid);

    if (ok && ioctl(fd, BLKPG, &(struct blkpg_ioctl_arg){.op = BLKPG_ADD_PARTITION, .datalen = sizeof(part), .data = &part}) < 0)
        dprintf(STDERR_FILENO, "disk: Linux will see the new partition once %s is restarted\n", name);
    close(fd);
    if (!ok)
        return fail(name);

    size_t len = strlen(name);

    printf("disk: added %s%s%d, ", name, len && name[len - 1] >= '0' && name[len - 1] <= '9' ? "p" : "", slot + 1);
    show_size(final + 1 - first);
    printf("\n");
    return 0;
}

static void u16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2);
}

static void u32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

// FAT32 over count sectors, which start hidden sectors into the drive, with
// clusters as big as Windows would make them, which needs at least 65525 of them.
static bool make_fat32(int fd, uint64_t hidden, uint64_t count, const char *label) {
    const uint64_t first = 0;
    uint32_t spc = count <= 532480 ? 1 : count <= 16777216 ? 8 : count <= 33554432 ? 16 : count <= 67108864 ? 32 : 64;
    uint32_t fat = (count - FAT_RESERVED + (256 * spc + 2) / 2 - 1) / ((256 * spc + 2) / 2);
    uint64_t data = FAT_RESERVED + 2 * (uint64_t)fat;
    uint32_t clusters = (count - data) / spc;
    uint8_t id[16], boot[SECTOR] = {0}, info[SECTOR] = {0}, fat_start[SECTOR] = {0}, root[SECTOR] = {0};

    if (count > UINT32_MAX || count < data || clusters < FAT_MIN_CLUSTERS) {
        dprintf(STDERR_FILENO, "disk: FAT32 needs between 33 MB and 2 TB\n");
        return false;
    }
    random_guid(id);

    memcpy(boot, "\xEB\x58\x90" "AXIMOS  ", 11);
    u16(boot + 11, SECTOR);
    boot[13] = spc;
    u16(boot + 14, FAT_RESERVED);
    boot[16] = 2;
    boot[21] = 0xF8;
    u16(boot + 24, 63);
    u16(boot + 26, 255);
    u32(boot + 28, hidden);
    u32(boot + 32, count);
    u32(boot + 36, fat);
    u32(boot + 44, 2); // the root directory's cluster
    u16(boot + 48, 1); // FSInfo
    u16(boot + 50, 6); // the copy of the boot sector
    boot[64] = 0x80;
    boot[66] = 0x29;
    memcpy(boot + 67, id, 4);
    memset(boot + 71, ' ', 11);
    memcpy(boot + 71, label, strlen(label) < 11 ? strlen(label) : 11);
    memcpy(boot + 82, "FAT32   ", 8);
    boot[510] = 0x55;
    boot[511] = 0xAA;

    u32(info, 0x41615252);
    u32(info + 484, 0x61417272);
    u32(info + 488, clusters - 1);
    u32(info + 492, 3);
    u32(info + 508, 0xAA550000);

    u32(fat_start, 0x0FFFFFF8);
    u32(fat_start + 4, 0x0FFFFFFF);
    u32(fat_start + 8, 0x0FFFFFFF); // the root directory, one cluster long

    memcpy(root, boot + 71, 11);
    root[11] = 0x08; // the volume's label

    // The boot sector goes last, so a format that fails part way leaves nothing
    // that looks like a filesystem.
    return zero(fd, first, data + spc) && put(fd, first + 1, info, 1) && put(fd, first + 6, boot, 1)
        && put(fd, first + 7, info, 1) && put(fd, first + FAT_RESERVED, fat_start, 1)
        && put(fd, first + FAT_RESERVED + fat, fat_start, 1) && put(fd, first + data, root, 1)
        && put(fd, first, boot, 1);
}

// ext4, through e2fsprogs' mkfs.ext4.
static bool make_ext4(const char *name, const char *label) {
    char path[64];
    int status;
    pid_t pid = fork();

    snprintf(path, sizeof(path), "/dev/%s", name);
    if (pid == 0) {
        execlp("mkfs.ext4", "mkfs.ext4", "-q", "-F", "-L", label, path, (char *)NULL);
        _exit(127);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// A drive, as sda, or one of its partitions, as sda1.
static int format(const char *target, const char *type, const char *label) {
    struct disk drive;
    char path[300];
    bool ext4 = !strcmp(type, "ext4");
    int fd;

    if (!ext4 && strcmp(type, "fat32")) {
        dprintf(STDERR_FILENO, "disk: it can make ext4 or fat32, not %s\n", type);
        return 1;
    }
    if ((fd = open_drive(target, &drive, true)) < 0)
        return 1;
    snprintf(path, sizeof(path), "/sys/class/block/%s/start", target);
    if (!confirm(target)) {
        close(fd);
        return 1;
    }

    bool ok = ext4 ? (close(fd), fd = -1, make_ext4(target, label)) : make_fat32(fd, sys_number(path), drive.sectors, label);

    if (fd >= 0)
        close(fd);
    if (!ok) {
        dprintf(STDERR_FILENO, "disk: couldn't format %s\n", target);
        return 1;
    }
    printf("disk: formatted %s as %s\n", target, ext4 ? "ext4" : "FAT32");
    return 0;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: disk [list]\n"
                            "       disk partition DRIVE [MIB...|rest] [-y]\n"
                            "       disk add DRIVE MIB|rest esp|linux\n"
                            "       disk format DRIVE|PARTITION ext4|fat32 [LABEL] [-y]\n"
                            "Lists the drives and what's on them, gives a drive a new GPT partition table,\n"
                            "or makes a filesystem on a drive or partition. partition makes one of each\n"
                            "size given, the first the EFI system partition, and fills the drive without\n"
                            "any. Both erase what was there, so they ask first, unless -y. add puts a\n"
                            "partition in a GPT drive's free space, in the first stretch it fits, or the\n"
                            "largest for rest, and leaves the others as they are.\n");

    int n = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y"))
            yes = true;
        else
            argv[++n] = argv[i];
    }
    if (n == 0 || !strcmp(argv[1], "list"))
        return list();
    if (!strcmp(argv[1], "partition") && n >= 2)
        return partition(argv[2], argv + 3, n - 2);
    if (!strcmp(argv[1], "add") && n == 4)
        return add(argv[2], argv[3], argv[4]);
    if (!strcmp(argv[1], "format") && (n == 3 || n == 4))
        return format(argv[2], argv[3], n == 4 ? argv[4] : "AXIMOS");
    dprintf(STDERR_FILENO, "disk: try disk --help\n");
    return 1;
}
