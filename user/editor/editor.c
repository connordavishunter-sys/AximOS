#include "syntax.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEXT_MAX (256 * 1024)
#define TEXT "##text"
#define FIND "##find"
#define REPLACE "##replace"
#define LINE "##line"
#define KEY_F3 (KEY_F1 + 2)

// What waits for a second go because it would lose changes that aren't saved.
enum action { NOTHING, NEW, OPEN, CLOSE };

static struct gui_window *window;
static struct gui_file_picker picker;
static char text[TEXT_MAX], path[GUI_PATH_MAX], status[GUI_PATH_MAX + 64];
static bool modified, quitting;
static enum action asked;

// The bar under the buttons, for finding, replacing or going to a line, and what's
// selected in the text, as of when it last had the keyboard.
static enum { NO_BAR, FINDING, REPLACING, GOING } bar;
static bool bar_focus;
static char wanted[256], replacement[256], line_number[16];
static size_t selected_from, selected_to;

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
    selected_from = selected_to = 0;
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
    selected_from = selected_to = 0;
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

// The text gets the keyboard, but not this frame's keys, which were for the bar.
static void select_text(struct gui_ui *ui, size_t from, size_t to) {
    gui_select_text(ui, TEXT, from, to);
    ui->key_count = 0;
    selected_from = from;
    selected_to = to;
    printf("editor: selected %zu %zu\n", from, to);
}

// Finds what's wanted after what's selected, going round to the start, and selects it.
static bool find_next(struct gui_ui *ui) {
    size_t length = strlen(wanted), after = selected_from > selected_to ? selected_from : selected_to;
    const char *found = length ? strstr(text + after, wanted) : NULL;

    found = found || !length ? found : strstr(text, wanted);
    if (!found) {
        snprintf(status, sizeof(status), length ? "Can't find %s" : "Nothing to find", wanted);
        return false;
    }
    select_text(ui, found - text, found - text + length);
    snprintf(status, sizeof(status), "Found %s", wanted);
    return true;
}

// Puts the replacement in place of count characters at, if it fits.
static bool replace_at(size_t at, size_t count) {
    size_t length = strlen(text), with = strlen(replacement);

    if (length - count + with >= sizeof(text))
        return false;
    memmove(text + at + with, text + at + count, length - at - count + 1);
    memcpy(text + at, replacement, with);
    set_modified(true);
    return true;
}

// Replaces what's selected, if it's what's wanted, then finds the next.
static void replace_one(struct gui_ui *ui) {
    size_t low = selected_from < selected_to ? selected_from : selected_to;
    size_t high = selected_from < selected_to ? selected_to : selected_from, length = strlen(wanted);

    if (length && high - low == length && !strncmp(text + low, wanted, length) && replace_at(low, length))
        selected_from = selected_to = low + strlen(replacement);
    find_next(ui);
}

static void replace_all(struct gui_ui *ui) {
    size_t length = strlen(wanted), at = 0;
    int count = 0;

    for (char *found; length && (found = strstr(text + at, wanted)); count++) {
        at = found - text;
        if (!replace_at(at, length))
            break;
        at += strlen(replacement);
    }
    snprintf(status, sizeof(status), "Replaced %d", count);
    printf("editor: replaced %d\n", count);
    select_text(ui, at, at);
}

static void go_to_line(struct gui_ui *ui) {
    long line = strtol(line_number, NULL, 10), at_line = 1;
    size_t at = 0;

    for (; at_line < line && text[at]; at++)
        at_line += text[at] == '\n';
    snprintf(status, sizeof(status), "Line %ld", at_line);
    select_text(ui, at, at);
    bar = NO_BAR;
}

static void open_bar(int which) {
    bar = which;
    bar_focus = true;
}

// A text box in the bar, which Enter does what it's for in.
static bool bar_box(struct gui_ui *ui, const char *prompt, const char *label, char *s, size_t size) {
    gui_label(ui, "%s", prompt);
    gui_same_line(ui);
    if (bar_focus)
        gui_set_focus(ui, label, s);
    bar_focus = false;
    gui_textbox(ui, label, s, size);
    gui_same_line(ui);
    return gui_focused(ui, label) && gui_key(ui, KEY_ENTER);
}

static void bar_strip(struct gui_ui *ui) {
    if (bar == GOING) {
        if (bar_box(ui, "Go to line", LINE, line_number, sizeof(line_number)) | gui_button(ui, "Go"))
            go_to_line(ui);
    } else {
        if (bar_box(ui, "Find", FIND, wanted, sizeof(wanted)) | gui_button(ui, "Next"))
            find_next(ui);
    }
    gui_same_line(ui);
    if (gui_button(ui, "Close") || gui_key(ui, KEY_ESCAPE))
        bar = NO_BAR;
    if (bar == REPLACING) {
        if (bar_box(ui, "With", REPLACE, replacement, sizeof(replacement)) | gui_button(ui, "Replace"))
            replace_one(ui);
        gui_same_line(ui);
        if (gui_button(ui, "All"))
            replace_all(ui);
    }
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
    if (gui_menu(ui, "Edit")) {
        if (gui_menu_item(ui, "Find..."))
            open_bar(FINDING);
        if (gui_menu_item(ui, "Find next"))
            find_next(ui);
        if (gui_menu_item(ui, "Replace..."))
            open_bar(REPLACING);
        if (gui_menu_item(ui, "Go to line..."))
            open_bar(GOING);
        gui_menu_end(ui);
    }
    if (gui_shortcut(ui, 'f'))
        open_bar(FINDING);
    if (gui_shortcut(ui, 'h'))
        open_bar(REPLACING);
    if (gui_shortcut(ui, 'g'))
        open_bar(GOING);
    if (gui_key(ui, KEY_F3))
        find_next(ui);
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
    if (bar)
        bar_strip(ui);

    gui_highlighter *highlight = syntax_for(path, text);
    static gui_highlighter *was = (gui_highlighter *)1;

    if (highlight != was)
        printf("editor: colouring %s\n", syntax_name(highlight));
    was = highlight;
    if (gui_code_area(ui, TEXT, text, sizeof(text), 0, highlight))
        set_modified(true);
    if (gui_focused(ui, TEXT)) {
        selected_from = ui->text_anchor;
        selected_to = ui->text_cursor;
    }
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
        char dropped[GUI_PATH_MAX];
        bool opening = gui_dropped_file(&e, dropped);

        if (e.type == GUI_CLOSE)
            close_window();
        if (opening && may_lose_changes(OPEN))
            open_file(dropped);
        if (gui_ui_event(&ui, &e) || e.type == GUI_CLOSE || opening)
            frame(&ui);
    }
    printf("editor: closed\n");
    gui_destroy_window(window);
    return 0;
}
