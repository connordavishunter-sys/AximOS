#include <aximos.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ZONE_DIR "/system/share/zoneinfo"
#define LOCALTIME "/system/etc/localtime"
#define NAME_FILE "/system/etc/timezone" // the zone's name, as localtime only has its rules
#define MAX_ZONES 1024

static char *zones[MAX_ZONES];
static int zone_count;

static int compare(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// Collects every zone below a directory, named relative to ZONE_DIR.
static void collect(const char *relative) {
    char path[PATH_MAX];
    DIR *dir;
    struct dirent *entry;

    snprintf(path, sizeof(path), "%s%s%s", ZONE_DIR, *relative ? "/" : "", relative);
    if (!(dir = opendir(path)))
        return;
    while ((entry = readdir(dir)) && zone_count < MAX_ZONES) {
        char name[PATH_MAX];
        struct stat st;

        if (entry->d_name[0] == '.')
            continue;
        snprintf(name, sizeof(name), "%s%s%s", relative, *relative ? "/" : "", entry->d_name);
        snprintf(path, sizeof(path), "%s/%s", ZONE_DIR, name);
        if (stat(path, &st) < 0)
            continue;
        if (S_ISDIR(st.st_mode))
            collect(name);
        else
            zones[zone_count++] = strdup(name);
    }
    closedir(dir);
}

static void describe(const char *name) {
    time_t now = time(NULL);
    struct tm t;
    char text[64];
    const char *tz = getenv("TZ");

    localtime_r(&now, &t);
    strftime(text, sizeof(text), "%H:%M %Z (%z)", &t);
    printf("%s, where it's %s\n", name, text);
    if (tz && *tz)
        printf("TZ is %s, which programs use instead\n", tz);
}

static bool is_zone_file(const char *path) {
    char magic[4];
    int fd = open(path, O_RDONLY);
    bool ok = fd >= 0 && read(fd, magic, 4) == 4 && memcmp(magic, "TZif", 4) == 0;

    if (fd >= 0)
        close(fd);
    return ok;
}

static bool copy_file(const char *from, const char *to) {
    char buf[4096];
    int in = open(from, O_RDONLY), out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    ssize_t n = 0;

    while (out >= 0 && (n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            n = -1;
            break;
        }
    }
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    return in >= 0 && out >= 0 && n == 0;
}

static int set_zone(const char *name) {
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/%s", ZONE_DIR, name);
    if (strstr(name, "..") || !is_zone_file(path)) {
        dprintf(STDERR_FILENO, "tz: no zone called %s, which tz -l would list\n", name);
        return 1;
    }

    // The new rules go in whole, so nothing reads half of them.
    FILE *named = NULL;

    if (!copy_file(path, LOCALTIME ".new") || rename(LOCALTIME ".new", LOCALTIME) < 0
        || !(named = fopen(NAME_FILE, "w")) || fprintf(named, "%s\n", name) < 0 || fclose(named) != 0) {
        dprintf(STDERR_FILENO, "tz: couldn't set the zone: %s\n", strerror(errno));
        return 1;
    }
    printf("The zone is now ");
    describe(name);
    return 0;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: tz [zone | -l [prefix]]\n"
                            "Shows the time zone, sets it to a zone such as Europe/London, or lists the\n"
                            "zones, or those starting with prefix. Only admins can set it.\n");

    if (argc > 1 && strcmp(argv[1], "-l") == 0) {
        size_t len = argc > 2 ? strlen(argv[2]) : 0;

        collect("");
        qsort(zones, zone_count, sizeof(*zones), compare);
        for (int i = 0; i < zone_count; i++) {
            if (len == 0 || strncmp(zones[i], argv[2], len) == 0)
                puts(zones[i]);
        }
        return 0;
    }
    // It's setuid root, as the zone's the whole system's, for root and admins to set.
    struct passwd *p = getpwuid(getuid());

    if (argc > 1 && getuid() != 0 && (!p || !account_is_admin(p->pw_name))) {
        dprintf(STDERR_FILENO, "tz: only accounts in the %s group can set the zone\n", ADMIN_GROUP);
        return 1;
    }
    if (argc > 1)
        return set_zone(argv[1]);

    char name[PATH_MAX] = "UTC";
    FILE *named = access(LOCALTIME, R_OK) == 0 ? fopen(NAME_FILE, "r") : NULL;

    if (named && fgets(name, sizeof(name), named))
        name[strcspn(name, "\n")] = '\0';
    if (named)
        fclose(named);
    describe(name);
    return 0;
}
