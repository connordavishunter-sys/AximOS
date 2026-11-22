#include <block.h>
#include <kmalloc.h>
#include <mm.h>
#include <serial.h>
#include <string.h>

#define BUCKET_COUNT 16384
#define MIN_CACHED_BYTES (4 * 1024 * 1024)
#define MAX_CACHED_BYTES (64 * 1024 * 1024)
#define STREAM_SECTORS 16 // a read of this many at once is taken to be going through a big file

struct block_device *block_devices[BLOCK_MAX_DEVICES];
int block_device_count;

// Entries are found by hash and evicted oldest first. What a big read brings in starts
// out oldest, so that copying a big file doesn't push out everything else: it's kept
// only if it's read again, or there's room to spare.
struct cache_entry {
    struct block_device *device;
    uint32_t lba;
    void *data;
    struct cache_entry *bucket_next;
    struct cache_entry *older;
    struct cache_entry *newer;
};

static struct cache_entry *buckets[BUCKET_COUNT];
static struct cache_entry *newest;
static struct cache_entry *oldest;
static uint64_t cached_bytes;
static uint64_t cache_limit;
static uint64_t hits;
static uint64_t misses;
static int optical_count;

bool block_register(struct block_device *device) {
    if (block_device_count == BLOCK_MAX_DEVICES)
        return false;

    block_devices[block_device_count++] = device;
    kprintf("block: %s has %u sectors of %u bytes\n", device->name, device->sector_count,
            device->sector_size);
    return true;
}

int block_next_optical_index(void) {
    return optical_count++;
}

void block_name_disk(struct block_device *device) {
    memcpy(device->name, "sda", 4);
    for (int i = 0; i < block_device_count; i++) {
        if (!strcmp(block_devices[i]->name, device->name)) {
            device->name[2]++;
            i = -1;
        }
    }
}

static struct cache_entry **bucket_for(const struct block_device *device, uint32_t lba) {
    uint32_t hash = (lba * 2654435761u) ^ ((uint64_t)device >> 4);

    return &buckets[hash % BUCKET_COUNT];
}

static struct cache_entry *find(const struct block_device *device, uint32_t lba) {
    for (struct cache_entry *e = *bucket_for(device, lba); e; e = e->bucket_next) {
        if (e->device == device && e->lba == lba)
            return e;
    }
    return NULL;
}

static void unlink_age(struct cache_entry *e) {
    if (e->newer)
        e->newer->older = e->older;
    else
        newest = e->older;
    if (e->older)
        e->older->newer = e->newer;
    else
        oldest = e->newer;
}

static void make_newest(struct cache_entry *e) {
    e->older = newest;
    e->newer = NULL;
    if (newest)
        newest->newer = e;
    else
        oldest = e;
    newest = e;
}

static void make_oldest(struct cache_entry *e) {
    e->newer = oldest;
    e->older = NULL;
    if (oldest)
        oldest->older = e;
    else
        newest = e;
    oldest = e;
}

static void evict(struct cache_entry *e) {
    struct cache_entry **link = bucket_for(e->device, e->lba);

    while (*link != e)
        link = &(*link)->bucket_next;
    *link = e->bucket_next;

    unlink_age(e);
    cached_bytes -= e->device->sector_size;
    kfree(e->data);
    kfree(e);
}

// Failing to cache only costs speed, so running out of memory isn't an error.
// A sixteenth of memory, within limits.
static uint64_t limit(void) {
    if (!cache_limit) {
        uint64_t share = mm_total_page_count() * PAGE_SIZE / 16;

        cache_limit = share < MIN_CACHED_BYTES ? MIN_CACHED_BYTES : share > MAX_CACHED_BYTES ? MAX_CACHED_BYTES : share;
    }
    return cache_limit;
}

static void remember(struct block_device *device, uint32_t lba, const void *buf, bool streaming) {
    size_t size = device->sector_size;

    while (oldest && cached_bytes + size > limit())
        evict(oldest);

    struct cache_entry *e = kmalloc(sizeof(*e));
    void *data = kmalloc(size);

    if (!e || !data) {
        kfree(e);
        kfree(data);
        return;
    }

    memcpy(data, buf, size);
    e->device = device;
    e->lba = lba;
    e->data = data;

    struct cache_entry **bucket = bucket_for(device, lba);

    e->bucket_next = *bucket;
    *bucket = e;
    if (streaming)
        make_oldest(e);
    else
        make_newest(e);
    cached_bytes += size;
}

static bool block_read_cached(struct block_device *device, uint32_t lba, void *buf) {
    struct cache_entry *e = find(device, lba);

    if (!e)
        return false;

    memcpy(buf, e->data, device->sector_size);
    unlink_age(e);
    make_newest(e);
    hits++;
    return true;
}

bool block_read(struct block_device *device, uint32_t lba, void *buf) {
    if (block_read_cached(device, lba, buf))
        return true;

    misses++;
    if (lba >= device->sector_count) {
        kprintf("block: %s lba %u out of range\n", device->name, lba);
        return false;
    }
    if (!device->read(device, lba, 1, buf)) {
        kprintf("block: %s read of lba %u failed\n", device->name, lba);
        return false;
    }

    remember(device, lba, buf, false);
    return true;
}

bool block_write(struct block_device *device, uint32_t lba, const void *buf) {
    if (!device->write || lba >= device->sector_count) {
        kprintf("block: can't write %s lba %u\n", device->name, lba);
        return false;
    }

    struct cache_entry *e = find(device, lba);

    // A failed write leaves the sector's contents unknown, so don't keep any.
    if (!device->write(device, lba, 1, buf)) {
        kprintf("block: %s write of lba %u failed\n", device->name, lba);
        if (e)
            evict(e);
        return false;
    }

    if (e) {
        memcpy(e->data, buf, device->sector_size);
        unlink_age(e);
        make_newest(e);
    } else {
        remember(device, lba, buf, false);
    }
    return true;
}

bool block_read_many(struct block_device *device, uint32_t lba, uint32_t count, void *buf) {
    uint8_t *out = buf;
    size_t size = device->sector_size;

    if (lba > device->sector_count || count > device->sector_count - lba) {
        kprintf("block: %s lba %u out of range\n", device->name, lba);
        return false;
    }

    while (count > 0) {
        if (block_read_cached(device, lba, out)) {
            lba++;
            out += size;
            count--;
            continue;
        }

        // As far as the sectors that aren't in the cache go.
        uint32_t run = 1;

        while (run < count && run < device->max_sectors && !find(device, lba + run))
            run++;

        misses += run;
        if (!device->read(device, lba, run, out)) {
            kprintf("block: %s read of lba %u failed\n", device->name, lba);
            return false;
        }
        for (uint32_t i = 0; i < run; i++)
            remember(device, lba + i, out + i * size, run >= STREAM_SECTORS);
        lba += run;
        out += run * size;
        count -= run;
    }
    return true;
}

bool block_write_many(struct block_device *device, uint32_t lba, uint32_t count, const void *buf) {
    const uint8_t *in = buf;
    size_t size = device->sector_size;

    if (!device->write || lba > device->sector_count || count > device->sector_count - lba) {
        kprintf("block: can't write %s lba %u\n", device->name, lba);
        return false;
    }

    while (count > 0) {
        uint32_t run = count < device->max_sectors ? count : device->max_sectors;
        bool ok = device->write(device, lba, run, in);

        // A failed write leaves the sectors' contents unknown, so don't keep any.
        for (uint32_t i = 0; i < run; i++) {
            struct cache_entry *e = find(device, lba + i);

            if (e && ok)
                memcpy(e->data, in + i * size, size);
            else if (e)
                evict(e);
        }
        if (!ok) {
            kprintf("block: %s write of lba %u failed\n", device->name, lba);
            return false;
        }
        lba += run;
        in += run * size;
        count -= run;
    }
    return true;
}

uint64_t block_cached_bytes(void) {
    return cached_bytes;
}

void block_flush_all(void) {
    for (int i = 0; i < block_device_count; i++) {
        if (block_devices[i]->flush)
            block_devices[i]->flush(block_devices[i]);
    }
    kprintf("block: cache had %lu hits and %lu misses, holding %lu KiB\n", hits, misses,
            cached_bytes / 1024);
}
