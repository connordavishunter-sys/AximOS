#include <block.h>
#include <elf.h>
#include <fat32.h>
#include <iso9660.h>
#include <process.h>
#include <procfs.h>
#include <serial.h>
#include <string.h>
#include <tmpfs.h>
#include <vfs.h>

#define MAX_MOUNTS 32
#define SECTOR_SIZE 512
#define MBR_TABLE_OFFSET 446
#define MBR_ENTRIES 4
#define MBR_TYPE_FAT32_CHS 0x0B
#define MBR_TYPE_FAT32_LBA 0x0C
#define MBR_TYPE_GPT 0xEE
#define GPT_ENTRY_SIZE 128
#define MAX_PARTITIONS 99

#define ROOT_CURSOR_DONE UINT64_MAX
#define ROOT_CURSOR_PROC (UINT64_MAX - 1)

// An unmounted one keeps its slot, so a node left pointing at it fails rather
// than finding whatever was mounted next, and loses its name.
struct vfs_mount {
    char device[VFS_DEVICE_NAME_MAX];
    const struct vfs_ops *ops;
    void *volume;
    struct block_device *drive;
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
    [VFS_DEVICE_PTMX] = "ptmx",
    [VFS_DEVICE_RANDOM] = "random",
    [VFS_DEVICE_URANDOM] = "urandom",
    [VFS_DEVICE_AUDIO] = "audio",
    [VFS_DEVICE_MIXER] = "mixer",
};
static bool lock_held;
static int lock_owner;

// /dev itself, which lists the mounts.
static int dev_readdir(void *volume, const struct vfs_node *node, uint64_t *cursor, struct dirent *entry) {
    (void)volume;
    (void)node;

    // The cursor counts the mounts first, then the possible devices, then disk.
    while (*cursor < (uint64_t)mount_count && !mounts[*cursor].device[0])
        ++*cursor;
    while (*cursor >= (uint64_t)mount_count) {
        uint64_t device = (*cursor)++ - mount_count;

        if (device > VFS_DEVICE_COUNT)
            return VFS_END;
        if (device == VFS_DEVICE_COUNT || device_present[device]) {
            const char *name = device == VFS_DEVICE_COUNT ? "disk" : device_names[device];

            memcpy(entry->d_name, name, strlen(name) + 1);
            entry->d_type = device == VFS_DEVICE_COUNT ? DT_DIR : DT_CHR;
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

// /dev/disk, which has each drive as it is, sector by sector.
static int disk_readdir(void *volume, const struct vfs_node *node, uint64_t *cursor, struct dirent *entry) {
    (void)volume;
    (void)node;

    if (*cursor >= (uint64_t)block_device_count)
        return VFS_END;

    const char *name = block_devices[(*cursor)++]->name;

    memcpy(entry->d_name, name, strlen(name) + 1);
    entry->d_type = DT_BLK;
    entry->d_size = 0;
    return VFS_OK;
}

static int gone_lookup(void *volume, const char *path, struct vfs_node *node) {
    (void)volume, (void)path, (void)node;
    return -ENODEV;
}

static int64_t gone_read(void *volume, const struct vfs_node *node, uint64_t offset, void *buf, size_t len) {
    (void)volume, (void)node, (void)offset, (void)buf, (void)len;
    return VFS_IO_ERROR;
}

static int gone_readdir(void *volume, const struct vfs_node *node, uint64_t *cursor, struct dirent *entry) {
    (void)volume, (void)node, (void)cursor, (void)entry;
    return VFS_END;
}

static bool gone_statfs(void *volume, struct statfs *out) {
    (void)volume, (void)out;
    return false;
}

static const struct vfs_ops gone_ops = {
    .type = "gone", .lookup = gone_lookup, .read = gone_read, .readdir = gone_readdir, .statfs = gone_statfs};

static struct vfs_mount proc_mount = {.device = "proc", .ops = &procfs_ops};
static struct vfs_mount tmp_mount = {.device = "tmpfs", .ops = &tmpfs_ops};

// Whether a path is a directory or under it: /proc for /proc/1 but not /process.
static bool under(const char *path, const char *dir, size_t len) {
    return memcmp(path, dir, len) == 0 && (path[len] == '/' || path[len] == '\0');
}

static const struct vfs_ops dev_ops = {.type = "devfs", .readdir = dev_readdir};
static struct vfs_mount dev_mount = {.device = "devfs", .ops = &dev_ops};
static const struct vfs_ops disk_ops = {.type = "devfs", .readdir = disk_readdir};
static struct vfs_mount disk_mount = {.device = "disk", .ops = &disk_ops};

static void add_mount(struct block_device *drive, const char *device, const struct vfs_ops *ops, void *volume) {
    if (mount_count == MAX_MOUNTS) {
        kprintf("vfs: too many filesystems, ignoring %s\n", device);
        return;
    }

    struct vfs_mount *mount = &mounts[mount_count++];
    memcpy(mount->device, device, strlen(device) + 1);
    mount->ops = ops;
    mount->volume = volume;
    mount->drive = drive;
    kprintf("vfs: mounted %s filesystem at /dev/%s\n", ops->type, device);
}

// Partitions are named after their drive and number, as sda1.
static void mount_partition(struct block_device *drive, int number, uint32_t lba) {
    size_t len = strlen(drive->name);
    char device[BLOCK_NAME_MAX + 3];
    void *volume;

    memcpy(device, drive->name, len);
    if (number >= 10)
        device[len++] = '0' + number / 10;
    device[len] = '0' + number % 10;
    device[len + 1] = '\0';

    if ((volume = fat32_mount(drive, lba)))
        add_mount(drive, device, &fat32_ops, volume);
    else
        kprintf("vfs: nothing to mount on /dev/%s\n", device);
}

// A GPT's header is in the second sector, and says where its entries are.
static void mount_gpt(struct block_device *drive, uint8_t *sector) {
    uint64_t entries_lba;
    uint32_t count, size;

    if (!block_read(drive, 1, sector) || memcmp(sector, "EFI PART", 8) != 0)
        return;
    memcpy(&entries_lba, &sector[72], sizeof(entries_lba));
    memcpy(&count, &sector[80], sizeof(count));
    memcpy(&size, &sector[84], sizeof(size));
    if (size < GPT_ENTRY_SIZE || SECTOR_SIZE % size)
        return;

    for (uint32_t i = 0; i < count && i < MAX_PARTITIONS; i++) {
        static const uint8_t unused[16];
        uint64_t first;

        if (!block_read(drive, entries_lba + i * size / SECTOR_SIZE, sector))
            return;

        const uint8_t *entry = &sector[i * size % SECTOR_SIZE];

        memcpy(&first, &entry[32], sizeof(first));
        if (memcmp(entry, unused, sizeof(unused)) != 0 && first < UINT32_MAX)
            mount_partition(drive, i + 1, first);
    }
}

static void mount_disk(struct block_device *drive) {
    void *volume = fat32_mount(drive, 0);
    if (volume) {
        add_mount(drive, drive->name, &fat32_ops, volume);
        return;
    }

    uint8_t sector[SECTOR_SIZE] __attribute__((aligned(2)));
    if (!block_read(drive, 0, sector) || sector[510] != 0x55 || sector[511] != 0xAA) {
        kprintf("vfs: nothing to mount on /dev/%s\n", drive->name);
        return;
    }

    struct mbr_entry entries[MBR_ENTRIES];
    memcpy(entries, &sector[MBR_TABLE_OFFSET], sizeof(entries));

    if (entries[0].type == MBR_TYPE_GPT) {
        mount_gpt(drive, sector);
        return;
    }
    for (int i = 0; i < MBR_ENTRIES; i++) {
        if (entries[i].type == MBR_TYPE_FAT32_CHS || entries[i].type == MBR_TYPE_FAT32_LBA)
            mount_partition(drive, i + 1, entries[i].lba);
    }
}

static void mount_disc(struct block_device *drive) {
    void *volume = iso9660_mount(drive);
    if (volume)
        add_mount(drive, drive->name, &iso9660_ops, volume);
    else
        kprintf("vfs: nothing to mount on /dev/%s\n", drive->name);
}

void vfs_mount_drive(struct block_device *drive) {
    if (drive->optical)
        mount_disc(drive);
    else
        mount_disk(drive);
}

void vfs_unmount_drive(struct block_device *drive) {
    for (int i = 0; i < mount_count; i++) {
        if (mounts[i].drive == drive && mounts[i].ops != &gone_ops) {
            kprintf("vfs: unmounted /dev/%s\n", mounts[i].device);
            mounts[i].ops = &gone_ops;
            mounts[i].device[0] = '\0';
        }
    }
    fat32_forget(drive);
}

bool vfs_drive_has_root(const struct block_device *drive) {
    return root_mount && root_mount->drive == drive;
}

struct block_device *vfs_node_drive(const struct vfs_node *node) {
    return node->mount ? node->mount->drive : NULL;
}

struct block_device *vfs_raw_drive(const struct vfs_node *node) {
    return node->mount == &disk_mount && !node->is_dir ? block_devices[node->location] : NULL;
}

bool vfs_mount_all(void) {
    for (int i = 0; i < block_device_count; i++)
        vfs_mount_drive(block_devices[i]);
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

// One that can be written to comes first, so an installed system is used rather
// than the disc it was installed from.
bool vfs_choose_root(const char *path) {
    for (int i = 0; i < 2 * mount_count; i++) {
        struct vfs_mount *mount = &mounts[i % mount_count];
        struct vfs_node node;

        if (i < mount_count && !mount->ops->write)
            continue;
        if (mount->ops->lookup(mount->volume, path + 1, &node) == VFS_OK) {
            root_mount = mount;
            kprintf("vfs: /dev/%s is the root filesystem\n", mount->device);
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

    if (under(full, "/proc", 5) || under(full, "/tmp", 4)) {
        size_t len = full[1] == 'p' ? 5 : 4;

        mount = full[1] == 'p' ? &proc_mount : &tmp_mount;
        rest = full[len] ? full + len + 1 : full + len;
    } else if (under(full, "/dev", 4)) {
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

        if (rest - device == 4 && memcmp(device, "disk", 4) == 0) {
            if (*rest == '\0' || strcmp(rest, "/") == 0) {
                *node = (struct vfs_node){.mount = &disk_mount, .is_dir = true};
                return VFS_OK;
            }
            for (int i = 0; i < block_device_count; i++) {
                if (strcmp(rest + 1, block_devices[i]->name) == 0) {
                    uint64_t bytes = (uint64_t)block_devices[i]->sector_count * block_devices[i]->sector_size;

                    *node = (struct vfs_node){.mount = &disk_mount, .location = i,
                                              .size = bytes < UINT32_MAX ? bytes : UINT32_MAX};
                    return VFS_OK;
                }
            }
            return VFS_NOT_FOUND;
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
        if (!mounts[i].device[0])
            continue;

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

    const char *tmp = "tmpfs /tmp tmpfs\n";
    size_t len = strlen(tmp);

    if (used + len < size) {
        memcpy(out + used, tmp, len);
        used += len;
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
        .st_dev = node->mount == &dev_mount ? MAX_MOUNTS + 1 : node->mount == &proc_mount ? MAX_MOUNTS + 2
                : node->mount == &tmp_mount ? MAX_MOUNTS + 3 : (uint64_t)(node->mount - mounts) + 1,
        .st_ino = (uint64_t)node->entry_lba << 16 | node->entry_pos,
        .st_mode = (node->is_dir                            ? S_IFDIR
                    : vfs_raw_drive(node)                   ? S_IFBLK
                    : vfs_device_of(node) != VFS_DEVICE_NONE ? S_IFCHR
                                                            : S_IFREG) | 0777,
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
