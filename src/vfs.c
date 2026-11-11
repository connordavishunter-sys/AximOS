#include <block.h>
#include <elf.h>
#include <fat32.h>
#include <iso9660.h>
#include <process.h>
#include <procfs.h>
#include <serial.h>
#include <string.h>
#include <vfs.h>

#define MAX_MOUNTS 16
#define SECTOR_SIZE 512
#define MBR_TABLE_OFFSET 446
#define MBR_ENTRIES 4
#define MBR_TYPE_FAT32_CHS 0x0B
#define MBR_TYPE_FAT32_LBA 0x0C

#define ROOT_CURSOR_DONE UINT64_MAX
#define ROOT_CURSOR_PROC (UINT64_MAX - 1)

struct vfs_mount {
    char device[VFS_DEVICE_NAME_MAX];
    const struct vfs_ops *ops;
    void *volume;
};

struct mbr_entry {
    uint8_t status;
    uint8_t chs_first[3];
    uint8_t type;
    uint8_t chs_last[3];
    uint32_t lba;
    uint32_t sectors;
} __attribute__((packed));

static struct vfs_mount mounts[MAX_MOUNTS];
static int mount_count;
static struct vfs_mount *root_mount;
static bool device_present[VFS_DEVICE_COUNT];
static const char *const device_names[VFS_DEVICE_COUNT] = {
    [VFS_DEVICE_MOUSE] = "mouse",
    [VFS_DEVICE_KEYBOARD] = "keyboard",
    [VFS_DEVICE_FB] = "fb0",
    [VFS_DEVICE_NULL] = "null",
    [VFS_DEVICE_ZERO] = "zero",
    [VFS_DEVICE_TTY] = "tty",
    [VFS_DEVICE_RANDOM] = "random",
    [VFS_DEVICE_URANDOM] = "urandom",
};
static bool lock_held;
static int lock_owner;

// /dev itself, which lists the mounts.
static int dev_readdir(void *volume, const struct vfs_node *node, uint64_t *cursor, struct dirent *entry) {
    (void)volume;
    (void)node;

    // The cursor counts the mounts first and then the possible devices.
    while (*cursor >= (uint64_t)mount_count) {
        uint64_t device = (*cursor)++ - mount_count;

        if (device >= VFS_DEVICE_COUNT)
            return VFS_END;
        if (device_present[device]) {
            memcpy(entry->d_name, device_names[device], strlen(device_names[device]) + 1);
            entry->d_type = DT_CHR;
            entry->d_size = 0;
            return VFS_OK;
        }
    }

    const char *device = mounts[(*cursor)++].device;
    memcpy(entry->d_name, device, strlen(device) + 1);
    entry->d_type = DT_DIR;
    entry->d_size = 0;
    return VFS_OK;
}

static struct vfs_mount proc_mount = {.device = "proc", .ops = &procfs_ops};

static const struct vfs_ops dev_ops = {.type = "devfs", .readdir = dev_readdir};
static struct vfs_mount dev_mount = {.device = "devfs", .ops = &dev_ops};

static void add_mount(const char *device, const struct vfs_ops *ops, void *volume) {
    if (mount_count == MAX_MOUNTS) {
        kprintf("vfs: too many filesystems, ignoring %s\n", device);
        return;
    }

    struct vfs_mount *mount = &mounts[mount_count++];
    memcpy(mount->device, device, strlen(device) + 1);
    mount->ops = ops;
    mount->volume = volume;
    kprintf("vfs: mounted %s filesystem at /dev/%s\n", ops->type, device);
}

static void mount_disk(struct block_device *drive) {
    void *volume = fat32_mount(drive, 0);
    if (volume) {
        add_mount(drive->name, &fat32_ops, volume);
        return;
    }

    uint8_t sector[SECTOR_SIZE] __attribute__((aligned(2)));
    if (!block_read(drive, 0, sector) || sector[510] != 0x55 || sector[511] != 0xAA) {
        kprintf("vfs: nothing to mount on /dev/%s\n", drive->name);
        return;
    }

    struct mbr_entry entries[MBR_ENTRIES];
    memcpy(entries, &sector[MBR_TABLE_OFFSET], sizeof(entries));

    for (int i = 0; i < MBR_ENTRIES; i++) {
        if (entries[i].type != MBR_TYPE_FAT32_CHS && entries[i].type != MBR_TYPE_FAT32_LBA)
            continue;

        size_t len = strlen(drive->name);
        char device[BLOCK_NAME_MAX + 2];

        memcpy(device, drive->name, len);
        device[len] = '1' + i;
        device[len + 1] = '\0';

        volume = fat32_mount(drive, entries[i].lba);
        if (volume)
            add_mount(device, &fat32_ops, volume);
        else
            kprintf("vfs: nothing to mount on /dev/%s\n", device);
    }
}

static void mount_disc(struct block_device *drive) {
    void *volume = iso9660_mount(drive);
    if (volume)
        add_mount(drive->name, &iso9660_ops, volume);
    else
        kprintf("vfs: nothing to mount on /dev/%s\n", drive->name);
}

bool vfs_mount_all(void) {
    for (int i = 0; i < block_device_count; i++) {
        if (block_devices[i]->optical)
            mount_disc(block_devices[i]);
        else
            mount_disk(block_devices[i]);
    }
    return mount_count > 0;
}

void vfs_lock(void) {
    while (lock_held && process_can_sleep())
        process_sleep_uninterruptible(&lock_held, 0);
    lock_held = true;
    lock_owner = process_can_sleep() ? process_thread_id() : 0;
}

void vfs_release_if_mine(void) {
    if (lock_held && lock_owner == process_thread_id())
        vfs_unlock();
}

void vfs_unlock(void) {
    lock_held = false;
    process_wakeup(&lock_held);
}

bool vfs_choose_root(const char *path) {
    for (int i = 0; i < mount_count; i++) {
        struct vfs_node node;

        if (mounts[i].ops->lookup(mounts[i].volume, path + 1, &node) == VFS_OK) {
            root_mount = &mounts[i];
            kprintf("vfs: /dev/%s is the root filesystem\n", mounts[i].device);
            return true;
        }
    }
    return false;
}

// Appends each part of a path to out, applying "." and "..".
static bool append_path(char *out, size_t *len, const char *path) {
    while (*path) {
        while (*path == '/')
            path++;

        const char *end = path;
        while (*end && *end != '/')
            end++;

        size_t part = end - path;

        if (part == 2 && path[0] == '.' && path[1] == '.') {
            while (*len > 0 && out[--*len] != '/')
                ;
        } else if (part > 0 && !(part == 1 && path[0] == '.')) {
            if (*len + part + 2 > PATH_MAX)
                return false;

            out[(*len)++] = '/';
            memcpy(&out[*len], path, part);
            *len += part;
        }
        path = end;
    }
    return true;
}

bool vfs_normalize(const char *cwd, const char *path, char out[PATH_MAX]) {
    size_t len = 0;

    if (*path == '\0' || (*path != '/' && !append_path(out, &len, cwd)))
        return false;
    if (!append_path(out, &len, path))
        return false;

    if (len == 0)
        out[len++] = '/';
    out[len] = '\0';
    return true;
}

static struct vfs_mount *find_mount(const char *device, size_t len) {
    for (int i = 0; i < mount_count; i++) {
        if (strlen(mounts[i].device) == len && memcmp(mounts[i].device, device, len) == 0)
            return &mounts[i];
    }
    return NULL;
}

// FAT32 has no links, so names that other systems' programs expect are mapped here.
static const char *const aliases[][2] = {
    {"/bin/sh", "/system/bin/dash"},
    {"/system/bin/sh", "/system/bin/dash"},
};

int vfs_lookup(const char *cwd, const char *path, struct vfs_node *node) {
    char full[PATH_MAX];

    if (*path == '\0')
        return -ENOENT;
    if (!vfs_normalize(cwd, path, full))
        return -ENAMETOOLONG;
    for (size_t i = 0; i < sizeof aliases / sizeof *aliases; i++) {
        if (strcmp(full, aliases[i][0]) == 0)
            memcpy(full, aliases[i][1], strlen(aliases[i][1]) + 1);
    }

    struct vfs_mount *mount = root_mount;
    const char *rest = full + 1;

    if (memcmp(full, "/proc", 5) == 0 && (full[5] == '/' || full[5] == '\0')) {
        mount = &proc_mount;
        rest = full[5] ? full + 6 : full + 5;
    } else if (memcmp(full, "/dev", 4) == 0 && (full[4] == '/' || full[4] == '\0')) {
        if (full[4] == '\0') {
            *node = (struct vfs_node){.mount = &dev_mount, .is_dir = true};
            return VFS_OK;
        }

        const char *device = full + 5;
        rest = device;
        while (*rest && *rest != '/')
            rest++;

        for (int i = 0; *rest == '\0' && i < VFS_DEVICE_COUNT; i++) {
            if (device_present[i] && strcmp(device, device_names[i]) == 0) {
                *node = (struct vfs_node){.mount = &dev_mount, .location = i};
                return path[strlen(path) - 1] == '/' ? -ENOTDIR : VFS_OK;
            }
        }

        mount = find_mount(device, rest - device);
        if (!mount)
            return VFS_NOT_FOUND;
        if (*rest == '/')
            rest++;
    }

    int status = mount->ops->lookup(mount->volume, rest, node);
    if (status != VFS_OK)
        return status;

    node->mount = mount;
    node->is_root = full[1] == '\0';

    // A trailing slash only makes sense on a directory.
    if (path[strlen(path) - 1] == '/' && !node->is_dir)
        return -ENOTDIR;
    return VFS_OK;
}

void vfs_add_device(enum vfs_device device) {
    device_present[device] = true;
}

enum vfs_device vfs_device_of(const struct vfs_node *node) {
    return node->mount == &dev_mount && !node->is_dir ? node->location : VFS_DEVICE_NONE;
}

int64_t vfs_read(const struct vfs_node *node, uint64_t offset, void *buf, size_t len) {
    if (!node->mount->ops->read)
        return VFS_IO_ERROR;
    return node->mount->ops->read(node->mount->volume, node, offset, buf, len);
}

int vfs_readdir(const struct vfs_node *node, uint64_t *cursor, struct dirent *entry) {
    if (*cursor == ROOT_CURSOR_DONE)
        return VFS_END;
    if (*cursor == ROOT_CURSOR_PROC) {
        memcpy(entry->d_name, "proc", 5);
        entry->d_type = DT_DIR;
        entry->d_size = 0;
        *cursor = ROOT_CURSOR_DONE;
        return VFS_OK;
    }

    int status = node->mount->ops->readdir(node->mount->volume, node, cursor, entry);
    if (status != VFS_END || !node->is_root)
        return status;

    memcpy(entry->d_name, "dev", 4);
    entry->d_type = DT_DIR;
    entry->d_size = 0;
    *cursor = ROOT_CURSOR_PROC;
    return VFS_OK;
}

// Mounts as lines of a device, where it is and its filesystem.
size_t vfs_list_mounts(char *out, size_t size) {
    size_t used = 0;

    for (int i = 0; i < mount_count; i++) {
        const char *parts[] = {"/dev/", mounts[i].device, " ", &mounts[i] == root_mount ? "/" : "/dev/",
                               &mounts[i] == root_mount ? "" : mounts[i].device, " ", mounts[i].ops->type, "\n"};

        for (size_t p = 0; p < sizeof(parts) / sizeof(parts[0]); p++) {
            size_t len = strlen(parts[p]);

            if (used + len >= size)
                return used;
            memcpy(out + used, parts[p], len);
            used += len;
        }
    }
    return used;
}

int vfs_refresh(struct vfs_node *node) {
    const struct vfs_ops *ops = node->mount->ops;

    return ops->refresh ? ops->refresh(node->mount->volume, node) : VFS_OK;
}

int64_t vfs_write(struct vfs_node *node, uint64_t offset, const void *buf, size_t len) {
    const struct vfs_ops *ops = node->mount->ops;

    if (!ops->write)
        return -EROFS;

    int64_t written = ops->write(node->mount->volume, node, offset, buf, len);

    elf_forget(node);
    return written;
}

int vfs_truncate(struct vfs_node *node, uint64_t size) {
    const struct vfs_ops *ops = node->mount->ops;

    if (!ops->truncate)
        return -EROFS;

    int status = ops->truncate(node->mount->volume, node, size);

    elf_forget(node);
    return status;
}

// Finds the directory a path would go in and the name it would have there.
static int split_path(const char *cwd, const char *path, struct vfs_node *parent, char name[PATH_MAX]) {
    char full[PATH_MAX];

    if (*path == '\0')
        return -ENOENT;
    if (!vfs_normalize(cwd, path, full))
        return -ENAMETOOLONG;

    // The root has no parent.
    char *slash = strrchr(full, '/');
    if (slash[1] == '\0')
        return -EBUSY;

    memcpy(name, slash + 1, strlen(slash + 1) + 1);
    *slash = '\0';

    int status = vfs_lookup("/", full[0] ? full : "/", parent);
    if (status != VFS_OK)
        return status;
    return parent->is_dir ? VFS_OK : -ENOTDIR;
}

int vfs_rename(const struct vfs_node *node, const char *cwd, const char *path) {
    struct vfs_node parent, existing;
    char name[PATH_MAX];

    if (!node->mount->ops->rename)
        return node->mount == &dev_mount ? -EBUSY : -EROFS;
    if (node->entry_lba == 0)
        return -EBUSY;

    int status = split_path(cwd, path, &parent, name);
    if (status != VFS_OK)
        return status;
    if (parent.mount != node->mount)
        return -EXDEV;

    // Only a file can replace a file, and renaming only the case of a name
    // finds the node itself here.
    status = vfs_lookup(cwd, path, &existing);
    if (status == VFS_OK && !vfs_same_file(&existing, node)) {
        if (existing.is_dir)
            return -EISDIR;
        if (node->is_dir)
            return -ENOTDIR;

        status = vfs_remove(&existing);
        if (status != VFS_OK)
            return status;
    } else if (status != VFS_OK && status != VFS_NOT_FOUND) {
        return status;
    }

    status = node->mount->ops->rename(node->mount->volume, node, &parent, name);
    elf_forget(node);
    return status;
}

int vfs_create(const char *cwd, const char *path, bool is_dir, struct vfs_node *node) {
    struct vfs_node parent;
    char name[PATH_MAX];

    int status = split_path(cwd, path, &parent, name);
    if (status != VFS_OK)
        return status;
    if (!parent.mount->ops->create)
        return parent.mount == &dev_mount ? -EPERM : -EROFS;

    status = parent.mount->ops->create(parent.mount->volume, &parent, name, is_dir, node);
    if (status == VFS_OK) {
        node->mount = parent.mount;
        node->is_root = false;
    }
    return status;
}

int vfs_remove(const struct vfs_node *node) {
    const struct vfs_ops *ops = node->mount->ops;

    if (!ops->remove)
        return node->mount == &dev_mount ? -EBUSY : -EROFS;
    if (node->entry_lba == 0)
        return -EBUSY;
    int status = ops->remove(node->mount->volume, node);

    elf_forget(node);
    return status;
}

void vfs_fill_stat(const struct vfs_node *node, struct stat *out) {
    *out = (struct stat){
        .st_dev = node->mount == &dev_mount ? MAX_MOUNTS + 1 : node->mount == &proc_mount ? MAX_MOUNTS + 2 : (uint64_t)(node->mount - mounts) + 1,
        .st_ino = (uint64_t)node->entry_lba << 16 | node->entry_pos,
        .st_mode = (node->is_dir ? S_IFDIR : vfs_device_of(node) != VFS_DEVICE_NONE ? S_IFCHR : S_IFREG) | 0777,
        .st_nlink = 1,
        .st_size = node->size,
        .st_blksize = SECTOR_SIZE,
        .st_blocks = (node->size + SECTOR_SIZE - 1) / SECTOR_SIZE,
        .st_atime = node->mtime,
        .st_mtime = node->mtime,
        .st_ctime = node->mtime,
    };
}

bool vfs_same_file(const struct vfs_node *a, const struct vfs_node *b) {
    return a->mount == b->mount && a->entry_lba == b->entry_lba && a->entry_pos == b->entry_pos;
}

bool vfs_statfs(const struct vfs_node *node, struct statfs *out) {
    const struct vfs_mount *mount = node->mount;

    if (!mount->ops->statfs || !mount->ops->statfs(mount->volume, out))
        return false;

    memcpy(out->f_type, mount->ops->type, strlen(mount->ops->type) + 1);
    memcpy(out->f_device, mount->device, strlen(mount->device) + 1);
    return true;
}
