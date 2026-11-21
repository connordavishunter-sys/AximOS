#include <clock.h>
#include <fat32.h>
#include <kmalloc.h>
#include <serial.h>
#include <string.h>

#define MAX_VOLUMES 16
#define SECTOR_SIZE 512

#define CLUSTER_MASK 0x0FFFFFFF
#define CLUSTER_EOC_MIN 0x0FFFFFF8
#define CLUSTER_END 0xFFFFFFFF

#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10
#define ATTR_LONG_NAME 0x0F

#define ENTRY_END 0x00
#define ENTRY_DELETED 0xE5
#define ENTRY_SIZE 32

#define CASE_LOWER_BASE 0x08
#define CASE_LOWER_EXT 0x10

#define LFN_LAST 0x40
#define LFN_ORDER_MASK 0x1F
#define LFN_MAX_ENTRIES 20
#define LFN_ENTRY_UNITS 13

#define FSINFO_LEAD_SIGNATURE 0x41615252
#define FSINFO_STRUCT_SIGNATURE 0x61417272
#define FSINFO_UNKNOWN 0xFFFFFFFF

#define CLUSTER_EOC_MARK 0x0FFFFFFF
#define ATTR_ARCHIVE 0x20
#define DIRECTORY_CHECKS_MAX 999999
#define DOTDOT_OFFSET ENTRY_SIZE
#define FAT_EPOCH_YEAR 1980

// Changes to the FAT and directories are kept in memory until an operation ends,
// then written to the journal and only then to their real places, so a crash leaves
// all of them or none. File contents don't go through it. The journal is a header
// sector, then the sector numbers of the changes, then the changes themselves. The
// header is written last and is what makes the rest count.
#define METADATA_NAME ".metadata"
#define JOURNAL_PATH METADATA_NAME "/journal"
#define JOURNAL_NAME "journal"
#define JOURNAL_MAGIC 0x4C4E52554F4A5841ull
#define JOURNAL_CAPACITY 1024
#define JOURNAL_TABLE_SECTORS (JOURNAL_CAPACITY * sizeof(uint32_t) / SECTOR_SIZE)
#define JOURNAL_DATA_START (1 + JOURNAL_TABLE_SECTORS)
#define JOURNAL_BYTES ((JOURNAL_DATA_START + JOURNAL_CAPACITY) * SECTOR_SIZE)
#define JOURNAL_ENTRIES_PER_SECTOR (SECTOR_SIZE / sizeof(uint32_t))
#define JOURNAL_BUCKETS 64

// Big operations commit in parts. A piece of a write can touch two FAT sectors per
// cluster, so it's sized to stay well inside what's left after the commit point.
#define JOURNAL_COMMIT_POINT 700
#define WRITE_PIECE_CLUSTERS 64

struct bpb {
    uint8_t jump[11];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entries;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t flags;
    uint16_t version;
    uint32_t root_cluster;
    uint16_t fsinfo_sector;
} __attribute__((packed));

struct dir_entry {
    char name[11];
    uint8_t attr;
    uint8_t case_flags;
    uint8_t create_tenths;
    uint16_t create_time, create_date;
    uint16_t access_date;
    uint16_t cluster_high;
    uint16_t time, date;
    uint16_t cluster_low;
    uint32_t size;
} __attribute__((packed));

struct lfn_entry {
    uint8_t order;
    uint16_t name1[5];
    uint8_t attr;
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t cluster;
    uint16_t name3[2];
} __attribute__((packed));

// A long name being collected. Entries are stored last part first on disk.
struct lfn_state {
    uint16_t name[LFN_MAX_ENTRIES * LFN_ENTRY_UNITS];
    uint8_t total;
    uint8_t expected;
    uint8_t checksum;
};

struct journal_header {
    uint64_t magic;
    uint32_t count;
    uint32_t checksum;
};

struct pending_sector {
    uint32_t lba;
    struct pending_sector *bucket_next;
    struct pending_sector *next;
    uint8_t data[SECTOR_SIZE];
};

struct fat32_volume {
    struct block_device *drive;
    uint32_t fat_lba;
    uint32_t fsinfo_lba;
    uint32_t data_lba;
    uint32_t sectors_per_cluster;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint32_t fat_size;
    uint32_t fat_count;
    uint32_t next_free;
    uint32_t metadata_cluster;
    uint32_t journal_start; // the journal is always one contiguous run
    bool journal_ready;
    bool journal_tried;
    bool making_journal; // changes are collected as usual, but there's nowhere safe to put them
    uint32_t pending_count;
    struct pending_sector *pending;
    struct pending_sector *pending_buckets[JOURNAL_BUCKETS];
};

// A position in a directory, which also bounds the walk in case of a cycle.
// next_entry also reports where the entry it found is stored.
struct dir_iter {
    uint32_t cluster;
    uint32_t index;
    uint32_t walked;
    uint64_t offset;
    uint32_t entry_lba;
    uint32_t entry_pos;
    uint32_t run_start;
};

static struct fat32_volume volumes[MAX_VOLUMES];
static int volume_count;

// The sector being worked on, shared by all reads and all volumes. The VFS lock
// means only one thing uses it at a time.
static uint8_t sector[SECTOR_SIZE] __attribute__((aligned(2)));
static const struct block_device *cached_drive;
static uint32_t cached_lba;

// Used for the journal's own sectors, so that working on them leaves sector alone.
static uint8_t journal_sector[SECTOR_SIZE] __attribute__((aligned(8)));
static uint8_t journal_table[SECTOR_SIZE] __attribute__((aligned(8)));

static struct pending_sector *find_pending(const struct fat32_volume *v, uint32_t lba) {
    for (struct pending_sector *p = v->pending_buckets[lba % JOURNAL_BUCKETS]; p; p = p->bucket_next) {
        if (p->lba == lba)
            return p;
    }
    return NULL;
}

static bool read_sector(const struct fat32_volume *v, uint32_t lba) {
    if (cached_drive == v->drive && cached_lba == lba)
        return true;

    cached_drive = NULL;

    const struct pending_sector *pending = find_pending(v, lba);

    if (pending)
        memcpy(sector, pending->data, SECTOR_SIZE);
    else if (!block_read(v->drive, lba, sector))
        return false;

    cached_drive = v->drive;
    cached_lba = lba;
    return true;
}

// Writes the cached sector straight to the disk, for file contents and for clusters
// nothing refers to yet.
static bool write_cached_data(const struct fat32_volume *v) {
    if (block_write(v->drive, cached_lba, sector))
        return true;

    cached_drive = NULL;
    return false;
}

// Keeps a changed FAT or directory sector for the journal to commit. Reads see it
// straight away.
static bool write_cached_sector(struct fat32_volume *v) {
    if (!v->journal_ready && !v->making_journal)
        return write_cached_data(v);

    struct pending_sector *pending = find_pending(v, cached_lba);

    if (!pending) {
        pending = v->pending_count < JOURNAL_CAPACITY ? kmalloc(sizeof(*pending)) : NULL;
        if (!pending) {
            cached_drive = NULL;
            return false;
        }

        struct pending_sector **bucket = &v->pending_buckets[cached_lba % JOURNAL_BUCKETS];

        pending->lba = cached_lba;
        pending->bucket_next = *bucket;
        *bucket = pending;
        pending->next = v->pending;
        v->pending = pending;
        v->pending_count++;
    }
    memcpy(pending->data, sector, SECTOR_SIZE);
    return true;
}

static bool cluster_valid(const struct fat32_volume *v, uint32_t cluster) {
    return cluster >= 2 && cluster - 2 < v->cluster_count;
}

static uint32_t cluster_lba(const struct fat32_volume *v, uint32_t cluster) {
    return v->data_lba + (cluster - 2) * v->sectors_per_cluster;
}

static uint32_t cluster_bytes(const struct fat32_volume *v) {
    return v->sectors_per_cluster * SECTOR_SIZE;
}

// Reads a raw FAT entry, without the reserved top bits.
static bool read_fat(const struct fat32_volume *v, uint32_t cluster, uint32_t *value) {
    uint32_t offset = cluster * 4;

    if (!read_sector(v, v->fat_lba + offset / SECTOR_SIZE))
        return false;

    memcpy(value, &sector[offset % SECTOR_SIZE], sizeof(*value));
    *value &= CLUSTER_MASK;
    return true;
}

// Follows the chain, setting next to CLUSTER_END at the end of it.
static bool next_cluster(const struct fat32_volume *v, uint32_t cluster, uint32_t *next) {
    uint32_t value;

    if (!read_fat(v, cluster, &value))
        return false;

    if (value >= CLUSTER_EOC_MIN) {
        *next = CLUSTER_END;
        return true;
    }
    if (!cluster_valid(v, value)) {
        kprintf("fat32: bad FAT entry %x for cluster %u\n", value, cluster);
        return false;
    }
    *next = value;
    return true;
}

static int fat32_lookup(void *volume, const char *path, struct vfs_node *node);
static void journal_open(struct fat32_volume *v);

void *fat32_mount(struct block_device *drive, uint32_t start_lba) {
    if (volume_count == MAX_VOLUMES)
        return NULL;

    struct fat32_volume *v = &volumes[volume_count];
    v->drive = drive;

    if (!read_sector(v, start_lba))
        return NULL;

    const struct bpb *bpb = (const struct bpb *)sector;
    uint32_t spc = bpb->sectors_per_cluster;

    if (sector[510] != 0x55 || sector[511] != 0xAA)
        return NULL;
    if (bpb->bytes_per_sector != SECTOR_SIZE || spc == 0 || (spc & (spc - 1)))
        return NULL;
    if (bpb->root_entries != 0 || bpb->fat_size_16 != 0 || bpb->fat_size_32 == 0)
        return NULL;
    if (bpb->reserved_sectors == 0 || bpb->fat_count == 0)
        return NULL;

    uint32_t data_offset = bpb->reserved_sectors + bpb->fat_count * bpb->fat_size_32;
    if (bpb->total_sectors_32 <= data_offset)
        return NULL;

    v->fat_lba = start_lba + bpb->reserved_sectors;
    v->fsinfo_lba = start_lba + bpb->fsinfo_sector;
    v->data_lba = start_lba + data_offset;
    v->sectors_per_cluster = spc;
    v->cluster_count = (bpb->total_sectors_32 - data_offset) / spc;
    v->root_cluster = bpb->root_cluster;
    v->fat_size = bpb->fat_size_32;
    v->fat_count = bpb->fat_count;
    v->next_free = 2;

    if (!cluster_valid(v, v->root_cluster))
        return NULL;

    kprintf("fat32: volume at lba %u, %u clusters of %u sectors, root cluster %u\n",
            start_lba, v->cluster_count, spc, v->root_cluster);
    journal_open(v);
    volume_count++;
    return v;
}

void fat32_forget(const struct block_device *drive) {
    if (cached_drive == drive)
        cached_drive = NULL;
}

// FAT has no time zone, and this OS treats every timestamp as UTC.
static int64_t entry_mtime(const struct dir_entry *e) {
    if (e->date == 0)
        return 0;

    struct clock_time time = {
        .year = FAT_EPOCH_YEAR + (e->date >> 9),
        .month = e->date >> 5 & 0xF,
        .day = e->date & 0x1F,
        .hour = e->time >> 11,
        .minute = e->time >> 5 & 0x3F,
        .second = (e->time & 0x1F) * 2,
    };
    return clock_to_epoch(&time);
}

static void set_modified_now(struct dir_entry *e) {
    struct clock_time now;

    clock_from_epoch(clock_now(), &now);
    e->date = (now.year - FAT_EPOCH_YEAR) << 9 | now.month << 5 | now.day;
    e->time = now.hour << 11 | now.minute << 5 | now.second / 2;
}

static uint16_t ascii_lower(uint16_t c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

// Converts one path component to its space-padded 8.3 form.
static bool to_short_name(const char *s, size_t len, char name[11]) {
    size_t out = 0, limit = 8;

    memset(name, ' ', 11);
    for (size_t i = 0; i < len; i++) {
        char c = s[i];

        if (c == '.' && limit == 8) {
            out = 8;
            limit = 11;
            continue;
        }
        if (out == limit)
            return false;
        name[out++] = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
    }
    return len > 0;
}

// The 8.3 name as a string, using the lowercase flags Windows and mtools set.
static void short_name_to_string(const struct dir_entry *e, char *out) {
    size_t len = 0;

    for (int i = 0; i < 8 && e->name[i] != ' '; i++)
        out[len++] = e->case_flags & CASE_LOWER_BASE ? ascii_lower(e->name[i]) : e->name[i];

    if (e->name[8] != ' ')
        out[len++] = '.';
    for (int i = 8; i < 11 && e->name[i] != ' '; i++)
        out[len++] = e->case_flags & CASE_LOWER_EXT ? ascii_lower(e->name[i]) : e->name[i];

    out[len] = '\0';
}

static void lfn_add(struct lfn_state *lfn, const struct lfn_entry *e) {
    uint8_t seq = e->order & LFN_ORDER_MASK;

    if (seq == 0 || seq > LFN_MAX_ENTRIES) {
        lfn->total = 0;
        return;
    }
    if (e->order & LFN_LAST) {
        lfn->total = seq;
        lfn->checksum = e->checksum;
    } else if (lfn->total == 0 || seq != lfn->expected || e->checksum != lfn->checksum) {
        lfn->total = 0;
        return;
    }
    lfn->expected = seq - 1;

    uint16_t *out = &lfn->name[(seq - 1) * LFN_ENTRY_UNITS];
    memcpy(out, e->name1, sizeof(e->name1));
    memcpy(out + 5, e->name2, sizeof(e->name2));
    memcpy(out + 11, e->name3, sizeof(e->name3));
}

static uint8_t short_name_checksum(const char *name) {
    uint8_t sum = 0;

    for (int i = 0; i < 11; i++)
        sum = ((sum & 1) << 7) + (sum >> 1) + (uint8_t)name[i];
    return sum;
}

// Long names are UCS-2 on disk. Anything outside ASCII becomes '?' for now.
static bool lfn_to_string(const struct lfn_state *lfn, const char *short_name, char *out) {
    if (lfn->total == 0 || lfn->expected != 0 || lfn->checksum != short_name_checksum(short_name))
        return false;

    size_t units = lfn->total * LFN_ENTRY_UNITS;
    size_t len = 0;

    while (len < units && len < NAME_MAX && lfn->name[len] != 0) {
        out[len] = lfn->name[len] < 0x80 ? lfn->name[len] : '?';
        len++;
    }
    out[len] = '\0';
    return true;
}

// Returns the next real entry and its name, hiding "." and "..".
static int next_entry(const struct fat32_volume *v, struct dir_iter *iter, struct dir_entry *out, char *name) {
    uint32_t per_cluster = cluster_bytes(v) / ENTRY_SIZE;
    struct lfn_state lfn;
    uint32_t run_start = 0;

    lfn.total = 0;
    for (;;) {
        if (iter->index == per_cluster) {
            if (++iter->walked >= v->cluster_count) {
                kprintf("fat32: directory chain does not terminate\n");
                return VFS_IO_ERROR;
            }
            if (!next_cluster(v, iter->cluster, &iter->cluster))
                return VFS_IO_ERROR;
            if (iter->cluster == CLUSTER_END)
                return VFS_END;
            iter->index = 0;
        }

        uint32_t lba = cluster_lba(v, iter->cluster) + iter->index / (SECTOR_SIZE / ENTRY_SIZE);
        if (!read_sector(v, lba))
            return VFS_IO_ERROR;

        uint32_t pos = iter->index % (SECTOR_SIZE / ENTRY_SIZE) * ENTRY_SIZE;
        const struct dir_entry *e = (const struct dir_entry *)&sector[pos];
        uint8_t first = e->name[0];

        if (first == ENTRY_END)
            return VFS_END;

        iter->index++;
        iter->offset += ENTRY_SIZE;

        if (first != ENTRY_DELETED && (e->attr & ATTR_LONG_NAME) == ATTR_LONG_NAME) {
            if (first & LFN_LAST)
                run_start = iter->offset - ENTRY_SIZE;
            lfn_add(&lfn, (const struct lfn_entry *)e);
            continue;
        }
        if (first == ENTRY_DELETED || first == '.' || (e->attr & ATTR_VOLUME_ID)) {
            lfn.total = 0;
            continue;
        }

        iter->entry_lba = lba;
        iter->entry_pos = pos;
        if (lfn_to_string(&lfn, e->name, name)) {
            iter->run_start = run_start;
        } else {
            short_name_to_string(e, name);
            iter->run_start = iter->offset - ENTRY_SIZE;
        }
        *out = *e;
        return VFS_OK;
    }
}

static bool names_equal(const char *a, const char *b, size_t b_len) {
    size_t i = 0;

    for (; i < b_len; i++) {
        if (a[i] == '\0' || ascii_lower((uint8_t)a[i]) != ascii_lower((uint8_t)b[i]))
            return false;
    }
    return a[i] == '\0';
}

// Matches a component against both the long and the 8.3 name of each entry.
static int find_entry(const struct fat32_volume *v, uint32_t cluster, const char *component,
                      size_t len, struct dir_entry *found, struct dir_iter *iter) {
    char short_name[11];
    char name[NAME_MAX + 1];
    bool has_short = to_short_name(component, len, short_name);

    *iter = (struct dir_iter){.cluster = cluster};
    for (;;) {
        int status = next_entry(v, iter, found, name);

        if (status == VFS_END)
            return VFS_NOT_FOUND;
        if (status != VFS_OK)
            return status;
        if (names_equal(name, component, len)
            || (has_short && memcmp(found->name, short_name, 11) == 0))
            return VFS_OK;
    }
}

static int fat32_lookup(void *volume, const char *path, struct vfs_node *node) {
    const struct fat32_volume *v = volume;
    uint32_t cluster = v->root_cluster;

    *node = (struct vfs_node){.location = cluster, .is_dir = true};

    while (*path) {
        const char *end = path;
        struct dir_entry entry;
        struct dir_iter iter;

        while (*end && *end != '/')
            end++;

        if (!node->is_dir)
            return VFS_NOT_FOUND;

        int status = find_entry(v, cluster, path, end - path, &entry, &iter);
        if (status != VFS_OK)
            return status;

        node->dir_location = cluster;
        node->dir_offset = iter.offset - ENTRY_SIZE;
        node->run_start = iter.run_start;
        node->entry_lba = iter.entry_lba;
        node->entry_pos = iter.entry_pos;

        cluster = (uint32_t)entry.cluster_high << 16 | entry.cluster_low;
        node->location = cluster;
        node->size = entry.size;
        node->mtime = entry_mtime(&entry);
        node->is_dir = entry.attr & ATTR_DIRECTORY;

        if (node->is_dir && !cluster_valid(v, cluster))
            return VFS_IO_ERROR;

        path = *end ? end + 1 : end;
    }
    return VFS_OK;
}

// Reads sectors in a row, going through read_sector if a change is waiting on any.
static bool read_run(const struct fat32_volume *v, uint32_t lba, uint32_t count, void *buf) {
    bool pending = false;

    for (uint32_t i = 0; v->pending_count && i < count && !pending; i++)
        pending = find_pending(v, lba + i);

    if (!pending)
        return block_read_many(v->drive, lba, count, buf);

    for (uint32_t i = 0; i < count; i++) {
        if (!read_sector(v, lba + i))
            return false;
        memcpy((uint8_t *)buf + i * SECTOR_SIZE, sector, SECTOR_SIZE);
    }
    return true;
}

static int64_t fat32_read(void *volume, const struct vfs_node *node, uint64_t offset, void *buf, size_t len) {
    const struct fat32_volume *v = volume;

    if (offset >= node->size)
        return 0;
    if (len > node->size - offset)
        len = node->size - offset;
    if (len == 0)
        return 0;

    uint32_t size = cluster_bytes(v);
    uint32_t cluster = node->location;
    uint8_t *out = buf;
    size_t remaining = len;

    if (!cluster_valid(v, cluster))
        goto corrupt;

    for (uint64_t skip = offset / size; skip > 0; skip--) {
        if (!next_cluster(v, cluster, &cluster))
            return VFS_IO_ERROR;
        if (cluster == CLUSTER_END)
            goto corrupt;
    }

    uint32_t within = offset % size;
    while (remaining > 0) {
        uint32_t lba = cluster_lba(v, cluster) + within / SECTOR_SIZE;
        uint32_t start = within % SECTOR_SIZE;

        // Whole sectors go straight to the caller, as many as there are side by side.
        if (start == 0 && remaining >= SECTOR_SIZE) {
            uint32_t run = 0;
            uint32_t want = remaining / SECTOR_SIZE;

            for (;;) {
                uint32_t left = (size - within) / SECTOR_SIZE;
                uint32_t take = left < want - run ? left : want - run;

                run += take;
                within += take * SECTOR_SIZE;
                if (run == want)
                    break;

                uint32_t next;

                if (!next_cluster(v, cluster, &next))
                    return VFS_IO_ERROR;
                if (next == CLUSTER_END)
                    goto corrupt;

                bool adjacent = next == cluster + 1;

                cluster = next;
                within = 0;
                if (!adjacent)
                    break;
            }

            if (!read_run(v, lba, run, out))
                return VFS_IO_ERROR;
            out += run * SECTOR_SIZE;
            remaining -= run * SECTOR_SIZE;

            if (remaining > 0 && within == size) {
                within = 0;
                if (!next_cluster(v, cluster, &cluster))
                    return VFS_IO_ERROR;
                if (cluster == CLUSTER_END)
                    goto corrupt;
            }
            continue;
        }

        size_t chunk = SECTOR_SIZE - start;

        if (chunk > remaining)
            chunk = remaining;
        if (!read_sector(v, lba))
            return VFS_IO_ERROR;

        memcpy(out, &sector[start], chunk);
        out += chunk;
        remaining -= chunk;
        within += chunk;

        if (remaining > 0 && within == size) {
            within = 0;
            if (!next_cluster(v, cluster, &cluster))
                return VFS_IO_ERROR;
            if (cluster == CLUSTER_END)
                goto corrupt;
        }
    }
    return len;

corrupt:
    kprintf("fat32: cluster chain shorter than file size\n");
    return VFS_IO_ERROR;
}

static int fat32_readdir(void *volume, const struct vfs_node *node, uint64_t *cursor, struct dirent *out) {
    const struct fat32_volume *v = volume;
    uint32_t size = cluster_bytes(v);
    struct dir_iter iter = {.cluster = node->location, .offset = *cursor};

    // Finding the cursor again means walking the chain, so a whole listing is
    // quadratic in the number of clusters. Directories are small enough.
    for (uint64_t skip = *cursor / size; skip > 0; skip--) {
        if (!next_cluster(v, iter.cluster, &iter.cluster))
            return VFS_IO_ERROR;
        if (iter.cluster == CLUSTER_END)
            return VFS_END;
    }
    iter.index = (*cursor % size) / ENTRY_SIZE;

    struct dir_entry entry;
    int status = next_entry(v, &iter, &entry, out->d_name);
    if (status != VFS_OK)
        return status;

    out->d_type = entry.attr & ATTR_DIRECTORY ? DT_DIR : DT_REG;
    out->d_size = entry.size;
    *cursor = iter.offset;
    return VFS_OK;
}

// The FSInfo sector saves scanning the FAT, which can be very slow on big disks.
static bool count_free_clusters(const struct fat32_volume *v, uint64_t *free) {
    if (!read_sector(v, v->fsinfo_lba))
        return false;

    uint32_t lead, signature, hint;
    memcpy(&lead, &sector[0], sizeof(lead));
    memcpy(&signature, &sector[484], sizeof(signature));
    memcpy(&hint, &sector[488], sizeof(hint));

    if (lead == FSINFO_LEAD_SIGNATURE && signature == FSINFO_STRUCT_SIGNATURE
        && hint != FSINFO_UNKNOWN && hint <= v->cluster_count) {
        *free = hint;
        return true;
    }

    *free = 0;
    for (uint32_t i = 0; i < v->cluster_count; i++) {
        uint32_t value;

        if (!read_fat(v, i + 2, &value))
            return false;
        if (value == 0)
            ++*free;
    }
    return true;
}

static bool fat32_statfs(void *volume, struct statfs *out) {
    const struct fat32_volume *v = volume;

    out->f_bsize = cluster_bytes(v);
    out->f_blocks = v->cluster_count;
    return count_free_clusters(v, &out->f_bfree);
}

// Stores a FAT entry in every copy of the FAT, keeping the reserved top bits.
static bool write_fat(struct fat32_volume *v, uint32_t cluster, uint32_t value) {
    uint32_t offset = cluster * 4;

    for (uint32_t copy = 0; copy < v->fat_count; copy++) {
        uint32_t lba = v->fat_lba + copy * v->fat_size + offset / SECTOR_SIZE;
        uint32_t old;

        if (!read_sector(v, lba))
            return false;

        memcpy(&old, &sector[offset % SECTOR_SIZE], sizeof(old));
        old = (old & ~(uint32_t)CLUSTER_MASK) | (value & CLUSTER_MASK);
        memcpy(&sector[offset % SECTOR_SIZE], &old, sizeof(old));

        if (!write_cached_sector(v))
            return false;
    }
    return true;
}

// Keeps FSInfo's free count right, so df stays accurate on other systems too.
static void adjust_free_count(struct fat32_volume *v, int delta) {
    uint32_t lead, signature, hint;

    if (!read_sector(v, v->fsinfo_lba))
        return;

    memcpy(&lead, &sector[0], sizeof(lead));
    memcpy(&signature, &sector[484], sizeof(signature));
    memcpy(&hint, &sector[488], sizeof(hint));

    if (lead != FSINFO_LEAD_SIGNATURE || signature != FSINFO_STRUCT_SIGNATURE || hint == FSINFO_UNKNOWN)
        return;

    hint += delta;
    memcpy(&sector[488], &hint, sizeof(hint));
    write_cached_sector(v);
}

static bool zero_cluster(const struct fat32_volume *v, uint32_t cluster) {
    uint32_t lba = cluster_lba(v, cluster);

    memset(sector, 0, SECTOR_SIZE);
    cached_drive = v->drive;
    for (uint32_t s = 0; s < v->sectors_per_cluster; s++) {
        cached_lba = lba + s;
        if (!write_cached_data(v))
            return false;
    }
    return true;
}

// Returns a new end-of-chain cluster linked after previous, if any, or 0 if the
// disk is full or failed.
static uint32_t alloc_cluster(struct fat32_volume *v, uint32_t previous, bool zeroed) {
    for (uint32_t i = 0; i < v->cluster_count; i++) {
        uint32_t cluster = 2 + (v->next_free - 2 + i) % v->cluster_count;
        uint32_t value;

        if (!read_fat(v, cluster, &value))
            return 0;
        if (value != 0)
            continue;

        if (!write_fat(v, cluster, CLUSTER_EOC_MARK))
            return 0;
        if (zeroed && !zero_cluster(v, cluster)) {
            write_fat(v, cluster, 0);
            return 0;
        }
        if (previous && !write_fat(v, previous, cluster)) {
            write_fat(v, cluster, 0);
            return 0;
        }

        adjust_free_count(v, -1);
        v->next_free = cluster;
        return cluster;
    }
    return 0;
}

static void flush_drive(const struct fat32_volume *v) {
    if (v->drive->flush)
        v->drive->flush(v->drive);
}

static uint32_t journal_lba(const struct fat32_volume *v, uint32_t index) {
    return v->journal_start + index;
}

static uint32_t checksum_add(uint32_t sum, const void *bytes, size_t len) {
    const uint8_t *b = bytes;

    for (size_t i = 0; i < len; i++)
        sum = (sum ^ b[i]) * 16777619u;
    return sum;
}

static bool write_journal_header(const struct fat32_volume *v, uint32_t count, uint32_t checksum) {
    memset(journal_sector, 0, SECTOR_SIZE);
    if (count)
        *(struct journal_header *)journal_sector = (struct journal_header){JOURNAL_MAGIC, count, checksum};
    if (!block_write(v->drive, journal_lba(v, 0), journal_sector))
        return false;
    flush_drive(v);
    return true;
}

// Puts everything in the journal, marks it complete, writes it to its real places
// and clears the mark. A crash before the mark loses the operation, and one after it
// finishes the operation at the next mount.
static bool commit_to_journal(const struct fat32_volume *v) {
    uint32_t checksum = 2166136261u;
    uint32_t index = 0;

    memset(journal_table, 0, SECTOR_SIZE);
    for (const struct pending_sector *p = v->pending; p; p = p->next, index++) {
        ((uint32_t *)journal_table)[index % JOURNAL_ENTRIES_PER_SECTOR] = p->lba;
        checksum = checksum_add(checksum, &p->lba, sizeof(p->lba));
        checksum = checksum_add(checksum, p->data, SECTOR_SIZE);

        if (!block_write(v->drive, journal_lba(v, JOURNAL_DATA_START + index), p->data))
            return false;
        if (index % JOURNAL_ENTRIES_PER_SECTOR == JOURNAL_ENTRIES_PER_SECTOR - 1 || !p->next) {
            if (!block_write(v->drive, journal_lba(v, 1 + index / JOURNAL_ENTRIES_PER_SECTOR), journal_table))
                return false;
            memset(journal_table, 0, SECTOR_SIZE);
        }
    }
    flush_drive(v);
    if (!write_journal_header(v, v->pending_count, checksum))
        return false;

    for (const struct pending_sector *p = v->pending; p; p = p->next) {
        if (!block_write(v->drive, p->lba, p->data))
            return false;
    }
    flush_drive(v);
    return write_journal_header(v, 0, 0);
}

// One sector always reaches the disk whole, so it needs no journal.
static bool journal_commit(struct fat32_volume *v) {
    bool ok = true;

    if (v->pending_count > 1 && v->journal_ready) {
        ok = commit_to_journal(v);
    } else {
        for (const struct pending_sector *p = v->pending; p && ok; p = p->next)
            ok = block_write(v->drive, p->lba, p->data);
    }

    while (v->pending) {
        struct pending_sector *next = v->pending->next;

        kfree(v->pending);
        v->pending = next;
    }
    v->pending_count = 0;
    memset(v->pending_buckets, 0, sizeof(v->pending_buckets));
    if (!ok) {
        cached_drive = NULL;
        kprintf("fat32: journal commit failed\n");
    }
    return ok;
}

// Frees in as many commits as it takes. The chain must no longer be in use, so a
// crash partway costs only the space not yet freed.
static bool free_chain(struct fat32_volume *v, uint32_t cluster) {
    int freed = 0;

    for (uint32_t i = 0; cluster_valid(v, cluster) && i < v->cluster_count; i++) {
        uint32_t next;

        if (!read_fat(v, cluster, &next) || !write_fat(v, cluster, 0))
            return false;

        freed++;
        cluster = next;
        if (v->pending_count >= JOURNAL_COMMIT_POINT) {
            adjust_free_count(v, freed);
            freed = 0;
            if (!journal_commit(v))
                return false;
        }
    }
    adjust_free_count(v, freed);
    return true;
}

// Finds where a byte of a directory is stored. If grow is set, the directory is
// extended to reach it. Returns VFS_END if it is past the end otherwise.
static int locate_dir_offset(struct fat32_volume *v, uint32_t dir, uint32_t offset, bool grow,
                             uint32_t *lba, uint32_t *pos) {
    uint32_t size = cluster_bytes(v);
    uint32_t cluster = dir;

    for (uint32_t skip = offset / size; skip > 0; skip--) {
        uint32_t next;

        if (!next_cluster(v, cluster, &next))
            return VFS_IO_ERROR;
        if (next == CLUSTER_END) {
            if (!grow)
                return VFS_END;

            next = alloc_cluster(v, cluster, true);
            if (next == 0)
                return -ENOSPC;
        }
        cluster = next;
    }

    uint32_t within = offset % size;
    *lba = cluster_lba(v, cluster) + within / SECTOR_SIZE;
    *pos = within % SECTOR_SIZE;
    return VFS_OK;
}

// Finds count free entries in a row. at_end is set if they include the end
// marker, after which nothing is in use.
static int find_free_entries(const struct fat32_volume *v, uint32_t dir, uint32_t count,
                             uint32_t *start, bool *at_end) {
    uint32_t per_cluster = cluster_bytes(v) / ENTRY_SIZE;
    uint32_t cluster = dir;
    uint32_t offset = 0;
    uint32_t run = 0;

    *at_end = false;
    for (uint32_t walked = 0; walked < v->cluster_count; walked++) {
        for (uint32_t i = 0; i < per_cluster; i++, offset += ENTRY_SIZE) {
            uint32_t lba = cluster_lba(v, cluster) + i / (SECTOR_SIZE / ENTRY_SIZE);

            if (!read_sector(v, lba))
                return VFS_IO_ERROR;

            uint8_t first = sector[i % (SECTOR_SIZE / ENTRY_SIZE) * ENTRY_SIZE];

            if (first == ENTRY_END) {
                *start = run ? *start : offset;
                *at_end = true;
                return VFS_OK;
            }
            if (first != ENTRY_DELETED) {
                run = 0;
                continue;
            }
            if (run++ == 0)
                *start = offset;
            if (run == count)
                return VFS_OK;
        }

        uint32_t next;
        if (!next_cluster(v, cluster, &next))
            return VFS_IO_ERROR;
        if (next == CLUSTER_END) {
            // The chain ran out. New clusters are zeroed, so this acts like the end marker.
            *start = run ? *start : offset;
            *at_end = true;
            return VFS_OK;
        }
        cluster = next;
    }
    return VFS_IO_ERROR;
}

static int write_dir_entry(struct fat32_volume *v, uint32_t dir, uint32_t offset, const void *entry,
                           uint32_t *lba, uint32_t *pos) {
    int status = locate_dir_offset(v, dir, offset, true, lba, pos);
    if (status != VFS_OK)
        return status;

    if (!read_sector(v, *lba))
        return VFS_IO_ERROR;

    memcpy(&sector[*pos], entry, ENTRY_SIZE);
    return write_cached_sector(v) ? VFS_OK : VFS_IO_ERROR;
}

static bool name_char_allowed(char c) {
    return (uint8_t)c >= 0x20 && (uint8_t)c < 0x7F && !(c == '"' || c == '*' || c == '/' || c == ':'
        || c == '<' || c == '>' || c == '?' || c == '\\' || c == '|');
}

static bool long_name_valid(const char *name) {
    size_t len = strlen(name);

    if (len == 0 || len > NAME_MAX || name[len - 1] == ' ' || name[len - 1] == '.')
        return false;
    for (size_t i = 0; i < len; i++) {
        if (!name_char_allowed(name[i]))
            return false;
    }
    return true;
}

static bool short_char_allowed(char c) {
    return name_char_allowed(c) && c != ' ' && c != '.' && c != '+' && c != ',' && c != ';'
        && c != '=' && c != '[' && c != ']';
}

// Fills one part of a short name exactly if the text can be stored in it, which
// means no case mix. lower is set if the case flag will be needed.
static bool short_part_exact(const char *text, size_t len, char *out, size_t max, bool *lower) {
    bool has_upper = false, has_lower = false;

    if (len > max)
        return false;

    for (size_t i = 0; i < len; i++) {
        char c = text[i];

        if (!short_char_allowed(c))
            return false;

        has_upper |= c >= 'A' && c <= 'Z';
        has_lower |= c >= 'a' && c <= 'z';
        out[i] = c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c;
    }
    *lower = has_lower;
    return !(has_upper && has_lower);
}

// Tries to store a name as 8.3 alone, so it needs no long name entries.
static bool name_fits_short(const char *name, char short_name[11], uint8_t *case_flags) {
    const char *dot = strrchr(name, '.');
    size_t len = strlen(name);
    size_t base_len = dot ? (size_t)(dot - name) : len;
    bool base_lower = false, ext_lower = false;

    memset(short_name, ' ', 11);
    if (base_len == 0 || !short_part_exact(name, base_len, short_name, 8, &base_lower))
        return false;
    if (dot && !short_part_exact(dot + 1, len - base_len - 1, &short_name[8], 3, &ext_lower))
        return false;

    *case_flags = (base_lower ? CASE_LOWER_BASE : 0) | (ext_lower ? CASE_LOWER_EXT : 0);
    return true;
}

static int short_name_exists(const struct fat32_volume *v, uint32_t dir, const char *short_name) {
    struct dir_iter iter = {.cluster = dir};
    struct dir_entry entry;
    char name[NAME_MAX + 1];

    for (;;) {
        int status = next_entry(v, &iter, &entry, name);

        if (status == VFS_END)
            return false;
        if (status != VFS_OK)
            return status;
        if (memcmp(entry.name, short_name, 11) == 0)
            return true;
    }
}

static void append_short_char(char *out, size_t *used, size_t max, char c) {
    if (*used == max)
        return;
    if (c == ' ' || c == '.')
        return;
    if (c >= 'a' && c <= 'z')
        c = c - 'a' + 'A';
    out[(*used)++] = short_char_allowed(c) ? c : '_';
}

// Makes the alias a long name needs, like LONGNA~1.TXT, unique in its directory.
static int generate_short_name(const struct fat32_volume *v, uint32_t dir, const char *name, char short_name[11]) {
    const char *dot = strrchr(name, '.');
    char base[6];
    size_t base_len = 0;
    char ext[3];
    size_t ext_len = 0;

    for (const char *p = name; *p && p != dot; p++)
        append_short_char(base, &base_len, sizeof(base), *p);
    for (const char *p = dot ? dot + 1 : ""; *p; p++)
        append_short_char(ext, &ext_len, sizeof(ext), *p);
    if (base_len == 0)
        base[base_len++] = '_';

    for (uint32_t n = 1; n <= DIRECTORY_CHECKS_MAX; n++) {
        char digits[8];
        size_t digit_count = 0;

        for (uint32_t rest = n; rest > 0; rest /= 10)
            digits[digit_count++] = '0' + rest % 10;

        size_t keep = base_len + digit_count + 1 > 8 ? 7 - digit_count : base_len;

        memset(short_name, ' ', 11);
        memcpy(short_name, base, keep);
        short_name[keep] = '~';
        for (size_t i = 0; i < digit_count; i++)
            short_name[keep + 1 + i] = digits[digit_count - 1 - i];
        memcpy(&short_name[8], ext, ext_len);

        int exists = short_name_exists(v, dir, short_name);
        if (exists < 0)
            return exists;
        if (!exists)
            return VFS_OK;
    }
    return VFS_IO_ERROR;
}

static void fill_lfn_entry(struct lfn_entry *e, const char *name, uint32_t count, uint32_t seq, uint8_t checksum) {
    uint16_t units[LFN_ENTRY_UNITS];
    size_t len = strlen(name);
    size_t start = (seq - 1) * LFN_ENTRY_UNITS;

    for (size_t i = 0; i < LFN_ENTRY_UNITS; i++) {
        size_t at = start + i;

        units[i] = at < len ? (uint8_t)name[at] : at == len ? 0 : 0xFFFF;
    }

    memset(e, 0, sizeof(*e));
    e->order = seq | (seq == count ? LFN_LAST : 0);
    e->attr = ATTR_LONG_NAME;
    e->checksum = checksum;
    memcpy(e->name1, &units[0], sizeof(e->name1));
    memcpy(e->name2, &units[5], sizeof(e->name2));
    memcpy(e->name3, &units[11], sizeof(e->name3));
}

static void fill_dir_entry(struct dir_entry *e, const char *short_name, uint8_t attr, uint8_t case_flags,
                           uint32_t cluster, uint32_t size) {
    memset(e, 0, sizeof(*e));
    memcpy(e->name, short_name, 11);
    e->attr = attr;
    e->case_flags = case_flags;
    set_modified_now(e);
    e->create_date = e->access_date = e->date;
    e->create_time = e->time;
    e->cluster_high = cluster >> 16;
    e->cluster_low = cluster;
    e->size = size;
}

// Writes the name's entries and returns where the last one went.
static int add_entries(struct fat32_volume *v, uint32_t dir, const char *name, const struct dir_entry *entry,
                       uint32_t lfn_count, struct vfs_node *node) {
    uint32_t start = 0;
    bool at_end;
    int status = find_free_entries(v, dir, lfn_count + 1, &start, &at_end);

    if (status != VFS_OK)
        return status;

    uint32_t lba = 0, pos = 0;
    uint8_t checksum = short_name_checksum(entry->name);

    for (uint32_t i = 0; i < lfn_count; i++) {
        struct lfn_entry lfn;

        fill_lfn_entry(&lfn, name, lfn_count, lfn_count - i, checksum);
        status = write_dir_entry(v, dir, start + i * ENTRY_SIZE, &lfn, &lba, &pos);
        if (status != VFS_OK)
            return status;
    }

    uint32_t last = start + lfn_count * ENTRY_SIZE;
    status = write_dir_entry(v, dir, last, entry, &lba, &pos);
    if (status != VFS_OK)
        return status;

    node->entry_lba = lba;
    node->entry_pos = pos;
    node->dir_location = dir;
    node->dir_offset = last;
    node->run_start = start;

    // Entries after the old end marker can hold anything, so it has to move.
    if (at_end) {
        uint32_t next_lba, next_pos;

        status = locate_dir_offset(v, dir, last + ENTRY_SIZE, false, &next_lba, &next_pos);
        if (status == VFS_OK && read_sector(v, next_lba) && sector[next_pos] != ENTRY_END) {
            sector[next_pos] = ENTRY_END;
            write_cached_sector(v);
        }
    }
    return VFS_OK;
}

static int make_directory_contents(struct fat32_volume *v, uint32_t cluster, uint32_t parent) {
    struct dir_entry entry;
    uint32_t lba, pos;
    char name[11];

    memset(name, ' ', 11);
    name[0] = '.';
    fill_dir_entry(&entry, name, ATTR_DIRECTORY, 0, cluster, 0);
    int status = write_dir_entry(v, cluster, 0, &entry, &lba, &pos);
    if (status != VFS_OK)
        return status;

    // A parent that is the root is recorded as cluster 0.
    name[1] = '.';
    fill_dir_entry(&entry, name, ATTR_DIRECTORY, 0, parent == v->root_cluster ? 0 : parent, 0);
    return write_dir_entry(v, cluster, ENTRY_SIZE, &entry, &lba, &pos);
}

// Works out the 8.3 form of a new name and how many long name entries it needs.
static int prepare_name(const struct fat32_volume *v, uint32_t dir, const char *name, char short_name[11],
                        uint8_t *case_flags, uint32_t *lfn_count) {
    *case_flags = 0;
    *lfn_count = 0;
    if (name_fits_short(name, short_name, case_flags))
        return VFS_OK;

    *lfn_count = (strlen(name) + LFN_ENTRY_UNITS - 1) / LFN_ENTRY_UNITS;
    return generate_short_name(v, dir, name, short_name);
}

static int create_node(struct fat32_volume *v, const struct vfs_node *parent, const char *name, bool is_dir,
                       struct vfs_node *node) {
    uint32_t dir = parent->location;
    struct dir_iter iter;
    struct dir_entry existing;

    if (!long_name_valid(name))
        return strlen(name) > NAME_MAX ? -ENAMETOOLONG : -EINVAL;

    int status = find_entry(v, dir, name, strlen(name), &existing, &iter);
    if (status == VFS_OK)
        return -EEXIST;
    if (status != VFS_NOT_FOUND)
        return status;

    char short_name[11];
    uint8_t case_flags = 0;
    uint32_t lfn_count = 0;

    status = prepare_name(v, dir, name, short_name, &case_flags, &lfn_count);
    if (status != VFS_OK)
        return status;

    uint32_t cluster = 0;
    if (is_dir) {
        cluster = alloc_cluster(v, 0, true);
        if (cluster == 0)
            return -ENOSPC;

        status = make_directory_contents(v, cluster, dir);
        if (status != VFS_OK) {
            free_chain(v, cluster);
            return status;
        }
    }

    struct dir_entry entry;
    fill_dir_entry(&entry, short_name, is_dir ? ATTR_DIRECTORY : ATTR_ARCHIVE, case_flags, cluster, 0);

    *node = (struct vfs_node){.location = cluster, .is_dir = is_dir};
    status = add_entries(v, dir, name, &entry, lfn_count, node);
    if (status != VFS_OK && cluster)
        free_chain(v, cluster);
    return status;
}

static int fat32_refresh(void *volume, struct vfs_node *node) {
    const struct fat32_volume *v = volume;

    if (node->entry_lba == 0)
        return VFS_OK;
    if (!read_sector(v, node->entry_lba))
        return VFS_IO_ERROR;

    const struct dir_entry *e = (const struct dir_entry *)&sector[node->entry_pos];

    node->location = (uint32_t)e->cluster_high << 16 | e->cluster_low;
    node->size = e->size;
    node->mtime = entry_mtime(e);
    return VFS_OK;
}

static int store_node(struct fat32_volume *v, const struct vfs_node *node) {
    if (!read_sector(v, node->entry_lba))
        return VFS_IO_ERROR;

    struct dir_entry *e = (struct dir_entry *)&sector[node->entry_pos];

    e->cluster_high = node->location >> 16;
    e->cluster_low = node->location;
    e->size = node->size;
    e->attr |= ATTR_ARCHIVE;
    set_modified_now(e);
    return write_cached_sector(v) ? VFS_OK : VFS_IO_ERROR;
}

static int64_t write_file(struct fat32_volume *v, struct vfs_node *node, uint64_t offset, const void *buf, size_t len);

// Ends the chain at the cluster holding the last byte of size, which isn't 0, and
// gets the clusters that were cut off.
static bool cut_chain(struct fat32_volume *v, const struct vfs_node *node, uint64_t size, uint32_t *rest) {
    uint32_t cluster = node->location;

    for (uint64_t skip = (size - 1) / cluster_bytes(v); skip > 0; skip--) {
        if (!next_cluster(v, cluster, &cluster))
            return false;
    }

    if (!read_fat(v, cluster, rest))
        return false;
    if (*rest >= CLUSTER_EOC_MIN)
        *rest = 0;
    return !*rest || write_fat(v, cluster, CLUSTER_EOC_MARK);
}

static int truncate_file(struct fat32_volume *v, struct vfs_node *node, uint64_t size) {
    if (node->is_dir || node->entry_lba == 0)
        return -EISDIR;
    if (size > UINT32_MAX)
        return -EFBIG;

    // Growing is writing past the end, which fills the gap with zeros.
    if (size > node->size) {
        int64_t written = write_file(v, node, size - 1, "", 1);

        return written < 0 ? written : VFS_OK;
    }

    // The entry changes first, in the same commit as the end of the chain, so that
    // the file never claims clusters that have been freed.
    uint32_t rest = 0;

    if (size == 0) {
        rest = node->location;
        node->location = 0;
    } else if (node->location && !cut_chain(v, node, size, &rest)) {
        return VFS_IO_ERROR;
    }
    node->size = size;

    int status = store_node(v, node);

    if (status == VFS_OK && rest && !free_chain(v, rest))
        return VFS_IO_ERROR;
    return status;
}

// The cluster after this one, adding one if the chain ends here, or 0 if it can't.
static uint32_t following_cluster(struct fat32_volume *v, uint32_t cluster) {
    uint32_t next;

    if (!next_cluster(v, cluster, &next))
        return 0;
    return next == CLUSTER_END ? alloc_cluster(v, cluster, false) : next;
}

// Writes without zero-filling. offset must be within or at the end of the file.
static int64_t write_data(struct fat32_volume *v, struct vfs_node *node, uint64_t offset,
                          const void *buf, size_t len) {
    uint32_t size = cluster_bytes(v);
    const uint8_t *in = buf;
    size_t written = 0;

    if (node->location == 0) {
        node->location = alloc_cluster(v, 0, false);
        if (node->location == 0)
            return 0;
    }

    uint32_t cluster = node->location;
    for (uint64_t skip = offset / size; skip > 0; skip--) {
        uint32_t next;

        if (!next_cluster(v, cluster, &next))
            return VFS_IO_ERROR;
        if (next == CLUSTER_END) {
            next = alloc_cluster(v, cluster, false);
            if (next == 0)
                return 0;
        }
        cluster = next;
    }

    uint32_t within = offset % size;
    while (written < len) {
        uint32_t start = within % SECTOR_SIZE;

        // Whole sectors need nothing read first, and go as many at a time as are side by
        // side. Clusters they run into are allocated before anything is written to them.
        if (start == 0 && len - written >= SECTOR_SIZE) {
            uint32_t lba = cluster_lba(v, cluster) + within / SECTOR_SIZE;
            uint32_t want = (len - written) / SECTOR_SIZE;
            uint32_t run = 0;
            bool out_of_room = false;

            for (;;) {
                uint32_t left = (size - within) / SECTOR_SIZE;
                uint32_t take = left < want - run ? left : want - run;

                run += take;
                within += take * SECTOR_SIZE;
                if (run == want)
                    break;

                uint32_t next = following_cluster(v, cluster);

                if (next == 0) {
                    out_of_room = true;
                    break;
                }

                bool adjacent = next == cluster + 1;

                cluster = next;
                within = 0;
                if (!adjacent)
                    break;
            }

            if (!block_write_many(v->drive, lba, run, &in[written]))
                break;
            if (cached_drive == v->drive && cached_lba >= lba && cached_lba - lba < run)
                cached_drive = NULL;

            written += run * SECTOR_SIZE;
            if (out_of_room)
                break;
        } else {
            size_t chunk = SECTOR_SIZE - start;

            if (chunk > len - written)
                chunk = len - written;
            if (!read_sector(v, cluster_lba(v, cluster) + within / SECTOR_SIZE))
                break;

            memcpy(&sector[start], &in[written], chunk);
            if (!write_cached_data(v))
                break;

            written += chunk;
            within += chunk;
        }

        if (written < len && within == size) {
            uint32_t next = following_cluster(v, cluster);

            if (next == 0)
                break;
            cluster = next;
            within = 0;
        }
    }
    return written;
}

// Commits at a point where the entry matches the chain, if the journal is filling up.
static int commit_if_full(struct fat32_volume *v, const struct vfs_node *node) {
    if (v->pending_count < JOURNAL_COMMIT_POINT)
        return VFS_OK;

    int status = store_node(v, node);

    if (status != VFS_OK)
        return status;
    return journal_commit(v) ? VFS_OK : VFS_IO_ERROR;
}

static int64_t write_file(struct fat32_volume *v, struct vfs_node *node, uint64_t offset, const void *buf, size_t len) {
    static const uint8_t zeros[SECTOR_SIZE];

    if (node->is_dir || node->entry_lba == 0)
        return -EISDIR;
    if (offset + len > UINT32_MAX)
        return -EFBIG;
    if (len == 0)
        return 0;

    // Seeking past the end leaves a gap that reads as zeros.
    while (node->size < offset) {
        uint64_t gap = offset - node->size;
        size_t chunk = gap < sizeof(zeros) ? gap : sizeof(zeros);
        int64_t count = write_data(v, node, node->size, zeros, chunk);

        if (count > 0)
            node->size += count;
        if (count != (int64_t)chunk) {
            store_node(v, node);
            return count < 0 ? count : -ENOSPC;
        }

        int status = commit_if_full(v, node);

        if (status != VFS_OK)
            return status;
    }

    const uint8_t *in = buf;
    size_t piece_size = WRITE_PIECE_CLUSTERS * cluster_bytes(v);
    size_t written = 0;
    int64_t failure = 0;

    while (written < len) {
        size_t piece = len - written < piece_size ? len - written : piece_size;
        int64_t count = write_data(v, node, offset + written, &in[written], piece);

        if (count <= 0) {
            failure = count;
            break;
        }

        written += count;
        if (offset + written > node->size)
            node->size = offset + written;
        if ((size_t)count < piece || commit_if_full(v, node) != VFS_OK)
            break;
    }
    if (store_node(v, node) != VFS_OK)
        return VFS_IO_ERROR;
    if (written == 0)
        return failure < 0 ? failure : -ENOSPC;
    return written;
}

// Marks a node's directory entries as deleted, leaving its clusters alone.
static int delete_entries(struct fat32_volume *v, const struct vfs_node *node) {
    for (uint32_t offset = node->run_start; offset <= node->dir_offset; offset += ENTRY_SIZE) {
        uint32_t lba, pos;

        if (locate_dir_offset(v, node->dir_location, offset, false, &lba, &pos) != VFS_OK
            || !read_sector(v, lba))
            return VFS_IO_ERROR;

        sector[pos] = ENTRY_DELETED;
        if (!write_cached_sector(v))
            return VFS_IO_ERROR;
    }
    return VFS_OK;
}

static int remove_node(struct fat32_volume *v, const struct vfs_node *node) {
    if (node->is_dir) {
        struct dir_iter iter = {.cluster = node->location};
        struct dir_entry entry;
        char name[NAME_MAX + 1];
        int status = next_entry(v, &iter, &entry, name);

        if (status != VFS_END)
            return status == VFS_OK ? -ENOTEMPTY : status;
    }

    int status = delete_entries(v, node);
    if (status != VFS_OK)
        return status;

    if (node->location && !free_chain(v, node->location))
        return VFS_IO_ERROR;
    return VFS_OK;
}

// Checks whether dir is ancestor or inside it. Paths can't answer this, because
// /x and /dev/hda/x are the same place.
static int is_inside(const struct fat32_volume *v, uint32_t dir, uint32_t ancestor) {
    for (uint32_t depth = 0; depth < v->cluster_count; depth++) {
        if (dir == ancestor)
            return true;
        if (dir == v->root_cluster)
            return false;

        uint32_t lba = cluster_lba(v, dir);

        if (!read_sector(v, lba + DOTDOT_OFFSET / SECTOR_SIZE))
            return VFS_IO_ERROR;

        const struct dir_entry *e = (const struct dir_entry *)&sector[DOTDOT_OFFSET % SECTOR_SIZE];
        dir = (uint32_t)e->cluster_high << 16 | e->cluster_low;

        // A parent that is the root is recorded as cluster 0.
        if (dir == 0)
            dir = v->root_cluster;
        if (!cluster_valid(v, dir))
            return VFS_IO_ERROR;
    }
    return VFS_IO_ERROR;
}

static int set_dotdot(struct fat32_volume *v, uint32_t dir, uint32_t parent) {
    uint32_t stored = parent == v->root_cluster ? 0 : parent;

    if (!read_sector(v, cluster_lba(v, dir) + DOTDOT_OFFSET / SECTOR_SIZE))
        return VFS_IO_ERROR;

    struct dir_entry *e = (struct dir_entry *)&sector[DOTDOT_OFFSET % SECTOR_SIZE];

    e->cluster_high = stored >> 16;
    e->cluster_low = stored;
    return write_cached_sector(v) ? VFS_OK : VFS_IO_ERROR;
}

// The new entries are written before the old ones go, so a failure in between
// leaves the file reachable rather than lost.
static int rename_node(struct fat32_volume *v, const struct vfs_node *node, const struct vfs_node *parent, const char *name) {
    uint32_t dir = parent->location;
    struct dir_iter iter;
    struct dir_entry entry;

    if (!long_name_valid(name))
        return strlen(name) > NAME_MAX ? -ENAMETOOLONG : -EINVAL;

    if (node->is_dir) {
        int inside = is_inside(v, dir, node->location);

        if (inside != false)
            return inside < 0 ? inside : -EINVAL;
    }

    int status = find_entry(v, dir, name, strlen(name), &entry, &iter);
    if (status == VFS_OK && (iter.entry_lba != node->entry_lba || iter.entry_pos != node->entry_pos))
        return -EEXIST;
    if (status != VFS_OK && status != VFS_NOT_FOUND)
        return status;

    char short_name[11];
    uint8_t case_flags;
    uint32_t lfn_count;

    status = prepare_name(v, dir, name, short_name, &case_flags, &lfn_count);
    if (status != VFS_OK)
        return status;

    if (!read_sector(v, node->entry_lba))
        return VFS_IO_ERROR;
    memcpy(&entry, &sector[node->entry_pos], sizeof(entry));
    memcpy(entry.name, short_name, sizeof(short_name));
    entry.case_flags = case_flags;

    struct vfs_node placed;
    status = add_entries(v, dir, name, &entry, lfn_count, &placed);
    if (status != VFS_OK)
        return status;

    status = delete_entries(v, node);
    if (status != VFS_OK)
        return status;

    if (node->is_dir && dir != node->dir_location)
        return set_dotdot(v, node->location, dir);
    return VFS_OK;
}

static bool is_metadata_name(const char *name) {
    return names_equal(METADATA_NAME, name, strlen(name));
}

static uint32_t journal_clusters(const struct fat32_volume *v) {
    return (JOURNAL_BYTES + cluster_bytes(v) - 1) / cluster_bytes(v);
}

// Finds where the journal is. Returns false if it isn't one whole run.
static bool journal_map(struct fat32_volume *v, const struct vfs_node *journal) {
    uint32_t cluster = journal->location;

    if (!cluster_valid(v, cluster))
        return false;
    v->journal_start = cluster_lba(v, cluster);

    for (uint32_t i = 1; i < journal_clusters(v); i++) {
        uint32_t next;

        if (!next_cluster(v, cluster, &next) || next != cluster + 1)
            return false;
        cluster = next;
    }
    return true;
}

// Finishes an operation a crash interrupted, if the journal holds a complete one.
static void journal_replay(struct fat32_volume *v) {
    if (!block_read(v->drive, journal_lba(v, 0), journal_sector))
        return;

    struct journal_header header = *(struct journal_header *)journal_sector;

    if (header.magic != JOURNAL_MAGIC || header.count == 0 || header.count > JOURNAL_CAPACITY)
        return;

    uint32_t checksum = 2166136261u;

    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < header.count; i++) {
            if (i % JOURNAL_ENTRIES_PER_SECTOR == 0
                && !block_read(v->drive, journal_lba(v, 1 + i / JOURNAL_ENTRIES_PER_SECTOR), journal_table))
                return;

            uint32_t lba = ((const uint32_t *)journal_table)[i % JOURNAL_ENTRIES_PER_SECTOR];

            if (!block_read(v->drive, journal_lba(v, JOURNAL_DATA_START + i), journal_sector))
                return;
            if (pass == 0) {
                checksum = checksum_add(checksum, &lba, sizeof(lba));
                checksum = checksum_add(checksum, journal_sector, SECTOR_SIZE);
            } else if (!block_write(v->drive, lba, journal_sector)) {
                return;
            }
        }
        if (pass == 0 && checksum != header.checksum) {
            kprintf("fat32: ignoring a damaged journal\n");
            write_journal_header(v, 0, 0);
            return;
        }
    }
    flush_drive(v);
    write_journal_header(v, 0, 0);
    cached_drive = NULL;
    kprintf("fat32: finished %u sector writes from the journal\n", header.count);
}

// Runs at mount and writes nothing unless there's a crash to recover from.
static void journal_open(struct fat32_volume *v) {
    struct vfs_node node;

    if (!v->drive->write || fat32_lookup(v, METADATA_NAME, &node) != VFS_OK || !node.is_dir)
        return;
    v->metadata_cluster = node.location;

    if (fat32_lookup(v, JOURNAL_PATH, &node) != VFS_OK || node.size != JOURNAL_BYTES || !journal_map(v, &node))
        return;
    journal_replay(v);
    v->journal_ready = true;
}

// Gives the journal a run of free clusters. Only its header's contents matter, so
// the rest doesn't have to be written.
static int size_journal(struct fat32_volume *v, struct vfs_node *journal) {
    int status = truncate_file(v, journal, 0);

    if (status != VFS_OK)
        return status;

    uint32_t needed = journal_clusters(v);
    uint32_t run = 0;
    uint32_t start = 0;

    for (uint32_t i = 0; i < v->cluster_count && run < needed; i++) {
        uint32_t value;

        if (!read_fat(v, i + 2, &value))
            return VFS_IO_ERROR;
        run = value == 0 ? run + 1 : 0;
        if (run == 1)
            start = i + 2;
    }
    if (run < needed)
        return -ENOSPC;

    uint32_t previous = 0;

    v->next_free = start;
    for (uint32_t i = 0; i < needed; i++) {
        previous = alloc_cluster(v, previous, false);
        if (previous == 0)
            return -ENOSPC;
    }
    journal->location = start;
    journal->size = JOURNAL_BYTES;
    return store_node(v, journal);
}

// Makes the journal the first time a volume is changed, so that only looking at a
// disk leaves it as it was. If that fails, changes just aren't protected.
static void journal_make(struct fat32_volume *v) {
    struct vfs_node root = {.location = v->root_cluster, .is_dir = true};
    struct vfs_node metadata, journal;

    v->journal_tried = true;
    if (v->journal_ready)
        return;
    v->making_journal = true;

    int status = fat32_lookup(v, METADATA_NAME, &metadata);

    if (status == VFS_NOT_FOUND)
        status = create_node(v, &root, METADATA_NAME, true, &metadata);
    if (status != VFS_OK || !metadata.is_dir)
        goto failed;
    v->metadata_cluster = metadata.location;

    status = fat32_lookup(v, JOURNAL_PATH, &journal);
    if (status == VFS_NOT_FOUND)
        status = create_node(v, &metadata, JOURNAL_NAME, false, &journal);
    if (status == VFS_OK && (journal.size != JOURNAL_BYTES || !journal_map(v, &journal))) {
        status = size_journal(v, &journal);
        if (status == VFS_OK && !journal_map(v, &journal))
            status = VFS_IO_ERROR;
    }
    if (status != VFS_OK)
        goto failed;

    v->making_journal = false;
    if (!journal_commit(v) || !write_journal_header(v, 0, 0))
        goto failed;

    v->journal_ready = true;
    return;

failed:
    v->making_journal = false;
    journal_commit(v);
    kprintf("fat32: couldn't make a journal, so changes aren't protected from crashes\n");
}

static bool is_protected(const struct fat32_volume *v, const struct vfs_node *node) {
    return v->metadata_cluster
        && (node->dir_location == v->metadata_cluster || (node->is_dir && node->location == v->metadata_cluster));
}

// Anything being put in the metadata folder, or in the place it belongs, is refused.
static bool is_protected_place(const struct fat32_volume *v, const struct vfs_node *parent, const char *name) {
    return parent->location == v->metadata_cluster
        || (parent->location == v->root_cluster && is_metadata_name(name));
}

static void journal_begin(struct fat32_volume *v) {
    if (!v->journal_tried)
        journal_make(v);
}

static int fat32_create(void *volume, const struct vfs_node *parent, const char *name, bool is_dir,
                        struct vfs_node *node) {
    struct fat32_volume *v = volume;

    journal_begin(v);
    if (is_protected_place(v, parent, name))
        return -EPERM;

    int status = create_node(v, parent, name, is_dir, node);

    return journal_commit(v) ? status : VFS_IO_ERROR;
}

static int64_t fat32_write(void *volume, struct vfs_node *node, uint64_t offset, const void *buf, size_t len) {
    struct fat32_volume *v = volume;

    journal_begin(v);
    if (is_protected(v, node))
        return -EPERM;

    int64_t written = write_file(v, node, offset, buf, len);

    return journal_commit(v) ? written : VFS_IO_ERROR;
}

static int fat32_truncate(void *volume, struct vfs_node *node, uint64_t size) {
    struct fat32_volume *v = volume;

    journal_begin(v);
    if (is_protected(v, node))
        return -EPERM;

    int status = truncate_file(v, node, size);

    return journal_commit(v) ? status : VFS_IO_ERROR;
}

static int fat32_remove(void *volume, const struct vfs_node *node) {
    struct fat32_volume *v = volume;

    journal_begin(v);
    if (is_protected(v, node))
        return -EPERM;

    int status = remove_node(v, node);

    return journal_commit(v) ? status : VFS_IO_ERROR;
}

static int fat32_rename(void *volume, const struct vfs_node *node, const struct vfs_node *parent, const char *name) {
    struct fat32_volume *v = volume;

    journal_begin(v);
    if (is_protected(v, node) || is_protected_place(v, parent, name))
        return -EPERM;

    int status = rename_node(v, node, parent, name);

    return journal_commit(v) ? status : VFS_IO_ERROR;
}

const struct vfs_ops fat32_ops = {
    .type = "fat32",
    .lookup = fat32_lookup,
    .read = fat32_read,
    .readdir = fat32_readdir,
    .statfs = fat32_statfs,
    .refresh = fat32_refresh,
    .write = fat32_write,
    .truncate = fat32_truncate,
    .create = fat32_create,
    .remove = fat32_remove,
    .rename = fat32_rename,
};
