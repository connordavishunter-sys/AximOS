#pragma once

#include <vfs.h>

// /proc: what the kernel knows, made up as text whenever it's read.
extern const struct vfs_ops procfs_ops;
