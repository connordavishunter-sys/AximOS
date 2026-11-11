#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEXT_MAX (256 * 1024)

// What waits for a second go because it would lose changes that aren't saved.
enum action { NOTHING, NEW, OPEN, CLOSE };

static struct gui_window *window;
static struct gui_file_picker picker;
static char text[TEXT_MAX], path[GUI_PATH_MAX], status[GUI_PATH_MAX + 64];
static bool modified, quitting;
static enum action asked;

static const char *file_name(void) {
    const char *slash = strrchr(path, '/');

    return path[0] ? (slash ? slash + 1 : path) : "Untitled";
}

// "*name - Editor", with the star while there are changes to save.
static void show_title(void) {
    char title[GUI_TITLE_MAX];

    snprintf(title, sizeof(title), "%s%s - Editor", modified ? "*" : "", file_name());
    gui_set_title(window, title);
    printf("editor: title %s\n", title);
}

static void set_modified(bool now) {
    if (modified == now)
        return;
    modified = now;
    asked = NOTHING;
    show_title();
}

static void open_file(const char *chosen) {
    int fd = open(chosen, O_RDONLY);
    ssize_t n = fd < 0 ? -1 : read(fd, text, sizeof(text));

    if (fd >= 0)
        close(fd);
    if (n < 0 || n == sizeof(text)) {
        snprintf(status, sizeof(status), n < 0 ? "Can't open %s: %s" : "%s is too big, over 256 kB", chosen,
                 strerror(errno));
        return;
    }
    text[n] = '\0';
    snprintf(path, sizeof(path), "%s", chosen);
    snprintf(status, sizeof(status), "Opened %s, %zd bytes", path, n);
    printf("editor: opened %s %zd bytes\n", path, n);
    modified = true; // so that the title changes
    set_modified(false);
}

static void save_file(const char *to) {
    int fd = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    size_t length = strlen(text);
    ssize_t n = fd < 0 ? -1 : write(fd, text, length);

    if (fd >= 0 && close(fd) < 0)
        n = -1;
    if (n != (ssize_t)length) {
        snprintf(status, sizeof(status), "Can't save %s: %s", to, strerror(errno));
        return;
    }
    snprintf(path, sizeof(path), "%s", to);
    snprintf(status, sizeof(status), "Saved %s, %zu bytes", path, length);
    printf("editor: saved %s %zu bytes\n", path, length);
    modified = true;
    set_modified(false);
}

// The picker starts in the folder of the file being edited.
static void pick(bool saving) {
    char folder[GUI_PATH_MAX];
    char *slash;

    snprintf(folder, sizeof(folder), "%s", path[0] ? path : "/");
    slash = strrchr(folder, '/');
    if (slash == folder)
        folder[1] = '\0';
    else if (slash)
        *slash = '\0';
    gui_picker_open(&picker, folder, saving && path[0] ? file_name() : "", saving);
    gui_post(&(struct gui_message){.type = GUI_REDRAW}); // to show it at once
}

static void save(void) {
    if (path[0])
        save_file(path);
    else
        pick(true);
}

// New, Open and closing would lose changes that aren't saved, so they ask first,
// and go ahead when asked for again.
static bool may_lose_changes(enum action action) {
    static const char *const verbs[] = {[NEW] = "New", [OPEN] = "Open", [CLOSE] = "close"};

    if (!modified || asked == action)
        return true;
    asked = action;
    snprintf(status, sizeof(status), "Changes aren't saved: %s again to lose them, or Save", verbs[action]);
    return false;
}

static void new_file(void) {
    if (!may_lose_changes(NEW))
        return;
    text[0] = path[0] = '\0';
    snprintf(status, sizeof(status), "New file");
    modified = true;
    set_modified(false);
}

static void open_picked(void) {
    if (may_lose_changes(OPEN))
        pick(false);
}

static void close_window(void) {
    quitting = may_lose_changes(CLOSE);
}

static void editing(struct gui_ui *ui) {
    gui_menu_bar(ui);
    if (gui_menu(ui, "File")) {
        if (gui_menu_item(ui, "New"))
            new_file();
        if (gui_menu_item(ui, "Open..."))
            open_picked();
        if (gui_menu_item(ui, "Save"))
            save();
        if (gui_menu_item(ui, "Save as..."))
            pick(true);
        if (gui_menu_item(ui, "Quit"))
            close_window();
        gui_menu_end(ui);
    }
    if (gui_shortcut(ui, 'n'))
        new_file();
    if (gui_shortcut(ui, 'o'))
        open_picked();
    if (gui_shortcut(ui, 's'))
        save();
    if (gui_shortcut(ui, 'S'))
        pick(true);

    if (gui_button(ui, "Open..."))
        open_picked();
    gui_same_line(ui);
    if (gui_button(ui, "Save"))
        save();
    gui_same_line(ui);
    gui_label(ui, "%s", status);
    if (gui_textarea(ui, "##text", text, sizeof(text), 0))
        set_modified(true);
}

static void frame(struct gui_ui *ui) {
    static char picking_in[GUI_PATH_MAX];

    gui_ui_begin(ui);
    if (!picker.open) {
        editing(ui);
    } else {
        bool saving = picker.saving;
        int result = gui_file_picker(ui, &picker);

        if (result == GUI_PICKED && saving)
            save_file(picker.path);
        else if (result == GUI_PICKED)
            open_file(picker.path);
        else if (result == GUI_PICK_CANCELLED)
            printf("editor: picking cancelled\n");
        if (picker.open && strcmp(picking_in, picker.folder))
            printf("editor: picking in %s\n", picker.folder);
        snprintf(picking_in, sizeof(picking_in), "%s", picker.open ? picker.folder : "");

        // The frame after shows the text again.
        if (result)
            gui_post(&(struct gui_message){.type = GUI_REDRAW});
    }
    gui_ui_end(ui);
    fflush(stdout);
}

// A window to write text in, saved to a file.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: editor [file]\n"
                            "Opens a window to edit the file in, or a new one.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "editor: wm isn't running\n");
        return 1;
    }

    struct gui_ui ui;
    struct gui_message e;

    window = gui_create_window("Editor", 640, 480);
    if (!window) {
        dprintf(STDERR_FILENO, "editor: couldn't make a window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    if (argc > 1)
        open_file(argv[1]);
    else
        show_title();
    frame(&ui);
    while (!quitting && gui_next_event(&e)) {
        if (e.type == GUI_CLOSE)
            close_window();
        if (gui_ui_event(&ui, &e) || e.type == GUI_CLOSE)
            frame(&ui);
    }
    printf("editor: closed\n");
    gui_destroy_window(window);
    return 0;
}
