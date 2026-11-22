#pragma once

#include <stddef.h>
#include <stdint.h>

#define BLOCK_NAME_MAX 8
#define BLOCK_MAX_DEVICES 16

// A disk or disc that sectors can be read from. Callers must hold the VFS lock
// once processes are running, since drivers sleep while the hardware works.
struct block_device {
    char name[BLOCK_NAME_MAX];
    bool optical;
    uint32_t sector_size;
    uint32_t sector_count;
    uint32_t max_sectors; // the most one command can move
    bool claimed;         // open for writing through /dev/disk
    void *driver;
    bool (*read)(struct block_device *device, uint32_t lba, uint32_t count, void *buf);
    bool (*write)(struct block_device *device, uint32_t lba, uint32_t count, const void *buf); // NULL if read-only
    void (*flush)(struct block_device *device);
};

extern struct block_device *block_devices[BLOCK_MAX_DEVICES];
extern int block_device_count;

// Adds a device, which must stay valid forever. Returns false if there's no room.
bool block_register(struct block_device *device);

// Discs are numbered in the order they're found, whatever their controller.
int block_next_optical_index(void);

// Names a disk after those already found, as sdb after sda.
void block_name_disk(struct block_device *device);

// These go through a cache of recently used sectors. Writes reach the device
// before they return, so the cache never holds anything the disk doesn't.
bool block_read(struct block_device *device, uint32_t lba, void *buf);
bool block_write(struct block_device *device, uint32_t lba, const void *buf);

// The same for many sectors in a row, in as few commands as the device allows. What's
// written this way isn't added to the cache, so a big file doesn't push out the FAT.
bool block_read_many(struct block_device *device, uint32_t lba, uint32_t count, void *buf);
bool block_write_many(struct block_device *device, uint32_t lba, uint32_t count, const void *buf);

// How much memory the cache is holding.
uint64_t block_cached_bytes(void);

// Makes every device commit what it has buffered, and reports how the cache did.
void block_flush_all(void);
