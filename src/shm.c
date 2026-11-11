#include <errno_values.h>
#include <kmalloc.h>
#include <mm.h>
#include <shm.h>
#include <string.h>
#include <syscall_numbers.h>

// Pages are recorded in a two-level table, so the object doesn't need a run of
// contiguous memory. Mappings hold their own references to the pages themselves,
// which is what lets them outlive the object.

#define NAME_MAX_LEN 64
#define ENTRIES_PER_PAGE (PAGE_SIZE / sizeof(uint64_t))
#define MAX_PAGES (ENTRIES_PER_PAGE * ENTRIES_PER_PAGE)

struct shm {
    struct shm *next;
    char name[NAME_MAX_LEN];
    int refs;
    bool linked;
    uint64_t size;
    uint64_t page_count;
    uint64_t directory; // physical page of physical pages of physical pages
};

static struct shm *objects;

static struct shm *find(const char *name) {
    for (struct shm *s = objects; s; s = s->next) {
        if (strcmp(s->name, name) == 0)
            return s;
    }
    return NULL;
}

static void remove_from_list(struct shm *shm) {
    struct shm **link = &objects;

    while (*link != shm)
        link = &(*link)->next;
    *link = shm->next;
    shm->linked = false;
}

static uint64_t *page_slot(const struct shm *shm, uint64_t index) {
    uint64_t *directory = mm_phys_to_virt(shm->directory);
    uint64_t *table = mm_phys_to_virt(directory[index / ENTRIES_PER_PAGE]);

    return &table[index % ENTRIES_PER_PAGE];
}

// Drops pages from the end, down to count.
static void shrink(struct shm *shm, uint64_t count) {
    while (shm->page_count > count) {
        uint64_t index = --shm->page_count;

        mm_free_page(*page_slot(shm, index));

        if (index % ENTRIES_PER_PAGE == 0) {
            uint64_t *directory = mm_phys_to_virt(shm->directory);

            mm_free_kernel_page(directory[index / ENTRIES_PER_PAGE]);
            directory[index / ENTRIES_PER_PAGE] = 0;
        }
    }
}

static bool grow(struct shm *shm, uint64_t count) {
    if (shm->directory == 0) {
        shm->directory = mm_alloc_kernel_page();
        if (shm->directory == 0)
            return false;
    }

    uint64_t *directory = mm_phys_to_virt(shm->directory);

    while (shm->page_count < count) {
        uint64_t index = shm->page_count;

        if (directory[index / ENTRIES_PER_PAGE] == 0) {
            directory[index / ENTRIES_PER_PAGE] = mm_alloc_kernel_page();
            if (directory[index / ENTRIES_PER_PAGE] == 0)
                return false;
        }

        uint64_t page = mm_alloc_page();
        if (page == 0)
            return false;

        *page_slot(shm, index) = page;
        shm->page_count++;
    }
    return true;
}

int shm_open(const char *name, int flags, struct shm **out) {
    size_t len = strlen(name);

    if (len == 0 || len >= NAME_MAX_LEN)
        return len ? -ENAMETOOLONG : -EINVAL;

    struct shm *shm = find(name);

    if (shm && (flags & O_CREAT) && (flags & O_EXCL))
        return -EEXIST;

    if (!shm) {
        if (!(flags & O_CREAT))
            return -ENOENT;

        shm = kmalloc(sizeof(*shm));
        if (!shm)
            return -ENOMEM;

        memcpy(shm->name, name, len + 1);
        shm->linked = true;
        shm->next = objects;
        objects = shm;
    }

    if (flags & O_TRUNC) {
        shrink(shm, 0);
        shm->size = 0;
    }

    shm->refs++;
    *out = shm;
    return 0;
}

struct shm *shm_ref(struct shm *shm) {
    shm->refs++;
    return shm;
}

static void destroy(struct shm *shm) {
    shrink(shm, 0);
    if (shm->directory)
        mm_free_kernel_page(shm->directory);
    kfree(shm);
}

void shm_unref(struct shm *shm) {
    if (--shm->refs == 0 && !shm->linked)
        destroy(shm);
}

int shm_unlink(const char *name) {
    struct shm *shm = find(name);

    if (!shm)
        return -ENOENT;

    remove_from_list(shm);
    if (shm->refs == 0)
        destroy(shm);
    return 0;
}

int shm_set_size(struct shm *shm, uint64_t size) {
    if (size > MAX_PAGES * PAGE_SIZE)
        return -EFBIG;

    uint64_t count = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    if (!grow(shm, count)) {
        shrink(shm, (shm->size + PAGE_SIZE - 1) / PAGE_SIZE);
        return -ENOMEM;
    }

    shrink(shm, count);
    shm->size = size;
    return 0;
}

uint64_t shm_size(const struct shm *shm) {
    return shm->size;
}

uint64_t shm_page(const struct shm *shm, uint64_t index) {
    return *page_slot(shm, index);
}
