#pragma once

#include <key_event.h>
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

    // From wm. Mouse positions are inside the window.
    GUI_CREATED,
    GUI_MOUSE_MOVE,   // x, y, buttons held
    GUI_MOUSE_BUTTON, // x, y, buttons held, and changed says which changed
    GUI_MOUSE_WHEEL,  // x, y and wheel
    GUI_KEY,          // key
    GUI_FOCUS,        // focused is whether the window now has the keyboard
    GUI_CLOSE,        // the close button was clicked, or Super+Q pressed
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

struct gui_window {
    uint32_t id;
    struct gui_canvas canvas;
};

// Connects to wm, returning false if it isn't running. The connection is closed
// when the program exits.
bool gui_connect(void);

// The connection, for poll.
int gui_fd(void);

// Returns NULL if wm refused or memory ran out.
struct gui_window *gui_create_window(const char *title, int width, int height);
void gui_destroy_window(struct gui_window *window);

// Shows what was drawn in part of a window, or all of it with gui_show.
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

// Text in the 8x8 font, scaled up by scale. Each character is 8 * scale wide.
#define GUI_CHAR_WIDTH 8
#define GUI_CHAR_HEIGHT 8
void gui_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale);
int gui_text_width(const char *text, int scale);
