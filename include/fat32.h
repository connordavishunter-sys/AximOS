#pragma once

#include <block.h>
#include <vfs.h>

extern const struct vfs_ops fat32_ops;

// Returns the volume starting at start_lba, or NULL if it isn't FAT32.
void *fat32_mount(struct block_device *drive, uint32_t start_lba);
