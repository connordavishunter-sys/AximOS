#include <dirent.h>
#include <fcntl.h>
#include <gui.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// The time zone is set as tz sets it: the zone's rules copied to LOCALTIME, and
// its name kept in NAME_FILE.
#define ZONE_DIR "/system/share/zoneinfo"
#define LOCALTIME "/system/etc/localtime"
#define NAME_FILE "/system/etc/timezone"
#define MAX_ZONES 1024

static const char *const themes[] = {"Dark", "Light"}, *const clocks[] = {"24 hour", "12 hour"};

static struct gui_settings settings;
static char *zones[MAX_ZONES];
static const char *shown[MAX_ZONES]; // the zones the search matches
static int zone_count, shown_count, chosen;
static char search[64], zone[PATH_MAX] = "UTC", status[PATH_MAX + 64];

static int compare(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// Every zone below a folder, named from ZONE_DIR, like Europe/London.
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

static bool contains(const char *text, const char *part) {
    size_t n = strlen(part);

    for (; *text; text++) {
        if (!strncasecmp(text, part, n))
            return true;
    }
    return !n;
}

// The zones the search matches, keeping the chosen one chosen if it still is.
static void filter(void) {
    const char *was = shown_count ? shown[chosen] : zone;

    shown_count = chosen = 0;
    for (int i = 0; i < zone_count; i++) {
        if (contains(zones[i], search)) {
            if (!strcmp(zones[i], was))
                chosen = shown_count;
            shown[shown_count++] = zones[i];
        }
    }
}

static bool copy_file(const char *from, const char *to) {
    char buf[4096];
    int in = open(from, O_RDONLY), out = in >= 0 ? open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    ssize_t n = 0;

    while (out >= 0 && (n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n)
            n = -1;
    }
    if (in >= 0)
        close(in);
    if (out >= 0 && close(out) < 0)
        n = -1;
    return out >= 0 && n == 0;
}

static void set_zone(const char *name) {
    char path[PATH_MAX];
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", ZONE_DIR, name);
    if (!copy_file(path, LOCALTIME) || !(f = fopen(NAME_FILE, "w"))) {
        snprintf(status, sizeof(status), "Couldn't set the time zone to %s", name);
        return;
    }
    fprintf(f, "%s\n", name);
    fclose(f);
    snprintf(zone, sizeof(zone), "%s", name);
    snprintf(status, sizeof(status), "The time zone is now %s", name);
    printf("settings: zone %s\n", name);
    gui_settings_changed(); // for the taskbar's clock
}

static void save(const char *what) {
    if (!gui_settings_save(&settings)) {
        snprintf(status, sizeof(status), "Couldn't save the settings");
        return;
    }
    printf("settings: %s\n", what);
    gui_settings_changed();
}

// Says where a widget is when that changes, for the tests to find it.
static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[8];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)))
        printf("settings: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

static void frame(struct gui_ui *ui) {
    int theme = settings.light, clock = settings.clock_12_hour;
    char now[32];
    time_t t = time(NULL);

    gui_ui_begin(ui);
    gui_heading(ui, "Settings");
    if (gui_dropdown(ui, "Theme", themes, 2, &theme)) {
        settings.light = theme;
        save(settings.light ? "theme light" : "theme dark");
    }
    report(ui, 0, "theme");
    if (gui_checkbox(ui, "Window shadows", &settings.shadows))
        save(settings.shadows ? "shadows on" : "shadows off");
    report(ui, 1, "shadows");
    if (gui_dropdown(ui, "Clock", clocks, 2, &clock)) {
        settings.clock_12_hour = clock;
        save(settings.clock_12_hour ? "clock 12" : "clock 24");
    }
    report(ui, 2, "clock");
    gui_separator(ui);

    strftime(now, sizeof(now), settings.clock_12_hour ? "%I:%M %p" : "%H:%M", localtime(&t));
    gui_label(ui, "Time zone: %s, where it's %s", zone, now + (settings.clock_12_hour && now[0] == '0'));
    if (gui_button(ui, "Use this zone") && shown_count)
        set_zone(shown[chosen]);
    report(ui, 3, "use");
    gui_same_line(ui);
    if (gui_textbox(ui, "##search", search, sizeof(search)))
        filter();
    report(ui, 4, "search");
    gui_label(ui, "%s", status);
    if (gui_list(ui, "##zones", shown, shown_count, &chosen, 0) == GUI_LIST_ACTIVATED)
        set_zone(shown[chosen]);
    report(ui, 5, "zones");
    gui_ui_end(ui);
    fflush(stdout);
}

// Settings for the desktop: its theme, shadows, clock and time zone.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: settings\n"
                            "Opens a window to change the theme, shadows, clock and time zone.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "settings: wm isn't running\n");
        return 1;
    }

    FILE *name = fopen(NAME_FILE, "r");
    struct gui_window *window = gui_create_window("Settings", 480, 540);
    struct gui_ui ui;
    struct gui_message e;

    if (!window) {
        dprintf(STDERR_FILENO, "settings: couldn't make a window\n");
        return 1;
    }
    if (name && fgets(zone, sizeof(zone), name))
        zone[strcspn(zone, "\n")] = '\0';
    if (name)
        fclose(name);
    gui_settings_load(&settings);
    collect("");
    qsort(zones, zone_count, sizeof(*zones), compare);
    filter();
    snprintf(status, sizeof(status), "Type to search the %d zones", zone_count);
    gui_ui_init(&ui, window);
    frame(&ui);
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
