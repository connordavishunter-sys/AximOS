#include <aximos.h>
#include <blkid/blkid.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

// Installs the system it's running from, the live one in memory, onto a drive: an
// EFI system partition of its own, with Limine, the kernel and the microcode, and
// the system on ext4, with a swap file. It goes in the drive's largest free space,
// or over the whole drive, or over an AximOS that's there, keeping its /home and
// accounts. Limine offers the systems the firmware has entries for too, and the
// firmware gets an entry for AximOS, first in its order.

#define ESP_MIB 256
#define MIN_MIB 4096         // for the system and some room
#define SWAP_MIB (16 * 1024) // the laptop's RAM, though never more than a quarter of the drive's space
#define SYSTEM "/run/install/system"
#define ESP "/run/install/esp"
#define MEDIUM "/run/install/medium" // the stick it's running from, for what's on it besides the live system
#define MEDIUM_LABEL "AXIMOS_LIVE"
#define DEV_ARCHIVE MEDIUM "/aximos-dev.tar.zst"
#define PAYLOAD "/system/boot"        // what goes on the ESP, which the build puts there
#define ACCOUNTS "/system/share/install" // the account files as they were built, without the live ones
#define MAX_DRIVES 16
#define MAX_PARTS 32
#define SECTOR 512
#define ALIGN 2048

struct span {
    uint64_t start, end;
};

struct drive {
    char name[32], aximos[40], esp[40]; // an AximOS's system partition and ESP, if it has one
    uint64_t sectors, free; // the largest free stretch
    bool gpt;
};

static bool yes, no_dev;
static uint64_t total, done;
static int shown = -1;

static void say(const char *text) {
    printf("aximos-install: %s\n", text);
    fflush(stdout);
}

static bool failed(const char *what) {
    printf("aximos-install: %s: %s\n", what, strerror(errno));
    return false;
}

static uint64_t sys_number(const char *fmt, const char *name) {
    char path[128];
    unsigned long long n = 0;

    snprintf(path, sizeof(path), fmt, name);

    FILE *f = fopen(path, "r");

    if (f) {
        if (fscanf(f, "%llu", &n) != 1)
            n = 0;
        fclose(f);
    }
    return n;
}

// What udev found on a device, which anyone can read, though only root can read the
// device: lines like "E:ID_FS_TYPE=ext4" in /run/udev/data/b259:1.
static bool udev_found(const char *name, const char *key, char *out, size_t size) {
    static const char *const keys[][2] = {{"TYPE", "ID_FS_TYPE="}, {"LABEL", "ID_FS_LABEL="},
                                          {"PTTYPE", "ID_PART_TABLE_TYPE="}, {"PART_ENTRY_UUID", "ID_PART_ENTRY_UUID="}};
    char path[128], line[300];
    unsigned major_n, minor_n;
    bool found = false;

    snprintf(path, sizeof(path), "/sys/class/block/%s/dev", name);

    FILE *f = fopen(path, "r");

    if (!f || fscanf(f, "%u:%u", &major_n, &minor_n) != 2) {
        if (f)
            fclose(f);
        return false;
    }
    fclose(f);
    snprintf(path, sizeof(path), "/run/udev/data/b%u:%u", major_n, minor_n);
    for (size_t k = 0; k < sizeof(keys) / sizeof(keys[0]); k++) {
        size_t len = strlen(keys[k][1]);

        for (f = strcmp(key, keys[k][0]) ? NULL : fopen(path, "r"); f && !found && fgets(line, sizeof(line), f);) {
            if (!strncmp(line, "E:", 2) && !strncmp(line + 2, keys[k][1], len)) {
                snprintf(out, size, "%.*s", (int)strcspn(line + 2 + len, "\n"), line + 2 + len);
                found = true;
            }
        }
        if (f)
            fclose(f);
    }
    return found;
}

// A value blkid finds on a device, like its TYPE, LABEL or PART_ENTRY_UUID, or
// without root, udev did.
static bool probe(const char *name, const char *key, char *out, size_t size) {
    char path[64];
    const char *value;
    bool found = false;

    snprintf(path, sizeof(path), "/dev/%s", name);

    blkid_probe p = blkid_new_probe_from_filename(path);

    if (!p)
        return udev_found(name, key, out, size);
    blkid_probe_enable_partitions(p, 1);
    blkid_probe_set_partitions_flags(p, BLKID_PARTS_ENTRY_DETAILS);
    if (blkid_do_safeprobe(p) == 0 && blkid_probe_lookup_value(p, key, &value, NULL) == 0) {
        snprintf(out, size, "%s", value);
        found = true;
    }
    blkid_free_probe(p);
    return found;
}

static bool is(const char *name, const char *key, const char *value) {
    char found[64];

    return probe(name, key, found, sizeof(found)) && !strcasecmp(found, value);
}

static int by_start(const void *a, const void *b) {
    const struct span *x = a, *y = b;

    return (x->start > y->start) - (x->start < y->start);
}

// Drives it could go on: not those it's running from, the stick or disc, or one in use
// by a system on it, unless that's AximOS, which is replaced.
static int drives(struct drive *out) {
    DIR *dir = opendir("/sys/block");
    struct dirent *e;
    int n = 0;

    while (dir && n < MAX_DRIVES && (e = readdir(dir))) {
        struct drive *d = &out[n];
        struct span used[MAX_PARTS];
        int parts = 0;
        char path[300];

        if (e->d_name[0] == '.' || !strncmp(e->d_name, "loop", 4) || !strncmp(e->d_name, "ram", 3)
            || !strncmp(e->d_name, "zram", 4) || !strncmp(e->d_name, "sr", 2) || sys_number("/sys/block/%s/ro", e->d_name)
            || is(e->d_name, "TYPE", "iso9660"))
            continue;
        *d = (struct drive){.sectors = sys_number("/sys/block/%s/size", e->d_name)};
        snprintf(d->name, sizeof(d->name), "%s", e->d_name);
        d->gpt = is(d->name, "PTTYPE", "gpt");

        snprintf(path, sizeof(path), "/sys/block/%s", d->name);

        DIR *sub = opendir(path);
        struct dirent *p;

        while (sub && parts < MAX_PARTS && (p = readdir(sub))) {
            char file[400];

            snprintf(file, sizeof(file), "/sys/block/%s/%s/partition", d->name, p->d_name);
            if (p->d_name[0] == '.' || access(file, F_OK))
                continue;
            snprintf(file, sizeof(file), "/sys/block/%s/%s/start", d->name, p->d_name);
            used[parts].start = sys_number("%s", file);
            snprintf(file, sizeof(file), "/sys/block/%s/%s/size", d->name, p->d_name);
            used[parts].end = used[parts].start + sys_number("%s", file);
            if (is(p->d_name, "LABEL", "AximOS") && is(p->d_name, "TYPE", "ext4"))
                snprintf(d->aximos, sizeof(d->aximos), "%s", p->d_name);
            if (is(p->d_name, "LABEL", "AXIMOS") && is(p->d_name, "TYPE", "vfat"))
                snprintf(d->esp, sizeof(d->esp), "%s", p->d_name);
            parts++;
        }
        if (sub)
            closedir(sub);

        // The largest stretch between partitions, from the first whole MiB to the
        // backup table at the end, as disk add finds them.
        qsort(used, parts, sizeof(used[0]), by_start);
        for (int i = 0, at = 0; d->gpt && i <= parts; i++) {
            uint64_t from = at ? (used[at - 1].end + ALIGN - 1) / ALIGN * ALIGN : ALIGN;
            uint64_t until = i < parts ? used[i].start : d->sectors - 33;

            if (until > from && until - from > d->free)
                d->free = until - from;
            if (i < parts && (!at || used[i].end > used[at - 1].end))
                at = i + 1;
        }
        n++;
    }
    if (dir)
        closedir(dir);
    return n;
}

static void show_mib(uint64_t sectors) {
    print_size(sectors * SECTOR, 0);
}

static int list(void) {
    struct drive d[MAX_DRIVES];
    int n = drives(d);

    for (int i = 0; i < n; i++) {
        printf("%s\t", d[i].name);
        show_mib(d[i].sectors);
        printf("\t");
        show_mib(d[i].free);
        printf("\t%s%s%s\n", d[i].aximos[0] ? "again " : "", d[i].gpt && d[i].free >= MIN_MIB * 2048ULL ? "free " : "",
               d[i].sectors >= MIN_MIB * 2048ULL ? "whole" : "");
    }
    return 0;
}

// Runs a program, giving the first line it prints that starts with want, if out.
static bool run(char *const argv[], const char *want, char *out, size_t size) {
    int pipes[2];
    pid_t pid;
    int status;

    if (pipe(pipes) < 0 || (pid = fork()) < 0)
        return failed(argv[0]);
    if (pid == 0) {
        dup2(pipes[1], 1);
        dup2(pipes[1], 2);
        close(pipes[0]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(pipes[1]);

    FILE *f = fdopen(pipes[0], "r");
    char line[512];

    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (out && want && !strncmp(line, want, strlen(want)) && !out[0])
            snprintf(out, size, "%s", line + strlen(want));
        else if (!want || strncmp(line, want, strlen(want)))
            printf("  %s\n", line);
    }
    fclose(f);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status)) {
        printf("aximos-install: %s didn't work\n", argv[0]);
        return false;
    }
    return true;
}

// Adds a partition with disk add, giving its name.
static bool add(const char *drive, const char *size, const char *type, char *name) {
    char added[64] = "";

    if (!run((char *const[]){"disk", "add", (char *)drive, (char *)size, (char *)type, NULL}, "disk: added ", added,
             sizeof(added)))
        return false;
    snprintf(name, 40, "%.*s", (int)strcspn(added, ","), added);
    return name[0];
}

static bool format(const char *partition, const char *type, const char *label) {
    return run((char *const[]){"disk", "format", (char *)partition, (char *)type, (char *)label, "-y", NULL}, NULL, NULL,
               0);
}

static bool mount_at(const char *partition, const char *at, const char *type) {
    char path[64];

    snprintf(path, sizeof(path), "/dev/%s", partition);
    mkdir("/run/install", 0755);
    mkdir(at, 0755);
    return mount(path, at, type, strcmp(type, "iso9660") ? 0 : MS_RDONLY, NULL) == 0 || failed(path);
}

static void progress(uint64_t bytes) {
    done += bytes;

    int percent = total ? done * 100 / total : 100;

    if (percent != shown) {
        printf("aximos-install: copied %d%%\n", percent > 100 ? 100 : percent);
        fflush(stdout);
        shown = percent;
    }
}

static bool copy_file(const char *from, const char *to, const struct stat *st) {
    int in = open(from, O_RDONLY | O_CLOEXEC), out = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    char buf[1 << 16];
    ssize_t n = 0;

    while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            n = -1;
            break;
        }
        progress(n);
    }
    if (in >= 0)
        close(in);
    if (out < 0 || n < 0) {
        if (out >= 0)
            close(out);
        return failed(to);
    }
    fchown(out, st->st_uid, st->st_gid);
    fchmod(out, st->st_mode & 07777);
    close(out);
    return true;
}

// Copies a tree, keeping owners, modes and links, but not what's mounted in it, such
// as /proc: those are left as empty folders. With measure, only adds up its size.
static bool copy_tree(const char *from, const char *to, dev_t device, bool measure) {
    DIR *dir = opendir(from);
    struct dirent *e;
    bool ok = dir != NULL;

    while (ok && (e = readdir(dir))) {
        char src[4096], dst[4096], link[4096];
        struct stat st;
        ssize_t n;

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(src, sizeof(src), "%s/%s", strcmp(from, "/") ? from : "", e->d_name);
        snprintf(dst, sizeof(dst), "%s/%s", to, e->d_name);
        if (lstat(src, &st) < 0 || !strcmp(src, PAYLOAD) || !strcmp(src, "/home"))
            continue;
        if (measure) {
            total += S_ISREG(st.st_mode) ? st.st_size : 0;
            if (S_ISDIR(st.st_mode) && st.st_dev == device)
                copy_tree(src, dst, device, true);
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            ok = (mkdir(dst, 0700) == 0 || errno == EEXIST) && (st.st_dev != device || copy_tree(src, dst, device, false));
            chown(dst, st.st_uid, st.st_gid);
            chmod(dst, st.st_mode & 07777);
        } else if (S_ISLNK(st.st_mode)) {
            ok = (n = readlink(src, link, sizeof(link) - 1)) >= 0;
            link[ok ? n : 0] = '\0';
            ok = ok && symlink(link, dst) == 0;
            lchown(dst, st.st_uid, st.st_gid);
        } else if (S_ISREG(st.st_mode)) {
            ok = copy_file(src, dst, &st);
        }
        if (!ok)
            failed(dst);
    }
    if (dir)
        closedir(dir);
    return ok;
}

static bool copy_one(const char *from, const char *to) {
    struct stat st;

    return stat(from, &st) == 0 ? copy_file(from, to, &st) : failed(from);
}

// The kept accounts, read before the system is replaced, and written back after.
static char *kept[3];
static const char *const account_files[] = {"passwd", "group", "shadow"};

static void keep_accounts(void) {
    for (int i = 0; i < 3; i++) {
        char path[128];

        snprintf(path, sizeof(path), SYSTEM "/system/etc/%s", account_files[i]);

        FILE *f = fopen(path, "r");
        size_t size = 0;

        if (f && fseek(f, 0, SEEK_END) == 0 && (size = ftell(f)) && (kept[i] = calloc(1, size + 1))) {
            rewind(f);
            if (fread(kept[i], 1, size, f) != size)
                free(kept[i]), kept[i] = NULL;
        }
        if (f)
            fclose(f);
    }
}

// Those of the system's own accounts and groups that came since the kept files were
// written, as sshd's, without touching the rest.
static void add_new_accounts(FILE *out, const char *built, const char *kept) {
    FILE *in = fopen(built, "r");
    char line[512];

    while (in && fgets(line, sizeof(line), in)) {
        size_t name = strcspn(line, ":");
        bool found = false;

        for (const char *at = kept; !found && at && *at; at = strchr(at, '\n') ? strchr(at, '\n') + 1 : NULL)
            found = !strncmp(at, line, name + 1);
        if (!found && line[name] == ':')
            fputs(line, out);
    }
    if (in)
        fclose(in);
}

// The accounts: those kept, or those the system was built with, not the live ones.
static bool put_accounts(void) {
    for (int i = 0; i < 3; i++) {
        char from[128], to[128];

        snprintf(from, sizeof(from), ACCOUNTS "/%s", account_files[i]);
        snprintf(to, sizeof(to), SYSTEM "/system/etc/%s", account_files[i]);
        if (!kept[i] && !copy_one(from, to))
            return false;

        FILE *f = kept[i] ? fopen(to, "w") : NULL;

        // People's accounts from before bash was theirs move from dash to it.
        for (char *line = kept[i], *end; f && i == 0 && line && *line; line = end ? end + 1 : NULL) {
            char *colon = line;
            int field = 0;

            end = strchr(line, '\n');
            for (; colon && field < 2; field++)
                colon = strchr(colon + 1, ':');
            if (colon && atoi(colon + 1) >= FIRST_UID && end && end - line > 15 && !strncmp(end - 15, ":/system/bin/sh", 15)) {
                fprintf(f, "%.*s:/system/bin/bash\n", (int)(end - 15 - line), line);
                continue;
            }
            fprintf(f, "%.*s", end ? (int)(end - line + 1) : (int)strlen(line), line);
        }
        if (kept[i] && (!f || (i != 0 && fputs(kept[i], f) < 0)))
            return failed(to);
        if (f) {
            add_new_accounts(f, from, kept[i]);
            fclose(f);
        }
    }
    return true;
}

static int remove_one(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)st;
    (void)ftw;
    return (type == FTW_DP ? rmdir(path) : unlink(path)) < 0 ? -1 : 0;
}

// Empties an AximOS that's there, but for /home and the swap file.
static bool empty_system(void) {
    DIR *dir = opendir(SYSTEM);
    struct dirent *e;
    bool ok = dir != NULL;

    while (ok && (e = readdir(dir))) {
        char path[300];

        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || !strcmp(e->d_name, "home")
            || !strcmp(e->d_name, "swapfile") || !strcmp(e->d_name, "lost+found"))
            continue;
        snprintf(path, sizeof(path), SYSTEM "/%s", e->d_name);
        ok = nftw(path, remove_one, 16, FTW_DEPTH | FTW_PHYS) == 0 || failed(path);
    }
    if (dir)
        closedir(dir);
    return ok;
}

// A swap file of a quarter of the system partition, at most SWAP_MIB, with the header
// mkswap writes in its first page.
static bool make_swap(uint64_t sectors) {
    uint64_t mib = sectors / 2048 / 4 < SWAP_MIB ? sectors / 2048 / 4 : SWAP_MIB;
    uint8_t page[4096] = {0};
    uint32_t version = 1, last = mib * 256 - 1;
    int fd;

    if (access(SYSTEM "/swapfile", F_OK) == 0)
        return true;
    if ((fd = open(SYSTEM "/swapfile", O_WRONLY | O_CREAT | O_EXCL, 0600)) < 0)
        return failed("swapfile");
    memcpy(page + 1024, &version, 4);
    memcpy(page + 1028, &last, 4);
    memcpy(page + 4096 - 10, "SWAPSPACE2", 10);

    bool ok = posix_fallocate(fd, 0, mib << 20) == 0 && pwrite(fd, page, sizeof(page), 0) == sizeof(page);

    close(fd);
    if (!ok)
        unlink(SYSTEM "/swapfile");
    return ok || failed("swapfile");
}

// Limine's menu: AximOS, then what the firmware has entries for, which it starts as
// they are. It waits for a choice.
static bool write_menu(const char *system, const char *esp) {
    char uuid[64], line[600];
    FILE *out = fopen(ESP "/boot/limine/limine.conf", "w"), *in;

    if (!out || !probe(system, "PART_ENTRY_UUID", uuid, sizeof(uuid)))
        return failed("limine.conf");
    fprintf(out, "timeout: no\n\n/AximOS\n    protocol: linux\n    path: boot():/boot/vmlinuz\n"
                 "    module_path: boot():/boot/microcode.cpio\n"
                 "    cmdline: root=PARTUUID=%s ro rootwait console=tty0 console=ttyS0 quiet\n",
            uuid);
    if ((in = popen("efiboot loaders 2>/dev/null", "r"))) {
        while (fgets(line, sizeof(line), in)) {
            char *partition = (strtok(line, "\t"), strtok(NULL, "\t")), *file = strtok(NULL, "\t"),
                 *title = strtok(NULL, "\n");
            char removable[100];

            if (!title || !strcmp(title, "AximOS") || !strcmp(partition, esp)
                || !probe(partition, "PART_ENTRY_UUID", uuid, sizeof(uuid)))
                continue;
            // Not what's on removable drives, such as the stick it's installed from.
            snprintf(removable, sizeof(removable), "/sys/class/block/%s/../removable", partition);
            if (sys_number("%s", removable))
                continue;
            for (char *c = file; *c; c++)
                *c = *c == '\\' ? '/' : *c;
            fprintf(out, "\n/%s\n    protocol: efi\n    path: guid(%s):%s\n", title, uuid, file);
        }
        pclose(in);
    }
    return fclose(out) == 0 || failed("limine.conf");
}

// The stick or disc it's running from, found by its label, mounted where it can be read.
static bool mount_medium(void) {
    DIR *dir = opendir("/sys/block");
    struct dirent *e;
    bool found = false;

    while (dir && !found && (e = readdir(dir))) {
        if (e->d_name[0] != '.' && is(e->d_name, "TYPE", "iso9660") && is(e->d_name, "LABEL", MEDIUM_LABEL)) {
            umount(MEDIUM);
            found = mount_at(e->d_name, MEDIUM, "iso9660");
        }
    }
    if (dir)
        closedir(dir);
    return found;
}

// The development tools, from their archive on the stick, which the live system doesn't
// hold in memory, into /system with the rest.
static bool add_dev_tools(void) {
    if (no_dev)
        return true;
    if (!mount_medium() || access(DEV_ARCHIVE, R_OK)) {
        say("the development tools aren't on what it's running from, so they're left out");
        return true;
    }
    say("adding the development tools");

    bool ok = run((char *const[]){"tar", "--zstd", "-xpf", DEV_ARCHIVE, "-C", SYSTEM, NULL}, NULL, NULL, 0);

    umount(MEDIUM);
    return ok;
}

// The first account, made as useradd would, in the installed system.
static bool make_account(const char *name, const char *password) {
    pid_t pid = fork();
    int status;

    if (pid == 0)
        _exit(chroot(SYSTEM) == 0 && chdir("/") == 0 && account_create(name, password, true) ? 0 : 1);
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status);
}

// Puts AximOS first in the firmware's order, in place of any entry it had.
static void add_entry(const char *esp) {
    FILE *in = popen("efiboot loaders 2>/dev/null", "r");
    char line[600];

    if (access("/sys/firmware/efi/efivars", F_OK)) {
        say("the firmware isn't UEFI's, so it has no entry to add");
        if (in)
            pclose(in);
        return;
    }
    while (in && fgets(line, sizeof(line), in)) {
        char *number = strtok(line, "\t"), *title = (strtok(NULL, "\t"), strtok(NULL, "\t"), strtok(NULL, "\n"));

        if (title && !strcmp(title, "AximOS"))
            run((char *const[]){"efiboot", "remove", number, NULL}, NULL, NULL, 0);
    }
    if (in)
        pclose(in);
    run((char *const[]){"efiboot", "add", (char *)esp, "/EFI/BOOT/BOOTX64.EFI", "AximOS", NULL}, NULL, NULL, 0);
}

static bool confirm(const char *what) {
    char answer[8];

    if (yes)
        return true;
    printf("%s Go on? [y/N] ", what);
    fflush(stdout);
    return fgets(answer, sizeof(answer), stdin) && (answer[0] == 'y' || answer[0] == 'Y');
}

static int install(const char *name, const char *how, bool erase, const char *account) {
    struct drive d[MAX_DRIVES], *drive = NULL;
    int n = drives(d);
    bool again = !strcmp(how, "again"), whole = !strcmp(how, "whole"), fresh = !again || erase;
    char esp[40] = "", system[40] = "", question[200], user[64] = "", password[256] = "";

    for (int i = 0; i < n; i++)
        drive = !strcmp(d[i].name, name) ? &d[i] : drive;
    if (!drive) {
        printf("aximos-install: %s isn't a drive it can go on\n", name);
        return 1;
    }
    if (again && !drive->aximos[0]) {
        printf("aximos-install: there's no AximOS on %s to install again\n", name);
        return 1;
    }
    if (!again && !whole && strcmp(how, "free")) {
        printf("aximos-install: it goes in free space, on the whole drive, or again over AximOS, not %s\n", how);
        return 1;
    }
    if (!whole && !again && !drive->gpt) {
        printf("aximos-install: %s has no GPT partition table, so no free space it can use\n", name);
        return 1;
    }
    if ((whole ? drive->sectors : again ? MIN_MIB * 2048ULL : drive->free) < MIN_MIB * 2048ULL) {
        printf("aximos-install: %s hasn't the %d MiB it needs\n", name, MIN_MIB);
        return 1;
    }
    snprintf(question, sizeof(question), whole ? "This erases everything on %s."
                                         : again ? (erase ? "This erases AximOS on %s, with its files." : "This replaces AximOS on %s, keeping /home and the accounts.")
                                                 : "This installs AximOS in the free space on %s.",
             name);
    if (!confirm(question))
        return 1;
    if (fresh) {
        snprintf(user, sizeof(user), "%s", account ? account : "");
        while (!account_name_ok(user)) {
            if (user[0])
                puts("A name is letters, digits, dots, dashes and underscores, starting with a letter.");
            if (!ask("Name for the first account: ", user, sizeof(user), false))
                return 1;
        }
        if (!ask_new_password(password, sizeof(password)))
            return 1;
    }

    // The partitions: over the whole drive, a new table with just the ESP, then the
    // system in the rest, as in free space.
    bool ok = true;
    char size[16];
    char last = name[strlen(name) - 1];

    snprintf(size, sizeof(size), "%d", ESP_MIB);
    if (whole) {
        ok = run((char *const[]){"disk", "partition", (char *)name, size, "-y", NULL}, NULL, NULL, 0);
        snprintf(esp, sizeof(esp), "%s%s1", name, last >= '0' && last <= '9' ? "p" : "");
        ok = ok && add(name, "rest", "linux", system);
    } else if (again) {
        snprintf(system, sizeof(system), "%s", drive->aximos);
        snprintf(esp, sizeof(esp), "%s", drive->esp);
        ok = esp[0] || add(name, size, "esp", esp);
    } else {
        ok = add(name, size, "esp", esp) && add(name, "rest", "linux", system);
    }
    ok = ok && run((char *const[]){"sh", "-c", "udevadm settle", NULL}, NULL, NULL, 0);
    ok = ok && format(esp, "fat32", "AXIMOS") && (!fresh || format(system, "ext4", "AximOS"));
    ok = ok && mount_at(system, SYSTEM, "ext4") && mount_at(esp, ESP, "vfat");
    if (!ok)
        return 1;
    printf("aximos-install: AximOS goes on %s, booting from %s\n", system, esp);

    // The system, then what boots it
    struct stat root;

    stat("/", &root);
    if (!fresh) {
        keep_accounts();
        ok = empty_system();
    }
    copy_tree("/", SYSTEM, root.st_dev, true);
    ok = ok && copy_tree("/", SYSTEM, root.st_dev, false) && put_accounts();
    mkdir(SYSTEM "/home", 0755);
    ok = ok && add_dev_tools() && make_swap(sys_number("/sys/class/block/%s/size", system));
    ok = ok && mkdir(ESP "/EFI", 0755) == 0 && mkdir(ESP "/EFI/BOOT", 0755) == 0 && mkdir(ESP "/boot", 0755) == 0
        && mkdir(ESP "/boot/limine", 0755) == 0 && copy_one(PAYLOAD "/BOOTX64.EFI", ESP "/EFI/BOOT/BOOTX64.EFI")
        && copy_one(PAYLOAD "/vmlinuz", ESP "/boot/vmlinuz") && copy_one(PAYLOAD "/microcode.cpio", ESP "/boot/microcode.cpio")
        && write_menu(system, esp);
    if (ok && fresh && !make_account(user, password))
        ok = failed("the account");
    memset(password, 0, sizeof(password));
    sync();
    umount(ESP);
    umount(SYSTEM);
    if (!ok)
        return 1;
    add_entry(esp);
    say("done, so AximOS starts when the computer's restarted");
    return 0;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: aximos-install [list]\n"
                            "       aximos-install DRIVE free|whole|again [NAME] [--erase] [--no-dev] [-y]\n"
                            "Installs AximOS from the live system onto a drive: in its largest free space,\n"
                            "over the whole drive, or again over the AximOS on it, which keeps its /home\n"
                            "and accounts, unless --erase. It asks for the first account's name, unless\n"
                            "given, and password. list gives each drive it can go on, its size, its free\n"
                            "space, and which of those it allows. It asks before changing a drive, unless\n"
                            "-y. The development tools, to build programs, go too, unless --no-dev. Only root\n"
                            "can.\n");

    char *args[4] = {0};
    int n = 0;
    bool erase = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-y"))
            yes = true;
        else if (!strcmp(argv[i], "--erase"))
            erase = true;
        else if (!strcmp(argv[i], "--no-dev"))
            no_dev = true;
        else if (n < 4)
            args[n++] = argv[i];
    }
    if (n == 0 || !strcmp(args[0], "list"))
        return list();
    struct stat root;

    if (getuid() != 0) {
        puts("aximos-install: only root can, as with sudo aximos-install");
        return 1;
    }
    if (stat("/", &root) == 0 && major(root.st_dev) != 0) {
        puts("aximos-install: installs from the live system, on the USB stick, not from an installed one");
        return 1;
    }
    if (n < 2 || n > 3) {
        puts("aximos-install: try aximos-install --help");
        return 1;
    }
    return install(args[0], args[1], erase, args[2]);
}
