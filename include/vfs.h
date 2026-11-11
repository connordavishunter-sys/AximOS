#pragma once

#include <errno_values.h>
#include <fs_types.h>
#include <stddef.h>
#include <stdint.h>

#define VFS_OK 0
#define VFS_NOT_FOUND (-ENOENT)
#define VFS_IO_ERROR (-EIO)
#define VFS_END (-1000) // not an error, so not an errno value

#define VFS_DEVICE_NAME_MAX 16

struct vfs_mount;

// A file or directory. location is the driver's own way to find it.
enum vfs_device {
    VFS_DEVICE_NONE,
    VFS_DEVICE_MOUSE,
    VFS_DEVICE_KEYBOARD,
    VFS_DEVICE_FB,
    VFS_DEVICE_NULL,
    VFS_DEVICE_ZERO,
    VFS_DEVICE_TTY,
    VFS_DEVICE_RANDOM,
    VFS_DEVICE_URANDOM,
    VFS_DEVICE_COUNT,
};

struct vfs_node {
    struct vfs_mount *mount;
    uint32_t location;
    uint32_t size;
    int64_t mtime; // seconds since 1970 in UTC, or 0 if unknown
    bool is_dir;
    bool is_root; // the root of the whole tree, which also lists /dev

    // Where the driver keeps this file's directory entry. entry_lba is 0 for
    // anything without one, such as a volume's root.
    uint32_t entry_lba;
    uint32_t entry_pos;
    uint32_t dir_location;
    uint32_t dir_offset;
    uint32_t run_start;
};

// What a filesystem driver provides. Paths are relative to the volume's root,
// with no leading slash and no "." or ".." parts, and "" is the root itself.
struct vfs_ops {
    const char *type;
    int (*lookup)(void *volume, const char *path, struct vfs_node *node);
    int64_t (*read)(void *volume, const struct vfs_node *node, uint64_t offset, void *buf, size_t len);
    // Advances cursor past the entry it returns, or returns VFS_END.
    int (*readdir)(void *volume, const struct vfs_node *node, uint64_t *cursor, struct dirent *entry);
    bool (*statfs)(void *volume, struct statfs *out);

    // Optional, and left out by read-only filesystems. Refresh rereads a node's
    // size and location, since other descriptors may have changed them.
    int (*refresh)(void *volume, struct vfs_node *node);
    int64_t (*write)(void *volume, struct vfs_node *node, uint64_t offset, const void *buf, size_t len);
    int (*truncate)(void *volume, struct vfs_node *node, uint64_t size);
    int (*create)(void *volume, const struct vfs_node *parent, const char *name, bool is_dir, struct vfs_node *node);
    int (*remove)(void *volume, const struct vfs_node *node);
    // Fails if the name is taken by anything but the node itself.
    int (*rename)(void *volume, const struct vfs_node *node, const struct vfs_node *parent, const char *name);
};

// Only one process can be inside the filesystems at a time, since working on them
// means sleeping on the disk. Everything below that touches a volume needs this,
// and it must not be held while waiting for anything but the disk.
void vfs_lock(void);
void vfs_unlock(void);

// For a thread that dies unexpectedly.
void vfs_release_if_mine(void);

// Mounts every recognised filesystem on every drive under /dev. Returns false
// if there were none.
bool vfs_mount_all(void);

// Writes a line about each mounted filesystem, returning how much that came to.
size_t vfs_list_mounts(char *out, size_t size);

// Makes a device exist in /dev.
void vfs_add_device(enum vfs_device device);

void vfs_fill_stat(const struct vfs_node *node, struct stat *out);

// Which device a node is, if any.
enum vfs_device vfs_device_of(const struct vfs_node *node);

// Makes the first mounted filesystem containing path the root of the tree.
bool vfs_choose_root(const char *path);

// Resolves a path, which is taken relative to cwd unless it is absolute.
int vfs_lookup(const char *cwd, const char *path, struct vfs_node *node);

// Writes the absolute form of a path, removing "." and ".." parts.
bool vfs_normalize(const char *cwd, const char *path, char out[PATH_MAX]);

// Returns the bytes read, short only at end of file, or VFS_IO_ERROR.
int64_t vfs_read(const struct vfs_node *node, uint64_t offset, void *buf, size_t len);

int vfs_readdir(const struct vfs_node *node, uint64_t *cursor, struct dirent *entry);
bool vfs_statfs(const struct vfs_node *node, struct statfs *out);

int vfs_refresh(struct vfs_node *node);

// Returns the bytes written, short only if the disk filled up, or a negative errno.
int64_t vfs_write(struct vfs_node *node, uint64_t offset, const void *buf, size_t len);

int vfs_truncate(struct vfs_node *node, uint64_t size);

// Fails if the path already exists or its filesystem is read-only.
int vfs_create(const char *cwd, const char *path, bool is_dir, struct vfs_node *node);

// Removes a file, or a directory if it is empty. Callers must make sure it
// isn't open.
int vfs_remove(const struct vfs_node *node);

bool vfs_same_file(const struct vfs_node *a, const struct vfs_node *b);

// Moves a node to a path on the same filesystem, replacing a file already there.
// Callers must make sure neither of them is open.
int vfs_rename(const struct vfs_node *node, const char *cwd, const char *path);
