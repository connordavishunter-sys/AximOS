#include "font.h"
#include <dirent.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// /system/etc/gui.conf has a setting on each line, its name then its value:
//
//     theme dark
//     shadows on
//     clock 24
//     resolution 1280x800
//     repeat 500 30
//     text smooth
//     mouse 5
//     app picture viewer
//     accent blue
//     taskbar bottom
//     autohide off
//     icons on
//     night off 50
//     airplane off
//
// Anything missing or not understood is left as it is by default.

#define SETTINGS "/system/etc/gui.conf"
#define WALLPAPER "/system/etc/wallpaper"
#define DEFAULT_WALLPAPER GUI_WALLPAPERS "/default.jpg"

const char *const gui_accent_names[GUI_ACCENTS] = {"Blue", "Red", "Green", "Purple", "Pink", "Orange", "Yellow",
                                                   "Light blue"};
const uint32_t gui_accent_colors[GUI_ACCENTS] = {0x3D63DD, 0xD83B3B, 0x2E9E4F, 0x8A4FD8,
                                                 0xD9488F, 0xE07B1F, 0xE5B700, 0x2FA4E7};
static const char *const accent_keys[GUI_ACCENTS] = {"blue", "red", "green", "purple", "pink", "orange", "yellow",
                                                     "lightblue"};

// White on all but yellow, which is too light for it.
uint32_t gui_on_accent(int accent) {
    return accent == 6 ? 0x1E2129 : 0xFFFFFF;
}

uint32_t gui_mix(uint32_t a, uint32_t b, int amount) {
    uint32_t keep = 256 - amount;

    return ((((a & 0xFF00FF) * keep + (b & 0xFF00FF) * amount) >> 8) & 0xFF00FF)
         | ((((a & 0x00FF00) * keep + (b & 0x00FF00) * amount) >> 8) & 0x00FF00);
}

void gui_settings_load(struct gui_settings *s) {
    FILE *f = fopen(SETTINGS, "r");
    char line[64], value[32], kind[16];
    int width, height;

    *s = (struct gui_settings){.shadows = true, .repeat_delay = KEYBOARD_REPEAT_DELAY_DEFAULT,
                               .repeat_rate = KEYBOARD_REPEAT_RATE_DEFAULT, .mouse_speed = 5,
                               .apps = {"files", "viewer", "player", "editor"}, .night_strength = 50};
    while (f && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "theme %31s", value) == 1)
            s->light = !strcmp(value, "light");
        else if (sscanf(line, "shadows %31s", value) == 1)
            s->shadows = strcmp(value, "off") != 0;
        else if (sscanf(line, "clock %31s", value) == 1)
            s->clock_12_hour = !strcmp(value, "12");
        else if (sscanf(line, "resolution %dx%d", &width, &height) == 2 && width > 0 && height > 0)
            s->width = width, s->height = height;
        else if (sscanf(line, "repeat %d %d", &width, &height) == 2 && width >= 0 && height >= 0)
            s->repeat_delay = width, s->repeat_rate = height;
        else if (sscanf(line, "text %31s", value) == 1)
            s->sharp_text = !strcmp(value, "sharp");
        else if (sscanf(line, "mouse %d", &width) == 1 && width >= 1 && width <= 10)
            s->mouse_speed = width;
        else if (sscanf(line, "accent %31s", value) == 1) {
            for (int i = 0; i < GUI_ACCENTS; i++)
                s->accent = !strcmp(value, accent_keys[i]) ? i : s->accent;
        } else if (sscanf(line, "taskbar %31s", value) == 1)
            s->taskbar_top = !strcmp(value, "top");
        else if (sscanf(line, "autohide %31s", value) == 1)
            s->autohide = !strcmp(value, "on");
        else if (sscanf(line, "icons %31s", value) == 1)
            s->hide_icons = !strcmp(value, "off");
        else if (sscanf(line, "night %31s %d", value, &width) == 2 && width >= 10 && width <= 100)
            s->night_light = !strcmp(value, "on"), s->night_strength = width;
        else if (sscanf(line, "airplane %31s", value) == 1)
            s->airplane = !strcmp(value, "on");
        else if (sscanf(line, "app %15s %31s", kind, value) == 2) {
            for (int i = 0; i < GUI_KINDS; i++) {
                if (!strcmp(kind, gui_kinds[i]))
                    snprintf(s->apps[i], sizeof(s->apps[i]), "%s", value);
            }
        }
    }
    if (f)
        fclose(f);
    font_smooth = !s->sharp_text;
}

bool gui_settings_save(const struct gui_settings *s) {
    FILE *f = fopen(SETTINGS, "w");

    if (!f)
        return false;
    fprintf(f, "theme %s\nshadows %s\nclock %s\n", s->light ? "light" : "dark", s->shadows ? "on" : "off",
            s->clock_12_hour ? "12" : "24");
    if (s->width)
        fprintf(f, "resolution %dx%d\n", s->width, s->height);
    fprintf(f, "repeat %d %d\ntext %s\nmouse %d\n", s->repeat_delay, s->repeat_rate, s->sharp_text ? "sharp" : "smooth",
            s->mouse_speed);
    for (int i = 0; i < GUI_KINDS; i++)
        fprintf(f, "app %s %s\n", gui_kinds[i], s->apps[i]);
    fprintf(f, "accent %s\ntaskbar %s\nautohide %s\nicons %s\nnight %s %d\nairplane %s\n", accent_keys[s->accent],
            s->taskbar_top ? "top" : "bottom", s->autohide ? "on" : "off", s->hide_icons ? "off" : "on",
            s->night_light ? "on" : "off", s->night_strength, s->airplane ? "on" : "off");
    return fclose(f) == 0;
}

const char *const gui_kinds[GUI_KINDS] = {"folder", "picture", "sound", "text"};

int gui_kind_of(const char *path) {
    struct stat st;
    const struct gui_image *icon;

    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return GUI_KIND_FOLDER;
    icon = gui_file_icon(path, false);
    return icon == gui_icon("image") ? GUI_KIND_PICTURE : icon == gui_icon("audio") ? GUI_KIND_SOUND : GUI_KIND_TEXT;
}

const char *gui_app_for(const char *path) {
    static struct gui_settings settings;

    gui_settings_load(&settings);
    return settings.apps[gui_kind_of(path)];
}

void gui_settings_changed(void) {
    gui_send(gui_fd(), &(struct gui_message){.type = GUI_SETTINGS});
}

void gui_wallpaper_get(char path[GUI_PATH_MAX]) {
    FILE *f = fopen(WALLPAPER, "r");

    snprintf(path, GUI_PATH_MAX, "%s", DEFAULT_WALLPAPER);
    if (f && fgets(path, GUI_PATH_MAX, f))
        path[strcspn(path, "\n")] = '\0';
    if (f)
        fclose(f);
}

bool gui_wallpaper_set(const char *path) {
    FILE *f = fopen(WALLPAPER, "w");

    if (!f)
        return false;
    fprintf(f, "%s\n", path);
    return fclose(f) == 0;
}

// In GUI_APPS, or one of its folders.
bool gui_app_path(const char *name, char path[GUI_PATH_MAX]) {
    DIR *dir;
    struct dirent *d;
    struct stat st;
    bool found = false;

    snprintf(path, GUI_PATH_MAX, GUI_APPS "/%s", name);
    if (!*name || strchr(name, '/') || (stat(path, &st) == 0 && !S_ISDIR(st.st_mode)))
        return *name && !strchr(name, '/');
    if (!(dir = opendir(GUI_APPS)))
        return false;
    while (!found && (d = readdir(dir))) {
        if (d->d_name[0] == '.')
            continue;
        snprintf(path, GUI_PATH_MAX, GUI_APPS "/%s/%s", d->d_name, name);
        found = stat(path, &st) == 0 && !S_ISDIR(st.st_mode);
    }
    closedir(dir);
    return found;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(a, b);
}

static int add_names(const char *folder, char (*names)[32], int count, int max, bool deeper) {
    DIR *dir = opendir(folder);
    struct dirent *d;

    while (dir && count < max && (d = readdir(dir))) {
        char path[GUI_PATH_MAX];
        struct stat st;

        snprintf(path, sizeof(path), "%s/%s", folder, d->d_name);
        if (d->d_name[0] == '.' || stat(path, &st) < 0)
            continue;
        if (!S_ISDIR(st.st_mode))
            snprintf(names[count++], 32, "%s", d->d_name);
        else if (deeper)
            count = add_names(path, names, count, max, false);
    }
    if (dir)
        closedir(dir);
    return count;
}

int gui_app_names(char (*names)[32], int max) {
    int count = add_names(GUI_APPS, names, 0, max, true);

    qsort(names, count, 32, compare_names);
    return count;
}

bool gui_is_program(const char *path) {
    char magic[4];
    int fd = open(path, O_RDONLY);
    bool elf = fd >= 0 && read(fd, magic, 4) == 4 && !memcmp(magic, "\x7f" "ELF", 4);

    if (fd >= 0)
        close(fd);
    return elf;
}
