#include <gui.h>
#include <stdio.h>
#include <string.h>

// /system/etc/gui.conf has a setting on each line, its name then its value:
//
//     theme dark
//     shadows on
//     clock 24
//     resolution 1280x800
//
// Anything missing or not understood is left as it is by default.

#define SETTINGS "/system/etc/gui.conf"
#define WALLPAPER "/system/etc/wallpaper"
#define DEFAULT_WALLPAPER GUI_WALLPAPERS "/default.jpg"

void gui_settings_load(struct gui_settings *s) {
    FILE *f = fopen(SETTINGS, "r");
    char line[64], value[32];
    int width, height;

    *s = (struct gui_settings){.light = false, .shadows = true, .clock_12_hour = false};
    while (f && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "theme %31s", value) == 1)
            s->light = !strcmp(value, "light");
        else if (sscanf(line, "shadows %31s", value) == 1)
            s->shadows = strcmp(value, "off") != 0;
        else if (sscanf(line, "clock %31s", value) == 1)
            s->clock_12_hour = !strcmp(value, "12");
        else if (sscanf(line, "resolution %dx%d", &width, &height) == 2 && width > 0 && height > 0)
            s->width = width, s->height = height;
    }
    if (f)
        fclose(f);
}

bool gui_settings_save(const struct gui_settings *s) {
    FILE *f = fopen(SETTINGS, "w");

    if (!f)
        return false;
    fprintf(f, "theme %s\nshadows %s\nclock %s\n", s->light ? "light" : "dark", s->shadows ? "on" : "off",
            s->clock_12_hour ? "12" : "24");
    if (s->width)
        fprintf(f, "resolution %dx%d\n", s->width, s->height);
    return fclose(f) == 0;
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
