#include <dirent.h>
#include <errno.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define MAX_ENTRIES 1024
#define EDITOR "/system/bin/gui/editor"

struct entry {
    char name[NAME_MAX + 1];
    bool folder;
    off_t size;
};

static struct entry entries[MAX_ENTRIES];
static char labels[MAX_ENTRIES][NAME_MAX + 2]; // names, with / after folders
static const char *items[MAX_ENTRIES];
static int count, selected;
static char folder[PATH_MAX] = "/", typed[PATH_MAX], new_name[NAME_MAX + 1];
static char status[PATH_MAX + 64], deleting[PATH_MAX];
static bool quitting;

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

// Lists a folder, going there, and choosing the entry called select if there is one.
static bool read_folder(const char *path, const char *select) {
    DIR *dir = opendir(path);
    struct dirent *d;

    if (!dir) {
        snprintf(status, sizeof(status), "Can't open %s: %s", path, strerror(errno));
        return false;
    }
    snprintf(folder, sizeof(folder), "%s", path);
    for (size_t n = strlen(folder); n > 1 && folder[n - 1] == '/'; n--)
        folder[n - 1] = '\0';
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
        printf(" %s", labels[i]);
        if (select && !strcmp(entries[i].name, select))
            selected = i;
    }
    printf("\n");
    snprintf(typed, sizeof(typed), "%s", folder);
    deleting[0] = '\0';
    describe();
    return true;
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

static void start(const char *program, const char *argument) {
    if (fork() == 0) {
        execl(program, program, argument, (char *)NULL);
        _exit(127);
    }
}

// Folders open here, and files in the editor.
static void open_selected(void) {
    char path[PATH_MAX];

    if (!count)
        return;
    path_of(path, entries[selected].name);
    if (entries[selected].folder) {
        read_folder(path, NULL);
        return;
    }
    printf("files: opening %s\n", path);
    start(EDITOR, path);
}

static void run_selected(void) {
    char path[PATH_MAX];

    if (!count || entries[selected].folder)
        return;
    path_of(path, entries[selected].name);
    printf("files: running %s\n", path);
    start(path, NULL);
}

// Deleting takes a second press, to be sure.
static void delete_selected(void) {
    char path[PATH_MAX];

    if (!count)
        return;
    path_of(path, entries[selected].name);
    if (strcmp(deleting, path)) {
        snprintf(deleting, sizeof(deleting), "%s", path);
        snprintf(status, sizeof(status), "Press Delete again to delete %s", entries[selected].name);
        return;
    }
    if ((entries[selected].folder ? rmdir(path) : unlink(path)) < 0) {
        snprintf(status, sizeof(status), "Can't delete %s: %s", entries[selected].name, strerror(errno));
        deleting[0] = '\0';
        return;
    }
    printf("files: deleted %s\n", path);
    read_folder(folder, NULL);
}

static void make_folder(void) {
    char path[PATH_MAX];

    if (!new_name[0]) {
        snprintf(status, sizeof(status), "Type the new folder's name first");
        return;
    }
    path_of(path, new_name);
    if (mkdir(path, 0755) < 0) {
        snprintf(status, sizeof(status), "Can't make %s: %s", new_name, strerror(errno));
        return;
    }
    printf("files: made %s\n", path);
    read_folder(folder, new_name);
    new_name[0] = '\0';
}

// Says where the list is when that changes, for the tests to find it.
static void report_list(struct gui_ui *ui) {
    static struct gui_rect was;
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&r, &was, sizeof(r)))
        printf("files: list at %d %d %d %d\n", r.x, r.y, r.width, r.height);
    was = r;
}

static void frame(struct gui_ui *ui) {
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ; // programs it started that have finished
    gui_ui_begin(ui);
    gui_menu_bar(ui);
    if (gui_menu(ui, "File")) {
        if (gui_menu_item(ui, "Open"))
            open_selected();
        if (gui_menu_item(ui, "Run"))
            run_selected();
        if (gui_menu_item(ui, "New folder"))
            make_folder();
        if (gui_menu_item(ui, "Delete"))
            delete_selected();
        if (gui_menu_item(ui, "Quit"))
            quitting = true;
        gui_menu_end(ui);
    }
    if (gui_menu(ui, "Go")) {
        if (gui_menu_item(ui, "Up"))
            go_up();
        if (gui_menu_item(ui, "Root"))
            read_folder("/", NULL);
        gui_menu_end(ui);
    }

    if (gui_button(ui, "Up"))
        go_up();
    gui_same_line(ui);
    if (gui_button(ui, "Go"))
        read_folder(typed, NULL);
    gui_same_line(ui);
    gui_textbox(ui, "##folder", typed, sizeof(typed));

    if (gui_button(ui, "Open"))
        open_selected();
    gui_same_line(ui);
    if (gui_button(ui, "Run"))
        run_selected();
    gui_same_line(ui);
    if (gui_button(ui, "Delete"))
        delete_selected();
    gui_same_line(ui);
    if (gui_button(ui, "New folder"))
        make_folder();
    gui_same_line(ui);
    gui_textbox(ui, "##name", new_name, sizeof(new_name));

    gui_label(ui, "%s", status);

    int result = gui_list(ui, "##entries", items, count, &selected, 0);

    report_list(ui);
    if (result == GUI_LIST_ACTIVATED) {
        open_selected();
    } else if (result == GUI_LIST_CHANGED) {
        deleting[0] = '\0';
        describe();
    }
    gui_ui_end(ui);
    fflush(stdout);
}

// A window showing what's in a folder, to go about them and open what's there.
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
