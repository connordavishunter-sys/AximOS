#include <aximos.h>
#include "account.h"
#include <dirent.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#define MAX_WALLPAPERS 12
#define THUMB_WIDTH 136
#define THUMB_HEIGHT 85
#define THUMBS_ACROSS 3

// The first time, it asks how the desktop should look, where in the world it is,
// and then for an account. After that, it logs in, or unlocks for whoever's logged in.
enum page { WELCOME, TIME, CREATE, LOG_IN, LOCKED };
static const char *const page_names[] = {"welcome", "time zone", "create", "log in", "locked"};
static const int sizes[][2] = {{480, 410}, {420, 420}, {360, 290}, {360, 230}, {360, 200}};
static const char *const themes[] = {"Dark", "Light"}, *const clocks[] = {"24 hour", "12 hour"};

static enum page page;
static struct gui_window *window;
static char name[ACCOUNT_NAME], password[128], again[128], message[96];
static struct account account;
static struct gui_settings settings;
static struct {
    char path[GUI_PATH_MAX], name[32];
    struct gui_image thumb;
} wallpapers[MAX_WALLPAPERS];
static int wallpaper_count;
static char wallpaper[GUI_PATH_MAX];
static char **zones, search[64], zone[GUI_PATH_MAX];
static const char **shown; // the zones the search matches
static int zone_count, shown_count, chosen;

// The wallpapers that come with the system, the default first.
static int compare_wallpapers(const void *a, const void *b) {
    const char *x = a, *y = b;
    bool x_default = strstr(x, "/default."), y_default = strstr(y, "/default.");

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
        char *n = wallpapers[i].name;

        snprintf(n, sizeof(wallpapers[i].name), "%s", strrchr(wallpapers[i].path, '/') + 1);
        n[strcspn(n, ".")] = '\0';
        n[0] = n[0] >= 'a' && n[0] <= 'z' ? n[0] - 32 : n[0];
        if (gui_image_load(wallpapers[i].path, &full)) {
            gui_image_scale(&full, THUMB_WIDTH, THUMB_HEIGHT, &wallpapers[i].thumb);
            gui_image_free(&full);
        }
    }
    gui_wallpaper_get(wallpaper);
}

// The zones whose names have the search in them, keeping the chosen one chosen if it still is.
static void filter(void) {
    const char *was = shown_count ? shown[chosen] : zone;
    size_t n = strlen(search);

    shown_count = chosen = 0;
    for (int i = 0; i < zone_count; i++) {
        bool match = !n;

        for (const char *p = zones[i]; *p && !match; p++)
            match = !strncasecmp(p, search, n);
        if (!match)
            continue;
        if (!strcmp(zones[i], was))
            chosen = shown_count;
        shown[shown_count++] = zones[i];
    }
}

// Says where a widget is when that changes, for the tests to find it.
static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[8 + MAX_WALLPAPERS];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)))
        printf("login: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

static void go_to(struct gui_ui *ui, enum page to) {
    page = to;
    message[0] = '\0';
    password[0] = again[0] = '\0';
    gui_resize(window, sizes[page][0], sizes[page][1]);
    gui_set_focus(ui, page == LOCKED ? "##password" : page == WELCOME ? "Theme" : page == TIME ? "##search" : "##name", NULL);
    ui->again = true;
    printf("login: %s, %dx%d\n", page_names[page], sizes[page][0], sizes[page][1]);
}

static void welcome(struct gui_ui *ui) {
    int theme = settings.light;

    gui_heading(ui, "Welcome to AximOS");
    gui_label(ui, "Choose how your desktop looks.");
    if (gui_dropdown(ui, "Theme", themes, 2, &theme)) {
        settings.light = theme;
        if (gui_settings_save(&settings))
            gui_settings_changed();
        printf("login: theme %s\n", themes[theme]);
    }
    report(ui, 0, "theme");
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
            printf("login: wallpaper %s\n", wallpaper);
            gui_settings_changed();
        }
        snprintf(label, sizeof(label), "wallpaper %s", wallpapers[i].name);
        report(ui, 8 + i, label);
    }
    if (gui_button(ui, "Next"))
        go_to(ui, TIME);
    report(ui, 1, "next");
}

// The time zone, found by searching, and whether the clock shows 12 or 24 hours.
static void time_zone(struct gui_ui *ui) {
    int clock = settings.clock_12_hour;
    bool next;

    gui_heading(ui, "Where are you?");
    gui_label(ui, "Choose your time zone, and how the clock shows the time.");
    if (gui_dropdown(ui, "Clock", clocks, 2, &clock)) {
        settings.clock_12_hour = clock;
        if (gui_settings_save(&settings))
            gui_settings_changed();
        printf("login: clock %s\n", clocks[clock]);
    }
    report(ui, 5, "clock");
    if (gui_textbox(ui, "##search", search, sizeof(search)))
        filter();
    report(ui, 6, "search");
    next = gui_list(ui, "##zones", shown, shown_count, &chosen, 9) == GUI_LIST_ACTIVATED;
    report(ui, 7, "zones");
    if (message[0])
        gui_label(ui, "%s", message);
    next |= gui_button(ui, "Next");
    report(ui, 1, "next");
    if (!next)
        return;
    if (!shown_count || !gui_time_zone_set(shown[chosen])) {
        snprintf(message, sizeof(message), shown_count ? "Couldn't set the time zone." : "No time zone has that in its name.");
        return;
    }
    printf("login: zone %s\n", shown[chosen]);
    gui_settings_changed(); // for the taskbar's clock
    go_to(ui, CREATE);
}

// Returns whether the account was made.
static bool create(struct gui_ui *ui) {
    bool submit = gui_key(ui, KEY_ENTER);

    gui_heading(ui, "Create your account");
    gui_label(ui, "Username");
    gui_textbox(ui, "##name", name, sizeof(name));
    gui_label(ui, "Password");
    gui_password(ui, "##password", password, sizeof(password));
    gui_label(ui, "Password again");
    gui_password(ui, "##again", again, sizeof(again));
    if (message[0])
        gui_label(ui, "%s", message);
    if (gui_button(ui, "Back")) {
        go_to(ui, TIME);
        return false;
    }
    report(ui, 2, "back");
    gui_same_line(ui);
    submit |= gui_button(ui, "Create account");
    report(ui, 3, "create");
    if (!submit)
        return false;

    const char *problem = !account_name_ok(name) ? "A username is letters, digits, dots, dashes and underscores."
                        : !password[0]           ? "Choose a password."
                        : strcmp(password, again)  ? "The passwords aren't the same."
                                                   : NULL;

    if (!problem && !account_save(&account, name, password))
        problem = "Couldn't save the account.";
    if (problem) {
        snprintf(message, sizeof(message), "%s", problem);
        printf("login: %s\n", problem);
        password[0] = again[0] = '\0';
        gui_set_focus(ui, account_name_ok(name) ? "##password" : "##name", name);
        return false;
    }
    printf("login: created %s\n", account.name);
    return true;
}

// Returns whether the password, and on the log-in page the name, were right.
static bool log_in(struct gui_ui *ui) {
    bool submit = gui_key(ui, KEY_ENTER);

    gui_heading(ui, page == LOCKED ? "Locked" : "Log in");
    if (page == LOCKED) {
        gui_label(ui, "%s is logged in.", account.name);
    } else {
        gui_label(ui, "Username");
        gui_textbox(ui, "##name", name, sizeof(name));
    }
    gui_label(ui, "Password");
    gui_password(ui, "##password", password, sizeof(password));
    if (message[0])
        gui_label(ui, "%s", message);
    submit |= gui_button(ui, page == LOCKED ? "Unlock" : "Log in");
    report(ui, 4, "submit");
    if (!submit)
        return false;

    // The name's checked after the password, so a wrong one takes as long.
    bool right = account_check(&account, password) && (page == LOCKED || !strcmp(name, account.name));

    password[0] = '\0';
    if (!right) {
        snprintf(message, sizeof(message), page == LOCKED ? "That password isn't right." : "Wrong username or password.");
        printf("login: wrong\n");
        gui_set_focus(ui, "##password", NULL);
        return false;
    }
    printf("login: logged in %s\n", account.name);
    return true;
}

static bool frame(struct gui_ui *ui) {
    bool done = false;

    gui_ui_begin(ui);
    if (page == WELCOME)
        welcome(ui);
    else if (page == TIME)
        time_zone(ui);
    else if (page == CREATE)
        done = create(ui);
    else
        done = log_in(ui);
    gui_ui_end(ui);
    fflush(stdout);
    return done;
}

// What wm shows until someone logs in: started by wm, and nothing else can unlock it.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: login [--lock]\n"
                            "The log-in screen, which wm starts. The first time, it sets up the desktop, the time\n"
                            "zone and an account.\n");
    if (!getenv("GUI_FD") || !gui_connect()) {
        dprintf(STDERR_FILENO, "login: only wm starts this\n");
        return 1;
    }

    struct gui_ui ui;
    struct gui_message e;
    bool done = false;

    page = !account_load(&account) ? WELCOME : argc > 1 && !strcmp(argv[1], "--lock") ? LOCKED : LOG_IN;
    if (page == WELCOME) {
        gui_settings_load(&settings);
        load_wallpapers();
        gui_time_zone_get(zone);
        zone_count = gui_time_zones(&zones);
        shown = calloc(zone_count + 1, sizeof(*shown));
        filter();
    }
    if (!(window = gui_create_window("Log in", sizes[page][0], sizes[page][1]))) {
        dprintf(STDERR_FILENO, "login: couldn't make its window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    go_to(&ui, page);
    frame(&ui);
    while (!done && gui_next_event(&e)) {
        if (gui_ui_event(&ui, &e))
            done = frame(&ui);
    }
    if (done)
        gui_send(gui_fd(), &(struct gui_message){.type = GUI_UNLOCK});
    return !done;
}
