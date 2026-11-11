#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int count;
static bool show_progress = true;
static int volume = 50;
static char name[32];

// Says where a widget is when that changes, for the tests to find it.
static void report(struct gui_ui *ui, int index, const char *what) {
    static struct gui_rect places[8];
    struct gui_rect r = gui_last_rect(ui);

    if (memcmp(&places[index], &r, sizeof(r)) != 0)
        printf("widgets: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

static void frame(struct gui_ui *ui) {
    gui_ui_begin(ui);
    gui_heading(ui, "Widgets");
    gui_label(ui, "Drawn by libgui, a frame at a time.");
    gui_separator(ui);

    if (gui_button(ui, "Count"))
        printf("widgets: count %d\n", ++count);
    report(ui, 0, "count");
    gui_same_line(ui);
    if (gui_button(ui, "Reset"))
        printf("widgets: count %d\n", count = 0);
    report(ui, 1, "reset");
    gui_same_line(ui);
    gui_label(ui, "pressed %d times", count);

    if (gui_checkbox(ui, "Show progress", &show_progress))
        printf("widgets: progress %s\n", show_progress ? "shown" : "hidden");
    report(ui, 2, "checkbox");
    if (gui_slider(ui, "Volume", &volume, 0, 100))
        printf("widgets: volume %d\n", volume);
    report(ui, 3, "slider");
    if (show_progress)
        gui_progress(ui, volume / 100.0f);

    if (gui_textbox(ui, "Name", name, sizeof(name)))
        printf("widgets: name %s\n", name);
    report(ui, 4, "textbox");
    gui_label(ui, name[0] ? "Hello, %s!" : "Type your name above.", name);
    gui_separator(ui);
    gui_label(ui, "Tab moves, Enter or Space presses.");
    gui_ui_end(ui);
    fflush(stdout);
}

// Every kind of widget libgui has, to try them out.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: widgets\n"
                            "Opens a window showing every kind of widget libgui has.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "widgets: wm isn't running\n");
        return 1;
    }

    struct gui_window *window = gui_create_window("Widgets", 380, 300);
    struct gui_ui ui;
    struct gui_message e;

    if (!window) {
        dprintf(STDERR_FILENO, "widgets: couldn't make a window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    frame(&ui);
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
