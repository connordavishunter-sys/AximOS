#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fb.h>
#include <sys/statfs.h>
#include <sys/utsname.h>
#include <unistd.h>

#define TEXT_MAX 2048
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
        if (strncmp(line, name, len) == 0 && line[len] == ':')
            return line + len + 2;
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

    snprintf(value, sizeof(value), "%.*s (%d)", len > 70 ? 70 : len, model, atoi(field(text, "cpus")));
    add_info("CPU", value);
}

static void add_memory(void) {
    static char text[TEXT_MAX];
    char used[32], total[32], value[96];

    if (!read_text("/proc/meminfo", text, sizeof(text)))
        return;

    long total_kb = atol(field(text, "MemTotal"));
    long free_kb = atol(field(text, "MemFree"));

    format_size(used, sizeof(used), (total_kb - free_kb) * 1024ULL);
    format_size(total, sizeof(total), total_kb * 1024ULL);
    snprintf(value, sizeof(value), "%s / %s (%ld%%)", used, total, total_kb ? (total_kb - free_kb) * 100 / total_kb : 0);
    add_info("Memory", value);
}

static void add_disk(void) {
    struct statfs fs;
    char used[32], total[32], value[96];

    if (statfs("/", &fs) < 0)
        return;

    uint64_t size = fs.f_blocks * fs.f_bsize;
    uint64_t taken = size - fs.f_bfree * fs.f_bsize;

    format_size(used, sizeof(used), taken);
    format_size(total, sizeof(total), size);
    snprintf(value, sizeof(value), "%s / %s (%lu%%) - %s", used, total, size ? (unsigned long)(taken * 100 / size) : 0, fs.f_type);
    add_info("Disk (/)", value);
}

static void add_display(void) {
    int fd = open("/dev/fb0", O_RDONLY);
    struct fb_info fb;
    char value[32];

    if (fd < 0)
        return;
    if (fb_info(fd, &fb) == 0) {
        snprintf(value, sizeof(value), "%ux%u", fb.width, fb.height);
        add_info("Display", value);
    }
    close(fd);
}

static void add_network(void) {
    struct net_info net;
    char value[64];

    if (net_info(&net) < 0)
        return;
    if (!net.has_device) {
        add_info("Network", "no card");
        return;
    }

    struct in_addr address = {htonl(net.address)};

    snprintf(value, sizeof(value), "%s (%s)", net.address ? inet_ntoa(address) : "no address", net.automatic ? "DHCP" : "static");
    add_info("Network", value);
}

// Whatever started this is the shell, more surely than the environment says.
static void add_shell(void) {
    static char text[TEXT_MAX];
    char path[64], value[64];

    snprintf(path, sizeof(path), "/proc/%d/status", getppid());
    if (!read_text(path, text, sizeof(text)))
        return;

    const char *name = field(text, "Name");

    snprintf(value, sizeof(value), "%.*s", (int)strcspn(name, "\n"), name);
    add_info("Shell", value);
}

// Each entry in the directory is a program.
static void add_programs(void) {
    DIR *dir = opendir("/system/bin");
    struct dirent *entry;
    char value[32];
    int count = 0;

    if (!dir)
        return;
    while ((entry = readdir(dir))) {
        if (entry->d_type != DT_DIR)
            count++;
    }
    closedir(dir);
    snprintf(value, sizeof(value), "%d", count);
    add_info("Programs", value);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: axfetch\n"
                            "Shows a summary of the system next to the logo.\n");

    struct utsname name;
    char title[INFO_MAX];
    char kernel[96];

    uname(&name);
    snprintf(title, sizeof(title), ACCENT "%s@%s" RESET, getenv("USER") ? getenv("USER") : "root", name.nodename);
    snprintf(kernel, sizeof(kernel), "%s %s", name.sysname, name.machine);
    add_info("OS", kernel);
    add_info("Kernel", name.version);
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
