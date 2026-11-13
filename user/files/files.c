#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ENTRIES 1024
#define MAX_TABS 8
#define EDITOR "/system/bin/gui/editor"
#define VIEWER "/system/bin/gui/viewer"
#define PLAYER "/system/bin/gui/player"
#define TERM "/system/bin/gui/term"
#define LIST "##entries"
#define ADDRESS "##folder"
#define ANSWER "##answer"
#define CUT "/tmp/files-cut" // the path on the clipboard, when it was cut, to move it
#define KEY_F2 (KEY_F1 + 1)
#define KEY_F5 (KEY_F1 + 4)

struct entry {
    char name[NAME_MAX + 1];
    bool folder;
    off_t size;
};

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

// What the strip under the address bar is asking, if anything.
static enum { NOTHING, RENAMING, NEW_FOLDER, NEW_FILE, DELETING } asking;
static char answer[NAME_MAX + 1];
static bool ask_focus; // the answer box, or the Delete button, gets the keyboard

static void path_of(char *out, const char *name) {
    snprintf(out, PATH_MAX, "%s%s%s", folder, strcmp(folder, "/") ? "/" : "", name);
}

// Folders first, then by name.
static int compare_entries(const void *a, const void *b) {
    const struct entry *x = a, *y = b;

    return x->folder != y->folder ? y->folder - x->folder : strcmp(x->name, y->name);
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
    snprintf(folder, PATH_MAX, "%s", went);
    name_tab(&tabs[current]);
    count = 0;
    while (count < MAX_ENTRIES && (d = readdir(dir))) {
        struct entry *e = &entries[count];
        char full[PATH_MAX];
        struct stat st;

        if (!strcmp(d->d_name, ".") || !strcmp(d->d_name, ".."))
            continue;
        snprintf(e->name, sizeof(e->name), "%s", d->d_name);
        path_of(full, e->name);
        e->folder = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
        e->size = e->folder ? 0 : st.st_size;
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
    if (chosen()->folder)
        read_folder(path, NULL);
    else
        open_with(icons[selected] == gui_icon("image")   ? VIEWER
                  : icons[selected] == gui_icon("audio") ? PLAYER
                                                         : EDITOR);
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

// A name in the folder that isn't taken: the same, or with (copy), (copy 2) and so on.
static void free_name(const char *name, char out[NAME_MAX + 1]) {
    char path[PATH_MAX];
    const char *dot = strrchr(name, '.');
    int stem = dot && dot != name ? (int)(dot - name) : (int)strlen(name);

    snprintf(out, NAME_MAX + 1, "%s", name);
    for (int i = 1; path_of(path, out), access(path, F_OK) == 0; i++) {
        if (i == 1)
            snprintf(out, NAME_MAX + 1, "%.*s (copy)%s", stem, name, name + stem);
        else
            snprintf(out, NAME_MAX + 1, "%.*s (copy %d)%s", stem, name, i, name + stem);
    }
}

// Copies what's on the clipboard here, or moves it if it was cut.
static void paste(void) {
    char *from = gui_clipboard_get(NULL), cut[PATH_MAX] = "", name[NAME_MAX + 1], to[PATH_MAX];
    const char *base;
    struct stat st;
    FILE *f = fopen(CUT, "r");
    size_t length = from ? strlen(from) : 0;
    bool moving, ok;

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
    moving = !strcmp(cut, from);
    base = strrchr(from, '/') + 1;
    path_of(to, base);
    if (moving && !strcmp(to, from)) { // already here
        free(from);
        return;
    }
    if (!strncmp(folder, from, length) && (folder[length] == '/' || !folder[length])) {
        snprintf(status, sizeof(status), "Can't put %s inside itself", base);
        free(from);
        return;
    }
    free_name(base, name);
    path_of(to, name);
    if (moving)
        ok = rename(from, to) == 0 || (errno == EXDEV && copy_tree(from, to) && delete_tree(from));
    else
        ok = copy_tree(from, to);
    if (ok) {
        printf("files: %s %s to %s\n", moving ? "moved" : "pasted", from, to);
        if (moving) {
            unlink(CUT);
            gui_clipboard_set(to, strlen(to)); // where it is now, to paste copies of
        }
        read_folder(folder, name);
    } else {
        snprintf(status, sizeof(status), "Can't paste %s: %s", base, strerror(errno));
    }
    free(from);
}

static void delete_selected(void) {
    char path[PATH_MAX];

    path_of(path, chosen()->name);
    if ((chosen()->folder ? rmdir(path) : unlink(path)) < 0) {
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
    static struct gui_rect places[4];
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
    gui_textbox(ui, ADDRESS, typed, sizeof(typed));
    report(ui, 1, "address");
    if (gui_focused(ui, ADDRESS) && gui_key(ui, KEY_ENTER))
        read_folder(typed, NULL);
}

// Asks for a name, or whether to delete, under the address bar.
static void asking_strip(struct gui_ui *ui) {
    static const char *const questions[] = {
        [RENAMING] = "Rename to", [NEW_FOLDER] = "New folder", [NEW_FILE] = "New file",
    };
    bool deleting = asking == DELETING, yes, no;

    if (ask_focus)
        gui_set_focus(ui, deleting ? "Delete" : ANSWER, deleting ? NULL : answer);
    ask_focus = false;
    if (deleting)
        gui_label(ui, "Delete %s?", chosen()->name);
    else
        gui_label(ui, "%s:", questions[asking]);
    gui_same_line(ui);
    yes = gui_button(ui, deleting ? "Delete" : "OK");
    gui_same_line(ui);
    no = gui_button(ui, "Cancel");
    if (!deleting) {
        gui_same_line(ui);
        gui_textbox(ui, ANSWER, answer, sizeof(answer));
        yes |= gui_focused(ui, ANSWER) && gui_key(ui, KEY_ENTER);
    }
    if (no || gui_key(ui, KEY_ESCAPE))
        asking = NOTHING;
    else if (yes)
        answered();
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

static void frame(struct gui_ui *ui) {
    int active = current, closing = 0, result;

    while (waitpid(-1, NULL, WNOHANG) > 0)
        ; // programs it started that have finished
    gui_ui_begin(ui);
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

    result = gui_list_icons(ui, LIST, items, icons, count, &selected, 0);

    struct gui_rect list = gui_last_rect(ui);

    report(ui, 2, "list");
    if (result == GUI_LIST_ACTIVATED)
        open_selected();
    else if (result == GUI_LIST_CHANGED && (asking == RENAMING || asking == DELETING))
        asking = NOTHING; // they were about the one that was selected
    if (result == GUI_LIST_CHANGED)
        describe();
    list_keys(ui);
    context_menu(ui, list);
    gui_ui_end(ui);
    fflush(stdout);
}

// A window showing what's in folders, in tabs, to go about them and open what's there.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: files [folder]\n"
                            "Opens a window showing what's in the folder, or /.\n");
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
    if (!read_folder(argc > 1 ? argv[1] : "/", NULL))
        read_folder("/", NULL);
    gui_ui_init(&ui, window);
    frame(&ui);
    while (!quitting && gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
