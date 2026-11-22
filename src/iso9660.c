#include <clock.h>
#include <iso9660.h>
#include <serial.h>
#include <string.h>

#define MAX_VOLUMES 4
#define SECTOR_SIZE 2048

#define PVD_LBA 16
#define PVD_TYPE 1
#define PVD_VOLUME_BLOCKS 80
#define PVD_BLOCK_SIZE 128
#define PVD_ROOT_RECORD 156

#define RECORD_LENGTH 0
#define RECORD_EXTENT 2
#define RECORD_SIZE 10
#define RECORD_DATE 18
#define RECORD_FLAGS 25
#define RECORD_NAME_LENGTH 32
#define RECORD_NAME 33
#define FLAG_DIRECTORY 0x02

#define RR_NAME_FLAGS 4
#define RR_NAME 5
#define RR_NAME_CONTINUES 0x01

struct iso9660_volume {
    struct block_device *drive;
    uint32_t volume_blocks;
    uint32_t root_extent;
    uint32_t root_size;
};

struct record {
    uint32_t extent;
    uint32_t size;
    int64_t mtime;
    bool is_dir;
    char name[NAME_MAX + 1];
};

static struct iso9660_volume volumes[MAX_VOLUMES];
static int volume_count;

// The sector being worked on, shared by all reads and all volumes. The VFS lock
// means only one thing uses it at a time.
static uint8_t sector[SECTOR_SIZE] __attribute__((aligned(2)));
static const struct block_device *cached_drive;
static uint32_t cached_lba;

static bool read_sector(struct block_device *drive, uint32_t lba) {
    if (cached_drive == drive && cached_lba == lba)
        return true;

    // On a disk, such as a USB stick the image was written to, sectors are smaller.
    uint32_t per = drive->sector_size < SECTOR_SIZE ? SECTOR_SIZE / drive->sector_size : 1;

    cached_drive = NULL;
    if (per == 1 ? !block_read(drive, lba, sector) : !block_read_many(drive, lba * per, per, sector))
        return false;

    cached_drive = drive;
    cached_lba = lba;
    return true;
}

// Numeric fields are stored in both byte orders. This reads the little endian one.
static uint32_t read_u32(const uint8_t *p) {
    uint32_t value;

    memcpy(&value, p, sizeof(value));
    return value;
}

void *iso9660_mount(struct block_device *drive) {
    cached_drive = NULL; // what was there may have been written over since
    if (volume_count == MAX_VOLUMES || !read_sector(drive, PVD_LBA))
        return NULL;
    if (sector[0] != PVD_TYPE || memcmp(&sector[1], "CD001", 5) != 0)
        return NULL;

    uint16_t block_size;
    memcpy(&block_size, &sector[PVD_BLOCK_SIZE], sizeof(block_size));
    if (block_size != SECTOR_SIZE)
        return NULL;

    struct iso9660_volume *v = &volumes[volume_count++];

    v->drive = drive;
    v->volume_blocks = read_u32(&sector[PVD_VOLUME_BLOCKS]);
    v->root_extent = read_u32(&sector[PVD_ROOT_RECORD + RECORD_EXTENT]);
    v->root_size = read_u32(&sector[PVD_ROOT_RECORD + RECORD_SIZE]);

    kprintf("iso9660: volume of %u blocks, root at lba %u\n", v->volume_blocks, v->root_extent);
    return v;
}

// Stored as years since 1900, month, day, hour, minute, second, then the offset
// from UTC in 15 minute steps.
static int64_t record_mtime(const uint8_t *date) {
    struct clock_time time = {
        .year = 1900 + date[0],
        .month = date[1],
        .day = date[2],
        .hour = date[3],
        .minute = date[4],
        .second = date[5],
    };

    if (time.month == 0 || time.day == 0)
        return 0;
    return clock_to_epoch(&time) - (int8_t)date[6] * 15 * 60;
}

// Plain ISO names are upper case with a version suffix, like "KERNEL.ELF;1".
static void plain_name(const uint8_t *in, size_t len, char *out) {
    size_t i = 0;

    for (; i < len && in[i] != ';'; i++)
        out[i] = in[i] >= 'A' && in[i] <= 'Z' ? in[i] - 'A' + 'a' : in[i];
    if (i > 0 && out[i - 1] == '.')
        i--;
    out[i] = '\0';
}

// Rock Ridge keeps the real name in an "NM" entry after the record's name.
// Entries continued in another sector aren't followed.
static bool rock_ridge_name(const uint8_t *record, size_t record_len, char *out) {
    size_t name_len = record[RECORD_NAME_LENGTH];
    size_t at = RECORD_NAME + name_len + (name_len % 2 == 0);
    size_t out_len = 0;
    bool found = false;

    while (at + 4 <= record_len) {
        size_t entry_len = record[at + 2];

        if (entry_len < 4 || at + entry_len > record_len)
            break;

        if (record[at] == 'N' && record[at + 1] == 'M' && entry_len >= RR_NAME) {
            size_t part = entry_len - RR_NAME;

            if (out_len + part > NAME_MAX)
                break;

            memcpy(&out[out_len], &record[at + RR_NAME], part);
            out_len += part;
            found = true;
            if (!(record[at + RR_NAME_FLAGS] & RR_NAME_CONTINUES))
                break;
        }
        at += entry_len;
    }

    out[out_len] = '\0';
    return found && out_len > 0;
}

// Returns the next record in a directory, hiding "." and "..".
static int next_record(const struct iso9660_volume *v, uint32_t extent, uint32_t size,
                       uint64_t *cursor, struct record *out) {
    while (*cursor < size) {
        if (!read_sector(v->drive, extent + *cursor / SECTOR_SIZE))
            return VFS_IO_ERROR;

        size_t at = *cursor % SECTOR_SIZE;
        const uint8_t *record = &sector[at];
        size_t len = record[RECORD_LENGTH];

        // Records don't cross sectors, and the rest of a sector is padded with zeros.
        if (len == 0) {
            *cursor += SECTOR_SIZE - at;
            continue;
        }

        size_t name_len = record[RECORD_NAME_LENGTH];
        if (at + len > SECTOR_SIZE || len < RECORD_NAME || RECORD_NAME + name_len > len) {
            kprintf("iso9660: bad directory record\n");
            return VFS_IO_ERROR;
        }

        *cursor += len;
        if (name_len == 1 && record[RECORD_NAME] <= 1)
            continue;

        out->extent = read_u32(&record[RECORD_EXTENT]);
        out->size = read_u32(&record[RECORD_SIZE]);
        out->mtime = record_mtime(&record[RECORD_DATE]);
        out->is_dir = record[RECORD_FLAGS] & FLAG_DIRECTORY;
        if (!rock_ridge_name(record, len, out->name))
            plain_name(&record[RECORD_NAME], name_len, out->name);
        return VFS_OK;
    }
    return VFS_END;
}

static char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

static bool names_equal(const char *a, const char *b, size_t b_len) {
    size_t i = 0;

    for (; i < b_len; i++) {
        if (a[i] == '\0' || ascii_lower(a[i]) != ascii_lower(b[i]))
            return false;
    }
    return a[i] == '\0';
}

static int iso9660_lookup(void *volume, const char *path, struct vfs_node *node) {
    const struct iso9660_volume *v = volume;

    *node = (struct vfs_node){.location = v->root_extent, .size = v->root_size, .is_dir = true};

    while (*path) {
        const char *end = path;
        uint64_t cursor = 0;
        struct record record;
        int status;

        while (*end && *end != '/')
            end++;

        if (!node->is_dir)
            return VFS_NOT_FOUND;

        do {
            status = next_record(v, node->location, node->size, &cursor, &record);
            if (status == VFS_END)
                return VFS_NOT_FOUND;
            if (status != VFS_OK)
                return status;
        } while (!names_equal(record.name, path, end - path));

        node->location = record.extent;
        node->size = record.size;
        node->mtime = record.mtime;
        node->is_dir = record.is_dir;
        path = *end ? end + 1 : end;
    }
    return VFS_OK;
}

static int64_t iso9660_read(void *volume, const struct vfs_node *node, uint64_t offset, void *buf, size_t len) {
    const struct iso9660_volume *v = volume;

    if (offset >= node->size)
        return 0;
    if (len > node->size - offset)
        len = node->size - offset;

    uint8_t *out = buf;
    size_t remaining = len;

    while (remaining > 0) {
        size_t start = offset % SECTOR_SIZE;
        size_t chunk = SECTOR_SIZE - start;

        if (chunk > remaining)
            chunk = remaining;
        if (!read_sector(v->drive, node->location + offset / SECTOR_SIZE))
            return VFS_IO_ERROR;

        memcpy(out, &sector[start], chunk);
        out += chunk;
        offset += chunk;
        remaining -= chunk;
    }
    return len;
}

static int iso9660_readdir(void *volume, const struct vfs_node *node, uint64_t *cursor, struct dirent *out) {
    struct record record;
    int status = next_record(volume, node->location, node->size, cursor, &record);

    if (status != VFS_OK)
        return status;

    memcpy(out->d_name, record.name, strlen(record.name) + 1);
    out->d_type = record.is_dir ? DT_DIR : DT_REG;
    out->d_size = record.size;
    return VFS_OK;
}

static bool iso9660_statfs(void *volume, struct statfs *out) {
    const struct iso9660_volume *v = volume;

    out->f_bsize = SECTOR_SIZE;
    out->f_blocks = v->volume_blocks;
    out->f_bfree = 0;
    return true;
}

const struct vfs_ops iso9660_ops = {
    .type = "iso9660",
    .lookup = iso9660_lookup,
    .read = iso9660_read,
    .readdir = iso9660_readdir,
    .statfs = iso9660_statfs,
};
