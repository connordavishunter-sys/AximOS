#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Lists drives, partitions them with GPT, and makes FAT32 filesystems on them,
// through /dev/disk, which takes a drive's filesystems away while it's open for
// writing and mounts what's there when it's closed.

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
    char name[16];
    uint64_t sectors;
    bool usable; // a disk that can be written to
    bool busy;
};

struct partition {
    int number;
    uint64_t first, count;
};

static const uint8_t esp_type[16] = {0x28, 0x73, 0x2A, 0xC1, 0x1F, 0xF8, 0xD2, 0x11,
                                     0xBA, 0x4B, 0x00, 0xA0, 0xC9, 0x3E, 0xC9, 0x3B};
static const uint8_t data_type[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                      0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
static bool yes;

static int fail(const char *what) {
    dprintf(STDERR_FILENO, "disk: %s: %s\n", what, strerror(errno));
    return 1;
}

static int disks(struct disk *out) {
    FILE *f = fopen("/proc/disks", "r");
    char line[128], kind[16], extra[32];
    unsigned long long sectors;
    unsigned size;
    int n = 0;

    if (!f)
        return 0;
    while (n < MAX_DISKS && fgets(line, sizeof(line), f)) {
        extra[0] = '\0';
        if (sscanf(line, "%15s %llu %u %15s %31[^\n]", out[n].name, &sectors, &size, kind, extra) < 4)
            continue;
        out[n].sectors = sectors * size / SECTOR;
        out[n].usable = !strcmp(kind, "disk") && !strstr(extra, "read-only");
        out[n].busy = strstr(extra, "busy");
        n++;
    }
    fclose(f);
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

// The partitions in a GPT, or failing that an MBR, numbered as the kernel names them.
static int partitions(int fd, struct partition *out) {
    static uint8_t table[GPT_TABLE_SECTORS * SECTOR];
    uint8_t mbr[SECTOR], header[SECTOR];
    int n = 0;

    if (!transfer(fd, 0, mbr, 1, false) || mbr[510] != 0x55 || mbr[511] != 0xAA)
        return 0;
    if (mbr[446 + 4] != 0xEE) {
        for (int i = 0; i < 4; i++) {
            const uint8_t *e = &mbr[446 + 16 * i];
            uint32_t first, count;

            memcpy(&first, e + 8, 4);
            memcpy(&count, e + 12, 4);
            if (e[4] && count)
                out[n++] = (struct partition){i + 1, first, count};
        }
        return n;
    }

    uint64_t entries;
    uint32_t count, size;

    if (!transfer(fd, 1, header, 1, false) || memcmp(header, "EFI PART", 8))
        return 0;
    memcpy(&entries, header + 72, 8);
    memcpy(&count, header + 80, 4);
    memcpy(&size, header + 84, 4);
    if (size != GPT_ENTRY_SIZE || count > GPT_ENTRIES || !transfer(fd, entries, table, GPT_TABLE_SECTORS, false))
        return 0;
    for (uint32_t i = 0; i < count && n < MAX_PARTITIONS; i++) {
        static const uint8_t unused[16];
        const uint8_t *e = &table[i * GPT_ENTRY_SIZE];
        uint64_t first, last;

        memcpy(&first, e + 32, 8);
        memcpy(&last, e + 40, 8);
        if (memcmp(e, unused, 16))
            out[n++] = (struct partition){i + 1, first, last - first + 1};
    }
    return n;
}

static void label_of(int fd, uint64_t lba, char label[12]) {
    uint8_t boot[SECTOR];

    label[0] = '\0';
    if (transfer(fd, lba, boot, 1, false) && !memcmp(boot + 82, "FAT32   ", 8)) {
        memcpy(label, boot + 71, 11);
        for (int i = 11; i > 0 && (label[i] == '\0' || label[i] == ' '); i--)
            label[i] = '\0';
        if (!strcmp(label, "NO NAME"))
            strcpy(label, "FAT32");
    } else {
        strcpy(label, "unformatted");
    }
}

static void show_size(uint64_t sectors) {
    print_size(sectors * SECTOR, 9);
}

static int list(void) {
    struct disk d[MAX_DISKS];
    int count = disks(d);

    for (int i = 0; i < count; i++) {
        char path[32], label[12];
        struct partition p[MAX_PARTITIONS];
        int fd, n = 0;

        printf("%-8s", d[i].name);
        show_size(d[i].sectors);
        printf("  %s\n", d[i].usable ? "disk" : "disc or read-only");
        snprintf(path, sizeof(path), "/dev/disk/%s", d[i].name);
        if (!d[i].usable || (fd = open(path, O_RDONLY)) < 0)
            continue;
        n = partitions(fd, p);
        if (n == 0) {
            label_of(fd, 0, label);
            if (strcmp(label, "unformatted"))
                printf("  (whole) %s\n", label);
        }
        for (int j = 0; j < n; j++) {
            label_of(fd, p[j].first, label);
            printf("  %s%-5d", d[i].name, p[j].number);
            show_size(p[j].count);
            printf("  %s\n", label);
        }
        close(fd);
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

// Opens a drive by name, for writing, which it must be possible to.
static int open_drive(const char *name, struct disk *out) {
    struct disk d[MAX_DISKS];
    int count = disks(d);
    char path[32];

    for (int i = 0; i < count; i++) {
        if (strcmp(d[i].name, name))
            continue;
        if (!d[i].usable) {
            dprintf(STDERR_FILENO, "disk: %s can't be written to\n", name);
            return -1;
        }
        *out = d[i];
        snprintf(path, sizeof(path), "/dev/disk/%s", name);

        int fd = open(path, O_RDWR);

        if (fd < 0 && errno == EBUSY)
            dprintf(STDERR_FILENO, "disk: %s is in use\n", name);
        else if (fd < 0)
            fail(path);
        return fd;
    }
    dprintf(STDERR_FILENO, "disk: no drive called %s\n", name);
    return -1;
}

static void put_header(uint8_t *h, uint64_t self, uint64_t other, uint64_t entries, uint64_t sectors,
                       const uint8_t guid[16], uint32_t table_crc) {
    uint32_t revision = 0x10000, header_size = 92, count = GPT_ENTRIES, size = GPT_ENTRY_SIZE, crc;
    uint64_t first = 2 + GPT_TABLE_SECTORS, last = sectors - 2 - GPT_TABLE_SECTORS;

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

// One partition for each size in MiB, and the rest of the drive for "rest" or
// when there are none. The first is the EFI system partition, which the firmware
// boots from.
static int partition(const char *name, char **sizes, int size_count) {
    static uint8_t table[GPT_TABLE_SECTORS * SECTOR];
    uint8_t mbr[SECTOR] = {0}, header[SECTOR], guid[16];
    struct disk d;
    uint64_t next = ALIGN;
    int fd, n = size_count ? size_count : 1;

    if (n > GPT_ENTRIES)
        n = GPT_ENTRIES;
    memset(table, 0, sizeof(table));
    if ((fd = open_drive(name, &d)) < 0)
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

    // The header and its entries at the start, and copies at the end.
    uint32_t table_crc = crc32(table, sizeof(table));
    bool ok;

    random_guid(guid);
    put_header(header, 1, d.sectors - 1, 2, d.sectors, guid, table_crc);
    // What was before the first partition goes, as a disc image's would still be found.
    ok = zero(fd, 2 + GPT_TABLE_SECTORS, ALIGN - 2 - GPT_TABLE_SECTORS) && put(fd, 0, mbr, 1) && put(fd, 1, header, 1)
        && put(fd, 2, table, GPT_TABLE_SECTORS);
    put_header(header, d.sectors - 1, 1, d.sectors - 1 - GPT_TABLE_SECTORS, d.sectors, guid, table_crc);
    ok = ok && put(fd, d.sectors - 1 - GPT_TABLE_SECTORS, table, GPT_TABLE_SECTORS) && put(fd, d.sectors - 1, header, 1);
    close(fd);
    if (!ok)
        return fail(name);
    printf("disk: partitioned %s\n", name);
    return 0;
}

static void u16(uint8_t *p, uint16_t v) {
    memcpy(p, &v, 2);
}

static void u32(uint8_t *p, uint32_t v) {
    memcpy(p, &v, 4);
}

// FAT32 over count sectors from first, with clusters as big as Windows would
// make them, which needs at least 65525 of them.
static bool make_fat32(int fd, uint64_t first, uint64_t count, const char *label) {
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
    u32(boot + 28, first);
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

// A drive, as sda, or one of its partitions, as sda1.
static int format(const char *target, const char *label) {
    struct disk d[MAX_DISKS], drive;
    struct partition p[MAX_PARTITIONS];
    int count = disks(d), best = -1;
    size_t len = 0;

    for (int i = 0; i < count; i++) {
        size_t l = strlen(d[i].name);

        if (!strncmp(target, d[i].name, l) && l > len && (!target[l] || (target[l] >= '0' && target[l] <= '9')))
            best = i, len = l;
    }
    if (best < 0) {
        dprintf(STDERR_FILENO, "disk: no drive called %s\n", target);
        return 1;
    }

    int fd = open_drive(d[best].name, &drive), number = target[len] ? atoi(target + len) : 0;
    uint64_t first = 0, sectors = drive.sectors;

    if (fd < 0)
        return 1;
    if (number) {
        int n = partitions(fd, p), i = 0;

        while (i < n && p[i].number != number)
            i++;
        if (i == n) {
            dprintf(STDERR_FILENO, "disk: %s has no partition %d\n", d[best].name, number);
            close(fd);
            return 1;
        }
        first = p[i].first;
        sectors = p[i].count;
    }
    if (!confirm(target) || !make_fat32(fd, first, sectors, label)) {
        close(fd);
        return 1;
    }
    close(fd);
    printf("disk: formatted %s as FAT32\n", target);
    return 0;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: disk [list]\n"
                            "       disk partition DRIVE [MIB...|rest] [-y]\n"
                            "       disk format DRIVE|PARTITION [LABEL] [-y]\n"
                            "Lists the drives and what's on them, gives a drive a new GPT partition table,\n"
                            "or makes a FAT32 filesystem on a drive or partition. partition makes one of\n"
                            "each size given, the first the EFI system partition, and fills the drive\n"
                            "without any. Both erase what was there, so they ask first, unless -y.\n");

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
    if (!strcmp(argv[1], "format") && (n == 2 || n == 3))
        return format(argv[2], n == 3 ? argv[3] : "AXIMOS");
    dprintf(STDERR_FILENO, "disk: try disk --help\n");
    return 1;
}
