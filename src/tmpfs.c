#include <clock.h>
#include <mm.h>
#include <string.h>
#include <tmpfs.h>

#define MAX_FILES 1024
#define PER_PAGE (PAGE_SIZE / sizeof(uint64_t))

// A file or directory, numbered by its place in files, the root being 0. A file's
// pages are found through an index page of up to 512 more index pages, each
// listing up to 512 pages of data, which allows a file of 1 GB.
struct file {
    bool used, is_dir;
    uint32_t parent, size;
    int64_t mtime;
    uint64_t index; // physical address of the top index page, or 0
    char name[NAME_MAX + 1];
};

static struct file files[MAX_FILES] = {[0] = {.used = true, .is_dir = true}};
static uint64_t pages_used;

// Files may use up to a quarter of memory between them.
static bool can_use_page(void) {
    return pages_used < mm_total_page_count() / 4;
}

static uint64_t new_page(void) {
    uint64_t phys = can_use_page() ? mm_alloc_kernel_page() : 0;

    pages_used += phys != 0;
    return phys;
}

static void free_page(uint64_t phys) {
    mm_free_kernel_page(phys);
    pages_used--;
}

// The slot in an index page for a page number, making the index pages on the way
// if make is set. NULL if there's none.
static uint64_t *slot_for(struct file *f, uint64_t page, bool make) {
    uint64_t *top, *middle;

    if (page >= PER_PAGE * PER_PAGE || (!f->index && (!make || !(f->index = new_page()))))
        return NULL;
    top = mm_phys_to_virt(f->index);
    if (!top[page / PER_PAGE] && (!make || !(top[page / PER_PAGE] = new_page())))
        return NULL;
    middle = mm_phys_to_virt(top[page / PER_PAGE]);
    return &middle[page % PER_PAGE];
}

// Frees the pages from a page number on, and the index pages left empty.
static void free_from(struct file *f, uint64_t first) {
    uint64_t *top;

    if (!f->index)
        return;
    top = mm_phys_to_virt(f->index);
    for (uint64_t i = 0; i < PER_PAGE; i++) {
        uint64_t *middle;

        if (!top[i] || (i + 1) * PER_PAGE <= first)
            continue;
        middle = mm_phys_to_virt(top[i]);
        for (uint64_t j = 0; j < PER_PAGE; j++) {
            if (middle[j] && i * PER_PAGE + j >= first) {
                free_page(middle[j]);
                middle[j] = 0;
            }
        }
        if (i * PER_PAGE >= first) {
            free_page(top[i]);
            top[i] = 0;
        }
    }
    if (first == 0) {
        free_page(f->index);
        f->index = 0;
    }
}

static void fill_node(uint32_t number, struct vfs_node *node) {
    const struct file *f = &files[number];

    // The VFS's own fields stay. A nonzero entry_lba tells it the node can be
    // renamed and removed, which the root can't, and with entry_pos names it.
    *node = (struct vfs_node){.mount = node->mount, .is_root = node->is_root, .location = number, .size = f->size,
                              .mtime = f->mtime, .is_dir = f->is_dir, .entry_lba = number};
}

static int find_child(uint32_t parent, const char *name, size_t len) {
    for (uint32_t i = 1; i < MAX_FILES; i++) {
        if (files[i].used && files[i].parent == parent && strlen(files[i].name) == len && !memcmp(files[i].name, name, len))
            return (int)i;
    }
    return -1;
}

static int tmpfs_lookup(void *volume, const char *path, struct vfs_node *node) {
    uint32_t at = 0;

    (void)volume;
    while (*path) {
        const char *end = path;
        int child;

        while (*end && *end != '/')
            end++;
        if (!files[at].is_dir)
            return -ENOTDIR;
        if ((child = find_child(at, path, end - path)) < 0)
            return VFS_NOT_FOUND;
        at = child;
        path = *end ? end + 1 : end;
    }
    *node = (struct vfs_node){0};
    fill_node(at, node);
    return VFS_OK;
}

static int64_t tmpfs_read(void *volume, const struct vfs_node *node, uint64_t offset, void *buf, size_t len) {
    struct file *f = &files[node->location];
    size_t done = 0;

    (void)volume;
    if (offset >= f->size)
        return 0;
    if (len > f->size - offset)
        len = f->size - offset;
    while (done < len) {
        uint64_t at = offset + done, *slot = slot_for(f, at / PAGE_SIZE, false);
        size_t in_page = PAGE_SIZE - at % PAGE_SIZE, n = len - done < in_page ? len - done : in_page;

        // A page never written is all zeroes.
        if (slot && *slot)
            memcpy((uint8_t *)buf + done, (uint8_t *)mm_phys_to_virt(*slot) + at % PAGE_SIZE, n);
        else
            memset((uint8_t *)buf + done, 0, n);
        done += n;
    }
    return (int64_t)len;
}

static int tmpfs_readdir(void *volume, const struct vfs_node *node, uint64_t *cursor, struct dirent *entry) {
    (void)volume;
    while (++*cursor < MAX_FILES) {
        const struct file *f = &files[*cursor];

        if (f->used && f->parent == node->location) {
            memcpy(entry->d_name, f->name, strlen(f->name) + 1);
            entry->d_type = f->is_dir ? DT_DIR : DT_REG;
            entry->d_size = f->size;
            return VFS_OK;
        }
    }
    return VFS_END;
}

static bool tmpfs_statfs(void *volume, struct statfs *out) {
    (void)volume;
    out->f_bsize = PAGE_SIZE;
    out->f_blocks = mm_total_page_count() / 4;
    out->f_bfree = out->f_blocks > pages_used ? out->f_blocks - pages_used : 0;
    return true;
}

static int tmpfs_refresh(void *volume, struct vfs_node *node) {
    (void)volume;
    if (!files[node->location].used)
        return VFS_NOT_FOUND;
    fill_node(node->location, node);
    return VFS_OK;
}

static int64_t tmpfs_write(void *volume, struct vfs_node *node, uint64_t offset, const void *buf, size_t len) {
    struct file *f = &files[node->location];
    size_t done = 0;

    (void)volume;
    if (offset + len > UINT32_MAX)
        return -EFBIG;
    while (done < len) {
        uint64_t at = offset + done, *slot = slot_for(f, at / PAGE_SIZE, true);
        size_t in_page = PAGE_SIZE - at % PAGE_SIZE, n = len - done < in_page ? len - done : in_page;

        if (!slot || (!*slot && !(*slot = new_page())))
            break;
        memcpy((uint8_t *)mm_phys_to_virt(*slot) + at % PAGE_SIZE, (const uint8_t *)buf + done, n);
        done += n;
    }
    if (offset + done > f->size)
        f->size = offset + done;
    f->mtime = clock_now();
    fill_node(node->location, node);
    return done || !len ? (int64_t)done : -ENOSPC;
}

static int tmpfs_truncate(void *volume, struct vfs_node *node, uint64_t size) {
    struct file *f = &files[node->location];

    (void)volume;
    if (size > UINT32_MAX)
        return -EFBIG;
    if (size < f->size) {
        uint64_t *slot = slot_for(f, size / PAGE_SIZE, false);

        // What's left of the last page is cleared, as growing again reads it.
        if (size % PAGE_SIZE && slot && *slot)
            memset((uint8_t *)mm_phys_to_virt(*slot) + size % PAGE_SIZE, 0, PAGE_SIZE - size % PAGE_SIZE);
        free_from(f, (size + PAGE_SIZE - 1) / PAGE_SIZE);
    }
    f->size = size;
    f->mtime = clock_now();
    fill_node(node->location, node);
    return VFS_OK;
}

static int tmpfs_create(void *volume, const struct vfs_node *parent, const char *name, bool is_dir, struct vfs_node *node) {
    size_t len = strlen(name);

    (void)volume;
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    if (find_child(parent->location, name, len) >= 0)
        return -EEXIST;
    for (uint32_t i = 1; i < MAX_FILES; i++) {
        if (!files[i].used) {
            files[i] = (struct file){.used = true, .is_dir = is_dir, .parent = parent->location, .mtime = clock_now()};
            memcpy(files[i].name, name, len + 1);
            *node = (struct vfs_node){0};
            fill_node(i, node);
            return VFS_OK;
        }
    }
    return -ENOSPC;
}

static int tmpfs_remove(void *volume, const struct vfs_node *node) {
    struct file *f = &files[node->location];

    (void)volume;
    for (uint32_t i = 1; f->is_dir && i < MAX_FILES; i++) {
        if (files[i].used && files[i].parent == node->location)
            return -ENOTEMPTY;
    }
    free_from(f, 0);
    f->used = false;
    return VFS_OK;
}

static int tmpfs_rename(void *volume, const struct vfs_node *node, const struct vfs_node *parent, const char *name) {
    struct file *f = &files[node->location];
    size_t len = strlen(name);

    (void)volume;
    if (len > NAME_MAX)
        return -ENAMETOOLONG;
    // A directory can't go inside itself.
    for (uint32_t at = parent->location; at; at = files[at].parent) {
        if (at == node->location)
            return -EINVAL;
    }
    f->parent = parent->location;
    memcpy(f->name, name, len + 1);
    return VFS_OK;
}

const struct vfs_ops tmpfs_ops = {
    .type = "tmpfs",
    .lookup = tmpfs_lookup,
    .read = tmpfs_read,
    .readdir = tmpfs_readdir,
    .statfs = tmpfs_statfs,
    .refresh = tmpfs_refresh,
    .write = tmpfs_write,
    .truncate = tmpfs_truncate,
    .create = tmpfs_create,
    .remove = tmpfs_remove,
    .rename = tmpfs_rename,
};
