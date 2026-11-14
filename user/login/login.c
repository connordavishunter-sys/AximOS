#include <dirent.h>
#include <gui.h>
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The one account, as its name, a random salt and the password's hash, in hex.
#define ACCOUNT "/system/etc/account"
#define SALT 16
#define HASH 32
#define ROUNDS 50000 // of SHA-256, so guessing is slow
#define NAME_SIZE 32
#define MAX_WALLPAPERS 12
#define THUMB_WIDTH 136
#define THUMB_HEIGHT 85
#define THUMBS_ACROSS 3

// The first time, it asks how the desktop should look and then for an account.
// After that, it logs in, or unlocks for whoever's logged in.
enum page { WELCOME, CREATE, LOG_IN, LOCKED };
static const char *const page_names[] = {"welcome", "create", "log in", "locked"};
static const int sizes[][2] = {{480, 410}, {360, 290}, {360, 230}, {360, 200}};
static const char *const themes[] = {"Dark", "Light"};

static enum page page;
static struct gui_window *window;
static char name[NAME_SIZE], password[128], again[128], message[96];
static struct {
    char name[NAME_SIZE];
    uint8_t salt[SALT], hash[HASH];
} account;
static struct gui_settings settings;
static struct {
    char path[GUI_PATH_MAX], name[32];
    struct gui_image thumb;
} wallpapers[MAX_WALLPAPERS];
static int wallpaper_count;
static char wallpaper[GUI_PATH_MAX];

static void hash(const uint8_t salt[SALT], const char *secret, uint8_t out[HASH]) {
    uint8_t round[HASH + SALT];
    size_t length = strlen(secret);
    uint8_t *first = malloc(SALT + length);

    memcpy(first, salt, SALT);
    memcpy(first + SALT, secret, length);
    mbedtls_sha256(first, SALT + length, out, 0);
    free(first);
    memcpy(round + HASH, salt, SALT);
    for (int i = 1; i < ROUNDS; i++) {
        memcpy(round, out, HASH);
        mbedtls_sha256(round, sizeof(round), out, 0);
    }
}

static void to_hex(const uint8_t *bytes, int count, char *out) {
    for (int i = 0; i < count; i++)
        sprintf(out + 2 * i, "%02x", bytes[i]);
}

static bool from_hex(const char *hex, uint8_t *out, int count) {
    for (int i = 0; i < count; i++) {
        unsigned byte;

        if (sscanf(hex + 2 * i, "%2x", &byte) != 1)
            return false;
        out[i] = (uint8_t)byte;
    }
    return strlen(hex) == (size_t)count * 2;
}

static bool load_account(void) {
    FILE *f = fopen(ACCOUNT, "r");
    char salt[2 * SALT + 1], digest[2 * HASH + 1];
    bool ok = f && fscanf(f, "%31s %32s %64s", account.name, salt, digest) == 3 && from_hex(salt, account.salt, SALT)
           && from_hex(digest, account.hash, HASH);

    if (f)
        fclose(f);
    return ok;
}

static bool save_account(void) {
    char salt[2 * SALT + 1], digest[2 * HASH + 1];
    FILE *f;

    snprintf(account.name, sizeof(account.name), "%s", name);
    if (getentropy(account.salt, SALT) < 0)
        return false;
    hash(account.salt, password, account.hash);
    to_hex(account.salt, SALT, salt);
    to_hex(account.hash, HASH, digest);
    if (!(f = fopen(ACCOUNT, "w")))
        return false;
    fprintf(f, "%s %s %s\n", account.name, salt, digest);
    return fclose(f) == 0;
}

// Compares every byte, so how long it takes says nothing about how close it was.
static bool password_right(void) {
    uint8_t tried[HASH], differ = 0;

    hash(account.salt, password, tried);
    for (int i = 0; i < HASH; i++)
        differ |= tried[i] ^ account.hash[i];
    return !differ;
}

static bool name_ok(const char *s) {
    for (const char *c = s; *c; c++) {
        if (!(*c >= 'a' && *c <= 'z') && !(*c >= 'A' && *c <= 'Z') && !(*c >= '0' && *c <= '9') && !strchr("._-", *c))
            return false;
    }
    return *s;
}

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
    gui_set_focus(ui, page == LOCKED ? "##password" : page == WELCOME ? "Theme" : "##name", NULL);
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
        go_to(ui, CREATE);
    report(ui, 1, "next");
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
        go_to(ui, WELCOME);
        return false;
    }
    report(ui, 2, "back");
    gui_same_line(ui);
    submit |= gui_button(ui, "Create account");
    report(ui, 3, "create");
    if (!submit)
        return false;

    const char *problem = !name_ok(name)        ? "A username is letters, digits, dots, dashes and underscores."
                        : !password[0]          ? "Choose a password."
                        : strcmp(password, again) ? "The passwords aren't the same."
                                                  : NULL;

    if (!problem && !save_account())
        problem = "Couldn't save the account.";
    if (problem) {
        snprintf(message, sizeof(message), "%s", problem);
        printf("login: %s\n", problem);
        password[0] = again[0] = '\0';
        gui_set_focus(ui, name_ok(name) ? "##password" : "##name", name);
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
    bool right = password_right() && (page == LOCKED || !strcmp(name, account.name));

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
                            "The log-in screen, which wm starts. The first time, it sets up the desktop and an account.\n");
    if (!getenv("GUI_FD") || !gui_connect()) {
        dprintf(STDERR_FILENO, "login: only wm starts this\n");
        return 1;
    }

    struct gui_ui ui;
    struct gui_message e;
    bool done = false;

    page = !load_account() ? WELCOME : argc > 1 && !strcmp(argv[1], "--lock") ? LOCKED : LOG_IN;
    if (page == WELCOME) {
        gui_settings_load(&settings);
        load_wallpapers();
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
