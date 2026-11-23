#include <aximos.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <mntent.h>
#include <net/if.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/statvfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#define TEXT_MAX 8192
#define NAME_MACHINE "x86_64"
#define RESET "\x1b[0m"
#define ACCENT "\x1b[1;36m"
#define LOGO_LINES 12
#define LOGO_WIDTH 22
#define MAX_INFO 12
#define INFO_MAX 128

static const char *const logo[LOGO_LINES] = {
    "          /\\          ",
    "         /  \\         ",
    "        / /\\ \\        ",
    "       / /  \\ \\       ",
    "      / /    \\ \\      ",
    "     / /  /\\  \\ \\     ",
    "    / /  /  \\  \\ \\    ",
    "   / /__/ /\\ \\__\\ \\   ",
    "  /_____ /  \\ _____\\  ",
    "       //    \\\\       ",
    "      //      \\\\      ",
    "      ~        ~      ",
};

static char info[MAX_INFO][INFO_MAX];
static int info_count;

static bool read_text(const char *path, char *out, size_t size) {
    int fd = open(path, O_RDONLY);

    if (fd < 0)
        return false;

    ssize_t got = read(fd, out, size - 1);

    close(fd);
    if (got < 0)
        return false;
    out[got] = '\0';
    return true;
}

static const char *field(const char *text, const char *name) {
    size_t len = strlen(name);

    for (const char *line = text; line; line = strchr(line, '\n'), line = line ? line + 1 : NULL) {
        if (strncmp(line, name, len) == 0 && line[len + strspn(line + len, "\t ")] == ':')
            return line + len + strspn(line + len, "\t ") + 2;
    }
    return "";
}

static void add_info(const char *label, const char *value) {
    if (info_count < MAX_INFO && value[0])
        snprintf(info[info_count++], INFO_MAX, ACCENT "%s" RESET ": %s", label, value);
}

// A size the way print_size would write it, but into a string.
static void format_size(char *out, size_t size, uint64_t bytes) {
    static const char *const units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int unit = 0;
    uint64_t tenths = 0;

    while (bytes >= 1024 && unit < 4) {
        tenths = bytes % 1024 * 10 / 1024;
        bytes /= 1024;
        unit++;
    }
    snprintf(out, size, "%lu.%lu %s", (unsigned long)bytes, (unsigned long)tenths, units[unit]);
}

static void add_uptime(void) {
    char text[64], value[64];

    if (!read_text("/proc/uptime", text, sizeof(text)))
        return;

    long seconds = atol(text);
    long days = seconds / 86400, hours = seconds / 3600 % 24, minutes = seconds / 60 % 60;

    if (days)
        snprintf(value, sizeof(value), "%ld days, %ld hours, %ld mins", days, hours, minutes);
    else if (hours)
        snprintf(value, sizeof(value), "%ld hours, %ld mins", hours, minutes);
    else if (minutes)
        snprintf(value, sizeof(value), "%ld mins", minutes);
    else
        snprintf(value, sizeof(value), "%ld secs", seconds);
    add_info("Uptime", value);
}

static void add_cpu(void) {
    static char text[TEXT_MAX];
    char value[INFO_MAX];

    if (!read_text("/proc/cpuinfo", text, sizeof(text)))
        return;

    const char *model = field(text, "model name");
    int len = strcspn(model, "\n");

    snprintf(value, sizeof(value), "%.*s (%ld)", len > 70 ? 70 : len, model, sysconf(_SC_NPROCESSORS_ONLN));
    add_info("CPU", value);
}

static void add_memory(void) {
    struct memory m;
    char used[32], total[32], value[96];

    if (!memory_read(&m))
        return;
    format_size(used, sizeof(used), m.total - m.available);
    format_size(total, sizeof(total), m.total);
    snprintf(value, sizeof(value), "%s / %s (%d%%)", used, total, (int)(m.total ? (m.total - m.available) * 100 / m.total : 0));
    add_info("Memory", value);
}

// The root filesystem's size, and its type, as /proc/mounts says.
static void add_disk(void) {
    struct statvfs fs;
    char used[32], total[32], value[96], type[32] = "";
    FILE *mounts = setmntent("/proc/mounts", "r");
    struct mntent *m;

    while (mounts && (m = getmntent(mounts))) {
        if (!strcmp(m->mnt_dir, "/"))
            snprintf(type, sizeof(type), "%s", m->mnt_type);
    }
    if (mounts)
        endmntent(mounts);
    if (statvfs("/", &fs) < 0)
        return;

    uint64_t size = (uint64_t)fs.f_blocks * fs.f_frsize, taken = size - (uint64_t)fs.f_bfree * fs.f_frsize;

    format_size(used, sizeof(used), taken);
    format_size(total, sizeof(total), size);
    snprintf(value, sizeof(value), "%s / %s (%lu%%) - %s", used, total, size ? (unsigned long)(taken * 100 / size) : 0, type);
    add_info("Disk (/)", value);
}

// The screen's size, from the framebuffer Linux shows its console on.
static void add_display(void) {
    char text[32];
    int width, height;

    if (read_text("/sys/class/graphics/fb0/virtual_size", text, sizeof(text)) && sscanf(text, "%d,%d", &width, &height) == 2) {
        snprintf(text, sizeof(text), "%dx%d", width, height);
        add_info("Display", text);
    }
}

// The first network card with an address.
static void add_network(void) {
    struct ifaddrs *list;
    char value[64] = "no address";

    if (getifaddrs(&list) < 0)
        return;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET && !(i->ifa_flags & IFF_LOOPBACK)) {
            snprintf(value, sizeof(value), "%s (%s)", inet_ntoa(((struct sockaddr_in *)i->ifa_addr)->sin_addr), i->ifa_name);
            break;
        }
    }
    freeifaddrs(list);
    add_info("Network", value);
}

// Whatever started this is the shell, more surely than the environment says, and
// its program's name, as a login shell calls itself -sh.
static void add_shell(void) {
    char path[64], program[256];
    ssize_t length;

    snprintf(path, sizeof(path), "/proc/%d/exe", getppid());
    if ((length = readlink(path, program, sizeof(program) - 1)) <= 0)
        return;
    program[length] = '\0';
    add_info("Shell", strrchr(program, '/') ? strrchr(program, '/') + 1 : program);
}

// Each file in a folder is a program, and so is each in the folders in it.
static int count_programs(const char *path) {
    DIR *dir = opendir(path);
    struct dirent *entry;
    int count = 0;

    while (dir && (entry = readdir(dir))) {
        char inside[256];

        if (entry->d_name[0] == '.')
            continue;
        snprintf(inside, sizeof(inside), "%s/%s", path, entry->d_name);
        count += entry->d_type == DT_DIR ? count_programs(inside) : 1;
    }
    if (dir)
        closedir(dir);
    return count;
}

static void add_programs(void) {
    char value[32];

    snprintf(value, sizeof(value), "%d", count_programs("/system/bin"));
    add_info("Programs", value);
}

// Who's running it.
static const char *user_name(void) {
    struct passwd *p = getpwuid(getuid());

    return p ? p->pw_name : "root";
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: axfetch\n"
                            "Shows a summary of the system next to the logo.\n");

    struct utsname name;
    char title[INFO_MAX];
    char kernel[96];

    uname(&name);
    snprintf(title, sizeof(title), ACCENT "%s@%s" RESET, user_name(), name.nodename);
    snprintf(kernel, sizeof(kernel), "%s %s", name.sysname, name.release);
    add_info("OS", "AximOS " NAME_MACHINE);
    add_info("Kernel", kernel);
    add_uptime();
    add_programs();
    add_shell();
    add_display();
    add_cpu();
    add_memory();
    add_disk();
    add_network();

    for (int i = 0; i < LOGO_LINES || i < info_count + 2; i++) {
        printf(ACCENT "%-*s" RESET "  ", LOGO_WIDTH, i < LOGO_LINES ? logo[i] : "");
        if (i == 0)
            printf("%s", title);
        else if (i == 1)
            printf("--------------");
        else if (i - 2 < info_count)
            printf("%s", info[i - 2]);
        printf("\n");
    }
    printf("\n" ACCENT "%*s" RESET, LOGO_WIDTH + 2, "");
    for (int color = 40; color <= 47; color++)
        printf("\x1b[%dm   ", color);
    printf(RESET "\n");
    return 0;
}
