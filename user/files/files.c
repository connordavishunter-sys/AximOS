#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 1024
#define MAX_TABS 8
#define EDITOR "/system/bin/gui/editor"
#define TERM "/system/bin/gui/term"
#define LIST "##entries"
#define ADDRESS "##folder"
#define ANSWER "##answer"
#define SEARCH "Search##search"
#define PLACES "##places"
#define SIDEBAR 120
#define CUT "/tmp/files-cut" // the path on the clipboard, when it was cut, to move it
#define KEY_F2 (KEY_F1 + 1)
#define KEY_F5 (KEY_F1 + 4)
#define DRAG_DISTANCE 6 // the mouse goes, held on an item, before it's dragged

struct entry {
    char name[NAME_MAX + 1];
    bool folder;
    off_t size;
    time_t modified;
};

static const char *const help = "Enter         Open\n"
                                "Backspace     Up a folder\n"
                                "Ctrl+T, W     New tab, close the tab\n"
                                "Ctrl+Tab      Next tab\n"
                                "Ctrl+C, X, V  Copy, cut, paste\n"
                                "F2            Rename\n"
                                "Delete        Delete\n"
                                "F5            Refresh\n"
                                "Ctrl+F        Search this folder\n"
                                "Ctrl+H        Show hidden files\n"
                                "Ctrl+Q        Quit\n"
                                "F1            This help";

// Entries are listed folders first, then by name, size or when they changed, the
// latter two biggest and newest first, leaving out those that start with a dot
// unless they're shown, and those without what's searched for in their names.
static enum { BY_NAME, BY_SIZE, BY_DATE } sort_by;
static bool show_hidden;
static char search[NAME_MAX + 1];

// The places down the side.
static const char *const place_names[] = {"Home", "Desktop", "Documents", "Pictures", "Computer"};
static char place_paths[5][PATH_MAX];

// Each tab is a folder, and what was selected in it. The one shown is listed.
struct tab {
    char folder[PATH_MAX], selected[NAME_MAX + 1], name[NAME_MAX + 1];
};

static struct tab tabs[MAX_TABS];
static const char *tab_names[MAX_TABS];
static int tab_count, current;

static struct entry entries[MAX_ENTRIES];
static char labels[MAX_ENTRIES][NAME_MAX + 2]; // names, with / after folders
static const char *items[MAX_ENTRIES];
static const struct gui_image *icons[MAX_ENTRIES];
static int count, selected;
static char *folder = tabs[0].folder, typed[PATH_MAX], status[PATH_MAX + 64];
static bool quitting;

// In the grid, each entry has a picture: a picture's own, made small, or its icon
// made bigger, kept for each icon.
#define BIG_ICON 48
static bool grid;
static struct gui_image thumbnails[MAX_ENTRIES];
static const struct gui_image *pictures[MAX_ENTRIES];
static struct {
    const struct gui_image *icon;
    struct gui_image big;
} big_icons[8];

// A file dropped on the window, for the frame to put where it was dropped.
static char dropped[PATH_MAX];
static bool drop_copies;

// What the strip under the address bar is asking, if anything.
static enum { NOTHING, RENAMING, NEW_FOLDER, NEW_FILE, DELETING, PROPERTIES } asking;
static char answer[NAME_MAX + 1];
static bool ask_focus; // the answer box, or the Delete button, gets the keyboard

static void path_of(char *out, const char *name) {
    snprintf(out, PATH_MAX, "%s%s%s", folder, strcmp(folder, "/") ? "/" : "", name);
}

static int compare_entries(const void *a, const void *b) {
    const struct entry *x = a, *y = b;

    if (x->folder != y->folder)
        return y->folder - x->folder;
    if (sort_by == BY_SIZE && x->size != y->size)
        return x->size < y->size ? 1 : -1;
    if (sort_by == BY_DATE && x->modified != y->modified)
        return x->modified < y->modified ? 1 : -1;
    return strcmp(x->name, y->name);
}

static bool has(const char *name, const char *part) {
    for (size_t n = strlen(part); *name; name++) {
        size_t i = 0;

        while (i < n && tolower((unsigned char)name[i]) == tolower((unsigned char)part[i]))
            i++;
        if (i == n)
            return true;
    }
    return !*part;
}

static void describe(void) {
    const struct entry *e = &entries[selected];

    if (!count)
        snprintf(status, sizeof(status), "%s is empty", folder);
    else if (e->folder)
        snprintf(status, sizeof(status), "%s: folder", e->name);
    else
        snprintf(status, sizeof(status), "%s: %lld bytes", e->name, (long long)e->size);
}

static void name_tab(struct tab *t) {
    const char *slash = strrchr(t->folder, '/');

    snprintf(t->name, sizeof(t->name), "%s", slash && slash[1] ? slash + 1 : "/");
}

static const struct gui_image *big_icon(const struct gui_image *icon) {
    for (int i = 0; icon && i < (int)(sizeof(big_icons) / sizeof(big_icons[0])); i++) {
        if (big_icons[i].icon == icon)
            return &big_icons[i].big;
        if (!big_icons[i].icon && gui_image_scale(icon, BIG_ICON, BIG_ICON, &big_icons[i].big)) {
            big_icons[i].icon = icon;
            return &big_icons[i].big;
        }
    }
    return icon;
}

static void forget_pictures(void) {
    for (int i = 0; i < MAX_ENTRIES; i++) {
        gui_image_free(&thumbnails[i]);
        pictures[i] = NULL;
    }
}

// Pictures for the grid, made small to fit GUI_THUMBNAIL, never bigger than they are.
static void make_pictures(void) {
    forget_pictures();
    for (int i = 0; i < count; i++) {
        char path[PATH_MAX];
        struct gui_image full;

        path_of(path, entries[i].name);
        pictures[i] = big_icon(icons[i]);
        if (icons[i] != gui_icon("image") || !gui_image_load(path, &full))
            continue;

        int big = full.width > full.height ? full.width : full.height;

        if (big <= GUI_THUMBNAIL) {
            thumbnails[i] = full;
            pictures[i] = &thumbnails[i];
            continue;
        }
        if (gui_image_scale(&full, full.width * GUI_THUMBNAIL / big > 0 ? full.width * GUI_THUMBNAIL / big : 1,
                            full.height * GUI_THUMBNAIL / big > 0 ? full.height * GUI_THUMBNAIL / big : 1, &thumbnails[i]))
            pictures[i] = &thumbnails[i];
        gui_image_free(&full);
    }
}

static void set_grid(bool on) {
    grid = on;
    if (grid)
        make_pictures();
    else
        forget_pictures();
    printf("files: view %s\n", grid ? "grid" : "list");
    gui_post(&(struct gui_message){.type = GUI_REDRAW}); // with the button's new name
}

// Lists a folder, going there in this tab, and choosing the entry called select if there is one.
static bool read_folder(const char *path, const char *select) {
    char went[PATH_MAX];
    DIR *dir = opendir(path);
    struct dirent *d;

    if (!dir) {
        snprintf(status, sizeof(status), "Can't open %s: %s", path, strerror(errno));
        return false;
    }
    snprintf(went, sizeof(went), "%s", path); // path may be folder itself
    for (size_t n = strlen(went); n > 1 && went[n - 1] == '/'; n--)
        went[n - 1] = '\0';
    if (strcmp(folder, went))
        search[0] = '\0'; // a search is in one folder
    snprintf(folder, PATH_MAX, "%s", went);
    name_tab(&tabs[current]);
    count = 0;
    while (count < MAX_ENTRIES && (d = readdir(dir))) {
        struct entry *e = &entries[count];
        char full[PATH_MAX];
        struct stat st;

        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, "..") || (d->d_name[0] == '.' && !show_hidden)
            || !has(d->d_name, search))
            continue;
        snprintf(e->name, sizeof(e->name), "%s", d->d_name);
        path_of(full, e->name);
        e->folder = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        e->size = e->folder ? 0 : st.st_size;
        e->modified = st.st_mtime;
        count++;
    }
    closedir(dir);
    qsort(entries, count, sizeof(*entries), compare_entries);

    printf("files: in %s:", folder);
    selected = 0;
    for (int i = 0; i < count; i++) {
        snprintf(labels[i], sizeof(labels[i]), "%s%s", entries[i].name, entries[i].folder ? "/" : "");
        items[i] = labels[i];
        icons[i] = strncmp(folder, "/system/bin", 11) || entries[i].folder ? gui_file_icon(entries[i].name, entries[i].folder)
                                                                         : gui_icon("program");
        printf(" %s", labels[i]);
        if (select && !strcmp(entries[i].name, select))
            selected = i;
    }
    printf("\n");
    snprintf(typed, sizeof(typed), "%s", folder);
    asking = NOTHING;
    describe();
    if (grid)
        make_pictures();
    return true;
}

static void refresh(void) {
    char name[NAME_MAX + 1];

    snprintf(name, sizeof(name), "%s", count ? entries[selected].name : "");
    read_folder(folder, name);
}

static void go_up(void) {
    char parent[PATH_MAX], *slash;

    snprintf(parent, sizeof(parent), "%s", folder);
    slash = strrchr(parent, '/');
    if (!slash || !strcmp(folder, "/"))
        return;
    *slash = '\0';
    read_folder(parent[0] ? parent : "/", slash + 1);
}

// Keeps what's selected in this tab, for when it's shown again.
static void remember(void) {
    if (count)
        snprintf(tabs[current].selected, sizeof(tabs[current].selected), "%s", entries[selected].name);
}

static void show_tab(int index) {
    current = index;
    folder = tabs[current].folder;
    if (!read_folder(folder, tabs[current].selected))
        read_folder("/", NULL);
    printf("files: tab %d of %d\n", current + 1, tab_count);
}

static void new_tab(const char *path) {
    if (tab_count == MAX_TABS) {
        snprintf(status, sizeof(status), "There can be %d tabs at most", MAX_TABS);
        return;
    }
    snprintf(tabs[tab_count].folder, PATH_MAX, "%s", path);
    tabs[tab_count].selected[0] = '\0';
    tab_names[tab_count] = tabs[tab_count].name;
    remember();
    show_tab(tab_count++);
}

// The last tab closing closes the window.
static void close_tab(int index) {
    if (tab_count == 1) {
        quitting = true;
        return;
    }
    remember();
    memmove(&tabs[index], &tabs[index + 1], (tab_count - index - 1) * sizeof(*tabs));
    tab_count--;
    current = current > index || current == tab_count ? current - 1 : current;
    for (int i = 0; i < tab_count; i++)
        tab_names[i] = tabs[i].name;
    show_tab(current);
}

static void start(const char *program, const char *argument, const char *in) {
    if (fork() == 0) {
        if (in && chdir(in) < 0)
            _exit(127);
        execl(program, program, argument, (char *)NULL);
        _exit(127);
    }
}

static const struct entry *chosen(void) {
    return count ? &entries[selected] : NULL;
}

static void open_with(const char *program) {
    char path[PATH_MAX];

    path_of(path, chosen()->name);
    printf("files: opening %s\n", path);
    start(program, path, NULL);
}

// Folders open here, pictures in the viewer, sounds in the player and other files
// in the editor.
static void open_selected(void) {
    char path[PATH_MAX];

    if (!chosen())
        return;
    path_of(path, chosen()->name);
    if (chosen()->folder) {
        read_folder(path, NULL);
    } else {
        char program[64];

        snprintf(program, sizeof(program), "/system/bin/gui/%s", gui_app_for(path));
        open_with(program);
    }
}

static void run_selected(void) {
    char path[PATH_MAX];

    path_of(path, chosen()->name);
    printf("files: running %s\n", path);
    start(path, NULL, NULL);
}

static void ask(int what) {
    asking = what;
    ask_focus = true;
    snprintf(answer, sizeof(answer), "%s", what == RENAMING ? chosen()->name : "");
}

static void copy_path(void) {
    char path[PATH_MAX];

    path_of(path, chosen()->name);
    gui_clipboard_set(path, strlen(path));
    snprintf(status, sizeof(status), "Copied %s", path);
}

// Puts the selected item on the clipboard, by its path, which pasting copies, or
// moves when it was cut.
static void copy_selected(bool cut) {
    char path[PATH_MAX];
    FILE *f;

    path_of(path, chosen()->name);
    if (!gui_clipboard_set(path, strlen(path)))
        return;
    unlink(CUT);
    if (cut && (f = fopen(CUT, "w"))) {
        fputs(path, f);
        fclose(f);
    }
    snprintf(status, sizeof(status), "%s %s", cut ? "Cut" : "Copied", chosen()->name);
    printf("files: %s %s\n", cut ? "cut" : "copied", path);
}

static bool copy_file(const char *from, const char *to, mode_t mode) {
    char buffer[16384];
    int in = open(from, O_RDONLY), out = in < 0 ? -1 : open(to, O_WRONLY | O_CREAT | O_EXCL, mode & 0777);
    ssize_t n = 0;

    while (out >= 0 && (n = read(in, buffer, sizeof(buffer))) > 0 && write(out, buffer, n) == n)
        ;
    if (in >= 0)
        close(in);
    return out >= 0 && close(out) == 0 && n == 0;
}

// Copies a file, or a folder and all in it.
static bool copy_tree(const char *from, const char *to) {
    struct stat st;
    struct dirent *d;
    DIR *dir;
    bool ok = true;

    if (stat(from, &st) < 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        return copy_file(from, to, st.st_mode);
    if (mkdir(to, st.st_mode & 0777) < 0 || !(dir = opendir(from)))
        return false;
    while (ok && (d = readdir(dir))) {
        char a[PATH_MAX], b[PATH_MAX];

        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        snprintf(a, sizeof(a), "%s/%s", from, d->d_name);
        snprintf(b, sizeof(b), "%s/%s", to, d->d_name);
        ok = copy_tree(a, b);
    }
    closedir(dir);
    return ok;
}

static bool delete_tree(const char *path) {
    struct stat st;
    struct dirent *d;
    DIR *dir;
    bool ok = true;

    if (lstat(path, &st) < 0)
        return false;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0;
    if (!(dir = opendir(path)))
        return false;
    while (ok && (d = readdir(dir))) {
        char inside[PATH_MAX];

        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        snprintf(inside, sizeof(inside), "%s/%s", path, d->d_name);
        ok = delete_tree(inside);
    }
    closedir(dir);
    return ok && rmdir(path) == 0;
}

// A name in a folder that isn't taken: the same, or with (copy), (copy 2) and so on.
static void free_name(const char *into, const char *name, char out[NAME_MAX + 1]) {
    char path[PATH_MAX];
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);

    snprintf(out, NAME_MAX + 1, "%s", name);
    for (int i = 1; snprintf(path, sizeof(path), "%s%s%s", into, strcmp(into, "/") ? "/" : "", out), access(path, F_OK) == 0;
         i++) {
        if (i == 1)
            snprintf(out, NAME_MAX + 1, "%.*s (copy)%s", stem, name, name + stem);
        else
            snprintf(out, NAME_MAX + 1, "%.*s (copy %d)%s", stem, name, i, name + stem);
    }
}

// Copies a file or folder into a folder, or moves it there, with a name that isn't
// taken, saying what it did as done. Returns where it went, or NULL.
static const char *put(const char *from, const char *into, bool moving, const char *done) {
    static char to[PATH_MAX];
    char name[NAME_MAX + 1], here[PATH_MAX];
    const char *base = strrchr(from, '/') ? strrchr(from, '/') + 1 : from;
    size_t length = strlen(from);
    bool ok;

    snprintf(here, sizeof(here), "%s%s%s", into, strcmp(into, "/") ? "/" : "", base);
    if (moving && !strcmp(here, from)) // already there
        return NULL;
    if (!strncmp(into, from, length) && (into[length] == '/' || !into[length])) {
        snprintf(status, sizeof(status), "Can't put %s inside itself", base);
        return NULL;
    }
    free_name(into, base, name);
    snprintf(to, sizeof(to), "%s%s%s", into, strcmp(into, "/") ? "/" : "", name);
    if (moving)
        ok = rename(from, to) == 0 || (errno == EXDEV && copy_tree(from, to) && delete_tree(from));
    else
        ok = copy_tree(from, to);
    if (!ok) {
        snprintf(status, sizeof(status), "Can't put %s in %s: %s", base, into, strerror(errno));
        return NULL;
    }
    printf("files: %s %s to %s\n", done, from, to);
    snprintf(status, sizeof(status), "%c%s %s", toupper((unsigned char)done[0]), done + 1, base);
    return to;
}

// Copies what's on the clipboard here, or moves it if it was cut.
static void paste(void) {
    char *from = gui_clipboard_get(NULL), cut[PATH_MAX] = "";
    struct stat st;
    FILE *f = fopen(CUT, "r");
    const char *to;

    if (f) {
        if (!fgets(cut, sizeof(cut), f))
            cut[0] = '\0';
        fclose(f);
    }
    if (!from || from[0] != '/' || stat(from, &st) < 0) {
        snprintf(status, sizeof(status), "There's nothing to paste");
        free(from);
        return;
    }

    bool moving = !strcmp(cut, from);

    if ((to = put(from, folder, moving, moving ? "moved" : "pasted"))) {
        if (moving) {
            unlink(CUT);
            gui_clipboard_set(to, strlen(to)); // where it is now, to paste copies of
        }
        read_folder(folder, strrchr(to, '/') + 1);
    }
    free(from);
}

// A file dropped on a folder, or a tab, goes there, and on the rest of the window
// it comes here, if it isn't here already. It's moved, or copied with Ctrl held.
static void take_drop(struct gui_ui *ui) {
    char into[PATH_MAX], from_folder[PATH_MAX];
    const char *to;

    snprintf(from_folder, sizeof(from_folder), "%.*s", (int)(strrchr(dropped, '/') - dropped), dropped);
    if (ui->drop_tab >= 0)
        snprintf(into, sizeof(into), "%s", tabs[ui->drop_tab].folder);
    else if (ui->drop_item >= 0 && entries[ui->drop_item].folder)
        path_of(into, entries[ui->drop_item].name);
    else
        snprintf(into, sizeof(into), "%s", folder);
    if ((!strcmp(into, folder) && !strcmp(from_folder[0] ? from_folder : "/", folder) && !drop_copies)
        || !strcmp(into, dropped))
        printf("files: dropped %s where it was\n", dropped);
    else if ((to = put(dropped, into, !drop_copies, drop_copies ? "copied" : "moved")))
        refresh();
    dropped[0] = '\0';
}

static void delete_selected(void) {
    char path[PATH_MAX];

    path_of(path, chosen()->name);
    if (!delete_tree(path)) {
        snprintf(status, sizeof(status), "Can't delete %s: %s", chosen()->name, strerror(errno));
        asking = NOTHING;
        return;
    }
    printf("files: deleted %s\n", path);
    read_folder(folder, NULL);
}

// Does what the strip asked with the name it was given.
static void answered(void) {
    char path[PATH_MAX], from[PATH_MAX];
    int fd = -1;

    if (asking == DELETING) {
        delete_selected();
        return;
    }
    if (!answer[0] || strchr(answer, '/')) {
        snprintf(status, sizeof(status), "Type a name, without a /");
        return;
    }
    path_of(path, answer);
    if (asking == RENAMING) {
        path_of(from, chosen()->name);
        if (rename(from, path) < 0) {
            snprintf(status, sizeof(status), "Can't rename %s: %s", chosen()->name, strerror(errno));
            return;
        }
        printf("files: renamed %s to %s\n", from, path);
    } else if (asking == NEW_FOLDER ? mkdir(path, 0755) < 0
                                    : (fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644)) < 0 || close(fd) < 0) {
        snprintf(status, sizeof(status), "Can't make %s: %s", answer, strerror(errno));
        return;
    } else {
        printf("files: made %s\n", path);
    }
    read_folder(folder, answer);
}

// Says where a widget is when that changes, for the tests to find it.
static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[6];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)))
        printf("files: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

static void address_bar(struct gui_ui *ui) {
    if (gui_button(ui, "Up"))
        go_up();
    report(ui, 0, "up");
    gui_same_line(ui);
    if (gui_button(ui, "Go"))
        read_folder(typed, NULL);
    gui_same_line(ui);
    if (gui_button(ui, grid ? "List" : "Grid"))
        set_grid(!grid);
    report(ui, 3, "view");
    gui_same_line(ui);
    gui_column_begin(ui, gui_room(ui) - 180);
    gui_textbox(ui, ADDRESS, typed, sizeof(typed));
    report(ui, 1, "address");
    gui_column_end(ui);
    if (gui_focused(ui, ADDRESS) && gui_key(ui, KEY_ENTER))
        read_folder(typed, NULL);

    // Searching lists only what has the words in its name.
    if (gui_shortcut(ui, 'f'))
        gui_set_focus(ui, SEARCH, search);
    if (gui_textbox(ui, SEARCH, search, sizeof(search))) {
        refresh();
        printf("files: searching for %s\n", search);
    }
}

static void set_sort(int by) {
    static const char *const names[] = {"name", "size", "date"};

    sort_by = by;
    refresh();
    printf("files: sorted by %s\n", names[by]);
}

static void toggle_hidden(void) {
    show_hidden = !show_hidden;
    refresh();
    printf("files: hidden files %s\n", show_hidden ? "shown" : "hidden");
}

// Places down the side, the one this folder is highlighted, and how the folder's
// listed under them.
static void sidebar(struct gui_ui *ui) {
    static const char *const sorts[] = {"By name", "By size", "By date"};
    int by = sort_by;
    bool hidden = show_hidden;
    const struct gui_image *place_icons[5];
    int at = -1;

    for (int i = 0; i < 5; i++) {
        place_icons[i] = gui_file_icon("", true);
        if (!strcmp(place_paths[i], folder))
            at = i;
    }
    gui_column_begin(ui, SIDEBAR);
    if (gui_list_icons(ui, PLACES, place_names, place_icons, 5, &at, 5) && at >= 0) {
        remember();
        read_folder(place_paths[at], NULL);
    }
    if (gui_dropdown(ui, "##sort", sorts, 3, &by))
        set_sort(by);
    report(ui, 4, "sort");
    if (gui_checkbox(ui, "Hidden", &hidden))
        toggle_hidden();
    report(ui, 5, "hidden");
    gui_column_end(ui);
}

// Asks for a name under the address bar.
static void asking_strip(struct gui_ui *ui) {
    static const char *const questions[] = {
        [RENAMING] = "Rename to", [NEW_FOLDER] = "New folder", [NEW_FILE] = "New file",
    };
    bool yes, no;

    if (ask_focus)
        gui_set_focus(ui, ANSWER, answer);
    ask_focus = false;
    gui_label(ui, "%s:", questions[asking]);
    gui_same_line(ui);
    yes = gui_button(ui, "OK");
    gui_same_line(ui);
    no = gui_button(ui, "Cancel");
    gui_same_line(ui);
    gui_textbox(ui, ANSWER, answer, sizeof(answer));
    yes |= gui_focused(ui, ANSWER) && gui_key(ui, KEY_ENTER);
    if (no || gui_key(ui, KEY_ESCAPE))
        asking = NOTHING;
    else if (yes)
        answered();
}

// How much is in a folder, all the way down, and how many files.
static off_t size_of_tree(const char *path, int *files) {
    struct stat st;
    struct dirent *d;
    DIR *dir;
    off_t total = 0;

    if (lstat(path, &st) < 0)
        return 0;
    if (!S_ISDIR(st.st_mode)) {
        ++*files;
        return st.st_size;
    }
    if (!(dir = opendir(path)))
        return 0;
    while ((d = readdir(dir))) {
        char inside[PATH_MAX];

        if (strcmp(d->d_name, ".") && strcmp(d->d_name, "..")) {
            snprintf(inside, sizeof(inside), "%s/%s", path, d->d_name);
            total += size_of_tree(inside, files);
        }
    }
    closedir(dir);
    return total;
}

static void human_size(char *out, size_t n, off_t size) {
    if (size < 1024)
        snprintf(out, n, "%lld bytes", (long long)size);
    else if (size < 1024 * 1024)
        snprintf(out, n, "%.1f kB (%lld bytes)", size / 1024.0, (long long)size);
    else
        snprintf(out, n, "%.1f MB (%lld bytes)", size / (1024.0 * 1024), (long long)size);
}

// What's known about the selected item, worked out once when it's asked for.
static void describe_properties(char *out, size_t n) {
    const struct entry *e = chosen();
    char path[PATH_MAX], size[64], when[32], kind[64];
    struct stat st;
    int files = 0;

    path_of(path, e->name);
    if (stat(path, &st) < 0) {
        snprintf(out, n, "Can't look at %s: %s", e->name, strerror(errno));
        return;
    }
    human_size(size, sizeof(size), e->folder ? size_of_tree(path, &files) : st.st_size);
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M", localtime(&st.st_mtime));
    if (e->folder)
        snprintf(kind, sizeof(kind), "Folder of %d files", files);
    else
        snprintf(kind, sizeof(kind), "%s", icons[selected] == gui_icon("image")   ? "Picture"
                                         : icons[selected] == gui_icon("audio") ? "Sound"
                                         : st.st_mode & 0111                    ? "Program"
                                                                                : "File");
    snprintf(out, n, "Name:      %s\nKind:      %s\nSize:      %s\nChanged:   %s\nIn:        %s\nMode:      %c%c%c%c%c%c%c%c%c",
             e->name, kind, size, when, folder, st.st_mode & 0400 ? 'r' : '-', st.st_mode & 0200 ? 'w' : '-',
             st.st_mode & 0100 ? 'x' : '-', st.st_mode & 040 ? 'r' : '-', st.st_mode & 020 ? 'w' : '-',
             st.st_mode & 010 ? 'x' : '-', st.st_mode & 04 ? 'r' : '-', st.st_mode & 02 ? 'w' : '-',
             st.st_mode & 01 ? 'x' : '-');
    printf("files: properties of %s: %s\n", path, size);
}

// Deleting and properties are dialogs over the window.
static void dialogs(struct gui_ui *ui) {
    static const char *const delete_buttons[] = {"Delete", "Cancel"}, *const ok[] = {"OK"};
    static char text[PATH_MAX + 512];
    static int shown = NOTHING;

    if (asking != DELETING && asking != PROPERTIES) {
        shown = NOTHING;
        return;
    }
    if (shown != (int)asking && asking == PROPERTIES)
        describe_properties(text, sizeof(text));
    else if (shown != (int)asking)
        snprintf(text, sizeof(text), "Delete %s%s?\nIt can't be undone.", chosen()->name,
                 chosen()->folder ? " and everything in it" : "");
    shown = asking;

    int button = gui_dialog(ui, text, asking == DELETING ? delete_buttons : ok, asking == DELETING ? 2 : 1);

    if (button == 1 && asking == DELETING)
        delete_selected();
    else if (button)
        asking = NOTHING;
}

// Right-clicking an item has what can be done with it, and anywhere else in the
// list what can be done in the folder.
static void context_menu(struct gui_ui *ui, struct gui_rect list) {
    if (!gui_context_menu(ui, "##menu", list))
        return;

    const struct entry *e = ui->clicked_item >= 0 ? chosen() : NULL;
    char path[PATH_MAX];

    if (e) {
        if (gui_menu_item(ui, "Open"))
            open_selected();
        if (e->folder && gui_menu_item(ui, "Open in new tab")) {
            path_of(path, e->name);
            new_tab(path);
        }
        if (!e->folder && gui_menu_item(ui, "Open in Editor"))
            open_with(EDITOR);
        if (!e->folder && gui_menu_item(ui, "Run"))
            run_selected();
        gui_menu_separator(ui);
        if (gui_menu_item(ui, "Cut"))
            copy_selected(true);
        if (gui_menu_item(ui, "Copy"))
            copy_selected(false);
        if (gui_menu_item(ui, "Copy path"))
            copy_path();
        if (gui_menu_item(ui, "Rename"))
            ask(RENAMING);
        if (gui_menu_item(ui, "Delete"))
            ask(DELETING);
        gui_menu_separator(ui);
        if (gui_menu_item(ui, "Properties"))
            ask(PROPERTIES);
    } else {
        if (gui_menu_item(ui, "New folder"))
            ask(NEW_FOLDER);
        if (gui_menu_item(ui, "New file"))
            ask(NEW_FILE);
        if (gui_menu_item(ui, "Paste"))
            paste();
        gui_menu_separator(ui);
        if (gui_menu_item(ui, "Open terminal here"))
            start(TERM, NULL, folder);
        if (gui_menu_item(ui, "Refresh"))
            refresh();
    }
    gui_menu_end(ui);
}

// Keys for the list, while it has the keyboard.
static void list_keys(struct gui_ui *ui) {
    if (!gui_focused(ui, LIST) || asking)
        return;
    if (gui_key(ui, KEY_BACKSPACE))
        go_up();
    else if (gui_key(ui, KEY_F5))
        refresh();
    else if (chosen() && gui_key(ui, KEY_F2))
        ask(RENAMING);
    else if (chosen() && gui_key(ui, KEY_DELETE))
        ask(DELETING);
    else if (chosen() && (gui_shortcut(ui, 'c') || gui_shortcut(ui, 'x')))
        copy_selected(gui_shortcut(ui, 'x'));
    else if (gui_shortcut(ui, 'v'))
        paste();
}

// Dragging an item takes it to another window, through wm, to open it there.
static void drag(struct gui_ui *ui, struct gui_rect list) {
    static bool may_drag;
    static int from_x, from_y;
    char path[PATH_MAX];

    if (ui->mouse_pressed && ui->clicked_item >= 0 && ui->mouse_x >= list.x && ui->mouse_y >= list.y
        && ui->mouse_x < list.x + list.width && ui->mouse_y < list.y + list.height) {
        may_drag = true;
        from_x = ui->mouse_x;
        from_y = ui->mouse_y;
    }
    if (!ui->mouse_down)
        may_drag = false;
    if (!may_drag || !chosen() || abs(ui->mouse_x - from_x) + abs(ui->mouse_y - from_y) < DRAG_DISTANCE)
        return;
    may_drag = false;
    path_of(path, chosen()->name);
    if (gui_drag_file(path))
        printf("files: dragging %s\n", path);
}

static void frame(struct gui_ui *ui) {
    int active = current, closing = 0, result;

    while (waitpid(-1, NULL, WNOHANG) > 0)
        ; // programs it started that have finished
    gui_ui_begin(ui);
    if (asking == DELETING || asking == PROPERTIES)
        dialogs(ui);
    else
        gui_help(ui, help);
    if (gui_shortcut(ui, 'h'))
        toggle_hidden();
    result = gui_tabs(ui, tab_names, tab_count, &active, &closing);
    if (result == GUI_TABS_CHANGED) {
        remember();
        show_tab(active);
    } else if (result == GUI_TABS_NEW)
        new_tab(folder);
    else if (result == GUI_TABS_CLOSE)
        close_tab(closing);
    address_bar(ui);
    if (asking)
        asking_strip(ui);
    gui_label(ui, "%s", status);
    sidebar(ui);

    result = grid ? gui_grid(ui, LIST, items, pictures, count, &selected)
                  : gui_list_icons(ui, LIST, items, icons, count, &selected, 0);

    struct gui_rect list = gui_last_rect(ui);

    report(ui, 2, "list");
    drag(ui, list);
    if (result == GUI_LIST_ACTIVATED)
        open_selected();
    else if (result == GUI_LIST_CHANGED && (asking == RENAMING || asking == DELETING))
        asking = NOTHING; // they were about the one that was selected
    if (result == GUI_LIST_CHANGED)
        describe();
    if (ui->dropping && dropped[0])
        take_drop(ui);
    list_keys(ui);
    context_menu(ui, list);
    gui_ui_end(ui);
    fflush(stdout);
}

// A window showing what's in folders, in tabs, to go about them and open what's there.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: files [folder]\n"
                            "Opens a window showing what's in the folder, or the home folder.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "files: wm isn't running\n");
        return 1;
    }

    struct gui_window *window = gui_create_window("Files", 560, 440);
    struct gui_ui ui;
    struct gui_message e;

    if (!window) {
        dprintf(STDERR_FILENO, "files: couldn't make a window\n");
        return 1;
    }
    tab_count = 1;
    tab_names[0] = tabs[0].name;
    const char *home = getenv("HOME");

    for (int i = 0; i < 4; i++)
        snprintf(place_paths[i], PATH_MAX, "%s%s%s", home ? home : "", i ? "/" : "", i ? place_names[i] : "");
    snprintf(place_paths[4], PATH_MAX, "/");

    if (!read_folder(argc > 1 ? argv[1] : home ? home : "/", NULL))
        read_folder("/", NULL);
    gui_ui_init(&ui, window);
    frame(&ui);
    while (!quitting && gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (gui_dropped_file(&e, dropped)) {
            drop_copies = e.key.modifiers & KEY_MOD_CTRL;
            gui_ui_drop(&ui, e.x, e.y);
            frame(&ui);
        }
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
