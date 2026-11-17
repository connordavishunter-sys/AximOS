#include <arpa/inet.h>
#include <audio_types.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <mouse_event.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_CLIENTS 32
#define MAX_WINDOWS 64
#define MAX_DAMAGE 32
#define MAX_SIZE 4096
#define CLIPBOARD_MAX (64 * 1024 * 1024)
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
#define SHADOW 8
#define SHADOW_DROP 0
#define SHADOW_ALPHA 80
#define SHADOW_FOCUSED 256
#define SHADOW_UNFOCUSED 140
#define FADE_MS 240
#define FRAME_MS 16

#define TASKBAR_HEIGHT 32
#define BAR_ALPHA 200 // out of 256: the taskbar and title bars show a little of what's under them
#define RADIUS 8      // of windows', menus' and panels' corners
#define SMALL_RADIUS 4 // of buttons and highlights
#define TASKBAR_BUTTON_WIDTH 160
#define LAUNCHER_WIDTH 64

#define APPS_DIR "/system/bin/gui"
#define ICONS "/system/share/icons"
#define MAX_ICONS 32
#define ICON_SIZE 16

#define MENU_WIDTH 200
#define ITEM_HEIGHT 24 // in menus and the Alt+Tab switcher
#define MENU_GAP 9     // a line between items in a menu
#define SWITCHER_WIDTH 360

// Colours, from the theme the settings choose. What's on the focused title bar,
// the red close button and the grey ones is white in both.
struct theme {
    uint32_t desktop, hint, border, title_focused, title_unfocused, title_text_unfocused;
    uint32_t taskbar, taskbar_line, taskbar_button, taskbar_minimized, text, dim_text, on_accent;
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
static struct theme theme_now; // the one chosen, with the accent colour
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
#define ON_ACCENT (theme->on_accent) // text on the accent colour
#define CLOSE_COLOR 0xE5484D
#define BUTTON_COLOR 0x56607A

// Super+Enter runs this.
#define LAUNCH_COMMAND "term"
#define RUN_COMMAND "run" // Super+R, and Run... in the menu
#define KEY_Q 0x10
#define KEY_R 0x13
#define KEY_L 0x26
#define TASKS_COMMAND "tasks" // Ctrl+Shift+Escape
#define HINT "Super: apps   Super+R: run   Super+Enter: terminal   Alt+Tab: switch   Super+Q: close   Super+arrows: tile   " \
             "Super+1-4: workspaces   Super+Shift+Q: quit"

// Four workspaces, each with its own windows, which Super and a number goes to, and
// with Shift as well, sends the focused window to.
#define WORKSPACES 4
#define KEY_1 0x02
#define WORKSPACE_SIZE 16 // each one's box on the taskbar, after the launcher
#define WORKSPACE_GAP 4

// Dragging a window to the left or right edge of the screen tiles it on that half,
// and to the top maximizes it, as an outline shows while it's held there.
enum snap { SNAP_NONE, SNAP_LEFT, SNAP_RIGHT, SNAP_TOP };
#define SNAP_BORDER 3

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
    bool bare; // the log-in screen's, with no frame, in the middle of the screen
    bool translucent; // its pixels' top bytes say how see-through they are, 0 for not at all
    int workspace;

    // While it opens or is restored, it grows from anim_from to its frame over
    // anim_ms, drawn from layer, a copy of it at full size.
    int anim_ms;
    int64_t anim_start;
    struct rect anim_from;
    struct gui_canvas layer;
};

enum part { PART_NONE, PART_CONTENT, PART_TITLE, PART_CLOSE, PART_MAXIMIZE, PART_MINIMIZE, PART_RESIZE };
enum { EDGE_LEFT = 1, EDGE_RIGHT = 2, EDGE_TOP = 4, EDGE_BOTTOM = 8 };

static struct gui_canvas screen; // composed here, then copied to the framebuffer
static uint8_t *framebuffer;
static int pitch, fb, keyboard;
static bool can_set_mode;

static struct client *clients[MAX_CLIENTS];
static struct window *windows[MAX_WINDOWS]; // bottom first
static int window_count;
static uint32_t next_id = 1;
static struct window *focused;

static uint8_t falloff[SHADOW + 1][SHADOW + 1];
// Out of 256, how much of each pixel in a corner, from the corner, is inside a
// window's rounded edge.
static uint16_t coverage[RADIUS][RADIUS];
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
static int clipboard_width, clipboard_height; // a picture's, or 0 for text

static struct rect damage[MAX_DAMAGE];
static int damage_count;

static int cursor_x, cursor_y;
static uint32_t buttons;
static struct window *dragging; // by its title bar
static int drag_x, drag_y;
static struct window *grabbed; // pressed in its contents, so it gets the mouse until release
static char drag_file[GUI_PATH_MAX]; // dragged from grabbed, to drop on a window
static const struct gui_image *drag_icon;
static uint16_t modifiers; // the keys held, which go with a drop: Ctrl copies, rather than moving
static struct window *resizing;
static int resize_x, resize_y, resize_width, resize_height;
static int resize_room; // above the frame, which it can't grow into
static bool quitting;
static int workspace; // the one shown
static enum snap snap; // where the window being dragged would go, if let go now

// With --login, the desktop is locked until the log-in screen, which wm starts
// with a connection of its own, says someone's logged in. While it's locked only
// that screen's window is shown, and it gets all the input.
#define LOGIN_PATH "/system/bin/login"
static bool login_mode, locked, logged_in;
static struct client *session; // the log-in screen's connection
static int64_t login_due = -1, login_started; // when to start it again, if it stops
static char **commands; // to start once someone first logs in
static int command_count;

// Menus: the start menu, and those from right-clicking the desktop, an icon on
// it, or the taskbar, with one item's submenu open at a time beside it.
enum action { ACT_NONE, ACT_LINE, ACT_APP, ACT_FOLDER, ACT_RUN, ACT_LOCK, ACT_LOG_OUT, ACT_REBOOT, ACT_SHUT_DOWN,
              ACT_OPEN, ACT_OPEN_WITH, ACT_OPEN_IN, ACT_RUN_FILE, ACT_CUT, ACT_COPY, ACT_COPY_PATH, ACT_RENAME,
              ACT_DELETE, ACT_PROPERTIES, ACT_REFRESH, ACT_SHOW_ICONS, ACT_POSITION, ACT_TOP, ACT_BOTTOM, ACT_AUTOHIDE };
#define MAX_ITEMS 40
struct item {
    char label[GUI_TITLE_MAX];
    char arg[GUI_PATH_MAX]; // the app or folder it's for
    const struct gui_image *icon;
    int action;
    bool check, checked; // a checkbox, and whether it's ticked
};
struct popup {
    struct item items[MAX_ITEMS];
    int count, choice;
    struct rect r;
};
static struct popup popups[2];       // the menu and its submenu
static int popup_depth, popup_focus; // how many are open, and which has the keyboard
static bool start_open;              // whether the menu's the start menu
static char popup_file[GUI_PATH_MAX]; // the desktop's icon a menu's about
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
    if (w->bare)
        return (struct rect){w->x, w->y, w->canvas.width, w->canvas.height};
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

// The taskbar goes along the top or the bottom of the screen. Hidden, it comes
// back while the mouse is at that edge, or on it.
static bool bar_shown = true;

static bool bar_visible(void) {
    return !settings.autohide || bar_shown;
}

// The taskbar and its flyouts float this far from the screen's edges, and from each other.
#define GAP 8

// Where windows go: everything but the taskbar and the gaps around it, unless it hides.
static struct rect desktop(void) {
    int used = TASKBAR_HEIGHT + 2 * GAP;

    if (settings.autohide)
        return (struct rect){0, 0, screen.width, screen.height};
    return (struct rect){0, settings.taskbar_top ? used : 0, screen.width, screen.height - used};
}

static struct rect taskbar(void) {
    return (struct rect){GAP, settings.taskbar_top ? GAP : screen.height - GAP - TASKBAR_HEIGHT, screen.width - 2 * GAP,
                         TASKBAR_HEIGHT};
}

// Where something height tall goes above or below the taskbar.
static int beside_bar(int height) {
    struct rect bar = taskbar();

    return settings.taskbar_top ? bar.y + bar.height + GAP : bar.y - GAP - height;
}

static struct rect launcher(void) {
    struct rect bar = taskbar();

    return (struct rect){bar.x + 4, bar.y + 4, LAUNCHER_WIDTH, TASKBAR_HEIGHT - 8};
}

static struct rect workspace_box(int index) {
    struct rect l = launcher();

    return (struct rect){l.x + l.width + 8 + index * (WORKSPACE_SIZE + WORKSPACE_GAP),
                         taskbar().y + (TASKBAR_HEIGHT - WORKSPACE_SIZE) / 2, WORKSPACE_SIZE, WORKSPACE_SIZE};
}

static struct rect snap_rect(enum snap to) {
    struct rect d = desktop();

    return to == SNAP_LEFT    ? (struct rect){0, d.y, d.width / 2, d.height}
         : to == SNAP_RIGHT ? (struct rect){d.width / 2, d.y, d.width / 2, d.height}
                            : d;
}

// What logging out, rebooting and shutting down wait for windows to close for.
enum { ITEM_LOG_OUT, ITEM_REBOOT, ITEM_SHUT_DOWN };

// In the middle of the desktop, with every window.
static struct rect switcher(void) {
    int height = window_count * ITEM_HEIGHT + 8;

    return (struct rect){(screen.width - SWITCHER_WIDTH) / 2, desktop().y + (desktop().height - height) / 2, SWITCHER_WIDTH,
                         height};
}

static struct window *switcher_window(int index) {
    return windows[window_count - 1 - index];
}

static bool contains(struct rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

// Windows on the workspace shown, and while it's locked, only the log-in screen's,
// which is on all of them.
static bool on_workspace(const struct window *w) {
    return w->bare || w->workspace == workspace;
}

static bool visible(const struct window *w) {
    return !w->minimized && on_workspace(w) && (!locked || w->client == session);
}

// Whether a window hides all of a rectangle: its frame does, but for the title
// bar, which shows a little of what's under it, and the rounded corners.
static bool covers(const struct window *w, struct rect r) {
    struct rect f = frame_of(w);
    int top = w->bare ? RADIUS : TITLE_HEIGHT + BORDER;

    return visible(w) && !w->translucent && !w->anim_ms && r.x >= f.x && r.y >= f.y + top && r.x + r.width <= f.x + f.width
        && r.y + r.height <= f.y + f.height - RADIUS;
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

// A pixel more all round, as a virtual machine's window that smooths the screen
// while scaling it blends each pixel into the next, and only redraws what changed.
// A dragged file's icon, by the cursor.
static struct rect drag_rect(void) {
    return (struct rect){cursor_x + 10, cursor_y + 14, ICON_SIZE, ICON_SIZE};
}

static void damage_cursor(void) {
    struct rect r = cursor_rect();

    add_damage((struct rect){r.x - 1, r.y - 1, r.width + 2, r.height + 2});
    if (drag_file[0])
        add_damage(drag_rect());
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

        bool corner_row = y < f.y + RADIUS || y >= f.y + f.height - RADIUS;

        for (int x = x0; x < x1; x++) {
            // The window covers this part, but for its rounded corners.
            if (beside_frame && x >= f.x && x < f.x + f.width && (!corner_row || (x >= f.x + RADIUS && x < f.x + f.width - RADIUS))) {
                x = corner_row ? f.x + f.width - RADIUS - 1 : f.x + f.width - 1;
                continue;
            }

            int dx = x < s.x ? s.x - x : x >= s.x + s.width ? x - (s.x + s.width - 1) : 0;
            uint32_t a = dx > SHADOW || dy > SHADOW ? 0 : falloff[dx][dy] * w->shadow >> 8;

            // In a rounded corner, only what's outside the curve has the shadow.
            if (beside_frame && x >= f.x && x < f.x + f.width) {
                int cx = x - f.x < f.x + f.width - 1 - x ? x - f.x : f.x + f.width - 1 - x;
                int cy = y - f.y < f.y + f.height - 1 - y ? y - f.y : f.y + f.height - 1 - y;

                a = a * (256 - coverage[cy][cx]) >> 8;
            }

            uint32_t keep = 256 - a;
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

static void make_coverage(void) {
    for (int y = 0; y < RADIUS; y++) {
        for (int x = 0; x < RADIUS; x++)
            coverage[y][x] = gui_corner_coverage(x, y, RADIUS);
    }
}

// A rectangle's corners, kept before it's drawn, or rounded after, on a canvas
// that's the part of the screen from ox, oy.
static void keep_corners(struct gui_canvas *c, struct rect r, int ox, int oy, uint32_t kept[4][GUI_MAX_RADIUS][GUI_MAX_RADIUS]) {
    gui_keep_corners(c, r.x - ox, r.y - oy, r.width, r.height, RADIUS, kept);
}

static void round_corners(struct gui_canvas *c, struct rect r, int ox, int oy, uint32_t kept[4][GUI_MAX_RADIUS][GUI_MAX_RADIUS],
                          uint32_t line) {
    gui_round_corners(c, r.x - ox, r.y - oy, r.width, r.height, RADIUS, kept, line);
}

// A colour over what's there, alpha of it out of 256.
static void blend_fill(struct gui_canvas *c, int x, int y, int width, int height, uint32_t color, int alpha) {
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + width > c->width ? c->width : x + width, y1 = y + height > c->height ? c->height : y + height;

    for (int row = y0; row < y1; row++) {
        uint32_t *p = c->pixels + (long)row * c->stride;

        for (int col = x0; col < x1; col++)
            p[col] = gui_mix(p[col], color, alpha);
    }
}

// See-through things blur what's under them by this much. Each blur reads that far
// around it, so the screen's drawn this far around what changed, for a few of them
// over each other: a submenu over a menu over a title bar.
#define BLUR 4
#define BLUR_MARGIN (4 * BLUR)

// Part of a canvas, each pixel the average of those up to BLUR away, across then down.
static void blur(struct gui_canvas *c, int x, int y, int width, int height) {
    static uint32_t *across;
    static size_t size;
    int x0 = x < 0 ? 0 : x, x1 = x + width > c->width ? c->width : x + width;
    int y0 = y < 0 ? 0 : y, y1 = y + height > c->height ? c->height : y + height;
    int top = y0 - BLUR < 0 ? 0 : y0 - BLUR, bottom = y1 + BLUR > c->height ? c->height : y1 + BLUR;
    int w = x1 - x0, taps = 2 * BLUR + 1;
    size_t needed = (size_t)w * (bottom - top);

    if (w <= 0 || y1 <= y0)
        return;
    if (needed > size) {
        uint32_t *bigger = realloc(across, needed * sizeof(*across));

        if (!bigger)
            return;
        across = bigger, size = needed;
    }
    for (int row = top; row < bottom; row++) {
        const uint32_t *p = c->pixels + (long)row * c->stride;

        for (int col = x0; col < x1; col++) {
            uint32_t rb = 0, g = 0;

            for (int k = -BLUR; k <= BLUR; k++) {
                int at = col + k < 0 ? 0 : col + k >= c->width ? c->width - 1 : col + k;

                rb += p[at] & 0xFF00FF, g += p[at] & 0x00FF00;
            }
            across[(long)(row - top) * w + col - x0] = ((rb >> 16) / taps) << 16 | ((rb & 0xFFFF) / taps) | ((g / taps) & 0x00FF00);
        }
    }
    for (int row = y0; row < y1; row++) {
        uint32_t *p = c->pixels + (long)row * c->stride;

        for (int col = x0; col < x1; col++) {
            uint32_t rb = 0, g = 0;

            for (int k = -BLUR; k <= BLUR; k++) {
                int at = row + k < top ? top : row + k >= bottom ? bottom - 1 : row + k;
                uint32_t q = across[(long)(at - top) * w + col - x0];

                rb += q & 0xFF00FF, g += q & 0x00FF00;
            }
            p[col] = ((rb >> 16) / taps) << 16 | ((rb & 0xFFFF) / taps) | ((g / taps) & 0x00FF00);
        }
    }
}

static void draw_icon(struct gui_canvas *c, const char *app, int x, int y, int ox, int oy) {
    const struct gui_image *icon = app_icon(app);

    if (icon)
        gui_draw_image(c, x - ox, y - oy, icon);
}

// While a window's copied to its layer for an animation, it's drawn plainly: its
// corners square and nothing blurred, as the layer has nothing under it.
static bool drawing_layer;

// A window's pixels. A see-through window's blur what's under them, and show it as
// much as their top bytes say.
static void draw_contents(struct gui_canvas *c, const struct window *w, int ox, int oy) {
    int x = w->x - ox, y = w->y - oy;
    int x0 = x < 0 ? 0 : x, x1 = x + w->canvas.width > c->width ? c->width : x + w->canvas.width;
    int y0 = y < 0 ? 0 : y, y1 = y + w->canvas.height > c->height ? c->height : y + w->canvas.height;

    if (!w->translucent || drawing_layer) {
        gui_copy(c, x, y, &w->canvas, 0, 0, w->canvas.width, w->canvas.height);
        return;
    }
    blur(c, x, y, w->canvas.width, w->canvas.height);
    for (int row = y0; row < y1; row++) {
        const uint32_t *from = w->canvas.pixels + (long)(row - y) * w->canvas.stride - x;
        uint32_t *to = c->pixels + (long)row * c->stride;

        for (int col = x0; col < x1; col++) {
            uint32_t p = from[col];

            to[col] = p >> 24 ? gui_mix(p, to[col], p >> 24) : p;
        }
    }
}

// Windows have rounded corners, but for those maximized or tiled, which meet the
// edges of the screen or each other.
static bool rounded(const struct window *w) {
    return !w->tiled;
}

static void draw_window(struct gui_canvas *c, const struct window *w, int ox, int oy) {
    uint32_t kept[4][GUI_MAX_RADIUS][GUI_MAX_RADIUS];
    struct rect frame = frame_of(w);
    bool round = rounded(w) && !drawing_layer;

    if (round)
        keep_corners(c, frame, ox, oy, kept);
    if (w->bare) {
        draw_contents(c, w, ox, oy);
        round_corners(c, frame, ox, oy, kept, GUI_NONE);
        return;
    }

    struct rect close = button_of(w, 0), maximize = button_of(w, 1), minimize = button_of(w, 2);
    uint32_t title_color = w == focused ? TITLE_FOCUSED : TITLE_UNFOCUSED;
    int text_y = w->y - TITLE_HEIGHT + (TITLE_HEIGHT - GUI_CHAR_HEIGHT) / 2 - oy;

    // Only the border's edges, as the title bar and contents cover the rest.
    gui_fill(c, frame.x - ox, frame.y - oy, frame.width, BORDER, BORDER_COLOR);
    gui_fill(c, frame.x - ox, frame.y + frame.height - BORDER - oy, frame.width, BORDER, BORDER_COLOR);
    gui_fill(c, frame.x - ox, frame.y - oy, BORDER, frame.height, BORDER_COLOR);
    gui_fill(c, frame.x + frame.width - BORDER - ox, frame.y - oy, BORDER, frame.height, BORDER_COLOR);
    if (!drawing_layer)
        blur(c, w->x - ox, w->y - TITLE_HEIGHT - oy, w->canvas.width, TITLE_HEIGHT);
    blend_fill(c, w->x - ox, w->y - TITLE_HEIGHT - oy, w->canvas.width, TITLE_HEIGHT, title_color,
               drawing_layer ? 256 : BAR_ALPHA);
    draw_icon(c, w->app, w->x + 5, w->y - TITLE_HEIGHT + (TITLE_HEIGHT - ICON_SIZE) / 2, ox, oy);

    // A title too long for the bar stops short of the buttons.
    struct gui_canvas title = *c;
    int right = minimize.x - 4 - ox;

    title.width = right < 0 ? 0 : right < c->width ? right : c->width;
    gui_text(&title, w->x + 10 + ICON_SIZE - ox, text_y, w->title, w == focused ? ON_ACCENT : TITLE_TEXT_UNFOCUSED, 1);
    gui_fill_rounded(c, close.x - ox, close.y - oy, BUTTON_SIZE, BUTTON_SIZE, SMALL_RADIUS, CLOSE_COLOR);
    gui_text(c, close.x + (BUTTON_SIZE - GUI_CHAR_WIDTH) / 2 - ox, text_y, "x", TITLE_TEXT, 1);
    gui_fill_rounded(c, maximize.x - ox, maximize.y - oy, BUTTON_SIZE, BUTTON_SIZE, SMALL_RADIUS, BUTTON_COLOR);
    gui_fill(c, maximize.x + 4 - ox, maximize.y + 4 - oy, 8, 8, TITLE_TEXT);
    gui_fill(c, maximize.x + 5 - ox, maximize.y + 6 - oy, 6, 5, BUTTON_COLOR);
    gui_fill_rounded(c, minimize.x - ox, minimize.y - oy, BUTTON_SIZE, BUTTON_SIZE, SMALL_RADIUS, BUTTON_COLOR);
    gui_fill(c, minimize.x + 4 - ox, minimize.y + 11 - oy, 8, 2, TITLE_TEXT);
    draw_contents(c, w, ox, oy);
    if (round)
        round_corners(c, frame, ox, oy, kept, BORDER_COLOR);
}

// The workspace's windows in the order they were made, which is how the taskbar
// shows them.
static int windows_by_age(struct window *out[MAX_WINDOWS]) {
    int count = 0;

    for (int i = 0; i < window_count; i++) {
        if (!on_workspace(windows[i]))
            continue;

        int j = count++;

        while (j > 0 && out[j - 1]->id > windows[i]->id) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = windows[i];
    }
    return count;
}

static struct rect tray(void);

static struct rect taskbar_button(int index, int count) {
    int start = workspace_box(WORKSPACES).x + 8, room = tray().x - 8 - start;
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

// Menus, the tray's panels, notifications and the switcher, with rounded corners
// like windows'.
// The taskbar, its flyouts, notifications and the switcher: rounded, outlined and see-through, blurring what's under them.
static void draw_glass(struct gui_canvas *c, struct rect r, int ox, int oy) {
    uint32_t kept[4][GUI_MAX_RADIUS][GUI_MAX_RADIUS];
    int x = r.x - ox, y = r.y - oy;

    keep_corners(c, r, ox, oy, kept);
    blur(c, x, y, r.width, r.height);
    blend_fill(c, x, y, r.width, r.height, TASKBAR_COLOR, BAR_ALPHA);
    gui_fill(c, x, y, r.width, 1, TASKBAR_LINE);
    gui_fill(c, x, y + r.height - 1, r.width, 1, TASKBAR_LINE);
    gui_fill(c, x, y, 1, r.height, TASKBAR_LINE);
    gui_fill(c, x + r.width - 1, y, 1, r.height, TASKBAR_LINE);
    round_corners(c, r, ox, oy, kept, TASKBAR_LINE);
}

// 12 hour time goes without a leading zero, as people write it.
static const char *clock_text(char out[16]) {
    time_t now = time(NULL);

    strftime(out, 16, settings.clock_12_hour ? "%I:%M %p" : "%H:%M", localtime(&now));
    return out + (settings.clock_12_hour && out[0] == '0');
}

// Notifications stack up from the bottom right corner, the newest lowest, until
// they've been up NOTICE_MS or they're clicked. The last HISTORY are kept for the
// tray, newest first, with how many came since it was last looked at.
#define MAX_NOTICES 4
#define NOTICE_MS 5000
#define NOTICE_WIDTH 300
#define NOTICE_HEIGHT 52
#define HISTORY 6
struct notice {
    char title[GUI_TITLE_MAX], text[GUI_TITLE_MAX], app[GUI_TITLE_MAX];
    char file[GUI_PATH_MAX]; // that clicking it opens, if there is one
    char time[8];
    int64_t until;
};
static struct notice notices[MAX_NOTICES], history[HISTORY];
static int notice_count, history_count, unseen;

static int64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// Windows grow in from a little smaller as they fade in, and shrink as they fade
// out, or fly to and from their taskbar buttons when minimized and restored.
#define OPEN_MS 180
#define CLOSE_MS 120
#define MINIMIZE_MS 200
#define ANIM_SCALE 240 // out of 256, the size windows start opening and finish closing at

// A window going away, closed or minimized, drawn from a copy of how it looked.
struct ghost {
    struct gui_canvas layer;
    bool rounded, translucent;
    int shadow, ms;
    int64_t start;
    struct rect from, to;
};
static struct ghost ghosts[MAX_WINDOWS];
static int ghost_count;

// How far through an animation it is, out of 256, and that eased: quickly then
// slowing for what arrives, and slowly then quickly for what goes.
static int progress(int64_t start, int ms) {
    int64_t t = (now_ms() - start) * 256 / ms;

    return t < 0 ? 0 : t > 256 ? 256 : (int)t;
}

static int ease_out(int t) {
    return 256 - (256 - t) * (256 - t) * (256 - t) / 65536;
}

static int ease_in(int t) {
    return t * t * t / 65536;
}

static struct rect between(struct rect a, struct rect b, int t) {
    return (struct rect){a.x + (b.x - a.x) * t / 256, a.y + (b.y - a.y) * t / 256, a.width + (b.width - a.width) * t / 256,
                         a.height + (b.height - a.height) * t / 256};
}

// A frame at ANIM_SCALE of its size, about its middle.
static struct rect shrunk(struct rect r) {
    int width = r.width * ANIM_SCALE / 256, height = r.height * ANIM_SCALE / 256;

    return (struct rect){r.x + (r.width - width) / 2, r.y + (r.height - height) / 2, width, height};
}

static struct rect with_shadow(struct rect r) {
    return (struct rect){r.x - SHADOW, r.y - SHADOW, r.width + 2 * SHADOW, r.height + 2 * SHADOW};
}

// Where a window's button is on the taskbar.
static struct rect button_for(const struct window *w) {
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);

    for (int i = 0; i < count; i++) {
        if (list[i] == w)
            return taskbar_button(i, count);
    }
    return taskbar();
}

// Draws a window, at full size, into a layer the size of its frame.
static bool fill_layer(struct gui_canvas *layer, const struct window *w) {
    struct rect f = frame_of(w);

    if (layer->width != f.width || layer->height != f.height) {
        uint32_t *pixels = realloc(layer->pixels, (size_t)f.width * f.height * sizeof(uint32_t));

        if (!pixels)
            return false;
        *layer = (struct gui_canvas){pixels, f.width, f.height, f.width};
    }
    drawing_layer = true;
    draw_window(layer, w, f.x, f.y);
    drawing_layer = false;
    return true;
}

// A layer drawn into r, alpha of it out of 256, with its shadow, its corners
// rounded, and see-through where its pixels say if it's translucent.
static void draw_layer(struct gui_canvas *c, const struct gui_canvas *layer, struct rect r, int alpha, int shadow,
                       bool round, bool translucent, int ox, int oy) {
    int x0 = r.x - ox < 0 ? 0 : r.x - ox, x1 = r.x + r.width - ox > c->width ? c->width : r.x + r.width - ox;
    int y0 = r.y - oy < 0 ? 0 : r.y - oy, y1 = r.y + r.height - oy > c->height ? c->height : r.y + r.height - oy;

    if (r.width <= 0 || r.height <= 0)
        return;
    if (settings.shadows && r.width > 4 * RADIUS && r.height > 4 * RADIUS) {
        struct window outline = {.x = r.x + BORDER, .y = r.y + TITLE_HEIGHT + BORDER, .shadow = shadow * alpha / 256};

        outline.canvas.width = r.width - 2 * BORDER;
        outline.canvas.height = r.height - TITLE_HEIGHT - 2 * BORDER;
        draw_shadow(c, &outline, ox, oy);
    }
    for (int y = y0; y < y1; y++) {
        int ly = (y + oy - r.y) * layer->height / r.height, cy = ly < layer->height - 1 - ly ? ly : layer->height - 1 - ly;
        const uint32_t *from = layer->pixels + (long)ly * layer->stride;
        uint32_t *to = c->pixels + (long)y * c->stride;

        for (int x = x0; x < x1; x++) {
            int lx = (x + ox - r.x) * layer->width / r.width, cx = lx < layer->width - 1 - lx ? lx : layer->width - 1 - lx;
            uint32_t p = from[lx];
            int a = translucent ? alpha * (256 - (int)(p >> 24)) / 256 : alpha;

            if (round && cx < RADIUS && cy < RADIUS)
                a = a * coverage[cy][cx] / 256;
            to[x] = gui_mix(to[x], p, a);
        }
    }
}

// Where an opening or restored window is now, and how much of it shows.
static struct rect arriving(const struct window *w, int *alpha) {
    int t = ease_out(progress(w->anim_start, w->anim_ms));

    *alpha = t;
    return between(w->anim_from, frame_of(w), t);
}

// All that a window covers, as it's drawn now.
static struct rect drawn_extent(const struct window *w) {
    return w->anim_ms ? bounds(with_shadow(w->anim_from), extent_of(w)) : extent_of(w);
}

static void arrive(struct window *w, struct rect from, int ms) {
    if (!settings.animations || w->bare)
        return;
    w->anim_from = from;
    w->anim_ms = ms;
    w->anim_start = now_ms();
}

// Leaves a copy of a window behind, going from its frame to a rectangle.
static void depart(const struct window *w, struct rect to, int ms) {
    struct ghost *g = &ghosts[ghost_count];

    if (!settings.animations || w->bare || !visible(w) || ghost_count == MAX_WINDOWS)
        return;
    *g = (struct ghost){.rounded = rounded(w), .translucent = w->translucent, .shadow = w->shadow, .ms = ms,
                        .start = now_ms(), .from = frame_of(w), .to = to};
    if (fill_layer(&g->layer, w))
        ghost_count++;
}

static void draw_ghosts(struct gui_canvas *c, int ox, int oy) {
    for (int i = 0; i < ghost_count; i++) {
        const struct ghost *g = &ghosts[i];
        int t = ease_in(progress(g->start, g->ms));

        draw_layer(c, &g->layer, between(g->from, g->to, t), 256 - t, g->shadow, g->rounded, g->translucent, ox, oy);
    }
}

// Moves each animation on a frame, returning whether any has further to go.
static bool animate(void) {
    bool going = false;

    for (int i = 0; i < window_count; i++) {
        struct window *w = windows[i];

        if (!w->anim_ms)
            continue;
        add_damage(drawn_extent(w));
        if (progress(w->anim_start, w->anim_ms) < 256 && fill_layer(&w->layer, w))
            going = true;
        else
            w->anim_ms = 0;
    }
    for (int i = 0; i < ghost_count; i++) {
        struct ghost *g = &ghosts[i];

        add_damage(bounds(with_shadow(g->from), with_shadow(g->to)));
        if (progress(g->start, g->ms) < 256) {
            going = true;
            continue;
        }
        free(g->layer.pixels);
        *g-- = ghosts[--ghost_count];
    }
    return going;
}

static struct rect notice_rect(int index) {
    int from_bottom = notice_count - index;

    int bottom = settings.taskbar_top ? screen.height : taskbar().y;

    return (struct rect){screen.width - NOTICE_WIDTH - GAP, bottom - from_bottom * (NOTICE_HEIGHT + GAP), NOTICE_WIDTH,
                         NOTICE_HEIGHT};
}

static void damage_notices(void) {
    for (int i = 0; i < notice_count; i++)
        add_damage(notice_rect(i));
}

static void remove_notice(int index) {
    damage_notices();
    memmove(notices + index, notices + index + 1, (notice_count - index - 1) * sizeof(*notices));
    notice_count--;
}

// Takes away those whose time is up, returning how long until the next one's is.
static int expire_notices(void) {
    int64_t now = now_ms(), next = -1;

    for (int i = notice_count - 1; i >= 0; i--) {
        if (notices[i].until <= now)
            remove_notice(i);
    }
    for (int i = 0; i < notice_count; i++)
        next = next < 0 || notices[i].until - now < next ? notices[i].until - now : next;
    return (int)next;
}

static int notice_at(int x, int y) {
    for (int i = 0; i < notice_count; i++) {
        if (contains(notice_rect(i), x, y))
            return i;
    }
    return -1;
}

static void draw_notice(struct gui_canvas *c, struct rect r, const struct notice *n, int ox, int oy) {
    int text_x = r.x + 12 + (n->app[0] ? ICON_SIZE + 10 : 0), width = r.x + r.width - 12 - text_x;

    if (n->app[0])
        draw_icon(c, n->app, r.x + 12, r.y + 10, ox, oy);
    fitted_text(c, text_x - ox, r.y + 8 - oy, width, n->title, TASKBAR_TEXT);
    fitted_text(c, text_x - ox, r.y + 8 + GUI_CHAR_HEIGHT + 4 - oy, width, n->text, TASKBAR_DIM_TEXT);
}

static void draw_notices(struct gui_canvas *c, int ox, int oy) {
    for (int i = 0; i < notice_count; i++) {
        struct rect r = notice_rect(i);

        draw_glass(c, r, ox, oy);
        gui_fill_rounded(c, r.x + 3 - ox, r.y + RADIUS - oy, 3, r.height - 2 * RADIUS, GUI_MAX_RADIUS, TITLE_FOCUSED);
        draw_notice(c, r, &notices[i], ox, oy);
    }
}

// The tray, left of the clock: quick settings, shown by the network's and the
// speaker's icons, and notifications, each opening a panel by it. The clock opens
// a calendar.
enum { TRAY_QUICK, TRAY_NOTICES, TRAY_ITEMS, TRAY_CALENDAR = TRAY_ITEMS };
enum { TILE_AIRPLANE, TILE_NIGHT, TILE_DARK, TILES };
#define DAY_WIDTH 32
#define DAY_HEIGHT 24
#define TRAY_ICON 24
#define PANEL_LINE 20
#define QUICK_WIDTH 320
#define TILE_HEIGHT 52
#define VOLUME_FILE "/system/etc/volume"
#define VOLUME_STEP 5
#define NET_CHECK_MS 5000 // how often the network's looked at, for its icon
static int panel = -1; // the one open
static int64_t panel_opened, menu_opened; // when they did, to fade them in
static void autohide(void);

// What the player's playing, which it keeps in NOW_PLAYING, shown under the volume.
#define NOW_PLAYING "/tmp/now-playing"
static char now_playing[GUI_TITLE_MAX];
static int mixer = -1, volume = 100;
static bool muted, setting_volume;
static struct net_info net;
static int calendar_month; // shown, from this one

static bool connected(void) {
    return net.has_device && net.address;
}

// With a sound card, the speaker's beside the network.
static struct rect tray_item(int item) {
    char clock[16];
    int width = gui_text_width(clock_text(clock), 1), right = taskbar().x + taskbar().width - width - 12 - 8;
    int y = taskbar().y + 4, quick = mixer >= 0 ? 2 * TRAY_ICON - 4 : TRAY_ICON;

    if (item == TRAY_CALENDAR)
        return (struct rect){right + 2, y, width + 16, TASKBAR_HEIGHT - 8};
    if (item == TRAY_NOTICES)
        return (struct rect){right - TRAY_ICON, y, TRAY_ICON, TASKBAR_HEIGHT - 8};
    return (struct rect){right - TRAY_ICON - quick - 2, y, quick, TASKBAR_HEIGHT - 8};
}

static struct rect tray(void) {
    struct rect first = tray_item(TRAY_QUICK), last = tray_item(TRAY_NOTICES);

    return (struct rect){first.x, first.y, last.x + last.width - first.x, first.height};
}

static int tray_item_at(int x, int y) {
    for (int i = 0; i <= TRAY_CALENDAR; i++) {
        if (contains(tray_item(i), x, y))
            return i;
    }
    return -1;
}

static int sound_height(void) {
    return mixer < 0 ? 0 : 32 + (now_playing[0] ? 22 : 0);
}

static struct rect panel_rect(int item) {
    static const int widths[] = {QUICK_WIDTH, NOTICE_WIDTH, 7 * DAY_WIDTH + 24};
    int heights[] = {12 + TILE_HEIGHT + 12 + sound_height() + 9 + 6 * PANEL_LINE + 8,
                     40 + (history_count ? history_count : 1) * (NOTICE_HEIGHT - 8), 40 + 7 * DAY_HEIGHT};
    struct rect t = tray_item(item);
    int x = t.x + t.width - widths[item], most = screen.width - GAP - widths[item];

    x = x < most ? x : most;
    return (struct rect){x > GAP ? x : GAP, beside_bar(heights[item]), widths[item], heights[item]};
}

// The buttons across the top of quick settings.
static struct rect quick_tile(int tile) {
    struct rect p = panel_rect(TRAY_QUICK);
    int width = (p.width - 24 - (TILES - 1) * 8) / TILES;

    return (struct rect){p.x + 12 + tile * (width + 8), p.y + 12, width, TILE_HEIGHT};
}

// The speaker, which mutes, and the slider beside it, where it can be clicked,
// with the line it's drawn on through its middle.
static struct rect mute_button(void) {
    struct rect p = panel_rect(TRAY_QUICK);

    return (struct rect){p.x + 10, p.y + 12 + TILE_HEIGHT + 12, 24, 20};
}

static struct rect volume_slider(void) {
    struct rect p = panel_rect(TRAY_QUICK), m = mute_button();

    return (struct rect){m.x + m.width + 10, m.y, p.x + p.width - 56 - (m.x + m.width + 10), 20};
}

// The calendar's arrows to the month before and after, either side of its name.
static struct rect month_arrow(bool next) {
    struct rect p = panel_rect(TRAY_CALENDAR);

    return (struct rect){next ? p.x + p.width - 8 - 24 : p.x + 8, p.y + 6, 24, 24};
}

static struct rect clear_button(void) {
    struct rect p = panel_rect(TRAY_NOTICES);

    return (struct rect){p.x + p.width - 64, p.y + 6, 56, 22};
}

static struct rect history_row(int index) {
    struct rect p = panel_rect(TRAY_NOTICES);

    return (struct rect){p.x + 4, p.y + 34 + index * (NOTICE_HEIGHT - 8), p.width - 8, NOTICE_HEIGHT - 8};
}

static void damage_tray(void) {
    add_damage(tray());
    add_damage(tray_item(TRAY_CALENDAR));
    if (panel >= 0)
        add_damage(panel_rect(panel));
}

static void flyout_leaves(struct rect r);

static void close_panel(void) {
    if (panel >= 0)
        flyout_leaves(panel_rect(panel));
    damage_tray();
    panel = -1;
    setting_volume = false;
    autohide();
}

static struct notice *add_notice(const char *title, const char *text, const char *app) {
    time_t now = time(NULL);

    damage_notices();
    damage_tray(); // before the history panel grows
    if (notice_count == MAX_NOTICES) // the oldest makes way
        memmove(notices, notices + 1, --notice_count * sizeof(*notices));

    struct notice *n = &notices[notice_count++];

    snprintf(n->title, sizeof(n->title), "%.*s", GUI_TITLE_MAX - 1, title);
    snprintf(n->text, sizeof(n->text), "%.*s", GUI_TITLE_MAX - 1, text);
    snprintf(n->app, sizeof(n->app), "%s", app ? app : "");
    strftime(n->time, sizeof(n->time), "%H:%M", localtime(&now));
    n->file[0] = '\0';
    n->until = now_ms() + NOTICE_MS;
    memmove(history + 1, history, (HISTORY - 1) * sizeof(*history));
    history[0] = *n;
    history_count += history_count < HISTORY;
    unseen += panel != TRAY_NOTICES;
    damage_notices();
    damage_tray();
    printf("wm: notice %s: %s\n", n->title, n->text);
    return n;
}

// A file to open for the notice just added, kept in its history as well.
static void notice_file(struct notice *n, const char *path) {
    snprintf(n->file, sizeof(n->file), "%s", path);
    snprintf(history[0].file, sizeof(history[0].file), "%s", path);
}

static void clear_history(void) {
    damage_tray();
    history_count = unseen = 0;
}

static void set_volume(int to, bool mute) {
    FILE *f;

    to = to < 0 ? 0 : to > 100 ? 100 : to;
    if (to == volume && mute == muted)
        return;
    volume = to;
    muted = mute;
    ioctl(mixer, MIXER_SET_VOLUME, &volume);
    ioctl(mixer, MIXER_SET_MUTE, &(int){muted});
    if ((f = fopen(VOLUME_FILE, "w"))) {
        fprintf(f, "%d%s\n", volume, muted ? " muted" : "");
        fclose(f);
    }
    printf("wm: volume %d%s\n", volume, muted ? ", muted" : "");
    damage_tray();
}

// What was set last time, which the sound card starts without.
static void load_volume(void) {
    FILE *f = fopen(VOLUME_FILE, "r");
    char word[16] = "";
    int was = 100;

    mixer = open("/dev/mixer", O_RDONLY | O_CLOEXEC);
    if (f) {
        if (fscanf(f, "%d %15s", &was, word) < 1)
            was = 100;
        fclose(f);
    }
    volume = -1; // so it's set whatever it is
    if (mixer >= 0)
        set_volume(was, !strcmp(word, "muted"));
}

static void set_volume_at(int x) {
    struct rect r = volume_slider();

    set_volume(((x - r.x) * 100 + r.width / 2) / r.width, muted);
}

// Looks at the network again, redrawing the tray if it's come or gone.
static void check_network(void) {
    bool was = connected();
    struct net_info now;

    if (net_info(&now) < 0)
        now = (struct net_info){0};
    if (panel == TRAY_QUICK && memcmp(&now, &net, sizeof(now)))
        add_damage(panel_rect(TRAY_QUICK));
    net = now;
    if (connected() != was) {
        printf("wm: network %s\n", connected() ? "connected" : "disconnected");
        add_damage(tray());
    }
}

static bool leap(int year) {
    return (year % 4 == 0 && year % 100) || year % 400 == 0;
}

// Monday as 0, by Sakamoto's method.
static int weekday(int year, int month, int day) {
    static const int offsets[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};

    year -= month < 2;
    return (year + year / 4 - year / 100 + year / 400 + offsets[month] + day + 6) % 7;
}

// The month shown, from this one, as a year and a month from 0.
static void shown_month(int *year, int *month) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    int months = t->tm_year * 12 + t->tm_mon + calendar_month;

    *year = 1900 + (months >= 0 ? months / 12 : (months - 11) / 12);
    *month = months - (*year - 1900) * 12;
}

static const char *const month_names[] = {"January", "February", "March", "April", "May", "June", "July",
                                          "August", "September", "October", "November", "December"};

// Says where a panel's parts are, for the tests to find them.
static void report_panel(int item) {
    struct rect p = panel_rect(item);

    printf("wm: panel %d at %d %d %d %d\n", item, p.x, p.y, p.width, p.height);
    if (item != TRAY_QUICK)
        return;
    for (int i = 0; i < TILES; i++) {
        struct rect t = quick_tile(i);

        printf("wm: tile %d at %d %d %d %d\n", i, t.x, t.y, t.width, t.height);
    }
    if (mixer >= 0) {
        struct rect m = mute_button(), v = volume_slider();

        printf("wm: mute at %d %d %d %d\nwm: slider at %d %d %d %d\n", m.x, m.y, m.width, m.height, v.x, v.y, v.width,
               v.height);
    }
}

static void open_panel(int item) {
    close_panel();
    calendar_month = 0;
    now_playing[0] = '\0';
    if (item == TRAY_QUICK) {
        FILE *f = fopen(NOW_PLAYING, "r");

        if (f && fgets(now_playing, sizeof(now_playing), f))
            now_playing[strcspn(now_playing, "\n")] = '\0';
        if (f)
            fclose(f);
        printf("wm: quick settings%s%s\n", now_playing[0] ? ", " : "", now_playing);
        check_network();
    }
    panel = item;
    panel_opened = now_ms();
    report_panel(item);
    autohide();
    if (item == TRAY_CALENDAR) {
        int year, month;

        shown_month(&year, &month);
        printf("wm: calendar %s %d\n", month_names[month], year);
    }
    if (item == TRAY_NOTICES) {
        unseen = 0;
        while (notice_count)
            remove_notice(0); // they're all in the panel
    }
    damage_tray();
}

static void draw_line(struct gui_canvas *c, struct rect p, int y, const char *label, const char *value, int ox,
                      int oy) {
    gui_text(c, p.x + 12 - ox, y - oy, label, TASKBAR_DIM_TEXT, 1);
    gui_text(c, p.x + 96 - ox, y - oy, value, TASKBAR_TEXT, 1);
}

static void draw_address(struct gui_canvas *c, struct rect p, int y, const char *label, uint32_t address, int ox,
                         int oy) {
    struct in_addr in = {htonl(address)};

    draw_line(c, p, y, label, address ? inet_ntoa(in) : "none", ox, oy);
}

static void draw_amount(struct gui_canvas *c, struct rect p, int y, const char *label, uint64_t bytes, int ox,
                        int oy) {
    char shown[32];

    snprintf(shown, sizeof(shown), bytes >= 10 << 20 ? "%llu MB" : bytes >= 10 << 10 ? "%llu KB" : "%llu bytes",
             (unsigned long long)(bytes >= 10 << 20 ? bytes >> 20 : bytes >= 10 << 10 ? bytes >> 10 : bytes));
    draw_line(c, p, y, label, shown, ox, oy);
}

static void draw_network(struct gui_canvas *c, struct rect p, int y, int ox, int oy) {
    const char *state = net.airplane      ? "Airplane mode"
                      : !net.has_device ? "No network card"
                      : !net.address    ? "Not connected"
                                        : "Connected";

    gui_text(c, p.x + 12 - ox, y - oy, "Network", TASKBAR_TEXT, 1);
    gui_text(c, p.x + p.width - 12 - gui_text_width(state, 1) - ox, y - oy, state,
             connected() ? TASKBAR_TEXT : TASKBAR_DIM_TEXT, 1);
    draw_address(c, p, y + PANEL_LINE, "Address", net.address, ox, oy);
    draw_address(c, p, y + 2 * PANEL_LINE, "Gateway", net.gateway, ox, oy);
    draw_address(c, p, y + 3 * PANEL_LINE, "DNS", net.dns, ox, oy);
    draw_amount(c, p, y + 4 * PANEL_LINE, "Received", net.rx_bytes, ox, oy);
    draw_amount(c, p, y + 5 * PANEL_LINE, "Sent", net.tx_bytes, ox, oy);
}

static void draw_history(struct gui_canvas *c, struct rect p, int ox, int oy) {
    struct rect clear = clear_button();

    gui_text(c, p.x + 12 - ox, p.y + 10 - oy, "Notifications", TASKBAR_TEXT, 1);
    if (history_count) {
        gui_fill_rounded(c, clear.x - ox, clear.y - oy, clear.width, clear.height, SMALL_RADIUS, TASKBAR_BUTTON);
        gui_text(c, clear.x + (clear.width - gui_text_width("Clear", 1)) / 2 - ox,
                 clear.y + (clear.height - GUI_CHAR_HEIGHT) / 2 - oy, "Clear", TASKBAR_TEXT, 1);
    } else {
        gui_text(c, p.x + 12 - ox, p.y + 44 - oy, "Nothing new", TASKBAR_DIM_TEXT, 1);
    }
    for (int i = 0; i < history_count; i++) {
        struct rect r = history_row(i);
        const struct notice *n = &history[i];

        if (contains(r, cursor_x, cursor_y) && n->file[0])
            gui_fill_rounded(c, r.x - ox, r.y - oy, r.width, r.height, SMALL_RADIUS, TASKBAR_BUTTON);
        gui_text(c, r.x + r.width - 8 - gui_text_width(n->time, 1) - ox, r.y + 4 - oy, n->time, TASKBAR_DIM_TEXT, 1);
        draw_notice(c, (struct rect){r.x - 4, r.y - 4, r.width - 6 * GUI_CHAR_WIDTH, r.height}, n, ox, oy);
    }
}

static bool tile_on(int tile) {
    return tile == TILE_AIRPLANE ? settings.airplane : tile == TILE_NIGHT ? settings.night_light : !settings.light;
}

// The tiles, the volume and what's playing, then the network.
static void draw_quick(struct gui_canvas *c, struct rect p, int ox, int oy) {
    static const char *const labels[] = {"Airplane", "Night light", "Dark mode"};
    static const char *const tile_icons[] = {"airplane", "night", "theme"};
    int network_y = p.y + 12 + TILE_HEIGHT + 12 + sound_height();

    for (int i = 0; i < TILES; i++) {
        struct rect t = quick_tile(i);
        const struct gui_image *icon = gui_icon(tile_icons[i]);
        bool on = tile_on(i);

        gui_fill_rounded(c, t.x - ox, t.y - oy, t.width, t.height, RADIUS, on ? TITLE_FOCUSED : TASKBAR_BUTTON);
        if (icon)
            gui_draw_image(c, t.x + (t.width - ICON_SIZE) / 2 - ox, t.y + 8 - oy, icon);
        gui_text(c, t.x + (t.width - gui_text_width(labels[i], 1)) / 2 - ox, t.y + t.height - GUI_CHAR_HEIGHT - 6 - oy,
                 labels[i], on ? ON_ACCENT : TASKBAR_TEXT, 1);
    }
    if (mixer >= 0) {
        struct rect r = volume_slider(), m = mute_button();
        int knob = r.x + r.width * volume / 100, line = r.y + r.height / 2;
        const struct gui_image *speaker = gui_icon(muted || !volume ? "muted" : "speaker");
        char percent[8];

        snprintf(percent, sizeof(percent), "%d%%", volume);
        if (contains(m, cursor_x, cursor_y))
            gui_fill_rounded(c, m.x - ox, m.y - oy, m.width, m.height, SMALL_RADIUS, TASKBAR_BUTTON);
        if (speaker)
            gui_draw_image(c, m.x + (m.width - ICON_SIZE) / 2 - ox, m.y + (m.height - ICON_SIZE) / 2 - oy, speaker);
        gui_fill_rounded(c, r.x - ox, line - 2 - oy, r.width, 4, GUI_MAX_RADIUS, TASKBAR_LINE);
        gui_fill_rounded(c, r.x - ox, line - 2 - oy, knob - r.x, 4, GUI_MAX_RADIUS, muted ? TASKBAR_DIM_TEXT : TITLE_FOCUSED);
        gui_fill_rounded(c, knob - 5 - ox, line - 8 - oy, 10, 16, GUI_MAX_RADIUS, muted ? TASKBAR_DIM_TEXT : TITLE_FOCUSED);
        gui_text(c, p.x + p.width - 12 - gui_text_width(percent, 1) - ox, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 - oy,
                 percent, muted ? TASKBAR_DIM_TEXT : TASKBAR_TEXT, 1);
        if (now_playing[0]) {
            char shown[GUI_TITLE_MAX + 16];

            snprintf(shown, sizeof(shown), "%.*s", (p.width - 24) / GUI_CHAR_WIDTH, now_playing);
            gui_text(c, p.x + 12 - ox, m.y + 28 - oy, shown, TASKBAR_DIM_TEXT, 1);
        }
    }
    gui_fill(c, p.x + 12 - ox, network_y - oy, p.width - 24, 1, TASKBAR_LINE);
    draw_network(c, p, network_y + 9, ox, oy);
}

static void draw_calendar(struct gui_canvas *c, struct rect p, int ox, int oy) {
    static const char *const days[] = {"Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"};
    static const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    time_t now = time(NULL);
    struct tm today = *localtime(&now);
    int year, month;
    char title[32];

    shown_month(&year, &month);
    snprintf(title, sizeof(title), "%s %d", month_names[month], year);
    gui_text(c, p.x + (p.width - gui_text_width(title, 1)) / 2 - ox, p.y + 10 - oy, title, TASKBAR_TEXT, 1);
    for (int next = 0; next < 2; next++) {
        struct rect a = month_arrow(next);

        gui_fill_rounded(c, a.x - ox, a.y - oy, a.width, a.height, SMALL_RADIUS, TASKBAR_BUTTON);
        gui_text(c, a.x + 8 - ox, a.y + 4 - oy, next ? ">" : "<", TASKBAR_TEXT, 1);
    }

    int left = p.x + 12, top = p.y + 40, first = weekday(year, month, 1);
    int length = lengths[month] + (month == 1 && leap(year));

    for (int i = 0; i < 7; i++)
        gui_text(c, left + i * DAY_WIDTH + 8 - ox, top + 4 - oy, days[i], TASKBAR_DIM_TEXT, 1);
    for (int day = 1; day <= length; day++) {
        int cell = first + day - 1, x = left + cell % 7 * DAY_WIDTH, y = top + (cell / 7 + 1) * DAY_HEIGHT;
        bool is_today = day == today.tm_mday && month == today.tm_mon && year == today.tm_year + 1900;
        char number[4];

        snprintf(number, sizeof(number), "%d", day);
        if (is_today)
            gui_fill_rounded(c, x + 2 - ox, y - oy, DAY_WIDTH - 4, DAY_HEIGHT - 2, SMALL_RADIUS, TITLE_FOCUSED);
        gui_text(c, x + DAY_WIDTH - 8 - gui_text_width(number, 1) - ox, y + 3 - oy, number,
                 is_today ? ON_ACCENT : cell % 7 >= 5 ? TASKBAR_DIM_TEXT : TASKBAR_TEXT, 1);
    }
}

static void draw_open_panel(struct gui_canvas *c, int unused, int ox, int oy) {
    struct rect p = panel_rect(panel);

    (void)unused;

    draw_glass(c, p, ox, oy);
    if (panel == TRAY_QUICK)
        draw_quick(c, p, ox, oy);
    else if (panel == TRAY_NOTICES)
        draw_history(c, p, ox, oy);
    else
        draw_calendar(c, p, ox, oy);
}

static void draw_tray(struct gui_canvas *c, int ox, int oy) {
    struct rect q = tray_item(TRAY_QUICK), b = tray_item(TRAY_NOTICES);
    const struct gui_image *network = gui_icon(net.airplane ? "airplane" : connected() ? "network" : "offline");
    const struct gui_image *speaker = gui_icon(muted || !volume ? "muted" : "speaker"), *bell = gui_icon("bell");
    int icon_y = q.y + (q.height - ICON_SIZE) / 2;

    for (int i = 0; i < TRAY_ITEMS; i++) {
        struct rect t = tray_item(i);

        if (panel == i || contains(t, cursor_x, cursor_y))
            gui_fill_rounded(c, t.x - ox, t.y - oy, t.width, t.height, SMALL_RADIUS, TASKBAR_BUTTON);
    }
    if (network)
        gui_draw_image(c, q.x + 4 - ox, icon_y - oy, network);
    if (speaker && mixer >= 0)
        gui_draw_image(c, q.x + q.width - 4 - ICON_SIZE - ox, icon_y - oy, speaker);
    if (bell)
        gui_draw_image(c, b.x + (b.width - ICON_SIZE) / 2 - ox, icon_y - oy, bell);
    if (unseen)
        gui_fill_rounded(c, b.x + b.width - 8 - ox, b.y + 1 - oy, 6, 6, GUI_MAX_RADIUS, CLOSE_COLOR);
}

static void open_file(const char *path);
static void save_settings(const char *what);

// A tile in quick settings turns what it's for on or off.
static void toggle_tile(int tile) {
    if (tile == TILE_AIRPLANE) {
        settings.airplane = !settings.airplane;
        save_settings(settings.airplane ? "airplane on" : "airplane off");
        check_network();
    } else if (tile == TILE_NIGHT) {
        settings.night_light = !settings.night_light;
        save_settings(settings.night_light ? "night light on" : "night light off");
    } else {
        settings.light = !settings.light;
        save_settings(settings.light ? "theme light" : "theme dark");
    }
}

// A click in an open panel does what it's on there, returning false if it's
// outside the panel.
static bool panel_click(void) {
    if (!contains(panel_rect(panel), cursor_x, cursor_y))
        return false;
    for (int i = 0; panel == TRAY_QUICK && i < TILES; i++) {
        if (contains(quick_tile(i), cursor_x, cursor_y))
            toggle_tile(i);
    }
    if (panel == TRAY_QUICK && mixer >= 0 && contains(volume_slider(), cursor_x, cursor_y)) {
        setting_volume = true;
        set_volume_at(cursor_x);
    } else if (panel == TRAY_QUICK && mixer >= 0 && contains(mute_button(), cursor_x, cursor_y)) {
        set_volume(volume, !muted);
    } else if (panel == TRAY_NOTICES && history_count && contains(clear_button(), cursor_x, cursor_y)) {
        clear_history();
        printf("wm: notifications cleared\n");
    } else if (panel == TRAY_CALENDAR && (contains(month_arrow(false), cursor_x, cursor_y)
                                          || contains(month_arrow(true), cursor_x, cursor_y))) {
        int year, month;

        calendar_month += contains(month_arrow(true), cursor_x, cursor_y) ? 1 : -1;
        shown_month(&year, &month);
        printf("wm: calendar %s %d\n", month_names[month], year);
        add_damage(panel_rect(TRAY_CALENDAR));
    }
    for (int i = 0; panel == TRAY_NOTICES && i < history_count; i++) {
        if (contains(history_row(i), cursor_x, cursor_y) && history[i].file[0]) {
            open_file(history[i].file);
            close_panel();
        }
    }
    return true;
}

static void draw_taskbar(struct gui_canvas *c, int ox, int oy) {
    struct rect bar = taskbar(), l = launcher();
    struct window *list[MAX_WINDOWS];
    int count = windows_by_age(list);
    int text_y = bar.y + (TASKBAR_HEIGHT - GUI_CHAR_HEIGHT) / 2 - oy;
    char clock[16];
    const char *shown = clock_text(clock);

    draw_glass(c, bar, ox, oy);
    gui_fill_rounded(c, l.x - ox, l.y - oy, l.width, l.height, SMALL_RADIUS, TASKBAR_BUTTON);
    gui_text(c, l.x + (l.width - gui_text_width("Apps", 1)) / 2 - ox, text_y, "Apps", TASKBAR_TEXT, 1);

    // A box for each workspace, the one shown in the accent colour, with a dot in
    // those that have windows.
    for (int i = 0; i < WORKSPACES; i++) {
        struct rect b = workspace_box(i);
        bool used = false;

        for (int j = 0; j < window_count && !used; j++)
            used = !windows[j]->bare && windows[j]->workspace == i;
        gui_fill_rounded(c, b.x - ox, b.y - oy, b.width, b.height, SMALL_RADIUS, i == workspace ? TITLE_FOCUSED : TASKBAR_BUTTON);
        if (used)
            gui_fill_rounded(c, b.x + b.width / 2 - 2 - ox, b.y + b.height / 2 - 2 - oy, 4, 4, GUI_MAX_RADIUS, i == workspace ? ON_ACCENT : TASKBAR_TEXT);
    }

    for (int i = 0; i < count; i++) {
        struct rect b = taskbar_button(i, count);
        struct window *w = list[i];
        uint32_t color = w == focused ? TITLE_FOCUSED : w->minimized ? TASKBAR_MINIMIZED : TASKBAR_BUTTON;

        gui_fill_rounded(c, b.x - ox, b.y - oy, b.width, b.height, SMALL_RADIUS, color);
        draw_icon(c, w->app, b.x + 4, b.y + (b.height - ICON_SIZE) / 2, ox, oy);
        fitted_text(c, b.x + 8 + ICON_SIZE - ox, text_y, b.width - 12 - ICON_SIZE, w->title,
                    w == focused ? ON_ACCENT : w->minimized ? TASKBAR_DIM_TEXT : TASKBAR_TEXT);
    }

    if (panel == TRAY_CALENDAR) {
        struct rect t = tray_item(TRAY_CALENDAR);

        gui_fill_rounded(c, t.x - ox, t.y - oy, t.width, t.height, SMALL_RADIUS, TASKBAR_BUTTON);
    }
    gui_text(c, bar.x + bar.width - gui_text_width(shown, 1) - 12 - ox, text_y, shown, TASKBAR_TEXT, 1);
    draw_tray(c, ox, oy);
}

// A row in the menu or the switcher, with an icon, if it's not NULL.
static void draw_item(struct gui_canvas *c, struct rect r, const char *text, const struct gui_image *icon, bool chosen,
                      uint32_t color, int ox, int oy) {
    int text_x = 12 + ICON_SIZE;

    if (chosen)
        gui_fill_rounded(c, r.x - ox, r.y - oy, r.width, r.height, SMALL_RADIUS, TITLE_FOCUSED);
    if (icon)
        gui_draw_image(c, r.x + 6 - ox, r.y + (r.height - ICON_SIZE) / 2 - oy, icon);
    fitted_text(c, r.x + text_x - ox, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 - oy, r.width - text_x - 8, text,
                chosen ? ON_ACCENT : color);
}

static int item_height(const struct item *item) {
    return item->action == ACT_LINE ? MENU_GAP : ITEM_HEIGHT;
}

static bool has_submenu(const struct item *item) {
    return item->action == ACT_FOLDER || item->action == ACT_OPEN_WITH || item->action == ACT_POSITION;
}

static bool choosable(const struct item *item) {
    return item->action != ACT_NONE && item->action != ACT_LINE;
}

static struct rect item_rect(const struct popup *p, int index) {
    int y = p->r.y + 4;

    for (int i = 0; i < index; i++)
        y += item_height(&p->items[i]);
    return (struct rect){p->r.x + 4, y, p->r.width - 8, item_height(&p->items[index])};
}

static int item_at(const struct popup *p, int x, int y) {
    for (int i = 0; i < p->count; i++) {
        if (choosable(&p->items[i]) && contains(item_rect(p, i), x, y))
            return i;
    }
    return -1;
}

static void damage_popups(void) {
    for (int i = 0; i < popup_depth; i++)
        add_damage(popups[i].r);
}

// A menu width wide at x, y, or left of x and above y if it doesn't fit there,
// kept on the screen.
static void place_popup(struct popup *p, int x, int y, int width) {
    int height = 8;

    for (int i = 0; i < p->count; i++)
        height += item_height(&p->items[i]);
    x = x + width > screen.width ? x - width : x;
    y = y + height > screen.height ? screen.height - height : y;
    p->r = (struct rect){x < 0 ? 0 : x, y < 0 ? 0 : y, width, height};
}

// A tick in a box.
static void draw_check(struct gui_canvas *c, int x, int y, bool on, int ox, int oy) {
    gui_frame_rounded(c, x - ox, y - oy, 14, 14, SMALL_RADIUS, on ? TITLE_FOCUSED : TASKBAR_COLOR, TASKBAR_TEXT);
}

static void draw_popup(struct gui_canvas *c, int level, int ox, int oy) {
    const struct popup *p = &popups[level];

    draw_glass(c, p->r, ox, oy);
    for (int i = 0; i < p->count; i++) {
        const struct item *item = &p->items[i];
        struct rect r = item_rect(p, i);
        bool chosen = i == p->choice;

        if (item->action == ACT_LINE) {
            gui_fill(c, r.x + 4 - ox, r.y + MENU_GAP / 2 - oy, r.width - 8, 1, TASKBAR_LINE);
            continue;
        }
        draw_item(c, r, item->label, item->icon, chosen && choosable(item),
                  item->action == ACT_NONE ? TASKBAR_DIM_TEXT : TASKBAR_TEXT, ox, oy);
        if (item->check)
            draw_check(c, r.x + 7, r.y + (r.height - 14) / 2, item->checked, ox, oy);
        if (has_submenu(item))
            gui_text(c, r.x + r.width - 14 - ox, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 - oy, ">",
                     chosen ? ON_ACCENT : TASKBAR_DIM_TEXT, 1);
    }
}

static void draw_switcher(struct gui_canvas *c, int ox, int oy) {
    struct rect s = switcher();

    draw_glass(c, s, ox, oy);
    for (int i = 0; i < window_count; i++) {
        struct window *w = switcher_window(i);

        draw_item(c, (struct rect){s.x + 4, s.y + 4 + i * ITEM_HEIGHT, s.width - 8, ITEM_HEIGHT}, w->title, app_icon(w->app),
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

static bool no_cursor; // while composing a screenshot

// Where a dragged window would snap to: an outline, over a quarter of the accent colour.
static void draw_snap(struct gui_canvas *c, int ox, int oy) {
    struct rect r = snap_rect(snap), inner = {r.x + SNAP_BORDER, r.y + SNAP_BORDER, r.width - 2 * SNAP_BORDER,
                                             r.height - 2 * SNAP_BORDER};
    int x0 = r.x > ox ? r.x : ox, x1 = r.x + r.width < ox + c->width ? r.x + r.width : ox + c->width;
    int y0 = r.y > oy ? r.y : oy, y1 = r.y + r.height < oy + c->height ? r.y + r.height : oy + c->height;
    uint32_t a = TITLE_FOCUSED;

    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->pixels + (long)(y - oy) * c->stride - ox;

        for (int x = x0; x < x1; x++) {
            uint32_t p = row[x];

            row[x] = !contains(inner, x, y) ? a
                   : ((((p & 0xFF00FF) * 3 + (a & 0xFF00FF)) >> 2) & 0xFF00FF)
                         | ((((p & 0x00FF00) * 3 + (a & 0x00FF00)) >> 2) & 0x00FF00);
        }
    }
}

// Icons on the desktop for what's in Desktop in the home folder, on a grid, with
// pictures as small copies of themselves. Clicking one selects it, double-clicking
// opens it, and dragging it moves it to another place on the grid, or into a
// folder there, or takes it to a window, as from Files. A file from Files dropped
// on the desktop is copied there. Where each goes is kept in LAYOUT, a line each:
// its column, its row and its name.
#define DESKTOP_ICON 32
#define DESKTOP_CELL_WIDTH 88
#define DESKTOP_CELL_HEIGHT 72
#define MAX_DESKTOP 64
#define DOUBLE_CLICK_MS 400
#define DRAG_DISTANCE 6
#define LAYOUT "/system/etc/desktop"
struct desktop_item {
    char name[256];
    bool folder;
    time_t changed;
    off_t size;
    int column, row;
    struct gui_image picture;
};
static struct desktop_item desktop_items[MAX_DESKTOP];
static int desktop_count, desktop_selected = -1, desktop_pressed = -1, pressed_x, pressed_y;
static int desktop_dragged = -1; // the one being dragged
static int64_t desktop_clicked; // when the one selected was clicked
static char desktop_dir[GUI_PATH_MAX];

// The grid, from the desktop's top left, leaving room for the hint at the bottom.
static int desktop_rows(void) {
    int rows = (desktop().height - 3 * GUI_CHAR_HEIGHT - 8) / DESKTOP_CELL_HEIGHT;

    return rows > 0 ? rows : 1;
}

static int desktop_columns(void) {
    int columns = (desktop().width - 8) / DESKTOP_CELL_WIDTH;

    return columns > 0 ? columns : 1;
}

static struct rect grid_cell(int column, int row) {
    struct rect d = desktop();

    return (struct rect){d.x + 8 + column * DESKTOP_CELL_WIDTH, d.y + 8 + row * DESKTOP_CELL_HEIGHT,
                         DESKTOP_CELL_WIDTH - 4, DESKTOP_CELL_HEIGHT - 4};
}

static struct rect desktop_cell(int index) {
    return grid_cell(desktop_items[index].column, desktop_items[index].row);
}

// The cell a point is in, kept on the grid.
static void cell_at(int x, int y, int *column, int *row) {
    struct rect d = desktop();

    *column = (x - d.x - 8) / DESKTOP_CELL_WIDTH;
    *row = (y - d.y - 8) / DESKTOP_CELL_HEIGHT;
    *column = *column < 0 ? 0 : *column >= desktop_columns() ? desktop_columns() - 1 : *column;
    *row = *row < 0 ? 0 : *row >= desktop_rows() ? desktop_rows() - 1 : *row;
}

static void desktop_path(int index, char out[GUI_PATH_MAX]) {
    snprintf(out, GUI_PATH_MAX, "%s/%s", desktop_dir, desktop_items[index].name);
}

static int desktop_item_at(int x, int y) {
    for (int i = 0; i < desktop_count && !settings.hide_icons; i++) {
        if (contains(desktop_cell(i), x, y))
            return i;
    }
    return -1;
}

static int item_in_cell(int column, int row) {
    for (int i = 0; i < desktop_count; i++) {
        if (desktop_items[i].column == column && desktop_items[i].row == row)
            return i;
    }
    return -1;
}

static int compare_desktop(const void *a, const void *b) {
    const struct desktop_item *x = a, *y = b;

    return x->folder != y->folder ? y->folder - x->folder : strcmp(x->name, y->name);
}

// A picture shrunk to fit, or an icon made bigger.
static void make_picture(struct desktop_item *item, const char *path) {
    const struct gui_image *icon = gui_file_icon(item->name, item->folder);
    struct gui_image full;

    if (icon == gui_icon("image") && gui_image_load(path, &full)) {
        int big = full.width > full.height ? full.width : full.height, w = full.width * DESKTOP_ICON / big,
            h = full.height * DESKTOP_ICON / big;

        if (!gui_image_scale(&full, w > 0 ? w : 1, h > 0 ? h : 1, &item->picture))
            item->picture = (struct gui_image){0};
        gui_image_free(&full);
    } else if (!icon || !gui_image_scale(icon, DESKTOP_ICON, DESKTOP_ICON, &item->picture)) {
        item->picture = (struct gui_image){0};
    }
}

static void save_layout(void) {
    FILE *f = fopen(LAYOUT, "w");

    for (int i = 0; f && i < desktop_count; i++)
        fprintf(f, "%d %d %s\n", desktop_items[i].column, desktop_items[i].row, desktop_items[i].name);
    if (f)
        fclose(f);
}

// Each item goes where LAYOUT says, if it's on the grid and free, and the rest in
// the first free cells, down each column from the left.
static void lay_out(void) {
    FILE *f = fopen(LAYOUT, "r");
    char line[300], name[256];
    int column, row;

    for (int i = 0; i < desktop_count; i++)
        desktop_items[i].column = desktop_items[i].row = -1;
    while (f && fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%d %d %255[^\n]", &column, &row, name) != 3 || column < 0 || row < 0
            || column >= desktop_columns() || row >= desktop_rows() || item_in_cell(column, row) >= 0)
            continue;
        for (int i = 0; i < desktop_count; i++) {
            if (!strcmp(desktop_items[i].name, name) && desktop_items[i].column < 0) {
                desktop_items[i].column = column;
                desktop_items[i].row = row;
            }
        }
    }
    if (f)
        fclose(f);
    for (int i = 0, cell = 0; i < desktop_count; i++) {
        while (desktop_items[i].column < 0 && cell < desktop_columns() * desktop_rows()) {
            if (item_in_cell(cell / desktop_rows(), cell % desktop_rows()) < 0) {
                desktop_items[i].column = cell / desktop_rows();
                desktop_items[i].row = cell % desktop_rows();
            }
            cell++;
        }
    }
    save_layout();
}

// Lists Desktop again, redrawing the desktop only if something's changed, or always.
static void load_desktop(bool always) {
    static struct desktop_item found[MAX_DESKTOP];
    DIR *dir = opendir(desktop_dir);
    struct dirent *d;
    int count = 0;

    while (dir && count < MAX_DESKTOP && (d = readdir(dir))) {
        char path[GUI_PATH_MAX];
        struct stat st;

        if (d->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", desktop_dir, d->d_name);
        if (stat(path, &st) < 0)
            continue;
        found[count] = (struct desktop_item){.folder = S_ISDIR(st.st_mode), .changed = st.st_mtime, .size = st.st_size};
        snprintf(found[count++].name, sizeof(found[0].name), "%s", d->d_name);
    }
    if (dir)
        closedir(dir);
    qsort(found, count, sizeof(*found), compare_desktop);

    bool same = count == desktop_count && !always;

    for (int i = 0; same && i < count; i++)
        same = !strcmp(found[i].name, desktop_items[i].name) && found[i].changed == desktop_items[i].changed
            && found[i].size == desktop_items[i].size && found[i].folder == desktop_items[i].folder;
    if (same)
        return;
    for (int i = 0; i < desktop_count; i++) {
        add_damage(desktop_cell(i));
        gui_image_free(&desktop_items[i].picture);
    }
    memcpy(desktop_items, found, count * sizeof(*found));
    desktop_count = count;
    desktop_selected = desktop_pressed = desktop_dragged = -1;
    lay_out();
    printf("wm: desktop:");
    for (int i = 0; i < count; i++) {
        char path[GUI_PATH_MAX];

        desktop_path(i, path);
        make_picture(&desktop_items[i], path);
        add_damage(desktop_cell(i));
        printf(" %s", desktop_items[i].name);
    }
    printf("\n");
}

// Half the colour over what's there.
static void tint(struct gui_canvas *c, struct rect r, int ox, int oy, uint32_t colour) {
    for (int y = r.y - oy < 0 ? 0 : r.y - oy; y < r.y + r.height - oy && y < c->height; y++) {
        uint32_t *row = c->pixels + (long)y * c->stride;

        for (int x = r.x - ox < 0 ? 0 : r.x - ox; x < r.x + r.width - ox && x < c->width; x++)
            row[x] = ((row[x] >> 1) & 0x7F7F7F) + ((colour >> 1) & 0x7F7F7F);
    }
}

static void draw_desktop(struct gui_canvas *c, int ox, int oy) {
    for (int i = 0; i < desktop_count && !settings.hide_icons; i++) {
        struct rect r = desktop_cell(i);
        const struct desktop_item *item = &desktop_items[i];
        char label[12];
        int width;

        if (i == desktop_selected)
            tint(c, r, ox, oy, TITLE_FOCUSED);
        if (item->picture.pixels)
            gui_draw_image(c, r.x + (r.width - item->picture.width) / 2 - ox,
                           r.y + 6 + (DESKTOP_ICON - item->picture.height) / 2 - oy, &item->picture);
        snprintf(label, sizeof(label), "%.10s", item->name);
        width = gui_text_width(label, 1);
        gui_text(c, r.x + (r.width - width) / 2 + 1 - ox, r.y + DESKTOP_ICON + 13 - oy, label, 0x000000, 1);
        gui_text(c, r.x + (r.width - width) / 2 - ox, r.y + DESKTOP_ICON + 12 - oy, label, 0xFFFFFF, 1);
    }
}

static void select_desktop_item(int index) {
    if (desktop_selected >= 0)
        add_damage(desktop_cell(desktop_selected));
    desktop_selected = index;
    if (index >= 0)
        add_damage(desktop_cell(index));
}

// A click on the desktop selects what's there, or nothing, and a second soon after opens it.
static void desktop_press(void) {
    int index = desktop_item_at(cursor_x, cursor_y);
    int64_t now = now_ms();
    char path[GUI_PATH_MAX];

    if (index >= 0 && index == desktop_selected && now - desktop_clicked < DOUBLE_CLICK_MS) {
        desktop_path(index, path);
        open_file(path);
        now = 0; // a third click starts again
    }
    select_desktop_item(index);
    desktop_clicked = now;
    desktop_pressed = index;
    pressed_x = cursor_x;
    pressed_y = cursor_y;
}

// Holding the mouse on an icon and moving it drags the file.
static void desktop_drag(void) {
    if (desktop_pressed < 0 || drag_file[0] || abs(cursor_x - pressed_x) + abs(cursor_y - pressed_y) < DRAG_DISTANCE)
        return;
    desktop_path(desktop_pressed, drag_file);
    drag_icon = gui_file_icon(drag_file, desktop_items[desktop_pressed].folder);
    desktop_dragged = desktop_pressed;
    desktop_pressed = -1;
    add_damage(drag_rect());
    printf("wm: dragging %s\n", drag_file);
}

// An icon let go on the desktop goes to the cell it's over, or into the folder
// there. Anything else there, it stays where it was.
static void desktop_drop(void) {
    struct desktop_item *item = &desktop_items[desktop_dragged];
    int column, row, there;

    cell_at(cursor_x, cursor_y, &column, &row);
    there = item_in_cell(column, row);
    if (there >= 0 && there != desktop_dragged && desktop_items[there].folder) {
        char from[GUI_PATH_MAX], to[GUI_PATH_MAX + 256];

        desktop_path(desktop_dragged, from);
        snprintf(to, sizeof(to), "%s/%s/%s", desktop_dir, desktop_items[there].name, item->name);
        if (rename(from, to) == 0)
            printf("wm: moved %s into %s\n", item->name, desktop_items[there].name);
        load_desktop(false);
        return;
    }
    if (there >= 0)
        return;
    add_damage(desktop_cell(desktop_dragged));
    item->column = column;
    item->row = row;
    add_damage(desktop_cell(desktop_dragged));
    save_layout();
    printf("wm: moved %s to %d %d\n", item->name, column, row);
}

// A copy of a file let go on the desktop goes in Desktop, in the cell it was let
// go over, unless it's there already.
static void copy_to_desktop(const char *path) {
    const char *slash = strrchr(path, '/');
    pid_t child;
    int column, row;
    FILE *f;

    if (!slash || ((size_t)(slash - path) == strlen(desktop_dir) && !strncmp(path, desktop_dir, slash - path)))
        return;
    if ((child = fork()) == 0) {
        execl("/system/bin/cp", "cp", "-r", path, desktop_dir, (char *)NULL);
        _exit(127);
    }
    if (child > 0)
        waitpid(child, NULL, 0);
    printf("wm: copied %s to the desktop\n", path);
    cell_at(cursor_x, cursor_y, &column, &row);
    if (item_in_cell(column, row) < 0 && (f = fopen(LAYOUT, "a"))) {
        fprintf(f, "%d %d %s\n", column, row, slash + 1);
        fclose(f);
    }
    load_desktop(false);
}

// Deleting from the desktop puts it in the trash.
static void trash_file(const char *path) {
    if (gui_trash(path))
        printf("wm: trashed %s\n", path);
    else
        add_notice("Couldn't move it to the trash", strerror(errno), NULL);
    load_desktop(false);
}

// Locking, logging out and logging in fade from what the screen showed to what
// it shows next. Locking and logging out hold what was there until the lock or
// log-in screen's window comes, or HOLD_MS goes by without it.
#define SCREEN_FADE_MS 250
#define HOLD_MS 1500

static struct {
    uint32_t *was; // the screen as it was, or NULL
    int width, height; // of the screen then
    bool holding;
    int64_t start; // of holding, then of fading
} screen_fade;

static void fade_screen(bool hold) {
    size_t size = (size_t)screen.width * screen.height * sizeof(uint32_t);

    if (screen_fade.was && (screen_fade.width != screen.width || screen_fade.height != screen.height)) {
        free(screen_fade.was);
        screen_fade.was = NULL;
    }
    if (!settings.animations || (!screen_fade.was && !(screen_fade.was = malloc(size))))
        return;
    screen_fade.width = screen.width;
    screen_fade.height = screen.height;
    for (int y = 0; y < screen.height; y++)
        memcpy(screen_fade.was + (long)y * screen.width, screen.pixels + (long)y * screen.stride, screen.width * sizeof(uint32_t));
    screen_fade.holding = hold;
    screen_fade.start = now_ms();
    add_damage((struct rect){0, 0, screen.width, screen.height});
}

// What the screen is now, after holding.
static void stop_holding(void) {
    if (screen_fade.was && screen_fade.holding) {
        screen_fade.holding = false;
        screen_fade.start = now_ms();
    }
}

// How much of the new screen shows, out of 256.
static int screen_shown(void) {
    if (!screen_fade.was || screen_fade.width != screen.width || screen_fade.height != screen.height)
        return 256;
    return screen_fade.holding ? 0 : ease_out(progress(screen_fade.start, SCREEN_FADE_MS));
}

static bool animate_screen(void) {
    if (!screen_fade.was)
        return false;
    if (screen_fade.holding && now_ms() - screen_fade.start >= HOLD_MS)
        stop_holding();
    add_damage((struct rect){0, 0, screen.width, screen.height});
    if (screen_shown() < 256)
        return true;
    free(screen_fade.was);
    screen_fade.was = NULL;
    return false;
}

// Menus and the taskbar's panels fade in as they slide FLYOUT_SLIDE towards the
// taskbar, and fade out sliding the other way, from a copy of how they looked.
#define FLYOUT_MS 150
#define FLYOUT_SLIDE 8

static struct {
    struct gui_canvas copy; // of the screen around it, from at
    struct rect at, r;
    int64_t start;
} flyout_ghost;

// Which way away from the taskbar is, down or up, as 1 or -1.
static int from_bar(void) {
    return settings.taskbar_top ? -1 : 1;
}

static int flyout_progress(int64_t start) {
    return settings.animations ? progress(start, FLYOUT_MS) : 256;
}

// Draws a flyout that opened at start, shifted and mixed with what's under it.
static void draw_flyout(struct gui_canvas *c, struct rect r, int64_t start,
                        void (*draw)(struct gui_canvas *, int, int, int), int arg, int ox, int oy) {
    static uint32_t *under;
    static size_t size;
    int t = ease_out(flyout_progress(start)), dy = (256 - t) * FLYOUT_SLIDE / 256 * from_bar();
    int x0 = r.x - ox < 0 ? 0 : r.x - ox, x1 = r.x + r.width - ox > c->width ? c->width : r.x + r.width - ox;
    int y0 = r.y + dy - oy < 0 ? 0 : r.y + dy - oy, y1 = r.y + dy + r.height - oy > c->height ? c->height : r.y + dy + r.height - oy;
    size_t needed = x1 > x0 && y1 > y0 ? (size_t)(x1 - x0) * (y1 - y0) : 0;

    if (t == 256 || !needed) {
        draw(c, arg, ox, oy - dy);
        return;
    }
    if (needed > size) {
        uint32_t *bigger = realloc(under, needed * sizeof(*under));

        if (!bigger) {
            draw(c, arg, ox, oy - dy);
            return;
        }
        under = bigger, size = needed;
    }
    for (int y = y0; y < y1; y++)
        memcpy(under + (size_t)(y - y0) * (x1 - x0), c->pixels + (long)y * c->stride + x0, (x1 - x0) * sizeof(*under));
    draw(c, arg, ox, oy - dy);
    for (int y = y0; y < y1; y++) {
        uint32_t *row = c->pixels + (long)y * c->stride, *was = under + (size_t)(y - y0) * (x1 - x0);

        for (int x = x0; x < x1; x++)
            row[x] = gui_mix(was[x - x0], row[x], t);
    }
}

static void draw_area(struct rect r, struct gui_canvas c);

// Keeps a copy of a flyout as it closes, to fade out.
static void flyout_leaves(struct rect r) {
    int x0 = r.x - BLUR_MARGIN < 0 ? 0 : r.x - BLUR_MARGIN, y0 = r.y - BLUR_MARGIN < 0 ? 0 : r.y - BLUR_MARGIN;
    int x1 = r.x + r.width + BLUR_MARGIN, y1 = r.y + r.height + BLUR_MARGIN;
    struct rect at = {x0, y0, (x1 < screen.width ? x1 : screen.width) - x0, (y1 < screen.height ? y1 : screen.height) - y0};
    uint32_t *pixels;

    if (!settings.animations || !(pixels = realloc(flyout_ghost.copy.pixels, (size_t)at.width * at.height * sizeof(uint32_t))))
        return;
    flyout_ghost.copy = (struct gui_canvas){pixels, at.width, at.height, at.width};
    flyout_ghost.at = at;
    flyout_ghost.r = r;
    flyout_ghost.start = now_ms();
    no_cursor = true;
    draw_area(at, flyout_ghost.copy);
    no_cursor = false;
    add_damage(at);
}

static void draw_flyout_ghost(struct gui_canvas *c, int ox, int oy) {
    struct rect r = flyout_ghost.r;
    int t = ease_in(flyout_progress(flyout_ghost.start)), dy = t * FLYOUT_SLIDE / 256 * from_bar();

    if (t == 256 || !flyout_ghost.copy.pixels)
        return;
    for (int y = r.y + dy; y < r.y + dy + r.height; y++) {
        const uint32_t *from = flyout_ghost.copy.pixels + (long)(y - dy - flyout_ghost.at.y) * flyout_ghost.copy.stride
                             - flyout_ghost.at.x;

        if (y < oy || y >= oy + c->height)
            continue;
        for (int x = r.x < ox ? ox : r.x; x < r.x + r.width && x < ox + c->width; x++) {
            uint32_t *to = c->pixels + (long)(y - oy) * c->stride + x - ox;

            *to = gui_mix(*to, from[x], 256 - t);
        }
    }
}

// Damages the flyouts while they move, returning whether any has further to go.
static bool animate_flyouts(void) {
    bool going = false;

    // Each goes on a frame past its end, to be drawn as it ends.
    if (popup_depth && settings.animations && now_ms() - menu_opened < FLYOUT_MS + FRAME_MS) {
        add_damage(with_shadow(popups[0].r));
        going = true;
    }
    if (panel >= 0 && settings.animations && now_ms() - panel_opened < FLYOUT_MS + FRAME_MS) {
        add_damage(with_shadow(panel_rect(panel)));
        going = true;
    }
    if (flyout_ghost.copy.pixels) {
        add_damage(with_shadow(flyout_ghost.r));
        if (flyout_progress(flyout_ghost.start) < 256)
            going = true;
        else {
            free(flyout_ghost.copy.pixels);
            flyout_ghost.copy.pixels = NULL;
        }
    }
    return going;
}

// Draws everything that overlaps a rectangle, on a canvas that is only that part
// of the screen.
static void draw_area(struct rect r, struct gui_canvas c) {
    int hint_y = desktop().y + desktop().height - 2 * GUI_CHAR_HEIGHT, first = -1;
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
        if (!locked) {
            gui_text(&c, GUI_CHAR_WIDTH - r.x, hint_y - r.y, HINT, HINT_COLOR, 1);
            draw_desktop(&c, r.x, r.y);
        }
        first = 0;
    }
    for (int i = first; i < window_count; i++) {
        struct window *w = windows[i];
        int alpha;

        if (!visible(w) || !overlaps(drawn_extent(w), r))
            continue;
        if (w->anim_ms) {
            struct rect now = arriving(w, &alpha);

            draw_layer(&c, &w->layer, now, alpha, w->shadow, rounded(w), w->translucent, r.x, r.y);
            continue;
        }
        if (settings.shadows)
            draw_shadow(&c, w, r.x, r.y);
        draw_window(&c, w, r.x, r.y);
    }
    draw_ghosts(&c, r.x, r.y);
    if (locked) {
        if (!no_cursor)
            draw_cursor(&c, r.x, r.y);
        return;
    }
    if (snap && overlaps(snap_rect(snap), r))
        draw_snap(&c, r.x, r.y);
    if (bar_visible() && overlaps(r, bar))
        draw_taskbar(&c, r.x, r.y);
    draw_notices(&c, r.x, r.y);
    if (popup_depth)
        draw_flyout(&c, popups[0].r, menu_opened, draw_popup, 0, r.x, r.y);
    if (popup_depth > 1)
        draw_popup(&c, 1, r.x, r.y);
    if (panel >= 0)
        draw_flyout(&c, panel_rect(panel), panel_opened, draw_open_panel, 0, r.x, r.y);
    draw_flyout_ghost(&c, r.x, r.y);
    if (switch_choice >= 0)
        draw_switcher(&c, r.x, r.y);
    if (drag_file[0] && drag_icon && !no_cursor) {
        struct rect d = drag_rect();

        gui_draw_image(&c, d.x - r.x, d.y - r.y, drag_icon);
    }
    if (!no_cursor)
        draw_cursor(&c, r.x, r.y);
}

// Composes all that changed before showing any of it, so a window being moved is
// never seen gone from both places.
// The night light takes away some of the blue, and less of the green, as the
// screen's shown, which is how these keep each.
static uint8_t night_green[256], night_blue[256];

static void make_night(void) {
    int k = settings.night_strength;

    for (int i = 0; i < 256; i++) {
        night_green[i] = i * (256 - k * 72 / 100) >> 8;
        night_blue[i] = i * (256 - k * 184 / 100) >> 8;
    }
}

static void show_row(uint32_t *to, const uint32_t *from, int width) {
    if (!settings.night_light) {
        memcpy(to, from, width * sizeof(uint32_t));
        return;
    }
    for (int x = 0; x < width; x++)
        to[x] = (from[x] & 0xFF0000) | night_green[from[x] >> 8 & 0xFF] << 8 | night_blue[from[x] & 0xFF];
}

// Draws a rectangle of the screen. As what's see-through blurs what's under it,
// a little more is drawn around it, on the side, so the blur reads what's really there.
static void compose(struct rect r) {
    static uint32_t *pixels;
    static size_t size;
    int x0 = r.x - BLUR_MARGIN < 0 ? 0 : r.x - BLUR_MARGIN, y0 = r.y - BLUR_MARGIN < 0 ? 0 : r.y - BLUR_MARGIN;
    int x1 = r.x + r.width + BLUR_MARGIN, y1 = r.y + r.height + BLUR_MARGIN;
    struct rect e = {x0, y0, (x1 < screen.width ? x1 : screen.width) - x0, (y1 < screen.height ? y1 : screen.height) - y0};
    size_t needed = (size_t)e.width * e.height;

    if (needed > size) {
        uint32_t *bigger = realloc(pixels, needed * sizeof(*pixels));

        if (!bigger)
            return;
        pixels = bigger, size = needed;
    }
    draw_area(e, (struct gui_canvas){pixels, e.width, e.height, e.width});
    for (int row = r.y; row < r.y + r.height; row++)
        memcpy(screen.pixels + (long)row * screen.stride + r.x, pixels + (long)(row - e.y) * e.width + r.x - e.x,
               r.width * sizeof(*pixels));
}

static void redraw(void) {
    static uint32_t mixed[MAX_SIZE];
    int shown = screen_shown();

    for (int i = 0; i < damage_count; i++)
        compose(damage[i]);
    for (int i = 0; i < damage_count; i++) {
        struct rect r = damage[i];

        for (int row = r.y; row < r.y + r.height; row++) {
            uint32_t *now = screen.pixels + (long)row * screen.stride + r.x;

            // Part way through fading, the row's what was there mixed with what's there now.
            if (shown < 256) {
                const uint32_t *was = screen_fade.was + (long)row * screen.width + r.x;

                for (int x = 0; x < r.width; x++)
                    mixed[x] = gui_mix(was[x], now[x], shown);
                now = mixed;
            }
            show_row((uint32_t *)(framebuffer + (long)row * pitch) + r.x, now, r.width);
        }
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
static void settings_changed(void);

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
        if (visible(windows[i])) {
            set_focus(windows[i]);
            return;
        }
    }
    set_focus(NULL);
}

static void minimize(struct window *w) {
    depart(w, button_for(w), MINIMIZE_MS);
    w->minimized = true;
    add_damage(extent_of(w));
    damage_taskbar();
    if (focused == w)
        focus_top();
}

// Takes away the outline of where a dragged window would snap to.
static void stop_snap(void) {
    if (snap)
        add_damage(snap_rect(snap));
    snap = SNAP_NONE;
}

static void go_to_workspace(int to);

static void show(struct window *w) {
    if (!on_workspace(w))
        go_to_workspace(w->workspace);
    if (w->minimized)
        arrive(w, button_for(w), MINIMIZE_MS);
    w->minimized = false;
    raise_window(w);
    set_focus(w);
    damage_taskbar();
}

static void go_to_workspace(int to) {
    if (to == workspace)
        return;
    workspace = to;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    focus_top();
    printf("wm: workspace %d\n", to + 1);
}

// The focused window goes to another workspace, and the next one here gets the focus.
static void send_to_workspace(struct window *w, int to) {
    if (!w || w->bare || w->workspace == to)
        return;
    add_damage(extent_of(w));
    damage_taskbar();
    w->workspace = to;
    focus_top();
    printf("wm: %s to workspace %d\n", w->title, to + 1);
}

static void remove_window(struct window *w) {
    int i = index_of(w);

    depart(w, shrunk(frame_of(w)), CLOSE_MS);
    add_damage(drawn_extent(w));
    if (switch_choice >= 0) {
        add_damage(switcher());
        switch_choice = window_count > 1 ? switch_choice % (window_count - 1) : -1;
    }
    damage_taskbar();
    memmove(&windows[i], &windows[i + 1], (window_count - i - 1) * sizeof(*windows));
    window_count--;
    if (dragging == w) {
        dragging = NULL;
        stop_snap();
    }
    if (grabbed == w)
        grabbed = NULL;
    if (resizing == w)
        resizing = NULL;
    if (focused == w) {
        focused = NULL;
        focus_top();
    }
    unmap_buffer(&w->canvas);
    free(w->layer.pixels);
    free(w);
}

static struct window *find_window(const struct client *client, uint32_t id) {
    for (int i = 0; i < window_count; i++) {
        if (windows[i]->client == client && windows[i]->id == id)
            return windows[i];
    }
    return NULL;
}

static void center(struct window *w) {
    w->x = (screen.width - w->canvas.width) / 2;
    w->y = (screen.height - w->canvas.height) / 2;
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
    w->bare = client == session;
    w->workspace = workspace;
    if (w->bare)
        center(w);
    memcpy(w->title, m->text, sizeof(w->title));
    w->title[sizeof(w->title) - 1] = '\0';
    windows[window_count++] = w;
    arrive(w, shrunk(frame_of(w)), OPEN_MS);
    damage_taskbar();
    if (switch_choice >= 0)
        add_damage(switcher());
    queue_message(client, &reply);
    queue_message(client, &(struct gui_message){
                              .type = GUI_SCREEN, .width = screen.width, .height = screen.height, .x = can_set_mode});
    add_damage(extent_of(w));
    if (visible(w)) // so nothing can take what's typed on the log-in screen
        set_focus(w);
    if (w->bare)
        stop_holding();
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
    if (w->bare) { // which sizes itself, rather than being asked to
        center(w);
        w->want_width = w->asked_width = c.width;
        w->want_height = w->asked_height = c.height;
    }
    add_damage(extent_of(w));
    w->asking = false;
    if (w->want_width != w->asked_width || w->want_height != w->asked_height)
        ask_size(w, w->want_width, w->want_height);
}

// A copy, ending in a zero byte, of the width bytes of shared memory a message
// names, to be freed, or NULL if there are none or more than max.
static char *copy_shared(const struct gui_message *m, size_t max) {
    size_t length = m->width > 0 && (size_t)m->width <= max ? (size_t)m->width : 0;
    int fd = length ? open_shared(m, O_RDONLY) : -1;
    void *shared = fd < 0 ? MAP_FAILED : mmap(NULL, length, PROT_READ, MAP_SHARED, fd, 0);
    char *copy = shared == MAP_FAILED ? NULL : malloc(length + 1);

    if (fd >= 0)
        close(fd);
    if (copy) {
        memcpy(copy, shared, length);
        copy[length] = '\0';
    }
    if (shared != MAP_FAILED)
        munmap(shared, length);
    return copy;
}

// Sends a message with data in new shared memory, named in its buffer, which the
// program removes.
static void send_shared(struct client *client, struct gui_message m, const char *data, size_t length) {
    static int created;
    int fd = -1;
    void *shared = MAP_FAILED;

    if (length) {
        snprintf(m.buffer, sizeof(m.buffer), "/wm-shared-%d", created++);
        fd = shm_open(m.buffer, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0 && ftruncate(fd, length) == 0)
            shared = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (fd >= 0)
            close(fd);
    }
    if (shared != MAP_FAILED) {
        memcpy(shared, data, length);
        munmap(shared, length);
        m.width = (int32_t)length;
    } else if (length) {
        shm_unlink(m.buffer);
    }
    queue_message(client, &m);
}

// A picture must be as long as its size says.
static void set_clipboard(const struct gui_message *m) {
    bool image = !strncmp(m->text, "image", sizeof(m->text));

    free(clipboard);
    clipboard = copy_shared(m, CLIPBOARD_MAX);
    clipboard_length = clipboard ? (size_t)m->width : 0;
    clipboard_width = image ? m->x : 0;
    clipboard_height = image ? m->y : 0;
    if (image && (m->x <= 0 || m->y <= 0 || (size_t)m->x * m->y * 4 != clipboard_length)) {
        free(clipboard);
        clipboard = NULL;
        clipboard_length = clipboard_width = clipboard_height = 0;
    }
}

static void send_clipboard(struct client *client) {
    struct gui_message m = {.type = GUI_CLIPBOARD, .x = clipboard_width, .y = clipboard_height};

    if (clipboard_width)
        snprintf(m.text, sizeof(m.text), "image");
    send_shared(client, m, clipboard, clipboard_length);
}

static void start_drag(const struct client *client, const struct gui_message *m) {
    char *path = grabbed && grabbed->client == client && buttons ? copy_shared(m, GUI_PATH_MAX - 1) : NULL;
    struct stat st;

    if (!path)
        return;
    snprintf(drag_file, sizeof(drag_file), "%s", path);
    free(path);
    drag_icon = gui_file_icon(drag_file, stat(drag_file, &st) == 0 && S_ISDIR(st.st_mode));
    add_damage(drag_rect());
    printf("wm: dragging %s\n", drag_file);
}

static void update_cursor(void);
static void unlock(void);

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
    case GUI_DRAG:
        start_drag(client, m);
        break;
    case GUI_UNLOCK:
        if (client == session && locked)
            unlock();
        break;
    case GUI_NOTIFY: // the %.*s in add_notice stops at the end of an unterminated title
        add_notice(m->text, m->buffer, w ? w->app : NULL);
        break;
    case GUI_SETTINGS:
        settings_changed();
        break;
    case GUI_ICON:
        if (w) {
            memcpy(w->app, m->text, sizeof(w->app));
            w->app[sizeof(w->app) - 1] = '\0';
            add_damage(extent_of(w));
            damage_taskbar();
        }
        break;
    case GUI_TRANSLUCENT:
        if (w) {
            w->translucent = m->x;
            add_damage(extent_of(w));
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

// The log-in screen stopping without anyone logging in is started again, after a
// second if it only just started, so one that can't run doesn't keep wm busy.
static void disconnect(int i) {
    for (int j = window_count - 1; j >= 0; j--) {
        if (windows[j]->client == clients[i])
            remove_window(windows[j]);
    }
    if (clients[i] == session) {
        session = NULL;
        if (locked)
            login_due = login_started + 1000;
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

static struct client *add_client(int fd) {
    int i = 0;

    while (i < MAX_CLIENTS && clients[i])
        i++;
    if (i == MAX_CLIENTS || !(clients[i] = calloc(1, sizeof(struct client)))) {
        close(fd);
        return NULL;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    clients[i]->fd = fd;
    return clients[i];
}

static void accept_client(int listener) {
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);

    if (fd >= 0)
        add_client(fd);
}

// Starts the log-in screen, or the lock screen, with the one end of a pair of
// sockets, so wm knows its connection from any other.
static void start_login(void) {
    int pair[2];

    login_due = -1;
    login_started = now_ms();
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
        printf("wm: can't start the log-in screen: %s\n", strerror(errno));
        login_due = login_started + 1000;
        return;
    }
    fcntl(pair[0], F_SETFD, FD_CLOEXEC);
    if (fork() == 0) {
        char fd[16];

        snprintf(fd, sizeof(fd), "%d", pair[1]);
        setenv("GUI_FD", fd, 1);
        execl(LOGIN_PATH, "login", logged_in ? "--lock" : (char *)NULL, (char *)NULL);
        _exit(127);
    }
    close(pair[1]);
    if (!(session = add_client(pair[0])))
        login_due = login_started + 1000;
}

static void close_menu(void);
static void launch(const char *command);

// Hides the desktop behind the lock screen, or the log-in screen once logged out.
static void lock(void) {
    close_menu();
    close_panel();
    switch_choice = -1;
    super_down = super_alone = false; // let go while locked
    dragging = grabbed = resizing = NULL;
    drag_file[0] = '\0';
    stop_snap();
    set_focus(NULL);
    fade_screen(true);
    locked = true;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    printf("wm: locked\n");
    if (!session)
        start_login();
}

// Logging out closes every program's connection, and so the programs, and
// forgets what they left: the clipboard and notifications.
static void log_out(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i] != session)
            disconnect(i);
    }
    free(clipboard);
    clipboard = NULL;
    clipboard_length = clipboard_width = clipboard_height = 0;
    notice_count = 0;
    clear_history();
    logged_in = false;
    workspace = 0;
    printf("wm: logged out\n");
    lock();
}

static void unlock(void) {
    fade_screen(false);
    locked = false;
    add_damage((struct rect){0, 0, screen.width, screen.height});
    focus_top();
    if (login_mode)
        printf("wm: unlocked\n");
    if (!logged_in) {
        logged_in = true;
        for (int i = 0; i < command_count; i++)
            launch(commands[i]);
        command_count = 0; // only the first time
    }
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
        int margin = w->bare ? 0 : RESIZE_MARGIN;
        struct rect grab = {f.x - margin, f.y - margin, f.width + 2 * margin, f.height + 2 * margin};

        if (!visible(w) || !contains(grab, x, y))
            continue;
        *edges = w->bare ? 0 : edges_at(f, x, y);
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
                   .buttons = buttons, .changed = changed, .wheel = wheel, .key.modifiers = modifiers,
               });
}

static void launch(const char *command) {
    if (fork() == 0) {
        execl("/system/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
}

// Opens a file in the app the settings choose for its kind, or runs it if it's a program.
static void open_file(const char *path) {
    const char *app = settings.apps[gui_kind_of(path)];
    char program[GUI_PATH_MAX];
    bool runs = gui_kind_of(path) != GUI_KIND_FOLDER && gui_is_program(path);

    if (!runs && !gui_app_path(app, program))
        return;
    printf(runs ? "wm: running %s\n" : "wm: opening %s in %s\n", path, app);
    if (fork() == 0) {
        if (runs)
            execl(path, strrchr(path, '/') ? strrchr(path, '/') + 1 : path, (char *)NULL);
        else
            execl(program, app, path, (char *)NULL);
        _exit(127);
    }
}

// Puts pixels on the clipboard, as a picture.
static bool copy_picture(const struct gui_canvas *from) {
    size_t row = (size_t)from->width * sizeof(uint32_t);
    char *copy = malloc(row * from->height);

    if (!copy)
        return false;
    for (int y = 0; y < from->height; y++)
        memcpy(copy + y * row, from->pixels + (long)y * from->stride, row);
    free(clipboard);
    clipboard = copy;
    clipboard_length = row * from->height;
    clipboard_width = from->width;
    clipboard_height = from->height;
    return true;
}

// Print Screen saves the screen, and with Alt the focused window, as it's shown but
// without the cursor, in a new file in Pictures, which the notice saying so opens.
// It's put on the clipboard as well.
static void screenshot(bool window) {
    struct rect r = {0, 0, screen.width, screen.height};
    char dir[GUI_PATH_MAX], path[GUI_PATH_MAX], stamp[32];
    const char *home = getenv("HOME");
    time_t now = time(NULL);

    if (window && focused) {
        struct rect f = frame_of(focused);
        int right = f.x + f.width < r.width ? f.x + f.width : r.width;
        int bottom = f.y + f.height < r.height ? f.y + f.height : r.height;

        r.x = f.x > 0 ? f.x : 0;
        r.y = f.y > 0 ? f.y : 0;
        r = (struct rect){r.x, r.y, right - r.x, bottom - r.y};
    }
    if (r.width <= 0 || r.height <= 0)
        return;
    snprintf(dir, sizeof(dir), "%s/Pictures", home && strcmp(home, "/") ? home : "");
    mkdir(dir, 0755);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d-%H%M%S", localtime(&now));
    snprintf(path, sizeof(path), "%s/screenshot-%s.png", dir, stamp);
    for (int i = 2; access(path, F_OK) == 0; i++)
        snprintf(path, sizeof(path), "%s/screenshot-%s-%d.png", dir, stamp, i);

    no_cursor = true;
    compose(r);
    no_cursor = false;
    add_damage(r); // to put the cursor back

    struct gui_canvas shot = {screen.pixels + (long)r.y * screen.stride + r.x, r.width, r.height, screen.stride};

    if (!gui_image_save(path, &shot)) {
        printf("wm: screenshot %s: %s\n", path, gui_image_error());
        add_notice("Couldn't save the screenshot", gui_image_error(), NULL);
        return;
    }
    printf("wm: screenshot %s %dx%d\n", path, r.width, r.height);
    notice_file(add_notice(copy_picture(&shot) ? "Screenshot saved and copied" : "Screenshot saved",
                           strrchr(path, '/') + 1, "viewer"),
                path);
}

static void close_menu(void) {
    if (popup_depth)
        flyout_leaves(popups[0].r);
    damage_popups();
    popup_depth = popup_focus = 0;
    start_open = false;
    autohide();
}

static struct item *add_item(struct popup *p, int action, const char *label, const char *arg,
                             const struct gui_image *icon) {
    static struct item spare; // what goes past the end of a full menu
    struct item *item = p->count < MAX_ITEMS ? &p->items[p->count++] : &spare;

    *item = (struct item){.action = action, .icon = icon};
    snprintf(item->label, sizeof(item->label), "%s", label);
    snprintf(item->arg, sizeof(item->arg), "%s", arg ? arg : "");
    return item;
}

static void add_check(struct popup *p, int action, const char *label, bool on) {
    struct item *item = add_item(p, action, label, NULL, NULL);

    item->check = true;
    item->checked = on;
}

struct named {
    char name[GUI_TITLE_MAX];
    bool folder;
};

static int compare_named(const void *a, const void *b) {
    const struct named *x = a, *y = b;

    return x->folder != y->folder ? y->folder - x->folder : strcmp(x->name, y->name);
}

// The apps in a folder, and with folders, the folders in it first, as they are.
static void add_apps(struct popup *p, const char *folder, bool folders) {
    struct named found[MAX_ITEMS];
    int count = 0;
    DIR *dir = opendir(folder);
    struct dirent *d;

    while (dir && count < MAX_ITEMS && (d = readdir(dir))) {
        char path[GUI_PATH_MAX];
        struct stat st;

        snprintf(path, sizeof(path), "%s/%s", folder, d->d_name);
        if (d->d_name[0] == '.' || stat(path, &st) < 0 || (S_ISDIR(st.st_mode) && !folders))
            continue;
        found[count].folder = S_ISDIR(st.st_mode);
        snprintf(found[count++].name, GUI_TITLE_MAX, "%s", d->d_name);
    }
    if (dir)
        closedir(dir);
    qsort(found, count, sizeof(*found), compare_named);
    for (int i = 0; i < count; i++) {
        char path[GUI_PATH_MAX];

        snprintf(path, sizeof(path), "%s/%s", folder, found[i].name);
        add_item(p, found[i].folder ? ACT_FOLDER : ACT_APP, found[i].name, path,
                 found[i].folder ? gui_icon("folder") : app_icon(found[i].name));
    }
}

static int first_choosable(const struct popup *p) {
    for (int i = 0; i < p->count; i++) {
        if (choosable(&p->items[i]))
            return i;
    }
    return -1;
}

// Says where a menu is, for the tests to find its items.
static void show_popup(int level) {
    struct popup *p = &popups[level];

    popup_depth = level + 1;
    if (level == 0)
        menu_opened = now_ms();
    add_damage(p->r);
    autohide();
    printf("wm: menu %d at %d %d %d %d\n", level, p->r.x, p->r.y, p->r.width, p->r.height);
}

// The apps, by the folders in APPS_DIR, listed afresh each time so new ones show up.
static void open_start_menu(void) {
    struct popup *p = &popups[0];

    close_panel();
    close_menu();
    p->count = 0;
    add_apps(p, APPS_DIR, true);
    if (!p->count)
        add_item(p, ACT_NONE, "No apps in " APPS_DIR, NULL, NULL);
    add_item(p, ACT_LINE, "", NULL, NULL);
    add_item(p, ACT_RUN, "Run...", NULL, app_icon(RUN_COMMAND));
    if (login_mode) { // locking and logging out are for a desktop someone logged in to
        add_item(p, ACT_LOCK, "Lock", NULL, gui_icon("lock"));
        add_item(p, ACT_LOG_OUT, "Log out", NULL, gui_icon("logout"));
    }
    add_item(p, ACT_REBOOT, "Reboot", NULL, gui_icon("reboot"));
    add_item(p, ACT_SHUT_DOWN, "Shut down", NULL, gui_icon("shutdown"));
    place_popup(p, GAP, 0, MENU_WIDTH);
    p->r.y = beside_bar(p->r.height);
    p->choice = first_choosable(p);
    start_open = true;
    show_popup(0);
}

// What can be done with an icon on the desktop, as in Files, or on the desktop itself.
static void open_desktop_menu(int index) {
    struct popup *p = &popups[0];

    close_panel();
    close_menu();
    p->count = 0;
    if (index >= 0) {
        bool folder = desktop_items[index].folder;
        char editor[GUI_PATH_MAX];

        desktop_path(index, popup_file);
        add_item(p, ACT_OPEN, "Open", NULL, NULL);
        if (!folder && gui_app_path("editor", editor))
            add_item(p, ACT_OPEN_IN, "Open in Editor", editor, NULL);
        if (!folder)
            add_item(p, ACT_OPEN_WITH, "Open with", NULL, NULL);
        if (!folder && gui_is_program(popup_file))
            add_item(p, ACT_RUN_FILE, "Run", NULL, NULL);
        add_item(p, ACT_LINE, "", NULL, NULL);
        add_item(p, ACT_CUT, "Cut", NULL, NULL);
        add_item(p, ACT_COPY, "Copy", NULL, NULL);
        add_item(p, ACT_COPY_PATH, "Copy path", NULL, NULL);
        add_item(p, ACT_RENAME, "Rename", NULL, NULL);
        add_item(p, ACT_DELETE, "Delete", NULL, NULL);
        add_item(p, ACT_LINE, "", NULL, NULL);
        add_item(p, ACT_PROPERTIES, "Properties", NULL, NULL);
    } else {
        add_item(p, ACT_REFRESH, "Refresh", NULL, NULL);
        add_check(p, ACT_SHOW_ICONS, "Show icons", !settings.hide_icons);
    }
    place_popup(p, cursor_x, cursor_y, MENU_WIDTH);
    p->choice = -1;
    show_popup(0);
}

static void open_taskbar_menu(void) {
    struct popup *p = &popups[0];

    close_panel();
    close_menu();
    p->count = 0;
    add_item(p, ACT_POSITION, "Position", NULL, NULL);
    add_check(p, ACT_AUTOHIDE, "Autohide taskbar", settings.autohide);
    place_popup(p, cursor_x, 0, MENU_WIDTH);
    p->r.y = beside_bar(p->r.height);
    p->choice = -1;
    show_popup(0);
}

static void close_submenu(void) {
    if (popup_depth > 1)
        add_damage(popups[1].r);
    popup_depth = 1;
    popup_focus = 0;
}

// Beside the item it's for, or on the other side if there isn't room.
static void open_submenu(int index) {
    const struct item *parent = &popups[0].items[index];
    struct popup *p = &popups[1], *menu = &popups[0];
    struct rect r = item_rect(menu, index);
    int width = 180, x = menu->r.x + menu->r.width - 2;

    close_submenu();
    p->count = 0;
    if (parent->action == ACT_FOLDER) {
        add_apps(p, parent->arg, false);
    } else if (parent->action == ACT_OPEN_WITH) {
        char names[MAX_ITEMS][32], path[GUI_PATH_MAX];
        int count = gui_app_names(names, MAX_ITEMS);

        for (int i = 0; i < count; i++) {
            if (gui_app_path(names[i], path))
                add_item(p, ACT_OPEN_IN, names[i], path, app_icon(names[i]));
        }
    } else {
        add_check(p, ACT_TOP, "Top", settings.taskbar_top);
        add_check(p, ACT_BOTTOM, "Bottom", !settings.taskbar_top);
    }
    if (!p->count)
        add_item(p, ACT_NONE, "Empty", NULL, NULL);
    place_popup(p, x + width > screen.width ? menu->r.x - width + 2 : x, r.y - 4, width);
    p->choice = -1;
    show_popup(1);
}

// Logging out, rebooting and shutting down ask every window to close first, so
// programs can save what they have, and go ahead once all have closed. Any still
// open after LEAVE_MS, waiting for an answer about changes, stop it.
#define LEAVE_MS 1500
static int leaving = -1; // the item waiting for windows to close
static int64_t leave_due;

static void leave(int item) {
    if (item == ITEM_LOG_OUT)
        log_out();
    else
        reboot(item == ITEM_REBOOT ? RB_AUTOBOOT : RB_POWER_OFF);
}

static void start_leaving(int item) {
    if (!window_count) {
        leave(item);
        return;
    }
    leaving = item;
    leave_due = now_ms() + LEAVE_MS;
    for (int i = 0; i < window_count; i++)
        send_to(windows[i], (struct gui_message){.type = GUI_CLOSE});
    printf("wm: asking %d windows to close\n", window_count);
}

// How long until it's time to give up waiting, or -1 if nothing is.
static int check_leaving(void) {
    if (leaving < 0)
        return -1;
    if (!window_count) {
        int item = leaving;

        leaving = -1;
        leave(item);
        return -1;
    }
    if (now_ms() < leave_due)
        return (int)(leave_due - now_ms());
    add_notice(leaving == ITEM_LOG_OUT ? "Not logged out" : leaving == ITEM_REBOOT ? "Not restarted" : "Not shut down",
               "A program is still open", NULL);
    leaving = -1;
    return -1;
}

static void load_desktop(bool always);
static void trash_file(const char *path);

// Saves a setting changed here, and tells everyone, as Settings does.
static void save_settings(const char *what) {
    if (gui_settings_save(&settings))
        printf("wm: %s\n", what);
    settings_changed();
}

// What the desktop's file is for the clipboard: its path, and when it's cut, where
// Files looks to move it, rather than copy it, when it's pasted.
#define CUT "/tmp/files-cut"
static void put_path(const char *path, bool cut) {
    FILE *f;

    free(clipboard);
    clipboard = strdup(path);
    clipboard_length = clipboard ? strlen(path) : 0;
    clipboard_width = clipboard_height = 0;
    unlink(CUT);
    if (cut && (f = fopen(CUT, "w"))) {
        fputs(path, f);
        fclose(f);
    }
    printf("wm: %s %s\n", cut ? "cut" : "copied", path);
}

// Runs a program, with an option and a file, either of which may be NULL.
static void start(const char *program, const char *option, const char *file) {
    const char *name = strrchr(program, '/') ? strrchr(program, '/') + 1 : program;

    if (fork() == 0) {
        execl(program, name, option ? option : file, option ? file : (char *)NULL, (char *)NULL);
        _exit(127);
    }
}

static void files_with(const char *option, const char *file) {
    char files[GUI_PATH_MAX];

    if (gui_app_path("files", files))
        start(files, option, file);
}

static void act(const struct item *item) {
    switch (item->action) {
    case ACT_APP:
        start(item->arg, NULL, NULL);
        break;
    case ACT_RUN:
        launch(RUN_COMMAND);
        break;
    case ACT_LOCK:
        lock();
        break;
    case ACT_LOG_OUT:
    case ACT_REBOOT:
    case ACT_SHUT_DOWN:
        start_leaving(item->action == ACT_LOG_OUT ? ITEM_LOG_OUT : item->action == ACT_REBOOT ? ITEM_REBOOT : ITEM_SHUT_DOWN);
        break;
    case ACT_OPEN:
        open_file(popup_file);
        break;
    case ACT_OPEN_IN:
        printf("wm: opening %s in %s\n", popup_file, item->label);
        start(item->arg, NULL, popup_file);
        break;
    case ACT_RUN_FILE:
        printf("wm: running %s\n", popup_file);
        start(popup_file, NULL, NULL);
        break;
    case ACT_CUT:
    case ACT_COPY:
    case ACT_COPY_PATH:
        put_path(popup_file, item->action == ACT_CUT);
        break;
    case ACT_RENAME:
    case ACT_PROPERTIES:
        files_with(item->action == ACT_RENAME ? "--rename" : "--properties", popup_file);
        break;
    case ACT_DELETE:
        trash_file(popup_file);
        break;
    case ACT_REFRESH:
        load_desktop(true);
        break;
    case ACT_SHOW_ICONS:
        settings.hide_icons = !settings.hide_icons;
        save_settings(settings.hide_icons ? "icons hidden" : "icons shown");
        break;
    case ACT_TOP:
    case ACT_BOTTOM:
        settings.taskbar_top = item->action == ACT_TOP;
        save_settings(settings.taskbar_top ? "taskbar top" : "taskbar bottom");
        break;
    case ACT_AUTOHIDE:
        settings.autohide = !settings.autohide;
        save_settings(settings.autohide ? "autohide on" : "autohide off");
        break;
    }
}

// An item with a submenu opens it, with the keyboard, and any other does what
// it's for, closing the menus.
static void choose(int level, int index) {
    struct item item = popups[level].items[index];

    if (has_submenu(&item) && level == 0) {
        open_submenu(index);
        popup_focus = 1;
        popups[1].choice = first_choosable(&popups[1]);
    } else if (choosable(&item) && !has_submenu(&item)) {
        close_menu();
        act(&item);
    }
}

// The mouse over a menu highlights what it's on, opening its submenu if it has one.
static void hover_menu(void) {
    for (int level = popup_depth - 1; level >= 0; level--) {
        struct popup *p = &popups[level];
        int i = item_at(p, cursor_x, cursor_y);
        bool open = popup_depth > 1 && p->choice == i;

        if (!contains(p->r, cursor_x, cursor_y))
            continue;
        popup_focus = level;
        if (i < 0 || i == p->choice)
            return;
        p->choice = i;
        add_damage(p->r);
        if (level == 0 && has_submenu(&p->items[i]) && !open)
            open_submenu(i);
        else if (level == 0 && !has_submenu(&p->items[i]))
            close_submenu();
        return;
    }
}

// A press in a menu chooses what it's on, and anywhere else closes the menus,
// returning whether it should go no further: when it was in one, or on the
// launcher, which would open the start menu again.
static bool menu_press(uint32_t changed) {
    for (int level = popup_depth - 1; level >= 0; level--) {
        struct popup *p = &popups[level];
        int i = item_at(p, cursor_x, cursor_y);

        if (!contains(p->r, cursor_x, cursor_y))
            continue;
        if (i >= 0 && (changed & MOUSE_BUTTON_LEFT))
            choose(level, i);
        return true;
    }

    bool on_launcher = start_open && bar_visible() && contains(launcher(), cursor_x, cursor_y);

    close_menu();
    return on_launcher;
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
        open_start_menu();
    for (int i = 0; i < WORKSPACES; i++) {
        if (contains(workspace_box(i), cursor_x, cursor_y))
            go_to_workspace(i);
    }
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

    // While it's locked, a press only goes to the log-in screen.
    if (locked) {
        if (w) {
            set_focus(w);
            grabbed = w;
            send_mouse(w, GUI_MOUSE_BUTTON, changed, 0);
        }
        return;
    }

    // Clicking a notification puts it away.
    int notice = popup_depth || panel >= 0 ? -1 : notice_at(cursor_x, cursor_y);

    if (notice >= 0) {
        if (notices[notice].file[0])
            open_file(notices[notice].file);
        remove_notice(notice);
        return;
    }

    // A click in an open panel does what it's on, and anywhere else closes it. Its
    // icon in the tray closes it too, and another's opens that one.
    int item = bar_visible() ? tray_item_at(cursor_x, cursor_y) : -1;

    if (panel >= 0) {
        int was = panel;

        if (panel_click())
            return;
        close_panel();
        if (item == was)
            return;
    }
    if (item >= 0 && (changed & MOUSE_BUTTON_LEFT)) {
        close_menu();
        open_panel(item);
        return;
    }

    // A click anywhere closes a menu, and does what it would have done unless it
    // was in the menu or on the launcher.
    if (popup_depth && menu_press(changed))
        return;
    if (bar_visible() && contains(taskbar(), cursor_x, cursor_y)) {
        if (changed & MOUSE_BUTTON_LEFT)
            taskbar_click();
        else if (changed & MOUSE_BUTTON_RIGHT)
            open_taskbar_menu();
        return;
    }
    if (!w) {
        if (changed & MOUSE_BUTTON_LEFT)
            desktop_press();
        else if (changed & MOUSE_BUTTON_RIGHT) {
            int index = desktop_item_at(cursor_x, cursor_y);

            select_desktop_item(index);
            open_desktop_menu(index);
        }
        return;
    }
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
        dragging = w;
        drag_x = cursor_x - w->x;
        drag_y = cursor_y - w->y;
    }
}

// A file let go over a window's contents goes to it, which comes to the top, with
// the keys held.
static void drop_file(void) {
    enum part part;
    int edges;
    struct window *w = window_at(cursor_x, cursor_y, &part, &edges);

    add_damage(drag_rect());
    if (!w && !locked && !(bar_visible() && contains(taskbar(), cursor_x, cursor_y)))
    {
        if (desktop_dragged >= 0)
            desktop_drop();
        else
            copy_to_desktop(drag_file);
    }
    desktop_dragged = -1;
    if (w && part == PART_CONTENT && !locked) {
        set_focus(w);
        raise_window(w);
        send_shared(w->client, (struct gui_message){.type = GUI_DROP, .window = w->id, .x = cursor_x - w->x,
                                                    .y = cursor_y - w->y, .key.modifiers = modifiers},
                    drag_file, strlen(drag_file));
        printf("wm: dropped %s on %s\n", drag_file, w->title);
    }
    drag_file[0] = '\0';
}

// A mouse that says how far it moved goes the speed the settings choose times
// that, out of 5, keeping what's left of a pixel for next time.
static int faster(int d, int axis) {
    static int left[2];
    int scaled = d * settings.mouse_speed + left[axis];

    left[axis] = scaled % 5;
    return scaled / 5;
}

// A hidden taskbar shows while the mouse is at its edge of the screen, or on it,
// or a menu or panel is open.
static void autohide(void) {
    // The bar, and the gap between it and the edge.
    struct rect bar = taskbar(), near = {0, settings.taskbar_top ? 0 : bar.y, screen.width, bar.height + GAP};
    bool at_edge = settings.taskbar_top ? cursor_y == 0 : cursor_y == screen.height - 1;
    bool show = !settings.autohide || at_edge || popup_depth || panel >= 0 || (bar_shown && contains(near, cursor_x, cursor_y));

    if (show == bar_shown)
        return;
    bar_shown = show;
    add_damage(bar);
    printf("wm: taskbar %s\n", show ? "shown" : "hidden");
}

static void handle_mouse(const struct mouse_event *e) {
    enum part part;
    int edges;
    uint32_t pressed = e->buttons & ~buttons, released = buttons & ~e->buttons;
    // To the nearest pixel, as the host gives the last one as a little under its share of the range.
    int64_t range = MOUSE_ABSOLUTE_MAX + 1;
    int x = e->absolute ? (int)(((int64_t)e->x * screen.width + range / 2) / range) : cursor_x + faster(e->dx, 0);
    int y = e->absolute ? (int)(((int64_t)e->y * screen.height + range / 2) / range) : cursor_y + faster(e->dy, 1);

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
    if (moved)
        autohide();

    // Windows can be dragged partly off the screen, but the title bar stays on it.
    // A tiled one goes back to its own size, as far across under the mouse as it was.
    if (dragging && moved) {
        int top = cursor_y - drag_y, bottom = desktop().y + desktop().height - BORDER, least = desktop().y + TITLE_HEIGHT;
        enum snap to = cursor_x <= 0 ? SNAP_LEFT : cursor_x >= screen.width - 1 ? SNAP_RIGHT : cursor_y <= 0 ? SNAP_TOP
                                                                                                              : SNAP_NONE;

        if (dragging->tiled) {
            struct rect r = dragging->restore;

            drag_x = drag_x * (r.width - 2 * BORDER) / dragging->canvas.width;
            untile(dragging);
        }
        move_window(dragging, cursor_x - drag_x, top < least ? least : top > bottom ? bottom : top);
        if (to != snap) {
            add_damage(snap_rect(snap ? snap : to));
            add_damage(snap_rect(to));
            snap = to;
        }
    }
    if (resizing && moved) {
        int edges = resizing->resize_edges, dx = cursor_x - resize_x, dy = cursor_y - resize_y;

        if (edges & EDGE_TOP && dy < -resize_room)
            dy = -resize_room;
        ask_size(resizing, resize_width + (edges & EDGE_RIGHT ? dx : edges & EDGE_LEFT ? -dx : 0),
                 resize_height + (edges & EDGE_BOTTOM ? dy : edges & EDGE_TOP ? -dy : 0));
    }

    // Menus highlight what's under the mouse, and so do the tray and its panels.
    if (popup_depth && moved)
        hover_menu();
    if (panel >= 0 && moved)
        add_damage(panel_rect(panel));
    if (moved && bar_visible())
        add_damage(tray());

    int wheel = e->wheel;

    // The volume follows the mouse while it's held on the slider, and the wheel
    // changes it over the speaker or the sound menu.
    if (setting_volume && moved)
        set_volume_at(cursor_x);
    if (released & MOUSE_BUTTON_LEFT)
        setting_volume = false;
    if (e->wheel && mixer >= 0 && !locked
        && ((bar_visible() && contains(tray_item(TRAY_QUICK), cursor_x, cursor_y))
            || (panel == TRAY_QUICK && contains(panel_rect(TRAY_QUICK), cursor_x, cursor_y)))) {
        set_volume(volume - e->wheel * VOLUME_STEP, muted);
        wheel = 0; // so no window gets it
    }

    struct window *target = grabbed ? grabbed : dragging || resizing ? NULL : window_at(cursor_x, cursor_y, &part, &edges);

    if (target && !grabbed && part != PART_CONTENT)
        target = NULL;
    if (target && moved)
        send_mouse(target, GUI_MOUSE_MOVE, 0, 0);
    if (target && wheel)
        send_mouse(target, GUI_MOUSE_WHEEL, 0, wheel);

    if (moved && (buttons & MOUSE_BUTTON_LEFT))
        desktop_drag();
    if (pressed)
        press(pressed);
    if (released && grabbed)
        send_mouse(grabbed, GUI_MOUSE_BUTTON, released, 0);
    if (!buttons && drag_file[0])
        drop_file();
    if (!buttons && dragging && snap) {
        place(dragging, snap_rect(snap));
        printf("wm: snapped %s %s\n", dragging->title, snap == SNAP_TOP ? "maximized" : snap == SNAP_LEFT ? "left" : "right");
        stop_snap();
    }
    if (!buttons) {
        dragging = NULL;
        grabbed = NULL;
        resizing = NULL;
        desktop_pressed = -1;
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

// Up and Down move through a menu, Right and Enter open a submenu, Left and Escape
// close it, Enter does what's highlighted, and Escape closes the menu.
static bool menu_key(const struct key_event *e) {
    uint16_t code = e->code;

    if (!popup_depth || (e->modifiers & KEY_MOD_SUPER)
        || !(code == KEY_UP || code == KEY_DOWN || code == KEY_LEFT || code == KEY_RIGHT || code == KEY_ENTER
             || code == KEY_ESCAPE))
        return false;
    if (!e->pressed)
        return true;

    struct popup *p = &popups[popup_focus];

    if (code == KEY_ESCAPE || code == KEY_LEFT) {
        if (popup_focus)
            close_submenu();
        else if (code == KEY_ESCAPE)
            close_menu();
    } else if (code == KEY_ENTER || code == KEY_RIGHT) {
        if (p->choice >= 0 && (code == KEY_ENTER || has_submenu(&p->items[p->choice])))
            choose(popup_focus, p->choice);
    } else if (p->count) {
        int step = code == KEY_DOWN ? 1 : p->count - 1, i = p->choice < 0 ? (code == KEY_DOWN ? p->count - 1 : 0) : p->choice;

        for (int tries = 0; tries < p->count; tries++) {
            i = (i + step) % p->count;
            if (choosable(&p->items[i])) {
                p->choice = i;
                break;
            }
        }
        add_damage(p->r);
        if (popup_focus == 0)
            close_submenu();
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
        if (popup_depth)
            close_menu();
        else
            open_start_menu();
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
                          : grabbed || (w && part == PART_CONTENT && !popup_depth) ? w->cursor
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

    modifiers = e->modifiers;
    if (locked) {
        if (focused && focused->client == session)
            send_to(focused, (struct gui_message){.type = GUI_KEY, .key = *e});
        return;
    }
    super_key(e);
    if (panel >= 0 && e->code == KEY_ESCAPE) {
        if (e->pressed)
            close_panel();
        return;
    }
    if (switch_key(e) || menu_key(e))
        return;
    if (e->code == KEY_PRINT_SCREEN || e->code == KEY_SYSRQ) { // which Alt+Print Screen is
        if (e->pressed)
            screenshot(e->code == KEY_SYSRQ || (e->modifiers & KEY_MOD_ALT));
        return;
    }
    if (super && e->code == KEY_Q) {
        if (e->pressed && (e->modifiers & KEY_MOD_SHIFT))
            quitting = true;
        else if (e->pressed && focused)
            send_to(focused, (struct gui_message){.type = GUI_CLOSE});
        return;
    }
    if (super && e->code >= KEY_1 && e->code < KEY_1 + WORKSPACES) {
        if (e->pressed && (e->modifiers & KEY_MOD_SHIFT))
            send_to_workspace(focused, e->code - KEY_1);
        else if (e->pressed)
            go_to_workspace(e->code - KEY_1);
        return;
    }
    if (e->code == KEY_ESCAPE && (e->modifiers & KEY_MOD_CTRL) && (e->modifiers & KEY_MOD_SHIFT)) {
        if (e->pressed)
            launch(TASKS_COMMAND);
        return;
    }
    if (super && e->code == KEY_L && login_mode) {
        if (e->pressed)
            lock();
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

// When the desktop changes, tiled windows keep their share of it, and the others
// move to stay on it.
static void fit_windows(struct rect was) {
    struct rect now = desktop();

    for (int i = 0; i < window_count; i++) {
        struct window *w = windows[i];
        struct rect f = frame_of(w);

        if (w->bare) {
            center(w);
            continue;
        }
        if (w->tiled) {
            place(w, (struct rect){(f.x - was.x) * now.width / was.width + now.x,
                                   (f.y - was.y) * now.height / was.height + now.y, f.width * now.width / was.width,
                                   f.height * now.height / was.height});
            continue;
        }
        f.x = f.x + f.width > now.x + now.width ? now.x + now.width - f.width : f.x;
        f.y = f.y + f.height > now.y + now.height ? now.y + now.height - f.height : f.y;
        move_window(w, (f.x > now.x ? f.x : now.x) + BORDER, (f.y > now.y ? f.y : now.y) + TITLE_HEIGHT + BORDER);
    }
}

// Changes the screen's resolution.
static void change_resolution(int width, int height) {
    struct fb_info info;
    struct rect was = desktop();
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
    damage_count = 0; // clipped to the old size, so they could run off this one
    if (framebuffer == MAP_FAILED) {
        quitting = true;
        return;
    }
    fit_windows(was);
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
    struct rect was = desktop();
    bool icons_were_hidden = settings.hide_icons;

    gui_settings_load(&settings);
    theme_now = *(settings.light ? &light : &dark);
    theme_now.title_focused = gui_accent_colors[settings.accent];
    theme_now.on_accent = gui_on_accent(settings.accent);
    theme = &theme_now;
    make_night();
    net_airplane(settings.airplane);
    autohide();
    if (desktop().y != was.y || desktop().height != was.height) {
        fit_windows(was);
        load_desktop(true); // on a grid that's moved
    }
    if (icons_were_hidden != settings.hide_icons)
        desktop_selected = -1;
    ioctl(keyboard, KEYBOARD_SET_REPEAT_DELAY, &settings.repeat_delay);
    ioctl(keyboard, KEYBOARD_SET_REPEAT_RATE, &settings.repeat_rate);

    // The volume may have been set elsewhere, as Settings does.
    int now_volume = volume, now_muted = muted;

    if (mixer >= 0 && ioctl(mixer, MIXER_GET_VOLUME, &now_volume) == 0 && ioctl(mixer, MIXER_GET_MUTE, &now_muted) == 0
        && (now_volume != volume || now_muted != muted)) {
        volume = now_volume;
        muted = now_muted;
        damage_tray();
    }
    if (can_set_mode && settings.width && (settings.width != screen.width || settings.height != screen.height))
        change_resolution(settings.width, settings.height);
    load_wallpaper();
    add_damage((struct rect){0, 0, screen.width, screen.height});
}

// wm, then every program, loads the settings again.
static void settings_changed(void) {
    load_settings();
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i])
            queue_message(clients[i], &(struct gui_message){.type = GUI_SETTINGS});
    }
}

static int fail(const char *what) {
    dprintf(STDERR_FILENO, "wm: %s: %s\n", what, strerror(errno));
    return 1;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: wm [--login] [command]...\n"
                            "Runs the window system, starting each command in it. Super+Shift+Q quits.\n"
                            "With --login, it starts locked, behind the log-in screen, and the commands\n"
                            "start once someone's logged in. Super+L locks it again.\n");
    login_mode = argc > 1 && !strcmp(argv[1], "--login");
    commands = argv + 1 + login_mode;
    command_count = argc - 1 - login_mode;
    signal(SIGPIPE, SIG_IGN);

    struct fb_info info;
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);

    if (fb < 0 || fb_info(fb, &info) < 0)
        return fail("/dev/fb0");

    int mouse = open("/dev/mouse", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    keyboard = open("/dev/keyboard", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
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

    // Programs start in the home folder, which has the desktop's.
    const char *home = getenv("HOME");

    snprintf(desktop_dir, sizeof(desktop_dir), "%s/Desktop", home && strcmp(home, "/") ? home : "");
    if (home && chdir(home) < 0)
        printf("wm: can't go to %s\n", home);
    mkdir(desktop_dir, 0755);
    add_damage((struct rect){0, 0, screen.width, screen.height});
    make_falloff();
    make_coverage();
    load_settings();
    load_volume();
    load_desktop(false);
    if (login_mode)
        lock();
    else
        unlock();
    printf("wm: running at %dx%d%s\n", screen.width, screen.height, can_set_mode ? "" : ", which can't change");
    fflush(stdout);

    char clock[16], shown_clock[16];
    int64_t net_checked = now_ms();

    snprintf(shown_clock, sizeof(shown_clock), "%s", clock_text(clock));
    check_network();
    while (!quitting) {
        struct pollfd fds[3 + MAX_CLIENTS] = {
            {.fd = listener, .events = POLLIN}, {.fd = mouse, .events = POLLIN}, {.fd = keyboard, .events = POLLIN},
        };

        for (int i = 0; i < MAX_CLIENTS; i++) {
            fds[3 + i].fd = clients[i] ? clients[i]->fd : -1;
            fds[3 + i].events = clients[i] ? POLLIN | (clients[i]->out_used ? POLLOUT : 0) : 0;
        }
        bool fading = fade_shadows() | animate() | animate_flyouts() | animate_screen();
        int notice = expire_notices(); // before drawing, so those that went are gone

        redraw();

        // The clock changes at the start of each minute, fading shadows each frame, and
        // the network's looked at every NET_CHECK_MS.
        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        int until_minute = (60 - now.tv_sec % 60) * 1000 - now.tv_nsec / 1000000;
        int timeout = fading ? FRAME_MS : until_minute > 0 ? until_minute : 1;
        int64_t until_check = net_checked + NET_CHECK_MS - now_ms();

        timeout = until_check < timeout ? (until_check > 0 ? (int)until_check : 0) : timeout;

        timeout = notice >= 0 && notice < timeout ? (notice > 0 ? notice : 1) : timeout;

        int leave_wait = check_leaving();

        timeout = leave_wait >= 0 && leave_wait < timeout ? leave_wait : timeout;
        if (login_due >= 0) {
            int64_t wait = login_due - now_ms();

            if (wait <= 0)
                start_login();
            else if (wait < timeout)
                timeout = (int)wait;
        }

        int ready = poll(fds, 3 + MAX_CLIENTS, timeout);

        if (ready < 0 && errno != EINTR)
            return fail("poll");
        if (strcmp(clock_text(clock), shown_clock)) {
            snprintf(shown_clock, sizeof(shown_clock), "%s", clock_text(clock));
            damage_taskbar();
        }
        if (now_ms() >= net_checked + NET_CHECK_MS) {
            check_network();
            load_desktop(false);
            net_checked = now_ms();
        }

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
