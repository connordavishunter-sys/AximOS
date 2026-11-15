#pragma once

#include <key_event.h>
#include <stddef.h>
#include <stdint.h>

// Programs draw windows through wm, the compositor. Each window's pixels are shared
// memory the program draws into, telling wm which part changed. Pixels are
// 0x00RRGGBB, like the screen's.

#define GUI_SOCKET "/tmp/wm.sock"
#define GUI_TITLE_MAX 64
#define GUI_PATH_MAX 1024

// Everything sent either way is one of these.
enum gui_message_type {
    // From programs. create gives the size, the title in text and the shared
    // memory's name in buffer. wm answers with GUI_CREATED.
    GUI_CREATE = 1,
    GUI_DAMAGE, // x, y, width and height of what changed
    GUI_DESTROY,
    GUI_BUFFER, // new pixels, width by height, named in buffer, after a resize
    GUI_TITLE,  // a new title, in text
    GUI_CURSOR, // the mouse cursor to show over the window: x is a gui_cursor
    GUI_ICON,   // the program's name in text, which picks its icon

    // Either way: settings have changed, and wm, then every program, loads them again.
    GUI_SETTINGS,

    // The clipboard, which wm keeps. Text goes either way in shared memory named
    // in buffer, width bytes long, which whoever gets it maps and then removes.
    GUI_CLIPBOARD_SET, // width and buffer, or width 0 to empty it
    GUI_CLIPBOARD_GET, // answered with GUI_CLIPBOARD
    GUI_NOTIFY,        // a notification: its title in text and what it says in buffer, from
                       // the window it's about, if it's for one, whose icon it has
    GUI_UNLOCK,        // from the log-in screen wm started, once someone's logged in

    // From wm. Mouse positions are inside the window.
    GUI_CREATED,
    GUI_MOUSE_MOVE,   // x, y, buttons held
    GUI_MOUSE_BUTTON, // x, y, buttons held, and changed says which changed
    GUI_MOUSE_WHEEL,  // x, y and wheel
    GUI_KEY,          // key
    GUI_FOCUS,        // focused is whether the window now has the keyboard
    GUI_CLOSE,        // the close button was clicked, or Super+Q pressed
    GUI_RESIZE,       // width and height wm would like the window to be
    GUI_CLIPBOARD,    // width and buffer, what's on the clipboard
    GUI_SCREEN,       // width and height of the screen, with x 1 if its resolution can change.
                      // After GUI_CREATED, and whenever it changes.

    // From libgui itself, through gui_post: draw the window again.
    GUI_REDRAW,
    GUI_TIMER, // x is the timer that's due
};

// The shapes of mouse cursor wm draws.
enum gui_cursor {
    GUI_CURSOR_ARROW,
    GUI_CURSOR_TEXT,
    GUI_CURSOR_RESIZE_H,    // left and right
    GUI_CURSOR_RESIZE_V,    // up and down
    GUI_CURSOR_RESIZE_NWSE, // top left and bottom right
    GUI_CURSOR_RESIZE_NESW, // top right and bottom left
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
// when the program exits. A program wm starts itself is given its connection, by
// number, in GUI_FD.
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

void gui_set_title(struct gui_window *window, const char *title);

// Icons are /system/share/icons/apps/name.png, and a new window has the one named
// after the program, as it was run.
void gui_set_icon(struct gui_window *window, const char *name);
void gui_set_cursor(struct gui_window *window, enum gui_cursor cursor);

// Shows a notification in the corner of the screen for a few seconds, with the
// window's icon, if it's given. What doesn't fit in GUI_TITLE_MAX is cut short.
bool gui_notify(struct gui_window *window, const char *title, const char *text);

// Puts text on the clipboard, returning false if memory ran out.
bool gui_clipboard_set(const char *text, size_t length);

// What's on the clipboard, ending in a zero byte, to be freed, or NULL if it's
// empty. length, if not NULL, gets its length.
char *gui_clipboard_get(size_t *length);

// Shows what was drawn on part of a window's canvas, or all of it with gui_show.
void gui_update(struct gui_window *window, int x, int y, int width, int height);
void gui_show(struct gui_window *window);

// Waits for the next event, returning false when wm has gone.
bool gui_next_event(struct gui_message *event);

// Puts an event at the end of those gui_next_event gives, as if it came from wm.
void gui_post(const struct gui_message *event);

// gui_next_event gives a GUI_TIMER every ms milliseconds, or only once without
// repeat, with x the number this returns. A timer that's fallen behind doesn't
// make up what it missed. Returns -1 when GUI_MAX_TIMERS are running.
#define GUI_MAX_TIMERS 16
int gui_timer_start(int ms, bool repeat);
void gui_timer_stop(int timer);

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

// The desktop's settings, kept in /system/etc/gui.conf.
struct gui_settings {
    bool light;         // the light theme, rather than the dark one
    bool shadows;       // under windows
    bool clock_12_hour; // on the taskbar, rather than 24 hour
    int width, height;  // the screen's resolution, or 0 to leave it as it started
    int repeat_delay;   // milliseconds a key's held before it repeats
    int repeat_rate;    // times a second it repeats then, or 0 for never
};

void gui_settings_load(struct gui_settings *settings);
bool gui_settings_save(const struct gui_settings *settings);

// Tells wm the settings have changed, and through it every program with a window.
void gui_settings_changed(void);

// The screen, as of the last GUI_SCREEN.
struct gui_screen {
    int width, height;
    bool can_resize; // whether its resolution can change
};

const struct gui_screen *gui_screen(void);

// The wallpaper, an image's path or "none", kept in /system/etc/wallpaper, and the
// ones that come with the system.
#define GUI_WALLPAPERS "/system/share/wallpapers"
void gui_wallpaper_get(char path[GUI_PATH_MAX]);
bool gui_wallpaper_set(const char *path);

// Images, as pixels of 0xAARRGGBB, from PNG, JPEG and BMP files. Each returns
// false if it couldn't, with gui_image_error saying why.
struct gui_image {
    uint32_t *pixels;
    int width, height;
};

bool gui_image_load(const char *path, struct gui_image *image);
bool gui_image_decode(const void *data, size_t size, struct gui_image *image);
const char *gui_image_error(void);
void gui_image_free(struct gui_image *image);

// A smoothly scaled copy, to be freed as well.
bool gui_image_scale(const struct gui_image *from, int width, int height, struct gui_image *to);

// Draws an image, blending what's partly transparent with what's under it.
void gui_draw_image(struct gui_canvas *canvas, int x, int y, const struct gui_image *image);
// Saves pixels as a PNG file, returning false, with gui_image_error saying why, if it couldn't.
bool gui_image_save(const char *path, const struct gui_canvas *from);

// Icons, 16 pixels square, from /system/share/icons/name.png, loaded once and
// kept. NULL if there's no such icon.
#define GUI_ICON_SIZE 16
const struct gui_image *gui_icon(const char *name);

// The icon for a file by what its name ends in, or for a folder.
const struct gui_image *gui_file_icon(const char *name, bool folder);

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
#define GUI_UI_MAX_SCROLLS 16
#define GUI_UI_MAX_DEPTH 4
#define GUI_UI_MAX_MENUS 8

// Where widgets go: across from left to right, and down from y, inside clip.
struct gui_layout {
    int left, right;
    int x, y, line_height, line_end;
    bool same_line;
    struct gui_rect clip;
};

// How far down something scrolling is, and how tall what's in it was last frame.
struct gui_scroll {
    uint32_t id;
    int offset, content;
};

struct gui_ui {
    struct gui_window *window;
    bool window_focused;

    // This frame's input.
    int mouse_x, mouse_y;
    bool mouse_down, mouse_pressed, mouse_released;
    bool double_click; // the press was the second in quick succession
    bool right_pressed;
    long last_press;   // when the last press was, in milliseconds
    int wheel;
    struct key_event keys[GUI_UI_MAX_KEYS];
    int key_count;

    // Widgets by the hash of their label: under the mouse, being pressed or
    // dragged, and with the keyboard.
    uint32_t hot, active, focus;
    enum gui_cursor cursor, shown_cursor; // what the widget under the mouse wants, and what wm shows
    size_t text_cursor, text_anchor; // in the text box that last had the keyboard, and
    uint32_t text_owner;             // where what's selected starts
    int blink_timer;                 // while a text box has the keyboard, the cursor blinks
    bool cursor_hidden, text_drawn;
    int drag_offset; // from the top of a scroll bar's thumb to the mouse

    // The widgets that can have the keyboard, in order, as of the last frame and
    // so far in this one, for Tab and Shift+Tab.
    uint32_t order[GUI_UI_MAX_FOCUSABLE], next_order[GUI_UI_MAX_FOCUSABLE];
    int order_count, next_count;

    // What scrolls, and which of it was innermost under the mouse last frame, so
    // gets the wheel.
    struct gui_scroll scrolls[GUI_UI_MAX_SCROLLS];
    uint32_t wheel_owner, next_wheel_owner;

    // The open dropdown or menu, which is drawn over everything else, and gets
    // the mouse when it's over it.
    uint32_t popup;
    bool popup_is_menu, popup_drawn, in_popup, mouse_in_popup;
    struct gui_rect popup_rect, outside_popup;
    uint32_t *popup_pixels;
    int item_y, item_width, item_index, bar_x;

    // The menus in the bar, as of the last frame and so far in this one. An open
    // menu has the keyboard: Up and Down move menu_choice through its items, of
    // which it had menu_items last frame, and Left and Right go to the next menu.
    uint32_t menus[GUI_UI_MAX_MENUS], next_menus[GUI_UI_MAX_MENUS];
    int menu_count, next_menu_count, menu_choice, menu_items, menu_height;
    int menu_x, menu_y; // where the mouse was when a context menu opened

    // The item in a list the last right-click was on, or -1 if none was.
    int clicked_item;
    struct key_event menu_keys[GUI_UI_MAX_KEYS];
    int menu_key_count;

    // A widget Tab moved the keyboard to, which the areas it's in scroll to show.
    uint32_t reveal;

    // Where the next widget goes, with what it was before each scrolling area.
    struct gui_layout layout;
    struct {
        struct gui_layout layout;
        struct gui_rect area, view;
        uint32_t id;
        int top;
    } saved[GUI_UI_MAX_DEPTH];
    int depth;
    struct gui_rect last;
    bool again; // something changed that was already drawn
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
// How much of the line is left, from where the next widget would go.
int gui_room(const struct gui_ui *ui);

// Where the last widget went, in the window.
struct gui_rect gui_last_rect(const struct gui_ui *ui);

void gui_heading(struct gui_ui *ui, const char *text);
void gui_label(struct gui_ui *ui, const char *format, ...) __attribute__((format(printf, 2, 3)));
void gui_separator(struct gui_ui *ui);
void gui_progress(struct gui_ui *ui, float fraction);

// Whether Ctrl and a letter were pressed this frame, for shortcuts. A capital
// letter is with Shift as well.
bool gui_shortcut(const struct gui_ui *ui, char letter);

// Whether a key was pressed this frame.
bool gui_key(const struct gui_ui *ui, uint16_t code);

// Whether the widget with label has the keyboard.
bool gui_focused(const struct gui_ui *ui, const char *label);

// Gives the widget with label the keyboard, and if it holds text, selects all of it.
void gui_set_focus(struct gui_ui *ui, const char *label, const char *text);

// These return true when they're pressed, or when what they hold changes.
bool gui_button(struct gui_ui *ui, const char *label);
// A button width by height, with its label in the middle, or as wide as the label if
// width is 0, and the usual height if height is 0.
bool gui_button_sized(struct gui_ui *ui, const char *label, int width, int height);
// A panel across the line, height tall, with text twice the usual size at its right.
void gui_display(struct gui_ui *ui, const char *text, int height);
bool gui_checkbox(struct gui_ui *ui, const char *label, bool *value);

// A picture, with the label under it, framed in the accent colour when selected.
bool gui_image_button(struct gui_ui *ui, const char *label, const struct gui_image *image, bool selected);
bool gui_slider(struct gui_ui *ui, const char *label, int *value, int min, int max);
bool gui_textbox(struct gui_ui *ui, const char *label, char *text, size_t size);

// A text box that shows a star for each character, and can't be copied from.
bool gui_password(struct gui_ui *ui, const char *label, char *text, size_t size);

// Text of several lines, lines tall, or down to the bottom of the window if lines
// is 0, scrolling to keep the cursor in sight. Enter starts a new line.
bool gui_textarea(struct gui_ui *ui, const char *label, char *text, size_t size, int lines);

// Picks one of count items, shown when clicked in a list under it, which scrolls
// when there are too many. Up and Down change it, open or not.
bool gui_dropdown(struct gui_ui *ui, const char *label, const char *const *items, int count, int *selected);

// All of count items in a list rows tall, or down to the bottom of the window if
// rows is 0, which scrolls, with selected highlighted. Returns GUI_LIST_CHANGED
// when the selection changes, and GUI_LIST_ACTIVATED when the selected item is
// double-clicked or Enter is pressed on it.
enum { GUI_LIST_CHANGED = 1, GUI_LIST_ACTIVATED };
int gui_list(struct gui_ui *ui, const char *label, const char *const *items, int count, int *selected, int rows);

// The same, with an icon before each item, where icons has one that isn't NULL.
int gui_list_icons(struct gui_ui *ui, const char *label, const char *const *items, const struct gui_image *const *icons,
                   int count, int *selected, int rows);

// A bar of menus across the top, which goes first in a frame. Each gui_menu that
// returns true is open, and has its items next, then gui_menu_end. An item returns
// true when it's chosen. F10 opens the first menu from the keyboard, the arrows move
// about, and Enter chooses.
void gui_menu_bar(struct gui_ui *ui);
bool gui_menu(struct gui_ui *ui, const char *label);
bool gui_menu_item(struct gui_ui *ui, const char *label);
void gui_menu_separator(struct gui_ui *ui);
void gui_menu_end(struct gui_ui *ui);

// A menu that opens where area is right-clicked, returning true while it's open,
// to be followed by its items and gui_menu_end like gui_menu. Right-clicking a list
// selects the item clicked, and says which in clicked_item.
bool gui_context_menu(struct gui_ui *ui, const char *label, struct gui_rect area);

// A bar of tabs across the top, which goes first in a frame, named names, with
// active the one shown. Clicking a tab, or Ctrl+Tab and Ctrl+Shift+Tab, changes
// active and returns GUI_TABS_CHANGED. A tab's x, or Ctrl+W for the active one,
// returns GUI_TABS_CLOSE with closing set to it, and + or Ctrl+T GUI_TABS_NEW, for
// the program to add or take away the tab. With closing NULL, there's no x or +.
enum { GUI_TABS_CHANGED = 1, GUI_TABS_NEW, GUI_TABS_CLOSE };
int gui_tabs(struct gui_ui *ui, const char *const *names, int count, int *active, int *closing);

// The widgets between these go in an area height tall, or down to the bottom of
// the window if height is 0, which the wheel and a scroll bar scroll when they
// don't fit, or Tab moves to something in them that can't be seen. Areas can go
// inside each other, GUI_UI_MAX_DEPTH deep.
void gui_scroll_begin(struct gui_ui *ui, const char *label, int height);
void gui_scroll_end(struct gui_ui *ui);

// A file picker, which fills the window while it's open, in place of what's
// usually there. It shows a folder's files, with folders first, to choose one to
// open, or to choose a name to save as. Typing a folder's path in the name box
// goes there.

struct gui_file_picker {
    bool open, saving, focus_name;
    char folder[GUI_PATH_MAX], name[256];
    char path[GUI_PATH_MAX]; // what was chosen
    char status[GUI_PATH_MAX + 64], replacing[GUI_PATH_MAX];
    char **items;
    const struct gui_image **icons;
    int count, selected;
};

enum { GUI_PICKED = 1, GUI_PICK_CANCELLED };

// Opens it on folder, with name in the name box.
void gui_picker_open(struct gui_file_picker *picker, const char *folder, const char *name, bool saving);

// Draws it, between gui_ui_begin and gui_ui_end. Returns GUI_PICKED with the
// chosen file's path in picker->path, GUI_PICK_CANCELLED when Cancel or Escape
// is pressed, and 0 until then. Either closes it.
int gui_file_picker(struct gui_ui *ui, struct gui_file_picker *picker);
