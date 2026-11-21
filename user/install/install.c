#include <dirent.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

// Installs the system it's running from onto a drive: one FAT32 partition, which
// the firmware boots from as the EFI system partition, with everything on it.

#define MAX_DRIVES 16
#define MIN_SECTORS (256 * 2048) // 256 MiB
#define CHUNK (64 * 1024)
#define STEP_MS 30 // how long to copy for between frames

enum page { CHOOSE, CONFIRM, COPYING, DONE, FAILED };

struct item {
    char *path;
    bool dir;
    off_t size;
};

static const char *const page_names[] = {"choose", "confirm", "copying", "done", "failed"};
static enum page page;
static struct gui_window *window;
static char drives[MAX_DRIVES][16], labels[MAX_DRIVES][48], target[32], message[128];
static const char *shown[MAX_DRIVES];
static int drive_count, chosen;
static struct item *items;
static int item_count, item_room, next_item;
static off_t total, copied, item_done;
static int in = -1, out = -1;
static bool prepared;

static long long now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static bool is_root_drive(const char *name) {
    FILE *f = fopen("/proc/mounts", "r");
    char device[32], where[32];
    bool found = false;

    while (f && fscanf(f, "%31s %31s %*s", device, where) == 2)
        found |= !strcmp(where, "/") && !strncmp(device + 5, name, strlen(name));
    if (f)
        fclose(f);
    return found;
}

// The disks big enough, but not the one it's running from.
static void find_drives(void) {
    FILE *f = fopen("/proc/disks", "r");
    char line[128], name[16], kind[16], extra[32];
    unsigned long long sectors;
    unsigned size;

    while (f && drive_count < MAX_DRIVES && fgets(line, sizeof(line), f)) {
        extra[0] = '\0';
        if (sscanf(line, "%15s %llu %u %15s %31[^\n]", name, &sectors, &size, kind, extra) < 4 || strcmp(kind, "disk")
            || extra[0] || sectors * size / 512 < MIN_SECTORS || is_root_drive(name))
            continue;
        strcpy(drives[drive_count], name);
        snprintf(labels[drive_count], sizeof(labels[0]), "%s, %llu MB", name, sectors * size / 1000000);
        shown[drive_count] = labels[drive_count];
        drive_count++;
    }
    if (f)
        fclose(f);
}

static void add(const char *path, bool dir, off_t size) {
    if (item_count == item_room && !(items = realloc(items, (item_room = item_room * 2 + 256) * sizeof(*items))))
        exit(1);
    items[item_count++] = (struct item){strdup(path), dir, size};
    total += size;
}

// Everything but what the kernel makes up, and what only booting from a disc needs.
static void collect(const char *dir) {
    DIR *d = opendir(dir[0] ? dir : "/");
    struct dirent *e;
    char path[PATH_MAX];
    struct stat st;

    while (d && (e = readdir(d))) {
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (!strcmp(path, "/dev") || !strcmp(path, "/proc") || !strcmp(e->d_name, "limine-uefi-cd.bin")
            || stat(path, &st) < 0)
            continue;
        add(path, S_ISDIR(st.st_mode), S_ISDIR(st.st_mode) ? 0 : st.st_size);
        if (S_ISDIR(st.st_mode) && strcmp(path, "/tmp"))
            collect(path);
    }
    if (d)
        closedir(d);
}

static void go_to(struct gui_ui *ui, enum page to) {
    page = to;
    gui_set_focus(ui, page == CHOOSE ? "Install" : page == CONFIRM ? "Erase and install" : page == DONE ? "Restart" : "Back",
                  NULL);
    ui->again = true;
    printf("install: %s\n", page_names[page]);
}

static void fail(struct gui_ui *ui, const char *what) {
    snprintf(message, sizeof(message), "%s", what);
    printf("install: failed: %s\n", message);
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    in = out = -1;
    go_to(ui, FAILED);
}

static bool run(char *const argv[]) {
    pid_t child = fork();
    int status;

    if (child == 0) {
        execv(argv[0], argv);
        _exit(127);
    }
    return child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// A new partition table and filesystem, then the list of what to copy.
static void start(struct gui_ui *ui) {
    char *drive = drives[chosen];

    prepared = true;
    snprintf(target, sizeof(target), "/dev/%s1", drive);
    if (!run((char *[]){"/system/bin/disk", "partition", drive, "-y", NULL}))
        return fail(ui, "The drive couldn't be partitioned.");
    if (!run((char *[]){"/system/bin/disk", "format", target + 5, "AXIMOS", "-y", NULL}) || access(target, F_OK))
        return fail(ui, "The drive couldn't be formatted.");
    item_count = next_item = 0;
    total = copied = 0;
    collect("");
    printf("install: copying %d files, %lld bytes, to %s\n", item_count, (long long)total, target);
    go_to(ui, COPYING);
}

// Copies for a little while, so the window keeps up.
static void copy_some(struct gui_ui *ui) {
    static char buf[CHUNK];
    char path[PATH_MAX];
    long long until = now_ms() + STEP_MS;

    if (!prepared)
        return start(ui);
    while (page == COPYING && now_ms() < until) {
        if (next_item == item_count) {
            printf("install: done\n");
            go_to(ui, DONE);
            return;
        }

        struct item *it = &items[next_item];

        snprintf(path, sizeof(path), "%s%s", target, it->path);
        if (it->dir) {
            if (mkdir(path, 0777) < 0)
                return fail(ui, "A folder couldn't be made.");
            next_item++;
            continue;
        }
        if (in < 0) {
            in = open(it->path, O_RDONLY);
            out = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
            item_done = 0;
            if (in < 0 || out < 0)
                return fail(ui, "A file couldn't be copied.");
        }

        ssize_t n = read(in, buf, sizeof(buf));

        if (n < 0 || (n > 0 && write(out, buf, n) != n))
            return fail(ui, "A file couldn't be copied. The drive may be full.");
        copied += n;
        item_done += n;
        if (n == 0) {
            close(in);
            close(out);
            in = out = -1;
            next_item++;
        }
    }
}

static void frame(struct gui_ui *ui) {
    gui_ui_begin(ui);
    switch (page) {
    case CHOOSE:
        gui_heading(ui, "Install AximOS");
        if (!drive_count) {
            gui_label(ui, "There's no drive to install on.");
            gui_label(ui, "It needs one of at least 256 MB.");
            break;
        }
        gui_label(ui, "Choose the drive to install on.");
        gui_list(ui, "##drives", shown, drive_count, &chosen, 6);
        if (gui_button(ui, "Install"))
            go_to(ui, CONFIRM);
        break;
    case CONFIRM:
        snprintf(message, sizeof(message), "Erase %s?", drives[chosen]);
        gui_heading(ui, message);
        gui_label(ui, "Everything on %s will be erased, and AximOS installed in its place.", drives[chosen]);
        if (gui_button(ui, "Back"))
            go_to(ui, CHOOSE);
        gui_same_line(ui);
        if (gui_button(ui, "Erase and install")) {
            prepared = false;
            go_to(ui, COPYING);
        }
        break;
    case COPYING:
        gui_heading(ui, "Installing");
        if (prepared)
            gui_label(ui, "Copying %s", next_item < item_count ? items[next_item].path : "");
        else
            gui_label(ui, "Making a partition on %s and formatting it", drives[chosen]);
        gui_progress(ui, total ? (float)copied / total : 1);
        break;
    case DONE:
        gui_heading(ui, "AximOS is installed");
        gui_label(ui, "Take out the install disc, then restart.");
        gui_label(ui, "The first boot sets up your account.");
        if (gui_button(ui, "Restart"))
            reboot(RB_AUTOBOOT);
        break;
    case FAILED:
        gui_heading(ui, "It couldn't be installed");
        gui_label(ui, "%s", message);
        if (gui_button(ui, "Back"))
            go_to(ui, CHOOSE);
        break;
    }
    gui_ui_end(ui);
    fflush(stdout);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: install\n"
                            "Installs AximOS on a drive, erasing what's there. Booting the install disc starts it.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "install: needs the desktop\n");
        return 1;
    }

    struct gui_ui ui;

    find_drives();
    if (!(window = gui_create_window("Install AximOS", 460, 280))) {
        dprintf(STDERR_FILENO, "install: couldn't make its window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    go_to(&ui, CHOOSE);

    // While copying, it draws between steps rather than waiting for events.
    for (bool redraw = true;;) {
        struct gui_message e;
        int got;

        if (redraw)
            frame(&ui);
        redraw = page == COPYING;
        if (redraw)
            copy_some(&ui);
        if ((got = gui_wait_event(&e, redraw ? 0 : -1)) < 0)
            break;
        if (got)
            redraw |= gui_ui_event(&ui, &e);
    }
    return 0;
}
