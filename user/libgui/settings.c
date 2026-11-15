#include "font.h"
#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

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
//
// Anything missing or not understood is left as it is by default.

#define SETTINGS "/system/etc/gui.conf"
#define WALLPAPER "/system/etc/wallpaper"
#define DEFAULT_WALLPAPER GUI_WALLPAPERS "/default.jpg"

void gui_settings_load(struct gui_settings *s) {
    FILE *f = fopen(SETTINGS, "r");
    char line[64], value[32], kind[16];
    int width, height;

    *s = (struct gui_settings){.shadows = true, .repeat_delay = KEYBOARD_REPEAT_DELAY_DEFAULT,
                               .repeat_rate = KEYBOARD_REPEAT_RATE_DEFAULT, .mouse_speed = 5,
                               .apps = {"files", "viewer", "player", "editor"}};
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
