#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <mouse_event.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fb.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 32
#define MAX_WINDOWS 64
#define MAX_DAMAGE 32
#define MAX_SIZE 4096
#define CLIPBOARD_MAX (1024 * 1024)
#define INBOX_SIZE (8 * sizeof(struct gui_message))
#define OUTBOX_SIZE (64 * sizeof(struct gui_message))

#define TITLE_HEIGHT 24
#define BORDER 1
#define BUTTON_SIZE 16
#define CASCADE 32
#define RESIZE_MARGIN 6 // outside the frame, where dragging resizes
#define CORNER 16       // how far along an edge from a corner dragging moves both edges
#define MIN_WIDTH 120
#define MIN_HEIGHT 48

// Shadows reach SHADOW past the frame, moved SHADOW_DROP down, and are darkest,
// out of 255, at SHADOW_ALPHA. Their strength, out of 256, fades over FADE_MS
// between what's right for focused windows and the rest, and from nothing for
// new windows, quickly at first and slowing to a stop.
#define SHADOW 16
#define SHADOW_DROP 4
#define SHADOW_ALPHA 120
#define SHADOW_FOCUSED 256
#define SHADOW_UNFOCUSED 140
#define FADE_MS 240
#define FRAME_MS 16

#define TASKBAR_HEIGHT 28
#define TASKBAR_BUTTON_WIDTH 160
#define LAUNCHER_WIDTH 64

#define APPS_DIR "/system/bin/gui"
#define ICONS "/system/share/icons"
#define MAX_ICONS 32
#define ICON_SIZE 16

// The wallpaper is this, unless /system/etc/wallpaper names another, or "none"
// for the plain colour.
#define MAX_APPS 32
#define MENU_WIDTH 200
#define ITEM_HEIGHT 24 // in the start menu and the Alt+Tab switcher
#define MENU_GAP 9     // between the apps and Run and the power items
#define SWITCHER_WIDTH 360

// Colours, from the theme the settings choose. What's on the focused title bar,
// the red close button and the grey ones is white in both.
struct theme {
    uint32_t desktop, hint, border, title_focused, title_unfocused, title_text_unfocused;
    uint32_t taskbar, taskbar_line, taskbar_button, taskbar_minimized, text, dim_text;
};

static const struct theme dark = {
    .desktop = 0x1E2430, .hint = 0x5C677D, .border = 0x0E1118, .title_focused = 0x3D63DD,
    .title_unfocused = 0x3A4150, .title_text_unfocused = 0xFFFFFF, .taskbar = 0x141821, .taskbar_line = 0x2A3140,
    .taskbar_button = 0x2E3646, .taskbar_minimized = 0x1D222C, .text = 0xFFFFFF, .dim_text = 0x8A93A6,
};
static const struct theme light = {
    .desktop = 0xD5DBE6, .hint = 0x6B7488, .border = 0x9AA2B2, .title_focused = 0x3D63DD,
    .title_unfocused = 0xD9DDE5, .title_text_unfocused = 0x2B2F38, .taskbar = 0xEDEFF3, .taskbar_line = 0xC9CED8,
    .taskbar_button = 0xDCE0E8, .taskbar_minimized = 0xE8EBF0, .text = 0x1E2129, .dim_text = 0x7A8396,
};
static const struct theme *theme = &dark;

#define DESKTOP_COLOR (theme->desktop)
#define HINT_COLOR (theme->hint)
#define BORDER_COLOR (theme->border)
#define TITLE_FOCUSED (theme->title_focused)
#define TITLE_UNFOCUSED (theme->title_unfocused)
#define TITLE_TEXT_UNFOCUSED (theme->title_text_unfocused)
#define TASKBAR_COLOR (theme->taskbar)
#define TASKBAR_LINE (theme->taskbar_line)
#define TASKBAR_BUTTON (theme->taskbar_button)
#define TASKBAR_MINIMIZED (theme->taskbar_minimized)
#define TASKBAR_TEXT (theme->text)
#define TASKBAR_DIM_TEXT (theme->dim_text)
#define TITLE_TEXT 0xFFFFFF
#define CLOSE_COLOR 0xE5484D
#define BUTTON_COLOR 0x56607A

// Super+Enter runs this.
#define LAUNCH_COMMAND "term"
#define RUN_COMMAND "run" // Super+R, and Run... in the menu
#define KEY_Q 0x10
#define KEY_R 0x13
#define HINT "Super: apps   Super+R: run   Super+Enter: terminal   Alt+Tab: switch   Super+Q: close   Super+arrows: tile   " \
             "Super+Shift+Q: quit"

struct rect {
    int x, y, width, height;
};

struct client {
    int fd;
    char inbox[INBOX_SIZE];
    size_t in_used;
    char outbox[OUTBOX_SIZE];
    size_t out_used;
    bool stuck; // stopped reading, with something that can't be dropped waiting
};

// x and y are where the contents start, inside the frame.
struct window {
    struct client *client;
    uint32_t id;
    int x, y;
    char title[GUI_TITLE_MAX];
    char app[GUI_TITLE_MAX]; // the program, which picks its icon
    struct gui_canvas canvas;
    bool minimized;

    // The size it's been asked to be. One request is out at a time, and the program
    // answers it with a new buffer of whatever size it chose.
    int want_width, want_height;
    int asked_width, asked_height;
    bool asking;

    // Where it was before being maximized or tiled, to go back to.
    bool tiled;
    struct rect restore;

    // The edges being dragged by a resize. When the left or top one moves, the
    // window moves with each new size so the opposite edge stays still.
    int resize_edges;

    enum gui_cursor cursor; // what the program wants over its contents
    int shadow, fade_from, fade_to;
    long fade_start;
};

enum part { PART_NONE, PART_CONTENT, PART_TITLE, PART_CLOSE, PART_MAXIMIZE, PART_MINIMIZE, PART_RESIZE };
enum { EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8 };

static struct gui_canvas screen; // composed here, then copied to the framebuffer
static uint8_t *framebuffer;
static int pitch, fb;
static bool can_set_mode;

static struct client *clients[MAX_CLIENTS];
static struct window *windows[MAX_WINDOWS]; // bottom first
static int window_count;
static uint32_t next_id = 1;
static struct window *focused;

static uint8_t falloff[SHADOW + 1][SHADOW + 1];
static struct gui_settings settings;
static struct gui_canvas wallpaper; // the screen's part of the scaled image, if there is one
static struct gui_image wallpaper_image; // scaled to cover the screen
static char wallpaper_path[GUI_PATH_MAX];
static struct {
    char name[GUI_TITLE_MAX];
    struct gui_image image; // no pixels if there's no such icon
} icons[MAX_ICONS];
static int icon_count;
static char *clipboard;
static size_t clipboard_length;

static struct rect damage[MAX_DAMAGE];
static int damage_count;

static int cursor_x, cursor_y;
static uint32_t buttons;
static struct window *dragging; // by its title bar
static int drag_x, drag_y;
static struct window *grabbed; // pressed in its contents, so it gets the mouse until release
static struct window *resizing;
static int resize_x, resize_y, resize_width, resize_height;
static int resize_room; // above the frame, which it can't grow into
static bool quitting;

// The start menu, and what's highlighted in it.
static bool menu_open;
static char apps[MAX_APPS][GUI_TITLE_MAX];
static int app_count, menu_choice;
static bool super_down, super_alone; // Super pressed, and nothing else since

// The Alt+Tab switcher, while Alt is held: which window it's on, counting from the top.
static int switch_choice = -1;

// Cursors, in black X and white dots, each with the point that's on the mouse's
// position. The up and down arrow is the left and right one turned, and one
// diagonal is the other mirrored.
static const char *const arrow_rows[] = {
    "X           ", "XX          ", "X.X         ", "X..X        ", "X...X       ",
    "X....X      ", "X.....X     ", "X......X    ", "X.......X   ", "X........X  ",
    "X.........X ", "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ",
    "XX   X..X   ", "X     X..X  ", "      X..X  ", "       XX   ",
};
static const char *const text_rows[] = {
    "XXX XXX", "X..X..X", "XXX.XXX", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ",
    "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "  X.X  ", "XXX.XXX", "X..X..X", "XXX XXX",
};
static const char *const across_rows[] = {
    "    X         X    ", "   XX         XX   ", "  X.XXXXXXXXXXX.X  ",
    " X...............X ", "X.................X", " X...............X ",
    "  X.XXXXXXXXXXX.X  ", "   XX         XX   ", "    X         X    ",
};
static const char *const diagonal_rows[] = {
    "XXXXXX       ", "X....X       ", "X...X        ", "X....X       ", "X.X...X      ",
    "XX X...X     ", "    X...X    ", "     X...X XX", "      X...X.X", "       X....X",
    "        X...X", "       X....X", "       XXXXXX",
};

struct cursor_shape {
    const char *const *rows;
    int width, height; // of rows, before turning
    int hot_x, hot_y;  // after
    bool turned, mirrored;
};

#define ROWS(r, width) r, width, (int)(sizeof(r) / sizeof(r[0]))
static const struct cursor_shape shapes[] = {
    [GUI_CURSOR_ARROW] = {ROWS(arrow_rows, 12), 0, 0, false, false},
    [GUI_CURSOR_TEXT] = {ROWS(text_rows, 7), 3, 8, false, false},
    [GUI_CURSOR_RESIZE_H] = {ROWS(across_rows, 19), 9, 4, false, false},
    [GUI_CURSOR_RESIZE_V] = {ROWS(across_rows, 19), 4, 9, true, false},
    [GUI_CURSOR_RESIZE_NWSE] = {ROWS(diagonal_rows, 13), 6, 6, false, false},
    [GUI_CURSOR_RESIZE_NESW] = {ROWS(diagonal_rows, 13), 6, 6, false, true},
};
static enum gui_cursor cursor_shown = GUI_CURSOR_ARROW;

static struct rect frame_of(const struct window *w) {
    return (struct rect){w->x - BORDER, w->y - TITLE_HEIGHT - BORDER, w->canvas.width + 2 * BORDER,
                         w->canvas.height + TITLE_HEIGHT + 2 * BORDER};
}

// The frame and its shadow.
static struct rect extent_of(const struct window *w) {
    struct rect f = frame_of(w);

    if (!settings.shadows)
        return f;
    return (struct rect){f.x - SHADOW, f.y - SHADOW + SHADOW_DROP, f.width + 2 * SHADOW, f.height + 2 * SHADOW};
}

// The title bar's buttons from the right: close, maximize, minimize.
static struct rect button_of(const struct window *w, int index) {
    int gap = (TITLE_HEIGHT - BUTTON_SIZE) / 2;

    return (struct rect){w->x + w->canvas.width - (index + 1) * (BUTTON_SIZE + gap),
                         w->y - TITLE_HEIGHT + gap, BUTTON_SIZE, BUTTON_SIZE};
}

// Where windows go: everything above the taskbar.
static struct rect desktop(void) {
    return (struct rect){0, 0, screen.width, screen.height - TASKBAR_HEIGHT};
}

static struct rect taskbar(void) {
    return (struct rect){0, screen.height - TASKBAR_HEIGHT, screen.width, TASKBAR_HEIGHT};
}

static struct rect launcher(void) {
    return (struct rect){4, taskbar().y + 4, LAUNCHER_WIDTH, TASKBAR_HEIGHT - 8};
}

// The menu's items are the apps, then these, with RUN_COMMAND's icon for Run.
enum { ITEM_RUN, ITEM_REBOOT, ITEM_SHUT_DOWN, EXTRA_ITEMS };
static const char *const extra_items[] = {"Run...", "Reboot", "Shut down"};

static int menu_items(void) {
    return app_count + EXTRA_ITEMS;
}

// Just above the launcher, as tall as its apps, with a line for "no apps" if there
// are none.
static struct rect menu(void) {
    int height = ((app_count ? app_count : 1) + EXTRA_ITEMS) * ITEM_HEIGHT + MENU_GAP + 8;

    return (struct rect){launcher().x, taskbar().y - height, MENU_WIDTH, height};
}

static struct rect menu_item(int index) {
    struct rect m = menu();
    int y = index < app_count ? index * ITEM_HEIGHT
                              : ((app_count ? app_count : 1) + index - app_count) * ITEM_HEIGHT + MENU_GAP;

    return (struct rect){m.x + 4, m.y + 4 + y, m.width - 8, ITEM_HEIGHT};
}

// In the middle of the desktop, with every window.
static struct rect switcher(void) {
    int height = window_count * ITEM_HEIGHT + 8;

    return (struct rect){(screen.width - SWITCHER_WIDTH) / 2, (desktop().height - height) / 2, SWITCHER_WIDTH, height};
}

static struct window *switcher_window(int index) {
    return windows[window_count - 1 - index];
}

static bool contains(struct rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

static int menu_item_at(int x, int y) {
    for (int i = 0; i < menu_items(); i++) {
        if (contains(menu_item(i), x, y))
            return i;
    }
    return -1;
}

// Whether a window's frame hides all of a rectangle.
static bool covers(const struct window *w, struct rect r) {
    struct rect f = frame_of(w);

    return !w->minimized && r.x >= f.x && r.y >= f.y && r.x + r.width <= f.x + f.width
        && r.y + r.height <= f.y + f.height;
}

static bool overlaps(struct rect a, struct rect b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height && b.y < a.y + a.height;
}

static long area(struct rect r) {
    return (long)r.width * r.height;
}

static struct rect bounds(struct rect a, struct rect b) {
    int x = a.x < b.x ? a.x : b.x, y = a.y < b.y ? a.y : b.y;
    int right = a.x + a.width > b.x + b.width ? a.x + a.width : b.x + b.width;
    int bottom = a.y + a.height > b.y + b.height ? a.y + a.height : b.y + b.height;

    return (struct rect){x, y, right - x, bottom - y};
}

// Clipped to the screen. When the list is full, everything merges into one.
static void add_damage(struct rect r) {
    if (r.x < 0) {
        r.width += r.x;
        r.x = 0;
    }
    if (r.y < 0) {
        r.height += r.y;
        r.y = 0;
    }
    if (r.width > screen.width - r.x)
        r.width = screen.width - r.x;
    if (r.height > screen.height - r.y)
        r.height = screen.height - r.y;
    if (r.width <= 0 || r.height <= 0)
        return;

    // A rectangle that overlaps another goes in with it when the two together are
    // no bigger than they are apart, as when a window moves a little, so that the
    // overlap isn't drawn twice. Merging can make more overlap, so it goes round.
    for (int i = 0; i < damage_count; i++) {
        struct rect both = bounds(damage[i], r);

        if (overlaps(damage[i], r) && area(both) <= area(damage[i]) + area(r)) {
            r = both;
            damage[i] = damage[--damage_count];
            i = -1;
        }
    }
    if (damage_count == MAX_DAMAGE) {
        for (int i = 1; i < damage_count; i++)
            damage[0] = bounds(damage[0], damage[i]);
        damage_count = 1;
        damage[0] = bounds(damage[0], r);
        return;
    }
    damage[damage_count++] = r;
}

static void damage_title(const struct window *w) {
    if (w)
        add_damage((struct rect){w->x, w->y - TITLE_HEIGHT, w->canvas.width, TITLE_HEIGHT});
}

static void damage_taskbar(void) {
    add_damage(taskbar());
}

static struct rect cursor_rect(void) {
    const struct cursor_shape *c = &shapes[cursor_shown];
    int width = c->turned ? c->height : c->width, height = c->turned ? c->width : c->height;

    return (struct rect){cursor_x - c->hot_x, cursor_y - c->hot_y, width, height};
}

static void damage_cursor(void) {
    add_damage(cursor_rect());
}

// How dark a shadow is by how far left or right, and up or down, a point is from
// the frame: (1 - d^2 / SHADOW^2)^2, which falls smoothly to nothing without
// needing a square root.
static void make_falloff(void) {
    for (int dx = 0; dx <= SHADOW; dx++) {
        for (int dy = 0; dy <= SHADOW; dy++) {
            long t = SHADOW * SHADOW - dx * dx - dy * dy;

            falloff[dx][dy] = t > 0 ? SHADOW_ALPHA * t * t / (SHADOW * SHADOW * SHADOW * SHADOW) : 0;
        }
    }
}

// Darkens what's under a window's shadow, outside its frame, by blending black over it.
static void draw_shadow(struct gui_canvas *c, const struct window *w, int ox, int oy) {
    struct rect f = frame_of(w), e = extent_of(w), s = {f.x, f.y + SHADOW_DROP, f.width, f.height};
    struct rect area = {ox, oy, c->width, c->height};
    int x0 = e.x > area.x ? e.x : area.x, x1 = e.x + e.width < area.x + area.width ? e.x + e.width : area.x + area.width;
    int y0 = e.y > area.y ? e.y : area.y, y1 = e.y + e.height < area.y + area.height ? e.y + e.height : area.y + area.height;

    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->pixels + (long)(y - oy) * c->stride - ox;
        int dy = y < s.y ? s.y - y : y >= s.y + s.height ? y - (s.y + s.height - 1) : 0;
        bool beside_frame = y >= f.y && y < f.y + f.height;

        for (int x = x0; x < x1; x++) {
            if (beside_frame && x >= f.x && x < f.x + f.width) {
                x = f.x + f.width - 1; // the window covers this part
                continue;
            }

            int dx = x < s.x ? s.x - x : x >= s.x + s.width ? x - (s.x + s.width - 1) : 0;
            uint32_t a = dx > SHADOW || dy > SHADOW ? 0 : falloff[dx][dy] * w->shadow >> 8, keep = 256 - a;
            uint32_t p = row[x];

            // Red and blue together, then green, each kept by keep/256.
            if (a)
                row[x] = ((p & 0xFF00FF) * keep >> 8 & 0xFF00FF) | ((p & 0x00FF00) * keep >> 8 & 0x00FF00);
        }
    }
}

// An app's icon, loaded the first time it's wanted, or the plain program icon.
static const struct gui_image *app_icon(const char *name) {
    char path[sizeof(ICONS) + 16 + GUI_TITLE_MAX];

    for (int i = 0; i < icon_count; i++) {
        if (!strcmp(icons[i].name, name))
            return icons[i].image.pixels ? &icons[i].image : name[0] ? app_icon("") : NULL;
    }
    if (icon_count == MAX_ICONS)
        return NULL;
    snprintf(icons[icon_count].name, sizeof(icons[icon_count].name), "%s", name);
    if (name[0])
        snprintf(path, sizeof(path), ICONS "/apps/%s.png", name);
    else
        snprintf(path, sizeof(path), ICONS "/program.png");
    gui_image_load(path, &icons[icon_count++].image);
    return app_icon(name);
}

static void draw_icon(struct gui_canvas *c, const char *app, int x, int y, int ox, int oy) {
    const struct gui_image *icon = app_icon(app);

    if (icon)
        gui_draw_image(c, x - ox, y - oy, icon);
}

static void draw_window(struct gui_canvas *c, const struct window *w, int ox, int oy) {
    struct rect frame = frame_of(w), close = button_of(w, 0), maximize = button_of(w, 1), minimize = button_of(w, 2);
    uint32_t title_color = w == focused ? TITLE_FOCUSED : TITLE_UNFOCUSED;
    int text_y = w->y - TITLE_HEIGHT + (TITLE_HEIGHT - GUI_CHAR_HEIGHT) / 2 - oy;

    // Only the border's edges, as the title bar and contents cover the rest.
    gui_fill(c, frame.x - ox, frame.y - oy, frame.width, BORDER, BORDER_COLOR);
    gui_fill(c, frame.x - ox, frame.y + frame.height - BORDER - oy, frame.width, BORDER, BORDER_COLOR);
    gui_fill(c, frame.x - ox, frame.y - oy, BORDER, frame.height, BORDER_COLOR);
    gui_fill(c, frame.x + frame.width - BORDER - ox, frame.y - oy, BORDER, frame.height, BORDER_COLOR);
    gui_fill(c, w->x - ox, w->y - TITLE_HEIGHT - oy, w->canvas.width, TITLE_HEIGHT, title_color);
    draw_icon(c, w->app, w->x + 5, w->y - TITLE_HEIGHT + (TITLE_HEIGHT - ICON_SIZE) / 2, ox, oy);
    gui_text(c, w->x + 10 + ICON_SIZE - ox, text_y, w->title, w == focused ? TITLE_TEXT : TITLE_TEXT_UNFOCUSED, 1);

    // A title too long for the bar goes under the buttons.
    gui_fill(c, minimize.x - 4 - ox, w->y - TITLE_HEIGHT - oy, w->x + w->canvas.width - minimize.x + 4, TITLE_HEIGHT,
             title_color);
    gui_fill(c, close.x - ox, close.y - oy, BUTTON_SIZE, BUTTON_SIZE, CLOSE_COLOR);
    gui_text(c, close.x + (BUTTON_SIZE - GUI_CHAR_WIDTH) / 2 - ox, text_y, "x", TITLE_TEXT, 1);
    gui_fill(c, maximize.x - ox, maximize.y - oy, BUTTON_SIZE, BUTTON_SIZE, BUTTON_COLOR);
    gui_fill(c, maximize.x + 4 - ox, maximize.y + 4 - oy, 8, 8, TITLE_TEXT);
    gui_fill(c, maximize.x + 5 - ox, maximize.y + 6 - oy, 6, 5, BUTTON_COLOR);
    gui_fill(c, minimize.x - ox, minimize.y - oy, BUTTON_SIZE, BUTTON_SIZE, BUTTON_COLOR);
    gui_fill(c, minimize.x + 4 - ox, minimize.y + 11 - oy, 8, 2, TITLE_TEXT);
    gui_copy(c, w->x - ox, w->y - oy, &w->canvas, 0, 0, w->canvas.width, w->canvas.height);
}

// Windows in the order they were made, which is how the taskbar shows them.
static int windows_by_age(struct window *out[MAX_WINDOWS]) {
    int count = 0;

    for (int i = 0; i < window_count; i++) {
        int j = count++;

        while (j > 0 && out[j - 1]->id > windows[i]->id) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = windows[i];
    }
    return count;
}

static struct rect taskbar_button(int index, int count) {
    int start = LAUNCHER_WIDTH + 12, room = screen.width - start - 64;
    int width = count && room / count < TASKBAR_BUTTON_WIDTH ? room / count : TASKBAR_BUTTON_WIDTH;

    return (struct rect){start + index * width, taskbar().y + 4, width - 4, TASKBAR_HEIGHT - 8};
}

// Text cut to fit a width, which is how titles go on the taskbar.
static void fitted_text(struct gui_canvas *c, int x, int y, int width, const char *text, uint32_t color) {
    char shown[GUI_TITLE_MAX];
    int fits = width / GUI_CHAR_WIDTH;

    snprintf(shown, sizeof(shown), "%.*s", fits > 0 ? fits : 0, text);
    gui_text(c, x, y, shown, color, 1);
}

static void draw_taskbar(struct gui_canvas *c, int ox, int oy) {
    struct rect bar = taskbar(), l = launcher();
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);
    int text_y = bar.y + (TASKBAR_HEIGHT - GUI_CHAR_HEIGHT) / 2 - oy;
    char clock[16], *shown;
    time_t now = time(NULL);

    gui_fill(c, bar.x - ox, bar.y - oy, bar.width, bar.height, TASKBAR_COLOR);
    gui_fill(c, bar.x - ox, bar.y - oy, bar.width, 1, TASKBAR_LINE);
    gui_fill(c, l.x - ox, l.y - oy, l.width, l.height, TASKBAR_BUTTON);
    gui_text(c, l.x + (l.width - gui_text_width("Apps", 1)) / 2 - ox, text_y, "Apps", TASKBAR_TEXT, 1);

    for (int i = 0; i < count; i++) {
        struct rect b = taskbar_button(i, count);
        struct window *w = list[i];
        uint32_t color = w == focused ? TITLE_FOCUSED : w->minimized ? TASKBAR_MINIMIZED : TASKBAR_BUTTON;

        gui_fill(c, b.x - ox, b.y - oy, b.width, b.height, color);
        draw_icon(c, w->app, b.x + 4, b.y + (b.height - ICON_SIZE) / 2, ox, oy);
        fitted_text(c, b.x + 8 + ICON_SIZE - ox, text_y, b.width - 12 - ICON_SIZE, w->title,
                    w == focused ? TITLE_TEXT : w->minimized ? TASKBAR_DIM_TEXT : TASKBAR_TEXT);
    }

    // 12 hour time goes without a leading zero, as people write it.
    strftime(clock, sizeof(clock), settings.clock_12_hour ? "%I:%M %p" : "%H:%M", localtime(&now));
    shown = clock + (settings.clock_12_hour && clock[0] == '0');
    gui_text(c, bar.width - gui_text_width(shown, 1) - 12 - ox, text_y, shown, TASKBAR_TEXT, 1);
}

static void draw_panel(struct gui_canvas *c, struct rect r, int ox, int oy) {
    gui_fill(c, r.x - ox, r.y - oy, r.width, r.height, TASKBAR_LINE);
    gui_fill(c, r.x + 1 - ox, r.y + 1 - oy, r.width - 2, r.height - 2, TASKBAR_COLOR);
}

// A row in the menu or the switcher, with the icon of app, if it's not NULL.
static void draw_item(struct gui_canvas *c, struct rect r, const char *text, const char *app, bool chosen,
                      uint32_t color, int ox, int oy) {
    int text_x = 12 + ICON_SIZE;

    if (chosen)
        gui_fill(c, r.x - ox, r.y - oy, r.width, r.height, TITLE_FOCUSED);
    if (app)
        draw_icon(c, app, r.x + 6, r.y + (r.height - ICON_SIZE) / 2, ox, oy);
    fitted_text(c, r.x + text_x - ox, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 - oy, r.width - text_x - 8, text,
                chosen ? TITLE_TEXT : color);
}

static void draw_menu(struct gui_canvas *c, int ox, int oy) {
    struct rect m = menu(), first_extra = menu_item(app_count);

    draw_panel(c, m, ox, oy);
    if (!app_count)
        draw_item(c, menu_item(0), "No apps in " APPS_DIR, NULL, false, TASKBAR_DIM_TEXT, ox, oy);
    for (int i = 0; i < menu_items(); i++)
        draw_item(c, menu_item(i), i < app_count ? apps[i] : extra_items[i - app_count],
                  i < app_count ? apps[i] : i == app_count + ITEM_RUN ? RUN_COMMAND : NULL,
                  i == menu_choice, TASKBAR_TEXT, ox, oy);
    gui_fill(c, m.x + 8 - ox, first_extra.y - MENU_GAP / 2 - 1 - oy, m.width - 16, 1, TASKBAR_LINE);
}

static void draw_switcher(struct gui_canvas *c, int ox, int oy) {
    struct rect s = switcher();

    draw_panel(c, s, ox, oy);
    for (int i = 0; i < window_count; i++) {
        struct window *w = switcher_window(i);

        draw_item(c, (struct rect){s.x + 4, s.y + 4 + i * ITEM_HEIGHT, s.width - 8, ITEM_HEIGHT}, w->title, w->app,
                  i == switch_choice, w->minimized ? TASKBAR_DIM_TEXT : TASKBAR_TEXT, ox, oy);
    }
}

static void draw_cursor(struct gui_canvas *c, int ox, int oy) {
    const struct cursor_shape *shape = &shapes[cursor_shown];
    struct rect r = cursor_rect();

    for (int y = 0; y < r.height; y++) {
        for (int x = 0; x < r.width; x++) {
            int col = shape->turned ? y : x, row = shape->turned ? x : y;
            char p = shape->rows[row][shape->mirrored ? shape->width - 1 - col : col];

            if (p != ' ')
                gui_fill(c, r.x + x - ox, r.y + y - oy, 1, 1, p == 'X' ? 0x000000 : 0xFFFFFF);
        }
    }
}

// Draws everything that overlaps a rectangle, through a canvas that is only that
// part of the screen.
static void compose(struct rect r) {
    struct gui_canvas c = {screen.pixels + r.y * screen.stride + r.x, r.width, r.height, screen.stride};
    int hint_y = desktop().height - 2 * GUI_CHAR_HEIGHT, first = -1;
    struct rect bar = taskbar();

    // Nothing under a window that covers all of it can be seen.
    for (int i = window_count - 1; i >= 0 && first < 0; i--) {
        if (covers(windows[i], r))
            first = i;
    }
    if (first < 0) {
        if (wallpaper.pixels)
            gui_copy(&c, 0, 0, &wallpaper, r.x, r.y, r.width, r.height);
        else
            gui_fill(&c, 0, 0, r.width, r.height, DESKTOP_COLOR);
        gui_text(&c, GUI_CHAR_WIDTH - r.x, hint_y - r.y, HINT, HINT_COLOR, 1);
        first = 0;
    }
    for (int i = first; i < window_count; i++) {
        if (!windows[i]->minimized && overlaps(extent_of(windows[i]), r)) {
            if (settings.shadows)
                draw_shadow(&c, windows[i], r.x, r.y);
            draw_window(&c, windows[i], r.x, r.y);
        }
    }
    if (r.y + r.height > bar.y)
        draw_taskbar(&c, r.x, r.y);
    if (menu_open)
        draw_menu(&c, r.x, r.y);
    if (switch_choice >= 0)
        draw_switcher(&c, r.x, r.y);
    draw_cursor(&c, r.x, r.y);
}

// Composes all that changed before showing any of it, so a window being moved is
// never seen gone from both places.
static void redraw(void) {
    for (int i = 0; i < damage_count; i++)
        compose(damage[i]);
    for (int i = 0; i < damage_count; i++) {
        struct rect r = damage[i];

        for (int row = r.y; row < r.y + r.height; row++)
            memcpy(framebuffer + (long)row * pitch + r.x * sizeof(uint32_t), screen.pixels + (long)row * screen.stride + r.x,
                   r.width * sizeof(uint32_t));
    }
    damage_count = 0;
}

// Sends without blocking, so a program that stops reading can't stop wm. Mouse
// movement is dropped when it's falling behind, and anything else disconnects it.
static void queue_message(struct client *client, const struct gui_message *m) {
    bool droppable = m->type == GUI_MOUSE_MOVE;

    if (OUTBOX_SIZE - client->out_used < sizeof(*m) * (droppable ? 8 : 1)) {
        client->stuck |= !droppable;
        return;
    }
    memcpy(client->outbox + client->out_used, m, sizeof(*m));
    client->out_used += sizeof(*m);
}

static bool flush_client(struct client *client) {
    if (client->stuck)
        return false;
    while (client->out_used > 0) {
        ssize_t n = write(client->fd, client->outbox, client->out_used);

        if (n < 0)
            return errno == EAGAIN || errno == EINTR;
        memmove(client->outbox, client->outbox + n, client->out_used - n);
        client->out_used -= n;
    }
    return true;
}

static void send_to(struct window *w, struct gui_message m) {
    m.window = w->id;
    queue_message(w->client, &m);
}

static long now_ms(void) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static void fade(struct window *w, int to) {
    w->fade_from = w->shadow;
    w->fade_to = to;
    w->fade_start = now_ms();
}

// Sets each fading shadow to how far it is through its fade, eased out as
// 1 - (1 - t)^3, returning whether any has further to go.
static bool fade_shadows(void) {
    const long whole = (long)FADE_MS * FADE_MS * FADE_MS;
    long now = now_ms();
    bool fading = false;

    for (int i = 0; i < window_count; i++) {
        struct window *w = windows[i];
        long left = FADE_MS - (now - w->fade_start);

        if (w->shadow == w->fade_to)
            continue;
        left = left < 0 ? 0 : left;
        w->shadow = w->fade_from + (w->fade_to - w->fade_from) * (whole - left * left * left) / whole;
        fading |= w->shadow != w->fade_to;
        if (!w->minimized)
            add_damage(extent_of(w));
    }
    return fading;
}

static void set_focus(struct window *w) {
    if (w == focused)
        return;
    damage_taskbar();
    if (focused) {
        send_to(focused, (struct gui_message){.type = GUI_FOCUS, .focused = 0});
        damage_title(focused);
        fade(focused, SHADOW_UNFOCUSED);
    }
    focused = w;
    if (w) {
        send_to(w, (struct gui_message){.type = GUI_FOCUS, .focused = 1});
        damage_title(w);
        fade(w, SHADOW_FOCUSED);
    }
}

static int index_of(const struct window *w) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i] == w)
            return i;
    }
    return -1;
}

static void raise_window(struct window *w) {
    int i = index_of(w);

    memmove(&windows[i], &windows[i + 1], (window_count - i - 1) * sizeof(*windows));
    windows[window_count - 1] = w;
    add_damage(extent_of(w));
}

static void load_settings(void);

// Opens shared memory a message names, removing the name. Returns its file, or -1.
static int open_shared(const struct gui_message *m, int flags) {
    char name[GUI_TITLE_MAX];
    int fd;

    // A name that isn't terminated is cut short rather than trusted.
    memcpy(name, m->buffer, sizeof(name));
    name[sizeof(name) - 1] = '\0';
    fd = shm_open(name, flags, 0600);
    shm_unlink(name);
    return fd;
}

// Maps the pixels a message names, and removes the name.
static bool map_buffer(const struct gui_message *m, struct gui_canvas *out) {
    size_t size = (size_t)m->width * m->height * sizeof(uint32_t);

    if (m->width <= 0 || m->height <= 0 || m->width > MAX_SIZE || m->height > MAX_SIZE)
        return false;

    int fd = open_shared(m, O_RDONLY);

    if (fd < 0)
        return false;
    *out = (struct gui_canvas){mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0), m->width, m->height, m->width};
    close(fd);
    return out->pixels != MAP_FAILED;
}

static void unmap_buffer(struct gui_canvas *c) {
    munmap(c->pixels, (size_t)c->width * c->height * sizeof(uint32_t));
}

// The top window that isn't minimized gets the focus.
static void focus_top(void) {
    for (int i = window_count - 1; i >= 0; i--) {
        if (!windows[i]->minimized) {
            set_focus(windows[i]);
            return;
        }
    }
    set_focus(NULL);
}

static void minimize(struct window *w) {
    w->minimized = true;
    add_damage(extent_of(w));
    damage_taskbar();
    if (focused == w)
        focus_top();
}

static void show(struct window *w) {
    w->minimized = false;
    raise_window(w);
    set_focus(w);
    damage_taskbar();
}

static void remove_window(struct window *w) {
    int i = index_of(w);

    add_damage(extent_of(w));
    if (switch_choice >= 0) {
        add_damage(switcher());
        switch_choice = window_count > 1 ? switch_choice % (window_count - 1) : -1;
    }
    damage_taskbar();
    memmove(&windows[i], &windows[i + 1], (window_count - i - 1) * sizeof(*windows));
    window_count--;
    if (dragging == w)
        dragging = NULL;
    if (grabbed == w)
        grabbed = NULL;
    if (resizing == w)
        resizing = NULL;
    if (focused == w) {
        focused = NULL;
        focus_top();
    }
    unmap_buffer(&w->canvas);
    free(w);
}

static struct window *find_window(const struct client *client, uint32_t id) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i]->client == client && windows[i]->id == id)
            return windows[i];
    }
    return NULL;
}

static void create_window(struct client *client, const struct gui_message *m) {
    struct gui_message reply = {.type = GUI_CREATED};
    struct window *w = window_count < MAX_WINDOWS ? calloc(1, sizeof(*w)) : NULL;

    if (!w || !map_buffer(m, &w->canvas)) {
        free(w);
        queue_message(client, &reply); // window 0 is a refusal
        return;
    }

    // Each new window goes a little further down and right than the last.
    int step = (next_id - 1) % 8;

    w->client = client;
    w->id = reply.window = next_id++;
    w->x = 80 + step * CASCADE;
    w->y = 60 + TITLE_HEIGHT + step * CASCADE;
    w->want_width = w->canvas.width;
    w->want_height = w->canvas.height;
    memcpy(w->title, m->text, sizeof(w->title));
    w->title[sizeof(w->title) - 1] = '\0';
    windows[window_count++] = w;
    damage_taskbar();
    if (switch_choice >= 0)
        add_damage(switcher());
    queue_message(client, &reply);
    queue_message(client, &(struct gui_message){
                              .type = GUI_SCREEN, .width = screen.width, .height = screen.height, .x = can_set_mode});
    add_damage(extent_of(w));
    set_focus(w);
}

// Asks for a new size, unless an answer is still awaited, in which case this is
// asked for once it comes.
static void ask_size(struct window *w, int width, int height) {
    struct rect d = desktop();

    w->want_width = width < MIN_WIDTH ? MIN_WIDTH : width > d.width ? d.width : width;
    w->want_height = height < MIN_HEIGHT ? MIN_HEIGHT : height > d.height ? d.height : height;
    if (w->asking || (w->want_width == w->canvas.width && w->want_height == w->canvas.height))
        return;
    w->asking = true;
    w->asked_width = w->want_width;
    w->asked_height = w->want_height;
    send_to(w, (struct gui_message){.type = GUI_RESIZE, .width = w->want_width, .height = w->want_height});
}

static void take_buffer(struct window *w, const struct gui_message *m) {
    struct gui_canvas c;

    if (!map_buffer(m, &c))
        return;
    add_damage(extent_of(w));
    if (w->resize_edges & EDGE_LEFT)
        w->x += w->canvas.width - c.width;
    if (w->resize_edges & EDGE_TOP)
        w->y += w->canvas.height - c.height;
    unmap_buffer(&w->canvas);
    w->canvas = c;
    add_damage(extent_of(w));
    w->asking = false;
    if (w->want_width != w->asked_width || w->want_height != w->asked_height)
        ask_size(w, w->want_width, w->want_height);
}

static void set_clipboard(const struct gui_message *m) {
    size_t length = m->width > 0 && m->width <= CLIPBOARD_MAX ? (size_t)m->width : 0;
    int fd = length ? open_shared(m, O_RDONLY) : -1;
    void *shared = fd < 0 ? MAP_FAILED : mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
    char *copy = shared == MAP_FAILED ? NULL : malloc(length);

    if (fd >= 0)
        close(fd);
    if (copy)
        memcpy(copy, shared, length);
    if (shared != MAP_FAILED)
        munmap(shared, length);
    free(clipboard);
    clipboard = copy;
    clipboard_length = copy ? length : 0;
}

// Answers with the clipboard in new shared memory, which the program removes.
static void send_clipboard(struct client *client) {
    static int created;
    struct gui_message reply = {.type = GUI_CLIPBOARD};
    int fd = -1;
    void *shared = MAP_FAILED;

    if (clipboard_length) {
        snprintf(reply.buffer, sizeof(reply.buffer), "/wm-clipboard-%d", created++);
        fd = shm_open(reply.buffer, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 && ftruncate(fd, clipboard_length) == 0)
            shared = mmap(NULL, clipboard_length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (fd >= 0)
            close(fd);
    }
    if (shared != MAP_FAILED) {
        memcpy(shared, clipboard, clipboard_length);
        munmap(shared, clipboard_length);
        reply.width = (int32_t)clipboard_length;
    } else if (clipboard_length) {
        shm_unlink(reply.buffer);
    }
    queue_message(client, &reply);
}

static void update_cursor(void);

static void handle_message(struct client *client, const struct gui_message *m) {
    struct window *w = find_window(client, m->window);

    switch (m->type) {
    case GUI_CREATE:
        create_window(client, m);
        break;
    case GUI_DAMAGE:
        if (w)
            add_damage((struct rect){w->x + m->x, w->y + m->y, m->width, m->height});
        break;
    case GUI_DESTROY:
        if (w)
            remove_window(w);
        break;
    case GUI_BUFFER:
        if (w)
            take_buffer(w, m);
        break;
    case GUI_TITLE:
        if (w) {
            memcpy(w->title, m->text, sizeof(w->title));
            w->title[sizeof(w->title) - 1] = '\0';
            damage_title(w);
            damage_taskbar();
        }
        break;
    case GUI_CLIPBOARD_SET:
        set_clipboard(m);
        break;
    case GUI_CLIPBOARD_GET:
        send_clipboard(client);
        break;
    case GUI_SETTINGS:
        load_settings();
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i])
                queue_message(clients[i], &(struct gui_message){.type = GUI_SETTINGS});
        }
        break;
    case GUI_ICON:
        if (w) {
            memcpy(w->app, m->text, sizeof(w->app));
            w->app[sizeof(w->app) - 1] = '\0';
            add_damage(extent_of(w));
            damage_taskbar();
        }
        break;
    case GUI_CURSOR:
        if (w && m->x >= 0 && m->x <= GUI_CURSOR_RESIZE_NESW) {
            w->cursor = m->x;
            update_cursor();
        }
        break;
    }
}

static void disconnect(int i) {
    for (int j = window_count - 1; j >= 0; j--) {
        if (windows[j]->client == clients[i])
            remove_window(windows[j]);
    }
    close(clients[i]->fd);
    free(clients[i]);
    clients[i] = NULL;
}

static void read_client(int i) {
    struct client *client = clients[i];
    ssize_t n = read(client->fd, client->inbox + client->in_used, INBOX_SIZE - client->in_used);

    if (n < 0 && (errno == EAGAIN || errno == EINTR))
        return;
    if (n <= 0) {
        disconnect(i);
        return;
    }
    client->in_used += n;

    size_t used = 0;

    for (; client->in_used - used >= sizeof(struct gui_message); used += sizeof(struct gui_message)) {
        struct gui_message m;

        memcpy(&m, client->inbox + used, sizeof(m));
        handle_message(client, &m);
    }
    memmove(client->inbox, client->inbox + used, client->in_used - used);
    client->in_used -= used;
}

static void accept_client(int listener) {
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    int i = 0;

    if (fd < 0)
        return;
    while (i < MAX_CLIENTS && clients[i])
        i++;
    if (i == MAX_CLIENTS || !(clients[i] = calloc(1, sizeof(struct client)))) {
        close(fd);
        return;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    clients[i]->fd = fd;
}

// Which edges of a frame a resize from a point would move: those it's on, or near
// the end of.
static int edges_at(struct rect f, int x, int y) {
    bool left = x < f.x + BORDER, right = x >= f.x + f.width - BORDER;
    bool top = y < f.y + BORDER, bottom = y >= f.y + f.height - BORDER;

    if (left || right) {
        top |= y < f.y + CORNER;
        bottom |= y >= f.y + f.height - CORNER;
    } else if (top || bottom) {
        left = x < f.x + CORNER;
        right = x >= f.x + f.width - CORNER;
    }
    return (left ? EDGE_LEFT : 0) | (right ? EDGE_RIGHT : 0) | (top ? EDGE_TOP : 0) | (bottom ? EDGE_BOTTOM : 0);
}

// Which window is at a point, and which part of it. edges says which edges a
// resize there would move.
static struct window *window_at(int x, int y, enum part *part, int *edges) {
    for (int i = window_count - 1; i >= 0; i--) {
        struct window *w = windows[i];
        struct rect f = frame_of(w);
        struct rect grab = {f.x - RESIZE_MARGIN, f.y - RESIZE_MARGIN, f.width + 2 * RESIZE_MARGIN,
                            f.height + 2 * RESIZE_MARGIN};

        if (w->minimized || !contains(grab, x, y))
            continue;
        *edges = edges_at(f, x, y);
        *part = *edges ? PART_RESIZE
              : contains(button_of(w, 0), x, y) ? PART_CLOSE
              : contains(button_of(w, 1), x, y) ? PART_MAXIMIZE
              : contains(button_of(w, 2), x, y) ? PART_MINIMIZE
              : contains((struct rect){w->x, w->y, w->canvas.width, w->canvas.height}, x, y) ? PART_CONTENT
                                                                                            : PART_TITLE;
        return w;
    }
    *part = PART_NONE;
    return NULL;
}

static void send_mouse(struct window *w, uint32_t type, uint32_t changed, int wheel) {
    send_to(w, (struct gui_message){
                   .type = type, .x = cursor_x - w->x, .y = cursor_y - w->y,
                   .buttons = buttons, .changed = changed, .wheel = wheel,
               });
}

static void launch(const char *command) {
    if (fork() == 0) {
        execl("/system/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
}

static void launch_app(const char *name) {
    char path[sizeof(APPS_DIR) + GUI_TITLE_MAX];

    snprintf(path, sizeof(path), APPS_DIR "/%s", name);
    if (fork() == 0) {
        execl(path, name, (char *)NULL);
        _exit(127);
    }
}

static int compare_names(const void *a, const void *b) {
    return strcmp(a, b);
}

// Lists the apps afresh each time, so new ones show up.
static void open_menu(void) {
    DIR *dir = opendir(APPS_DIR);
    struct dirent *entry;

    app_count = 0;
    while (dir && app_count < MAX_APPS && (entry = readdir(dir))) {
        if (entry->d_name[0] != '.')
            snprintf(apps[app_count++], GUI_TITLE_MAX, "%s", entry->d_name);
    }
    if (dir)
        closedir(dir);
    qsort(apps, app_count, sizeof(*apps), compare_names);
    menu_open = true;
    menu_choice = 0;
    add_damage(menu());
}

static void close_menu(void) {
    if (menu_open)
        add_damage(menu());
    menu_open = false;
}

static void choose(int index) {
    if (index >= 0 && index < app_count)
        launch_app(apps[index]);
    else if (index == app_count + ITEM_RUN)
        launch(RUN_COMMAND);
    else if (index >= app_count && index < menu_items())
        reboot(index == app_count + ITEM_REBOOT ? RB_AUTOBOOT : RB_POWER_OFF);
    close_menu();
}

static void move_window(struct window *w, int x, int y) {
    add_damage(extent_of(w));
    w->x = x;
    w->y = y;
    add_damage(extent_of(w));
}

// Puts a window's frame on part of the desktop, keeping where it was to go back to.
static void place(struct window *w, struct rect frame) {
    if (!w->tiled)
        w->restore = frame_of(w);
    w->tiled = true;
    w->resize_edges = 0;
    move_window(w, frame.x + BORDER, frame.y + TITLE_HEIGHT + BORDER);
    ask_size(w, frame.width - 2 * BORDER, frame.height - TITLE_HEIGHT - 2 * BORDER);
}

static void untile(struct window *w) {
    struct rect r = w->restore;

    w->tiled = false;
    w->resize_edges = 0;
    move_window(w, r.x + BORDER, r.y + TITLE_HEIGHT + BORDER);
    ask_size(w, r.width - 2 * BORDER, r.height - TITLE_HEIGHT - 2 * BORDER);
}

static void toggle_maximized(struct window *w) {
    if (w->tiled)
        untile(w);
    else
        place(w, desktop());
}

static void tile_half(struct window *w, bool right) {
    struct rect d = desktop();

    place(w, (struct rect){right ? d.width / 2 : 0, 0, d.width / 2, d.height});
}

static void taskbar_click(void) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    if (contains(launcher(), cursor_x, cursor_y))
        open_menu();
    for (int i = 0; i < count; i++) {
        if (!contains(taskbar_button(i, count), cursor_x, cursor_y))
            continue;
        if (list[i] == focused)
            minimize(list[i]);
        else
            show(list[i]);
    }
}

static void press(uint32_t changed) {
    enum part part;
    int edges;
    struct window *w = window_at(cursor_x, cursor_y, &part, &edges);

    super_alone = false;

    // A click anywhere closes the menu, and does what it would have done unless it
    // was in the menu or on the launcher.
    if (menu_open) {
        struct rect m = menu();
        bool in_menu = contains(m, cursor_x, cursor_y);

        if (in_menu && (changed & MOUSE_BUTTON_LEFT))
            choose(menu_item_at(cursor_x, cursor_y));
        close_menu();
        if (in_menu || contains(launcher(), cursor_x, cursor_y))
            return;
    }
    if (contains(taskbar(), cursor_x, cursor_y)) {
        if (changed & MOUSE_BUTTON_LEFT)
            taskbar_click();
        return;
    }
    if (!w)
        return;
    set_focus(w);
    if (w != windows[window_count - 1])
        raise_window(w);

    if (part == PART_CONTENT) {
        grabbed = w;
        send_mouse(w, GUI_MOUSE_BUTTON, changed, 0);
    } else if (part == PART_CLOSE) {
        send_to(w, (struct gui_message){.type = GUI_CLOSE});
    } else if (part == PART_MAXIMIZE) {
        toggle_maximized(w);
    } else if (part == PART_MINIMIZE) {
        minimize(w);
    } else if (part == PART_RESIZE && (changed & MOUSE_BUTTON_LEFT)) {
        resizing = w;
        w->resize_edges = edges;
        resize_x = cursor_x;
        resize_y = cursor_y;
        resize_width = w->canvas.width;
        resize_height = w->canvas.height;
        resize_room = frame_of(w).y;
        w->tiled = false;
    } else if (changed & MOUSE_BUTTON_LEFT) {
        w->tiled = false;
        dragging = w;
        drag_x = cursor_x - w->x;
        drag_y = cursor_y - w->y;
    }
}

static void handle_mouse(const struct mouse_event *e) {
    enum part part;
    int edges;
    uint32_t pressed = e->buttons & ~buttons, released = buttons & ~e->buttons;
    int x = e->absolute ? (int)((int64_t)e->x * screen.width / (MOUSE_ABSOLUTE_MAX + 1)) : cursor_x + e->dx;
    int y = e->absolute ? (int)((int64_t)e->y * screen.height / (MOUSE_ABSOLUTE_MAX + 1)) : cursor_y + e->dy;

    x = x < 0 ? 0 : x >= screen.width ? screen.width - 1 : x;
    y = y < 0 ? 0 : y >= screen.height ? screen.height - 1 : y;

    bool moved = x != cursor_x || y != cursor_y;

    if (moved) {
        damage_cursor();
        cursor_x = x;
        cursor_y = y;
        damage_cursor();
    }
    buttons = e->buttons;

    // Windows can be dragged partly off the screen, but the title bar stays on it.
    if (dragging && moved) {
        int top = cursor_y - drag_y, bottom = desktop().height - BORDER;

        move_window(dragging, cursor_x - drag_x, top < TITLE_HEIGHT ? TITLE_HEIGHT : top > bottom ? bottom : top);
    }
    if (resizing && moved) {
        int edges = resizing->resize_edges, dx = cursor_x - resize_x, dy = cursor_y - resize_y;

        if (edges & EDGE_TOP && dy < -resize_room)
            dy = -resize_room;
        ask_size(resizing, resize_width + (edges & EDGE_RIGHT ? dx : edges & EDGE_LEFT ? -dx : 0),
                 resize_height + (edges & EDGE_BOTTOM ? dy : edges & EDGE_TOP ? -dy : 0));
    }

    // The menu highlights what's under the mouse.
    if (menu_open && moved) {
        int choice = menu_item_at(cursor_x, cursor_y);

        if (choice >= 0 && choice != menu_choice) {
            menu_choice = choice;
            add_damage(menu());
        }
    }

    struct window *target = grabbed ? grabbed : dragging || resizing ? NULL : window_at(cursor_x, cursor_y, &part, &edges);

    if (target && !grabbed && part != PART_CONTENT)
        target = NULL;
    if (target && moved)
        send_mouse(target, GUI_MOUSE_MOVE, 0, 0);
    if (target && e->wheel)
        send_mouse(target, GUI_MOUSE_WHEEL, 0, e->wheel);

    if (pressed)
        press(pressed);
    if (released && grabbed)
        send_mouse(grabbed, GUI_MOUSE_BUTTON, released, 0);
    if (!buttons) {
        dragging = NULL;
        grabbed = NULL;
        resizing = NULL;
    }
    update_cursor();
}

static void finish_switch(bool chosen) {
    struct window *w = switcher_window(switch_choice);

    add_damage(switcher());
    switch_choice = -1;
    if (chosen)
        show(w);
}

// Alt+Tab goes down the windows, from the one under the focused one, and Shift goes
// back up. Letting go of Alt picks the one it's on, and Escape stays where it was.
static bool switch_key(const struct key_event *e) {
    if (e->code == KEY_TAB && (e->modifiers & KEY_MOD_ALT)) {
        if (e->pressed && window_count) {
            int step = e->modifiers & KEY_MOD_SHIFT ? -1 : 1;

            switch_choice = ((switch_choice < 0 ? 0 : switch_choice) + step + window_count) % window_count;
            add_damage(switcher());
        }
        return true;
    }
    if (switch_choice < 0)
        return false;
    if (e->code == KEY_ESCAPE && e->pressed) {
        finish_switch(false);
        return true;
    }
    if (!(e->modifiers & KEY_MOD_ALT))
        finish_switch(true);
    return false;
}

// Up and Down move through the menu, Enter runs what's highlighted, and Escape closes it.
static bool menu_key(const struct key_event *e) {
    if (!menu_open || (e->modifiers & KEY_MOD_SUPER)
        || !(e->code == KEY_UP || e->code == KEY_DOWN || e->code == KEY_ENTER || e->code == KEY_ESCAPE))
        return false;
    if (!e->pressed)
        return true;
    if (e->code == KEY_ENTER)
        choose(menu_choice);
    else if (e->code == KEY_ESCAPE)
        close_menu();
    else {
        menu_choice = (menu_choice + (e->code == KEY_DOWN ? 1 : menu_items() - 1)) % menu_items();
        add_damage(menu());
    }
    return true;
}

// Super pressed and let go on its own opens or closes the menu.
static void super_key(const struct key_event *e) {
    bool is_super = e->code == KEY_LEFT_SUPER || e->code == KEY_RIGHT_SUPER;

    if (is_super && e->pressed && !super_down)
        super_alone = true;
    else if (!is_super && e->pressed)
        super_alone = false;
    if (is_super)
        super_down = e->pressed;
    if (is_super && !e->pressed && super_alone) {
        if (menu_open)
            close_menu();
        else
            open_menu();
    }
}

static enum gui_cursor resize_cursor(int edges) {
    bool across = edges & (EDGE_LEFT | EDGE_RIGHT), down = edges & (EDGE_TOP | EDGE_BOTTOM);

    if (across && down)
        return !(edges & EDGE_LEFT) == !(edges & EDGE_TOP) ? GUI_CURSOR_RESIZE_NWSE : GUI_CURSOR_RESIZE_NESW;
    return across ? GUI_CURSOR_RESIZE_H : GUI_CURSOR_RESIZE_V;
}

// The cursor shows what dragging would do: resize on an edge, and over a window's
// contents, whatever its program wants.
static void update_cursor(void) {
    enum part part = PART_NONE;
    int edges = 0;
    struct window *w = resizing ? resizing : grabbed ? grabbed : window_at(cursor_x, cursor_y, &part, &edges);
    enum gui_cursor shape = resizing                                         ? resize_cursor(resizing->resize_edges)
                          : grabbed || (w && part == PART_CONTENT && !menu_open) ? w->cursor
                          : part == PART_RESIZE && !dragging                 ? resize_cursor(edges)
                                                                             : GUI_CURSOR_ARROW;

    if (shape == cursor_shown)
        return;
    damage_cursor();
    cursor_shown = shape;
    damage_cursor();
}

// Super with something else is for wm, and every other key goes to the focused window.
static void handle_key(const struct key_event *e) {
    bool super = e->modifiers & KEY_MOD_SUPER;

    super_key(e);
    if (switch_key(e) || menu_key(e))
        return;
    if (super && e->code == KEY_Q) {
        if (e->pressed && (e->modifiers & KEY_MOD_SHIFT))
            quitting = true;
        else if (e->pressed && focused)
            send_to(focused, (struct gui_message){.type = GUI_CLOSE});
        return;
    }
    if (super && (e->code == KEY_ENTER || e->code == KEY_R)) {
        if (e->pressed)
            launch(e->code == KEY_R ? RUN_COMMAND : LAUNCH_COMMAND);
        return;
    }

    // Up maximizes, left and right take half the screen, and down goes back or minimizes.
    if (super && (e->code == KEY_UP || e->code == KEY_DOWN || e->code == KEY_LEFT || e->code == KEY_RIGHT)) {
        if (!e->pressed || !focused)
            return;
        if (e->code == KEY_UP)
            toggle_maximized(focused);
        else if (e->code == KEY_DOWN && focused->tiled)
            untile(focused);
        else if (e->code == KEY_DOWN)
            minimize(focused);
        else
            tile_half(focused, e->code == KEY_RIGHT);
        return;
    }
    if (focused)
        send_to(focused, (struct gui_message){.type = GUI_KEY, .key = *e});
}

// Scaled to cover the screen, keeping its shape, with what's left over either side
// cut off equally. It's only loaded again when it or the screen's size changes.
static void load_wallpaper(void) {
    char path[GUI_PATH_MAX];
    struct gui_image image;

    gui_wallpaper_get(path);
    if (!strcmp(path, wallpaper_path) && wallpaper.width == screen.width && wallpaper.height == screen.height)
        return;
    snprintf(wallpaper_path, sizeof(wallpaper_path), "%s", path);
    gui_image_free(&wallpaper_image);
    wallpaper = (struct gui_canvas){0};
    if (!strcmp(path, "none"))
        return;
    if (!gui_image_load(path, &image)) {
        printf("wm: wallpaper %s: %s\n", path, gui_image_error());
        return;
    }

    bool wider = (long)screen.width * image.height >= (long)screen.height * image.width;
    int width = wider ? screen.width : (int)((long)image.width * screen.height / image.height);
    int height = wider ? (int)((long)image.height * screen.width / image.width) : screen.height;
    bool ok = gui_image_scale(&image, width, height, &wallpaper_image);

    gui_image_free(&image);
    if (!ok) {
        printf("wm: wallpaper %s: %s\n", path, gui_image_error());
        return;
    }
    for (long i = 0; i < (long)width * height; i++)
        wallpaper_image.pixels[i] &= 0xFFFFFF;
    wallpaper = (struct gui_canvas){
        wallpaper_image.pixels + (long)(height - screen.height) / 2 * width + (width - screen.width) / 2,
        screen.width, screen.height, width,
    };
}

// Changes the screen's resolution. Tiled windows keep their share of the desktop,
// and the others move to stay on it.
static void change_resolution(int width, int height) {
    struct fb_info info;
    struct rect was = desktop(), now;
    uint32_t *pixels = malloc((size_t)width * height * sizeof(uint32_t));

    if (!pixels || fb_set_mode(fb, width, height) < 0 || fb_info(fb, &info) < 0) {
        free(pixels);
        printf("wm: can't change to %dx%d\n", width, height);
        return;
    }
    munmap(framebuffer, (size_t)pitch * screen.height);
    pitch = info.pitch;
    framebuffer = mmap(NULL, (size_t)pitch * info.height, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    free(screen.pixels);
    screen = (struct gui_canvas){pixels, info.width, info.height, info.width};
    if (framebuffer == MAP_FAILED) {
        quitting = true;
        return;
    }
    now = desktop();
    for (int i = 0; i < window_count; i++) {
        struct window *w = windows[i];
        struct rect f = frame_of(w);

        if (w->tiled) {
            place(w, (struct rect){f.x * now.width / was.width, f.y * now.height / was.height,
                                   f.width * now.width / was.width, f.height * now.height / was.height});
            continue;
        }
        f.x = f.x + f.width > now.width ? now.width - f.width : f.x;
        f.y = f.y + f.height > now.height ? now.height - f.height : f.y;
        move_window(w, (f.x > 0 ? f.x : 0) + BORDER, (f.y > 0 ? f.y : 0) + TITLE_HEIGHT + BORDER);
    }
    cursor_x = cursor_x < screen.width ? cursor_x : screen.width - 1;
    cursor_y = cursor_y < screen.height ? cursor_y : screen.height - 1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i])
            queue_message(clients[i], &(struct gui_message){
                                          .type = GUI_SCREEN, .width = screen.width, .height = screen.height, .x = 1});
    }
    printf("wm: running at %dx%d\n", screen.width, screen.height);
}

// Settings change the colours, shadows, clock, resolution and wallpaper, so
// everything is drawn again.
static void load_settings(void) {
    gui_settings_load(&settings);
    theme = settings.light ? &light : &dark;
    if (can_set_mode && settings.width && (settings.width != screen.width || settings.height != screen.height))
        change_resolution(settings.width, settings.height);
    load_wallpaper();
    add_damage((struct rect){0, 0, screen.width, screen.height});
}

static int fail(const char *what) {
    dprintf(STDERR_FILENO, "wm: %s: %s\n", what, strerror(errno));
    return 1;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: wm [command]...\n"
                            "Runs the window system, starting each command in it. Super+Shift+Q quits.\n");
    signal(SIGPIPE, SIG_IGN);

    struct fb_info info;
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);

    if (fb < 0 || fb_info(fb, &info) < 0)
        return fail("/dev/fb0");

    int mouse = open("/dev/mouse", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    int keyboard = open("/dev/keyboard", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = GUI_SOCKET};
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); // none of these for the programs it starts

    if (mouse < 0)
        return fail("/dev/mouse");
    if (keyboard < 0)
        return fail("/dev/keyboard");
    if (listener < 0 || bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(listener, 8) < 0)
        return fail(GUI_SOCKET);
    fcntl(listener, F_SETFL, O_NONBLOCK);

    pitch = info.pitch;
    can_set_mode = info.can_set_mode;
    framebuffer = mmap(NULL, (size_t)pitch * info.height, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    screen = (struct gui_canvas){malloc((size_t)info.width * info.height * sizeof(uint32_t)), info.width,
                                 info.height, info.width};
    if (framebuffer == MAP_FAILED || !screen.pixels)
        return fail("screen");

    cursor_x = screen.width / 2;
    cursor_y = screen.height / 2;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    for (int i = 1; i < argc; i++)
        launch(argv[i]);
    make_falloff();
    load_settings();
    printf("wm: running at %dx%d%s\n", screen.width, screen.height, can_set_mode ? "" : ", which can't change");
    fflush(stdout);

    while (!quitting) {
        struct pollfd fds[3 + MAX_CLIENTS] = {
            {.fd = listener, .events = POLLIN}, {.fd = mouse, .events = POLLIN}, {.fd = keyboard, .events = POLLIN},
        };

        for (int i = 0; i < MAX_CLIENTS; i++) {
            fds[3 + i].fd = clients[i] ? clients[i]->fd : -1;
            fds[3 + i].events = clients[i] ? POLLIN | (clients[i]->out_used ? POLLOUT : 0) : 0;
        }
        bool fading = fade_shadows();

        redraw();

        // The clock changes at the start of each minute, and fading shadows each frame.
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        int until_minute = (60 - now.tv_sec % 60) * 1000 - now.tv_nsec / 1000000;
        int timeout = fading ? FRAME_MS : until_minute > 0 ? until_minute : 1;

        int ready = poll(fds, 3 + MAX_CLIENTS, timeout);

        if (ready < 0 && errno != EINTR)
            return fail("poll");
        if (ready == 0)
            damage_taskbar();

        if (fds[0].revents & POLLIN)
            accept_client(listener);

        struct mouse_event mouse_events[32];
        ssize_t n = fds[1].revents & POLLIN ? read(mouse, mouse_events, sizeof(mouse_events)) : 0;

        for (ssize_t i = 0; i < n / (ssize_t)sizeof(*mouse_events); i++)
            handle_mouse(&mouse_events[i]);

        struct key_event key_events[32];
        n = fds[2].revents & POLLIN ? read(keyboard, key_events, sizeof(key_events)) : 0;
        for (ssize_t i = 0; i < n / (ssize_t)sizeof(*key_events); i++)
            handle_key(&key_events[i]);

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] && (fds[3 + i].revents & (POLLIN | POLLHUP | POLLERR)))
                read_client(i);
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] && !flush_client(clients[i]))
                disconnect(i);
        }
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
    }
    printf("wm: quit\n");
    return 0;
}
