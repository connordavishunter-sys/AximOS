#pragma once

#include <block.h>
#include <vfs.h>

extern const struct vfs_ops iso9660_ops;

// Returns the volume on a disc, or NULL if it isn't ISO 9660.
void *iso9660_mount(struct block_device *drive);
