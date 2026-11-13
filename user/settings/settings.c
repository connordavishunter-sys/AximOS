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
#define MAX_WALLPAPERS 12
#define THUMB_WIDTH 136 // 16:10, like the wallpapers
#define THUMB_HEIGHT 85
#define THUMBS_ACROSS 3
#define MAX_RESOLUTIONS 16

enum { APPEARANCE, DISPLAY, TIME, TABS };
static const char *const tabs[] = {"Appearance", "Display", "Date & time"};
static const char *const themes[] = {"Dark", "Light"}, *const clocks[] = {"24 hour", "12 hour"};

// What the screen can be, as far as QEMU's graphics card goes. Their widths are
// multiples of 8, which it needs.
static const int sizes[][2] = {
    {800, 600},   {1024, 768},  {1152, 864},  {1280, 720},  {1280, 800}, {1280, 1024},
    {1440, 900},  {1600, 900},  {1680, 1050}, {1920, 1080}, {1920, 1200}, {2560, 1440},
};

static struct gui_settings settings;
static int tab;
static char status[PATH_MAX + 64];

static char *zones[MAX_ZONES];
static const char *shown[MAX_ZONES]; // the zones the search matches
static int zone_count, shown_count, chosen;
static char search[64], zone[PATH_MAX] = "UTC";

static struct {
    char path[GUI_PATH_MAX], name[32];
    struct gui_image thumb;
} wallpapers[MAX_WALLPAPERS];
static int wallpaper_count;
static char wallpaper[GUI_PATH_MAX];

static char resolution_names[MAX_RESOLUTIONS][16];
static const char *resolutions[MAX_RESOLUTIONS];
static int resolution_count, picked = -1, wanted_width, wanted_height; // picked, until it's applied

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

// The wallpapers that come with the system, the default first, each with a
// thumbnail and a name from its file's.
static int compare_wallpapers(const void *a, const void *b) {
    const char *x = a, *y = b;
    bool x_default = !strcmp(x + strlen(x) - 11, "default.jpg"), y_default = !strcmp(y + strlen(y) - 11, "default.jpg");

    return x_default != y_default ? y_default - x_default : strcmp(x, y);
}

static void load_wallpapers(void) {
    DIR *dir = opendir(GUI_WALLPAPERS);
    struct dirent *entry;

    while (dir && wallpaper_count < MAX_WALLPAPERS && (entry = readdir(dir))) {
        if (entry->d_name[0] != '.')
            snprintf(wallpapers[wallpaper_count++].path, GUI_PATH_MAX, GUI_WALLPAPERS "/%s", entry->d_name);
    }
    if (dir)
        closedir(dir);
    qsort(wallpapers, wallpaper_count, sizeof(*wallpapers), compare_wallpapers);
    for (int i = 0; i < wallpaper_count; i++) {
        struct gui_image full;
        char *name = wallpapers[i].name;

        snprintf(name, sizeof(wallpapers[i].name), "%s", strrchr(wallpapers[i].path, '/') + 1);
        name[strcspn(name, ".")] = '\0';
        name[0] = name[0] >= 'a' && name[0] <= 'z' ? name[0] - 32 : name[0];
        if (gui_image_load(wallpapers[i].path, &full)) {
            gui_image_scale(&full, THUMB_WIDTH, THUMB_HEIGHT, &wallpapers[i].thumb);
            gui_image_free(&full);
        }
    }
    gui_wallpaper_get(wallpaper);
}

// The sizes the screen can be, with the one it is now, if that's not among them.
static void list_resolutions(void) {
    const struct gui_screen *s = gui_screen();
    bool listed = false;

    resolution_count = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        if (!listed && (sizes[i][0] > s->width || (sizes[i][0] == s->width && sizes[i][1] >= s->height))) {
            listed = true;
            if (sizes[i][0] != s->width || sizes[i][1] != s->height)
                snprintf(resolution_names[resolution_count++], 16, "%dx%d", s->width, s->height);
        }
        snprintf(resolution_names[resolution_count++], 16, "%dx%d", sizes[i][0], sizes[i][1]);
    }
    if (!listed)
        snprintf(resolution_names[resolution_count++], 16, "%dx%d", s->width, s->height);
    for (int i = 0; i < resolution_count; i++)
        resolutions[i] = resolution_names[i];
}

// What wm made of a resolution asked for.
static void screen_changed(void) {
    const struct gui_screen *s = gui_screen();

    printf("settings: screen %dx%d%s\n", s->width, s->height, s->can_resize ? "" : " fixed");
    if (wanted_width && (s->width != wanted_width || s->height != wanted_height))
        snprintf(status, sizeof(status), "The screen couldn't be %dx%d", wanted_width, wanted_height);
    wanted_width = 0;
    picked = -1;
    list_resolutions();
}

// Says where a widget is when that changes, for the tests to find it.
static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[8 + MAX_WALLPAPERS];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)))
        printf("settings: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

static void appearance(struct gui_ui *ui) {
    int theme = settings.light;

    if (gui_dropdown(ui, "Theme", themes, 2, &theme)) {
        settings.light = theme;
        save(settings.light ? "theme light" : "theme dark");
    }
    report(ui, 0, "theme");
    if (gui_checkbox(ui, "Window shadows", &settings.shadows))
        save(settings.shadows ? "shadows on" : "shadows off");
    report(ui, 1, "shadows");
    gui_separator(ui);
    gui_label(ui, "Wallpaper");
    for (int i = 0; i < wallpaper_count; i++) {
        char label[64];

        if (i % THUMBS_ACROSS)
            gui_same_line(ui);
        snprintf(label, sizeof(label), "%s##wallpaper", wallpapers[i].name);
        if (gui_image_button(ui, label, wallpapers[i].thumb.pixels ? &wallpapers[i].thumb : NULL,
                             !strcmp(wallpaper, wallpapers[i].path))
            && gui_wallpaper_set(wallpapers[i].path)) {
            snprintf(wallpaper, sizeof(wallpaper), "%s", wallpapers[i].path);
            printf("settings: wallpaper %s\n", wallpaper);
            gui_settings_changed();
        }
        snprintf(label, sizeof(label), "wallpaper %s", wallpapers[i].name);
        report(ui, 8 + i, label);
    }
}

static void display(struct gui_ui *ui) {
    const struct gui_screen *s = gui_screen();
    char now[16];
    int at = 0;

    if (!s->can_resize) {
        gui_label(ui, "The screen is %dx%d, and its resolution can't be changed.", s->width, s->height);
        return;
    }
    snprintf(now, sizeof(now), "%dx%d", s->width, s->height);
    for (int i = 0; i < resolution_count; i++)
        at = strcmp(resolutions[i], now) ? at : i;

    // Choosing one only picks it, so going through them doesn't change the screen each time.
    int shown_at = picked >= 0 ? picked : at;

    if (gui_dropdown(ui, "Resolution", resolutions, resolution_count, &shown_at))
        picked = shown_at;
    report(ui, 2, "resolution");
    if (gui_button(ui, "Apply") && picked >= 0 && picked != at) {
        sscanf(resolutions[picked], "%dx%d", &settings.width, &settings.height);
        wanted_width = settings.width;
        wanted_height = settings.height;
        status[0] = '\0';
        save(resolutions[picked]);
    }
    report(ui, 7, "apply");
    gui_same_line(ui);
    gui_label(ui, "The screen is %s.", now);
    gui_label(ui, "%s", status);
}

static void date_and_time(struct gui_ui *ui) {
    int clock = settings.clock_12_hour;
    char now[32];
    time_t t = time(NULL);

    if (gui_dropdown(ui, "Clock", clocks, 2, &clock)) {
        settings.clock_12_hour = clock;
        save(settings.clock_12_hour ? "clock 12" : "clock 24");
    }
    report(ui, 3, "clock");
    gui_separator(ui);
    strftime(now, sizeof(now), settings.clock_12_hour ? "%I:%M %p" : "%H:%M", localtime(&t));
    gui_label(ui, "Time zone: %s, where it's %s", zone, now + (settings.clock_12_hour && now[0] == '0'));
    if (gui_button(ui, "Use this zone") && shown_count)
        set_zone(shown[chosen]);
    report(ui, 4, "use");
    gui_same_line(ui);
    if (gui_textbox(ui, "##search", search, sizeof(search)))
        filter();
    report(ui, 5, "search");
    gui_label(ui, "%s", status);
    if (gui_list(ui, "##zones", shown, shown_count, &chosen, 0) == GUI_LIST_ACTIVATED)
        set_zone(shown[chosen]);
    report(ui, 6, "zones");
}

static void frame(struct gui_ui *ui) {
    int was = tab;

    gui_ui_begin(ui);
    gui_tabs(ui, tabs, TABS, &tab, NULL);
    if (tab != was) {
        printf("settings: tab %s\n", tabs[tab]);
        status[0] = '\0';
    }
    if (tab == APPEARANCE)
        appearance(ui);
    else if (tab == DISPLAY)
        display(ui);
    else
        date_and_time(ui);
    gui_ui_end(ui);
    fflush(stdout);
}

// Settings for the desktop: its theme, shadows, wallpaper, resolution, clock and time zone.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: settings\n"
                            "Opens a window to change the theme, shadows, wallpaper, resolution, clock and time zone.\n");
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
    load_wallpapers();
    gui_ui_init(&ui, window);
    frame(&ui);
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (e.type == GUI_SCREEN)
            screen_changed();
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
