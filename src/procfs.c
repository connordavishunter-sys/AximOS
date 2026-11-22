#include <acpi.h>
#include <block.h>
#include <clock.h>
#include <cpu.h>
#include <irq.h>
#include <mm.h>
#include <net.h>
#include <process.h>
#include <procfs.h>
#include <serial.h>
#include <string.h>

// A file is its kind, and for those in a process's directory that process.
#define PID_SHIFT 8
#define KIND_MASK 0xFF
#define NAME_CPUID_LEAF 0x80000002u

enum proc_kind {
    PROC_ROOT,
    PROC_BATTERY,
    PROC_CPUINFO,
    PROC_DISKS,
    PROC_LOG,
    PROC_MEMINFO,
    PROC_MOUNTS,
    PROC_NET,
    PROC_STAT,
    PROC_UPTIME,
    PROC_VERSION,
    PROC_FIRST_PROCESS_KIND,
    PROC_PID_DIR = PROC_FIRST_PROCESS_KIND,
    PROC_PID_CMDLINE,
    PROC_PID_STATUS,
};

static const char *const root_names[] = {
    [PROC_BATTERY] = "battery", [PROC_CPUINFO] = "cpuinfo", [PROC_DISKS] = "disks", [PROC_LOG] = "log", [PROC_MEMINFO] = "meminfo", [PROC_MOUNTS] = "mounts", [PROC_NET] = "net",
    [PROC_STAT] = "stat", [PROC_UPTIME] = "uptime", [PROC_VERSION] = "version",
};

static const char *const process_names[] = {
    [PROC_PID_CMDLINE - PROC_PID_CMDLINE] = "cmdline",
    [PROC_PID_STATUS - PROC_PID_CMDLINE] = "status",
};

// Output that stops quietly at the end of its page.
struct text {
    char *out;
    size_t used;
};

static void add(struct text *t, const char *s) {
    size_t len = strlen(s);

    if (len > PAGE_SIZE - t->used)
        len = PAGE_SIZE - t->used;
    memcpy(t->out + t->used, s, len);
    t->used += len;
}

static void add_number(struct text *t, uint64_t value) {
    char digits[21];
    int at = sizeof(digits) - 1;

    digits[at] = '\0';
    do {
        digits[--at] = '0' + value % 10;
        value /= 10;
    } while (value);
    add(t, &digits[at]);
}

static void add_field(struct text *t, const char *name, uint64_t value, const char *suffix) {
    add(t, name);
    add(t, ":\t");
    add_number(t, value);
    add(t, suffix);
}

static void add_address(struct text *t, const char *name, uint32_t address) {
    add(t, name);
    add(t, ":\t");
    add_number(t, address >> 24);
    add(t, ".");
    add_number(t, address >> 16 & 0xFF);
    add(t, ".");
    add_number(t, address >> 8 & 0xFF);
    add(t, ".");
    add_number(t, address & 0xFF);
    add(t, "\n");
}

static void make_cpuinfo(struct text *t) {
    uint32_t name[13] = {0};
    uint32_t top = 0x80000000u, b, c, d;

    __asm__ volatile("cpuid" : "+a"(top), "=b"(b), "=c"(c), "=d"(d));
    for (uint32_t i = 0; top >= NAME_CPUID_LEAF + 2 && i < 3; i++) {
        uint32_t a = NAME_CPUID_LEAF + i;

        __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "=c"(c), "=d"(d));
        name[i * 4] = a;
        name[i * 4 + 1] = b;
        name[i * 4 + 2] = c;
        name[i * 4 + 3] = d;
    }

    const char *model = (const char *)name;

    while (*model == ' ')
        model++;
    add(t, "model name:\t");
    add(t, *model ? model : "unknown");
    add(t, "\n");
    add_field(t, "cpus", cpu_count(), "\n");
}

static void make_meminfo(struct text *t) {
    add_field(t, "MemTotal", mm_total_page_count() * PAGE_SIZE / 1024, " kB\n");
    add_field(t, "MemFree", mm_free_page_count() * PAGE_SIZE / 1024, " kB\n");
    add_field(t, "Kernel", mm_kernel_page_count() * PAGE_SIZE / 1024, " kB\n");
}

static void make_net(struct text *t) {
    struct net_info info;

    net_get_info(&info);
    add_field(t, "device", info.has_device, "\n");
    add_address(t, "address", info.address);
    add_address(t, "netmask", info.netmask);
    add_address(t, "gateway", info.gateway);
    add_address(t, "dns", info.dns);
    add_field(t, "rx_packets", info.rx_packets, "\n");
    add_field(t, "tx_packets", info.tx_packets, "\n");
}

// Ticks say how long it's been, so idle against them is how busy each CPU was.
static void make_stat(struct text *t) {
    add_field(t, "ticks_per_second", TIMER_HZ, "\n");
    add_field(t, "ticks", irq_ticks(), "\n");
    for (int i = 0; i < cpu_count(); i++)
        add_field(t, "cpu_idle", cpu_idle_ticks(i), "\n");
}

static void make_uptime(struct text *t) {
    uint64_t ticks = irq_ticks();
    uint64_t hundredths = ticks * 100 / TIMER_HZ % 100;

    add_number(t, ticks / TIMER_HZ);
    add(t, hundredths < 10 ? ".0" : ".");
    add_number(t, hundredths);
    add(t, "\n");
}

static void make_status(struct text *t, const struct process_info *info) {
    const char *name = strrchr(info->exe, '/');

    add(t, "Name:\t");
    add(t, name ? name + 1 : info->exe);
    add(t, "\nState:\t");
    add(t, info->state == 'R' ? "R (running)" : info->state == 'Z' ? "Z (exited)" : "S (sleeping)");
    add_field(t, "\nPid", info->pid, "\n");
    add_field(t, "PPid", info->parent, "\n");
    add_field(t, "Pgid", info->group, "\n");
    add_field(t, "Threads", info->threads, "\n");
    add_field(t, "CpuTicks", info->cpu_ticks, "\n");
    add_field(t, "Memory", info->pages * PAGE_SIZE / 1024, " kB\n");
    add(t, "Exe:\t");
    add(t, info->exe);
    add(t, "\nCwd:\t");
    add(t, info->cwd);
    add(t, "\n");
}

// Each drive's name, sectors and their size, and whether it's a disc, read-only or
// being written to directly.
static void make_disks(struct text *t) {
    for (int i = 0; i < block_device_count; i++) {
        const struct block_device *d = block_devices[i];

        add(t, d->name);
        add(t, " ");
        add_number(t, d->sector_count);
        add(t, " ");
        add_number(t, d->sector_size);
        add(t, d->optical ? " disc" : " disk");
        add(t, d->write ? "" : " read-only");
        add(t, d->claimed ? " busy\n" : "\n");
    }
}

// As the firmware reports it, each time it's read.
static void make_battery(struct text *t) {
    struct acpi_battery b;

    if (!acpi_battery(&b)) {
        add(t, "no battery\n");
        return;
    }
    if (b.present && b.percent >= 0)
        add_field(t, "charge", b.percent, "%\n");
    add(t, "state:\t");
    add(t, !b.present ? "missing\n" : b.charging ? "charging\n" : b.discharging ? "discharging\n" : "not charging\n");
    add(t, "power:\t");
    add(t, b.on_ac < 0 ? "unknown\n" : b.on_ac ? "ac\n" : "battery\n");
}

static bool make_file(uint32_t kind, int pid, struct text *t) {
    struct process_info info;

    switch (kind) {
    case PROC_BATTERY:
        make_battery(t);
        return true;
    case PROC_CPUINFO:
        make_cpuinfo(t);
        return true;
    case PROC_DISKS:
        make_disks(t);
        return true;
    case PROC_MEMINFO:
        make_meminfo(t);
        return true;
    case PROC_MOUNTS:
        t->used = vfs_list_mounts(t->out, PAGE_SIZE);
        return true;
    case PROC_NET:
        make_net(t);
        return true;
    case PROC_STAT:
        make_stat(t);
        return true;
    case PROC_UPTIME:
        make_uptime(t);
        return true;
    case PROC_VERSION:
        add(t, "AximOS 0.1 x86_64\n");
        return true;
    }

    if (!process_get_info(pid, &info))
        return false;
    if (kind == PROC_PID_STATUS) {
        make_status(t, &info);
    } else {
        add(t, info.cmdline);
        add(t, "\n");
    }
    return true;
}

static void set_node(struct vfs_node *node, uint32_t kind, int pid, bool is_dir) {
    uint32_t location = (uint32_t)pid << PID_SHIFT | kind;

    // Nothing here has a directory entry, but this keeps files from looking the same.
    *node = (struct vfs_node){.location = location, .is_dir = is_dir, .mtime = clock_boot_epoch() + (int64_t)(irq_ticks() / TIMER_HZ), .entry_lba = 1, .entry_pos = location};
}

// Reads a whole number, returning false if that's not all there is before the end.
static bool parse_pid(const char *s, size_t len, int *out) {
    int value = 0;

    if (len == 0 || len > 9)
        return false;
    for (size_t i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        value = value * 10 + (s[i] - '0');
    }
    *out = value;
    return value > 0;
}

static int procfs_lookup(void *volume, const char *path, struct vfs_node *node) {
    (void)volume;

    if (*path == '\0') {
        set_node(node, PROC_ROOT, 0, true);
        return VFS_OK;
    }

    size_t first = 0;

    while (path[first] && path[first] != '/')
        first++;

    const char *slash = path[first] ? path + first : NULL;

    for (uint32_t kind = PROC_ROOT + 1; kind < PROC_FIRST_PROCESS_KIND; kind++) {
        if (strlen(root_names[kind]) == first && memcmp(root_names[kind], path, first) == 0) {
            if (slash)
                return -ENOTDIR;
            set_node(node, kind, 0, false);
            return VFS_OK;
        }
    }

    int pid;
    struct process_info info;

    if (first == 4 && memcmp(path, "self", 4) == 0)
        pid = process_pid();
    else if (!parse_pid(path, first, &pid))
        return VFS_NOT_FOUND;
    if (!process_get_info(pid, &info))
        return VFS_NOT_FOUND;

    if (!slash) {
        set_node(node, PROC_PID_DIR, pid, true);
        return VFS_OK;
    }
    for (uint32_t i = 0; i < sizeof(process_names) / sizeof(process_names[0]); i++) {
        if (strcmp(process_names[i], slash + 1) == 0) {
            set_node(node, PROC_PID_CMDLINE + i, pid, false);
            return VFS_OK;
        }
    }
    return VFS_NOT_FOUND;
}

// It's all made up again for each read, so a reader sees it as it is by then.
static int64_t procfs_read(void *volume, const struct vfs_node *node, uint64_t offset, void *buf, size_t len) {
    (void)volume;

    // The kernel's log is bigger than a page, so it's read as it is.
    if ((node->location & KIND_MASK) == PROC_LOG)
        return log_read(offset, buf, len);

    uint64_t phys = mm_alloc_kernel_page();

    if (!phys)
        return VFS_IO_ERROR;

    struct text text = {mm_phys_to_virt(phys), 0};
    int64_t result = VFS_NOT_FOUND;

    if (make_file(node->location & KIND_MASK, node->location >> PID_SHIFT, &text)) {
        result = offset >= text.used ? 0 : text.used - offset < len ? (int64_t)(text.used - offset) : (int64_t)len;
        if (result > 0)
            memcpy(buf, text.out + offset, result);
    }
    mm_free_kernel_page(phys);
    return result;
}

static void set_entry(struct dirent *entry, const char *name, uint8_t type) {
    memcpy(entry->d_name, name, strlen(name) + 1);
    entry->d_type = type;
    entry->d_size = 0;
}

static int procfs_readdir(void *volume, const struct vfs_node *node, uint64_t *cursor, struct dirent *entry) {
    (void)volume;

    if ((node->location & KIND_MASK) == PROC_PID_DIR) {
        if (*cursor >= sizeof(process_names) / sizeof(process_names[0]))
            return VFS_END;
        set_entry(entry, process_names[(*cursor)++], DT_REG);
        return VFS_OK;
    }

    // The fixed names, then processes, with the cursor going on from the last PID.
    if (*cursor + 1 < PROC_FIRST_PROCESS_KIND) {
        set_entry(entry, root_names[++*cursor], DT_REG);
        return VFS_OK;
    }

    int after = *cursor - (PROC_FIRST_PROCESS_KIND - 1);
    int pid = process_next_pid(after);

    if (pid == 0)
        return VFS_END;

    char name[12];
    int at = sizeof(name) - 1;

    name[at] = '\0';
    for (int value = pid; value; value /= 10)
        name[--at] = '0' + value % 10;
    set_entry(entry, &name[at], DT_DIR);
    *cursor = PROC_FIRST_PROCESS_KIND - 1 + pid;
    return VFS_OK;
}

const struct vfs_ops procfs_ops = {.type = "procfs", .lookup = procfs_lookup, .read = procfs_read, .readdir = procfs_readdir};
