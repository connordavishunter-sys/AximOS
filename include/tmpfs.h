#pragma once

#include <vfs.h>

// /tmp: files kept in memory, which start empty each boot.
extern const struct vfs_ops tmpfs_ops;
