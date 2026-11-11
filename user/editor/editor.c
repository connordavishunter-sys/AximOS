#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEXT_MAX (256 * 1024)
#define PATH_SIZE 256

static struct gui_window *window;
static char text[TEXT_MAX], path[PATH_SIZE], status[PATH_SIZE + 64];
static bool modified, closing, quitting;

// "*name - Editor", with the star while there are changes to save.
static void show_title(void) {
    const char *slash = strrchr(path, '/');
    char title[GUI_TITLE_MAX];

    snprintf(title, sizeof(title), "%s%s - Editor", modified ? "*" : "", path[0] ? (slash ? slash + 1 : path) : "Untitled");
    gui_set_title(window, title);
    printf("editor: title %s\n", title);
}

static void set_modified(bool now) {
    if (modified == now)
        return;
    modified = now;
    closing = false;
    show_title();
}

static void open_file(void) {
    int fd = path[0] ? open(path, O_RDONLY) : -1;
    ssize_t n = fd < 0 ? -1 : read(fd, text, sizeof(text));

    if (fd >= 0)
        close(fd);
    if (n < 0) {
        snprintf(status, sizeof(status), path[0] ? "Can't open %s: %s" : "Type a file's path, then Open", path,
                 strerror(errno));
        return;
    }
    if (n == sizeof(text)) {
        snprintf(status, sizeof(status), "%s is too big, over %d kB", path, TEXT_MAX / 1024);
        text[0] = '\0';
        return;
    }
    text[n] = '\0';
    snprintf(status, sizeof(status), "Opened %s, %zd bytes", path, n);
    printf("editor: opened %s %zd bytes\n", path, n);
    modified = true; // so that set_modified changes the title
    set_modified(false);
}

static void save_file(void) {
    int fd = path[0] ? open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644) : -1;
    size_t length = strlen(text);
    ssize_t n = fd < 0 ? -1 : write(fd, text, length);

    if (fd >= 0 && close(fd) < 0)
        n = -1;
    if (n != (ssize_t)length) {
        snprintf(status, sizeof(status), path[0] ? "Can't save %s: %s" : "Type a file's path, then Save", path,
                 strerror(errno));
        return;
    }
    snprintf(status, sizeof(status), "Saved %s, %zu bytes", path, length);
    printf("editor: saved %s %zu bytes\n", path, length);
    set_modified(false);
}

static void new_file(void) {
    text[0] = path[0] = '\0';
    snprintf(status, sizeof(status), "New file");
    modified = true;
    set_modified(false);
}

// Closing with changes not saved asks first, and closing again goes ahead.
static void close_window(void) {
    if (!modified || closing) {
        quitting = true;
        return;
    }
    closing = true;
    snprintf(status, sizeof(status), "Changes aren't saved: close again to lose them, or Save");
}

static void frame(struct gui_ui *ui) {
    gui_ui_begin(ui);
    gui_menu_bar(ui);
    if (gui_menu(ui, "File")) {
        if (gui_menu_item(ui, "New"))
            new_file();
        if (gui_menu_item(ui, "Open"))
            open_file();
        if (gui_menu_item(ui, "Save"))
            save_file();
        if (gui_menu_item(ui, "Quit"))
            close_window();
        gui_menu_end(ui);
    }
    if (gui_shortcut(ui, 's'))
        save_file();
    if (gui_shortcut(ui, 'o'))
        open_file();
    if (gui_shortcut(ui, 'n'))
        new_file();

    if (gui_button(ui, "Open"))
        open_file();
    gui_same_line(ui);
    if (gui_button(ui, "Save"))
        save_file();
    gui_same_line(ui);
    gui_textbox(ui, "##path", path, sizeof(path));
    gui_label(ui, "%s", status);
    if (gui_textarea(ui, "##text", text, sizeof(text), 0))
        set_modified(true);
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
    if (argc > 1) {
        snprintf(path, sizeof(path), "%s", argv[1]);
        open_file();
    } else {
        new_file();
    }
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
