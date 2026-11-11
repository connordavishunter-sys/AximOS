#pragma once

#include <key_event.h>
#include <stddef.h>
#include <stdint.h>

// Programs draw windows through wm, the compositor. Each window's pixels are shared
// memory the program draws into, telling wm which part changed. Pixels are
// 0x00RRGGBB, like the screen's.

#define GUI_SOCKET "/tmp/wm.sock"
#define GUI_TITLE_MAX 64

// Everything sent either way is one of these.
enum gui_message_type {
    // From programs. create gives the size, the title in text and the shared
    // memory's name in buffer. wm answers with GUI_CREATED.
    GUI_CREATE = 1,
    GUI_DAMAGE, // x, y, width and height of what changed
    GUI_DESTROY,
    GUI_BUFFER, // new pixels, width by height, named in buffer, after a resize

    // From wm. Mouse positions are inside the window.
    GUI_CREATED,
    GUI_MOUSE_MOVE,   // x, y, buttons held
    GUI_MOUSE_BUTTON, // x, y, buttons held, and changed says which changed
    GUI_MOUSE_WHEEL,  // x, y and wheel
    GUI_KEY,          // key
    GUI_FOCUS,        // focused is whether the window now has the keyboard
    GUI_CLOSE,        // the close button was clicked, or Super+Q pressed
    GUI_RESIZE,       // width and height wm would like the window to be
};

struct gui_message {
    uint32_t type;
    uint32_t window;
    int32_t x, y, width, height;
    uint32_t buttons, changed;
    int32_t wheel;
    uint32_t focused;
    struct key_event key;
    char text[GUI_TITLE_MAX];
    char buffer[GUI_TITLE_MAX];
};

// Something to draw on: the screen's copy, or a window.
struct gui_canvas {
    uint32_t *pixels;
    int width, height;
    int stride; // pixels from one row to the next
};

// Programs draw on canvas, which is private, and updates copy what changed to the
// pixels wm shows, so it never shows a half-drawn frame.
struct gui_window {
    uint32_t id;
    struct gui_canvas canvas;
    uint32_t *shown;

    // New shown pixels from gui_resize, which wm switches to with the next update.
    char pending[GUI_TITLE_MAX];
    uint32_t *old_shown;
    size_t old_size;
};

// Connects to wm, returning false if it isn't running. The connection is closed
// when the program exits.
bool gui_connect(void);

// The connection, for poll.
int gui_fd(void);

// Returns NULL if wm refused or memory ran out.
struct gui_window *gui_create_window(const char *title, int width, int height);
void gui_destroy_window(struct gui_window *window);

// Gives the window a new size, usually when GUI_RESIZE asks, with a blank canvas to
// draw on. wm goes on showing the old size until the next update. Returns
// false, leaving the window as it was, if memory ran out.
bool gui_resize(struct gui_window *window, int width, int height);

// Shows what was drawn on part of a window's canvas, or all of it with gui_show.
void gui_update(struct gui_window *window, int x, int y, int width, int height);
void gui_show(struct gui_window *window);

// Waits for the next event, returning false when wm has gone.
bool gui_next_event(struct gui_message *event);

// Reads or writes one whole message on a connection, for wm.
bool gui_receive(int fd, struct gui_message *message);
bool gui_send(int fd, const struct gui_message *message);

// Drawing, clipped to the canvas.
void gui_fill(struct gui_canvas *canvas, int x, int y, int width, int height, uint32_t color);
void gui_copy(struct gui_canvas *canvas, int x, int y, const struct gui_canvas *from, int from_x, int from_y,
              int width, int height);

// Text in the 8x16 font, scaled up by scale. Each character is 8 * scale wide.
#define GUI_CHAR_WIDTH 8
#define GUI_CHAR_HEIGHT 16
void gui_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale);
int gui_text_width(const char *text, int scale);

// Widgets, drawn immediately in the style of Dear ImGui. Each frame, a program
// calls them in order, top to bottom, and each draws itself and says what was done
// to it. Frames are drawn after events:
//
//     while (gui_next_event(&e)) {
//         if (e.type == GUI_CLOSE)
//             break;
//         if (gui_ui_event(&ui, &e)) {
//             gui_ui_begin(&ui);
//             if (gui_button(&ui, "Save"))
//                 save();
//             gui_ui_end(&ui);
//         }
//     }
//
// Labels name widgets as well as showing on them, so two with the same label need
// "##" after it and something different, which isn't shown. Tab and Shift+Tab move
// the keyboard between widgets, and Enter or Space presses what it's on.

struct gui_rect {
    int x, y, width, height;
};

#define GUI_UI_MAX_KEYS 16
#define GUI_UI_MAX_FOCUSABLE 64

struct gui_ui {
    struct gui_window *window;
    bool window_focused;

    // This frame's input.
    int mouse_x, mouse_y;
    bool mouse_down, mouse_pressed, mouse_released;
    struct key_event keys[GUI_UI_MAX_KEYS];
    int key_count;

    // Widgets by the hash of their label: under the mouse, being pressed or
    // dragged, and with the keyboard.
    uint32_t hot, active, focus;
    size_t text_cursor;   // in the text box that last had the keyboard
    uint32_t text_owner;

    // The widgets that can have the keyboard, in order, as of the last frame and
    // so far in this one, for Tab and Shift+Tab.
    uint32_t order[GUI_UI_MAX_FOCUSABLE], next_order[GUI_UI_MAX_FOCUSABLE];
    int order_count, next_count;

    // Where the next widget goes.
    int x, y, line_height, line_end;
    bool same_line;
    struct gui_rect last;
};

void gui_ui_init(struct gui_ui *ui, struct gui_window *window);

// Takes an event's input, and resizes the window when wm asks. Returns whether the
// frame should be drawn again, which is false for events it has no use for.
bool gui_ui_event(struct gui_ui *ui, const struct gui_message *event);

// A frame starts with a blank window, and ends by showing it.
void gui_ui_begin(struct gui_ui *ui);
void gui_ui_end(struct gui_ui *ui);

// Puts the next widget to the right of the last, rather than under it.
void gui_same_line(struct gui_ui *ui);

// Where the last widget went, in the window.
struct gui_rect gui_last_rect(const struct gui_ui *ui);

void gui_heading(struct gui_ui *ui, const char *text);
void gui_label(struct gui_ui *ui, const char *format, ...) __attribute__((format(printf, 2, 3)));
void gui_separator(struct gui_ui *ui);
void gui_progress(struct gui_ui *ui, float fraction);

// These return true when they're pressed, or when what they hold changes.
bool gui_button(struct gui_ui *ui, const char *label);
bool gui_checkbox(struct gui_ui *ui, const char *label, bool *value);
bool gui_slider(struct gui_ui *ui, const char *label, int *value, int min, int max);
bool gui_textbox(struct gui_ui *ui, const char *label, char *text, size_t size);
