#include <gui.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MARGIN 10
#define SPACING 8
#define PADDING 5 // around text in a frame
#define FRAME_HEIGHT (GUI_CHAR_HEIGHT + 2 * PADDING)
#define ROW_HEIGHT (GUI_CHAR_HEIGHT + 6) // in lists, menus and dropdowns
#define CHECK_SIZE 16
#define GRAB_WIDTH 10
#define SCROLLBAR 10
#define MIN_THUMB 16
#define WHEEL_STEP (3 * ROW_HEIGHT)
#define MENU_WIDTH 180
#define DROPDOWN_ROWS 8 // at most, with more scrolling
#define DOUBLE_CLICK_MS 400
#define TAB_BAR (FRAME_HEIGHT + 4)
#define TAB_WIDTH 170 // at most, with narrower ones when there are many
#define TAB_CLOSE 16
#define SEPARATOR_HEIGHT 9 // in a menu

// Colours, from the theme the settings choose.
struct theme {
    uint32_t background, panel, popup, text, dim_text, frame, frame_hot, accent, accent_hot, on_accent;
    uint32_t focus_ring, separator, selection, thumb;
};

static const struct theme dark = {
    .background = 0x1E2129, .panel = 0x181B21, .popup = 0x2B303B, .text = 0xE6E8EE, .dim_text = 0x8A93A6,
    .frame = 0x2B303B, .frame_hot = 0x363D4B, .accent = 0x3D63DD, .accent_hot = 0x5578E8, .on_accent = 0xFFFFFF,
    .focus_ring = 0x8FA8F5, .separator = 0x3A4150, .selection = 0x34519E, .thumb = 0x4A5263,
};
static const struct theme light = {
    .background = 0xF3F4F7, .panel = 0xFFFFFF, .popup = 0xFFFFFF, .text = 0x1E2129, .dim_text = 0x6B7280,
    .frame = 0xE2E5EB, .frame_hot = 0xD3D8E1, .accent = 0x3D63DD, .accent_hot = 0x5578E8, .on_accent = 0xFFFFFF,
    .focus_ring = 0x5578E8, .separator = 0xC9CED8, .selection = 0xB9CAF7, .thumb = 0xB4BBC8,
};
static const struct theme *theme = &dark;

#define BACKGROUND (theme->background)
#define PANEL (theme->panel) // behind scrolling areas and lists
#define POPUP (theme->popup)
#define TEXT (theme->text)
#define DIM_TEXT (theme->dim_text)
#define FRAME (theme->frame)
#define FRAME_HOT (theme->frame_hot)
#define ACCENT (theme->accent)
#define ACCENT_HOT (theme->accent_hot)
#define ON_ACCENT (theme->on_accent) // text on the accent
#define FOCUS_RING (theme->focus_ring)
#define SEPARATOR (theme->separator)
#define SELECTION (theme->selection)
#define THUMB (theme->thumb)

static struct gui_rect intersect(struct gui_rect a, struct gui_rect b) {
    int x = a.x > b.x ? a.x : b.x, y = a.y > b.y ? a.y : b.y;
    int right = a.x + a.width < b.x + b.width ? a.x + a.width : b.x + b.width;
    int bottom = a.y + a.height < b.y + b.height ? a.y + a.height : b.y + b.height;

    return (struct gui_rect){x, y, right > x ? right - x : 0, bottom > y ? bottom - y : 0};
}

static bool contains(struct gui_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

static struct gui_rect whole(const struct gui_ui *ui) {
    return (struct gui_rect){0, 0, ui->window->canvas.width, ui->window->canvas.height};
}

// The window, cut down to the clip rectangle, which drawing goes through.
static struct gui_canvas clipped(const struct gui_ui *ui) {
    struct gui_canvas *w = &ui->window->canvas;
    struct gui_rect c = ui->layout.clip;

    return (struct gui_canvas){w->pixels + (long)c.y * w->stride + c.x, c.width, c.height, w->stride};
}

static void fill(struct gui_ui *ui, int x, int y, int width, int height, uint32_t color) {
    struct gui_canvas c = clipped(ui);

    gui_fill(&c, x - ui->layout.clip.x, y - ui->layout.clip.y, width, height, color);
}

static void draw_image(struct gui_ui *ui, int x, int y, const struct gui_image *image) {
    struct gui_canvas c = clipped(ui);

    gui_draw_image(&c, x - ui->layout.clip.x, y - ui->layout.clip.y, image);
}

static void text(struct gui_ui *ui, int x, int y, const char *s, uint32_t color, int scale) {
    struct gui_canvas c = clipped(ui);

    gui_text(&c, x - ui->layout.clip.x, y - ui->layout.clip.y, s, color, scale);
}

// What a label shows: everything before any "##".
static int shown_length(const char *label) {
    const char *hidden = strstr(label, "##");

    return hidden ? (int)(hidden - label) : (int)strlen(label);
}

// Text cut to fit a width.
static void draw_fitted(struct gui_ui *ui, int x, int y, int width, const char *s, int length, uint32_t color) {
    char shown[256];
    int fits = width / GUI_CHAR_WIDTH;

    snprintf(shown, sizeof(shown), "%.*s", length < fits ? length : fits > 0 ? fits : 0, s);
    text(ui, x, y, shown, color, 1);
}

static void draw_label(struct gui_ui *ui, int x, int y, const char *label, uint32_t color) {
    draw_fitted(ui, x, y, 255 * GUI_CHAR_WIDTH, label, shown_length(label), color);
}

static uint32_t id_of(const char *label) {
    uint32_t hash = 2166136261u;

    for (; *label; label++)
        hash = (hash ^ (uint8_t)*label) * 16777619u;
    return hash ? hash : 1;
}

static void load_theme(void) {
    struct gui_settings settings;

    gui_settings_load(&settings);
    theme = settings.light ? &light : &dark;
}

void gui_ui_init(struct gui_ui *ui, struct gui_window *window) {
    *ui = (struct gui_ui){.window = window, .window_focused = true, .mouse_x = -1, .mouse_y = -1};
    load_theme();
}

bool gui_ui_event(struct gui_ui *ui, const struct gui_message *e) {
    switch (e->type) {
    case GUI_MOUSE_MOVE:
    case GUI_MOUSE_BUTTON:
    case GUI_MOUSE_WHEEL:
        ui->mouse_x = e->x;
        ui->mouse_y = e->y;
        if (e->type == GUI_MOUSE_BUTTON && (e->changed & 1)) {
            struct timespec now;

            ui->mouse_down = e->buttons & 1;
            ui->mouse_pressed |= ui->mouse_down;
            ui->mouse_released |= !ui->mouse_down;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (ui->mouse_down) {
                long ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;

                ui->double_click = ms - ui->last_press < DOUBLE_CLICK_MS;
                ui->last_press = ui->double_click ? 0 : ms; // a third press starts again
            }
        }
        if (e->type == GUI_MOUSE_BUTTON && (e->changed & 2) && (e->buttons & 2))
            ui->right_pressed = true;
        if (e->type == GUI_MOUSE_WHEEL)
            ui->wheel += e->wheel;
        return true;
    case GUI_KEY:
        if (e->key.pressed && ui->key_count < GUI_UI_MAX_KEYS)
            ui->keys[ui->key_count++] = e->key;
        return e->key.pressed;
    case GUI_FOCUS:
        ui->window_focused = e->focused;
        if (!e->focused)
            ui->popup = 0; // menus and dropdowns close when another window is chosen
        return true;
    case GUI_RESIZE:
        return gui_resize(ui->window, e->width, e->height);
    case GUI_REDRAW:
    case GUI_SCREEN:
        return true;
    case GUI_SETTINGS:
        load_theme();
        return true;
    }
    return false;
}

static bool key_pressed(const struct gui_ui *ui, uint16_t code) {
    for (int i = 0; i < ui->key_count; i++) {
        if (ui->keys[i].code == code)
            return true;
    }
    return false;
}

void gui_ui_begin(struct gui_ui *ui) {
    struct gui_rect all = whole(ui);

    ui->layout = (struct gui_layout){
        .left = MARGIN, .right = all.width - MARGIN, .x = MARGIN, .y = MARGIN, .line_end = MARGIN, .clip = all,
    };
    ui->depth = 0;
    fill(ui, 0, 0, all.width, all.height, BACKGROUND);
    ui->hot = 0;
    ui->cursor = GUI_CURSOR_ARROW;
    ui->next_count = 0;
    ui->next_wheel_owner = 0;
    ui->next_menu_count = 0;
    ui->popup_drawn = false;

    // An open popup has the mouse when it's over it. A click anywhere else, or
    // Escape, closes it, and does nothing more, but a right-click can open another.
    ui->mouse_in_popup = ui->popup && contains(ui->popup_rect, ui->mouse_x, ui->mouse_y);
    if (ui->popup && (((ui->mouse_pressed || ui->right_pressed) && !ui->mouse_in_popup) || key_pressed(ui, KEY_ESCAPE))) {
        ui->popup = 0;
        ui->mouse_pressed = false;
    }

    // F10 opens the first menu, or closes the one that's open, which then has the keyboard.
    if (key_pressed(ui, KEY_F10) && ui->menu_count) {
        ui->popup = ui->popup && ui->popup_is_menu ? 0 : ui->menus[0];
        ui->popup_is_menu = true;
        ui->menu_choice = 0;
    }
    ui->menu_key_count = 0;
    if (ui->popup && ui->popup_is_menu) {
        memcpy(ui->menu_keys, ui->keys, ui->key_count * sizeof(*ui->keys));
        ui->menu_key_count = ui->key_count;
        ui->key_count = 0;
    }

    // Clicking where there's no widget takes the keyboard away, and one there gives it back.
    if (ui->mouse_pressed && !ui->mouse_in_popup)
        ui->focus = 0;

    // Tab goes to the next widget that can have the keyboard, and Shift+Tab back,
    // in the order they came last time, going round at the ends.
    for (int i = 0; i < ui->key_count && ui->order_count; i++) {
        int at = -1;

        if (ui->keys[i].code != KEY_TAB || (ui->keys[i].modifiers & KEY_MOD_CTRL))
            continue; // Ctrl+Tab is for tabs
        for (int j = 0; j < ui->order_count; j++) {
            if (ui->order[j] == ui->focus)
                at = j;
        }

        bool back = ui->keys[i].modifiers & KEY_MOD_SHIFT;

        at = at < 0 ? (back ? ui->order_count - 1 : 0) : (at + (back ? ui->order_count - 1 : 1)) % ui->order_count;
        ui->focus = ui->reveal = ui->order[at];
    }
}

void gui_ui_end(struct gui_ui *ui) {
    // The popup goes back over whatever was drawn on it after.
    if (ui->popup_pixels) {
        struct gui_rect r = ui->popup_rect;
        struct gui_canvas saved = {ui->popup_pixels, r.width, r.height, r.width};

        gui_copy(&ui->window->canvas, r.x, r.y, &saved, 0, 0, r.width, r.height);
        free(ui->popup_pixels);
        ui->popup_pixels = NULL;
    }
    if (!ui->popup_drawn && !ui->again)
        ui->popup = 0;

    memcpy(ui->order, ui->next_order, ui->next_count * sizeof(*ui->order));
    memcpy(ui->menus, ui->next_menus, ui->next_menu_count * sizeof(*ui->menus));
    ui->menu_count = ui->next_menu_count;
    ui->reveal = 0;
    ui->order_count = ui->next_count;
    ui->wheel_owner = ui->next_wheel_owner;
    if (ui->mouse_released)
        ui->active = 0;
    ui->mouse_pressed = ui->mouse_released = ui->right_pressed = false;
    ui->wheel = 0;
    ui->key_count = 0;
    gui_show(ui->window);
    if (ui->cursor != ui->shown_cursor)
        gui_set_cursor(ui->window, ui->cursor);
    ui->shown_cursor = ui->cursor;
    if (ui->again)
        gui_post(&(struct gui_message){.type = GUI_REDRAW, .window = ui->window->id});
    ui->again = false;
}

void gui_same_line(struct gui_ui *ui) {
    ui->layout.same_line = true;
}


struct gui_rect gui_last_rect(const struct gui_ui *ui) {
    return ui->last;
}

// Where the next widget starts: beside the last, or under it.
static void next_spot(const struct gui_ui *ui, int *x, int *y) {
    const struct gui_layout *l = &ui->layout;

    if (l->same_line && l->line_height) {
        *x = l->line_end + SPACING;
        *y = l->y;
    } else {
        *x = l->left;
        *y = l->y + (l->line_height ? l->line_height + SPACING : 0);
    }
}

// Room for a widget, under the last or beside it.
static struct gui_rect place(struct gui_ui *ui, int width, int height) {
    struct gui_layout *l = &ui->layout;

    bool new_line = !l->same_line || !l->line_height;

    next_spot(ui, &l->x, &l->y);
    if (new_line)
        l->line_height = 0;
    l->same_line = false;
    ui->last = (struct gui_rect){l->x, l->y, width, height};
    l->line_end = l->x + width;
    if (height > l->line_height)
        l->line_height = height;
    return ui->last;
}

// From where the next widget goes down to the bottom of what can be seen, for
// widgets that fill it, but never less than min.
static int height_left(const struct gui_ui *ui, int min) {
    int x, y, left;

    next_spot(ui, &x, &y);
    left = ui->layout.clip.y + ui->layout.clip.height - MARGIN - y;
    return left > min ? left : min;
}

// What's left of the line, for widgets that stretch.
static int room(const struct gui_ui *ui) {
    int x, y;

    next_spot(ui, &x, &y);
    return ui->layout.right - x;
}

// Whether the mouse is over part of a widget that can be seen, and isn't under
// an open popup.
static bool over(const struct gui_ui *ui, struct gui_rect r) {
    return contains(r, ui->mouse_x, ui->mouse_y) && contains(ui->layout.clip, ui->mouse_x, ui->mouse_y)
        && (ui->in_popup || !ui->mouse_in_popup);
}

// The mouse's part in a widget: whether it's over it, and pressing makes it
// active and gives it the keyboard. Returns whether it was clicked: let go over it.
static bool interact(struct gui_ui *ui, uint32_t id, struct gui_rect r) {
    bool is_over = over(ui, r);

    if (is_over && (!ui->active || ui->active == id))
        ui->hot = id;
    if (is_over && ui->mouse_pressed) {
        ui->active = id;
        ui->focus = id;
    }
    return ui->mouse_released && ui->active == id && is_over;
}

static struct gui_scroll *scroll_of(struct gui_ui *ui, uint32_t id);

// Scrolls the areas the last widget is in to show it, from the innermost out. That
// shows in the next frame, which comes at once.
static void reveal(struct gui_ui *ui) {
    struct gui_rect r = ui->last;

    for (int d = ui->depth - 1; d >= 0; d--) {
        struct gui_rect view = ui->saved[d].view;
        int shift = r.y < view.y                               ? r.y - view.y - PADDING
                  : r.y + r.height > view.y + view.height ? r.y + r.height - view.y - view.height + PADDING
                                                               : 0;

        scroll_of(ui, ui->saved[d].id)->offset += shift;
        r.y -= shift;
        ui->again |= shift != 0;
    }
}

// Takes part in Tab's order, returning whether it has the keyboard.
static bool focusable(struct gui_ui *ui, uint32_t id) {
    if (id == ui->reveal)
        reveal(ui);
    if (ui->next_count < GUI_UI_MAX_FOCUSABLE)
        ui->next_order[ui->next_count++] = id;
    return ui->focus == id && ui->window_focused;
}

static void draw_focus(struct gui_ui *ui, struct gui_rect r) {
    fill(ui, r.x - 2, r.y - 2, r.width + 4, 1, FOCUS_RING);
    fill(ui, r.x - 2, r.y + r.height + 1, r.width + 4, 1, FOCUS_RING);
    fill(ui, r.x - 2, r.y - 2, 1, r.height + 4, FOCUS_RING);
    fill(ui, r.x + r.width + 1, r.y - 2, 1, r.height + 4, FOCUS_RING);
}

static void draw_border(struct gui_ui *ui, struct gui_rect r, uint32_t color) {
    fill(ui, r.x, r.y, r.width, 1, color);
    fill(ui, r.x, r.y + r.height - 1, r.width, 1, color);
    fill(ui, r.x, r.y, 1, r.height, color);
    fill(ui, r.x + r.width - 1, r.y, 1, r.height, color);
}

void gui_heading(struct gui_ui *ui, const char *s) {
    struct gui_rect r = place(ui, gui_text_width(s, 2), 2 * GUI_CHAR_HEIGHT);

    text(ui, r.x, r.y, s, TEXT, 2);
}

void gui_label(struct gui_ui *ui, const char *format, ...) {
    char s[256];
    va_list args;

    va_start(args, format);
    vsnprintf(s, sizeof(s), format, args);
    va_end(args);

    // Beside a framed widget, it lines up with that widget's text.
    bool beside = ui->layout.same_line && ui->layout.line_height;
    struct gui_rect r = place(ui, gui_text_width(s, 1), beside ? ui->layout.line_height : GUI_CHAR_HEIGHT);

    text(ui, r.x, r.y + (r.height - GUI_CHAR_HEIGHT) / 2, s, TEXT, 1);
}

void gui_separator(struct gui_ui *ui) {
    struct gui_rect r = place(ui, room(ui), 1);

    fill(ui, r.x, r.y, r.width, 1, SEPARATOR);
}

void gui_progress(struct gui_ui *ui, float fraction) {
    struct gui_rect r = place(ui, room(ui), FRAME_HEIGHT);
    char s[16];

    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    snprintf(s, sizeof(s), "%d%%", (int)(fraction * 100 + 0.5f));
    fill(ui, r.x, r.y, r.width, r.height, FRAME);
    fill(ui, r.x, r.y, (int)(r.width * fraction), r.height, ACCENT);
    text(ui, r.x + (r.width - gui_text_width(s, 1)) / 2, r.y + PADDING, s, TEXT, 1);
}

bool gui_button(struct gui_ui *ui, const char *label) {
    uint32_t id = id_of(label);
    struct gui_rect r = place(ui, shown_length(label) * GUI_CHAR_WIDTH + 2 * PADDING + 4, FRAME_HEIGHT);
    bool clicked = interact(ui, id, r);
    bool focused = focusable(ui, id);

    clicked |= focused && (key_pressed(ui, KEY_ENTER) || key_pressed(ui, KEY_SPACE));
    bool pressed = ui->active == id && ui->hot == id;

    fill(ui, r.x, r.y, r.width, r.height, pressed ? ACCENT : ui->hot == id ? FRAME_HOT : FRAME);
    draw_label(ui, r.x + PADDING + 2, r.y + PADDING, label, pressed ? ON_ACCENT : TEXT);
    if (focused)
        draw_focus(ui, r);
    return clicked;
}

bool gui_image_button(struct gui_ui *ui, const char *label, const struct gui_image *image, bool selected) {
    uint32_t id = id_of(label);
    int width = image ? image->width : 64, height = image ? image->height : 40;
    struct gui_rect r = place(ui, width + 6, height + 6 + GUI_CHAR_HEIGHT + 4);
    struct gui_rect picture = {r.x, r.y, width + 6, height + 6};
    bool clicked = interact(ui, id, picture), focused = focusable(ui, id);
    uint32_t edge = selected ? ACCENT : ui->hot == id ? FRAME_HOT : FRAME;

    clicked |= focused && (key_pressed(ui, KEY_ENTER) || key_pressed(ui, KEY_SPACE));
    fill(ui, picture.x, picture.y, picture.width, picture.height, edge);
    fill(ui, picture.x + 2, picture.y + 2, width + 2, height + 2, BACKGROUND);
    if (image)
        draw_image(ui, picture.x + 3, picture.y + 3, image);
    draw_fitted(ui, r.x + (r.width - shown_length(label) * GUI_CHAR_WIDTH) / 2, r.y + picture.height + 4, r.width, label,
                shown_length(label), selected ? TEXT : DIM_TEXT);
    if (focused)
        draw_focus(ui, picture);
    return clicked;
}

bool gui_checkbox(struct gui_ui *ui, const char *label, bool *value) {
    uint32_t id = id_of(label);
    struct gui_rect r = place(ui, CHECK_SIZE + 8 + shown_length(label) * GUI_CHAR_WIDTH, CHECK_SIZE);
    bool toggled = interact(ui, id, r);
    bool focused = focusable(ui, id);

    toggled |= focused && key_pressed(ui, KEY_SPACE);
    if (toggled)
        *value = !*value;
    fill(ui, r.x, r.y, CHECK_SIZE, CHECK_SIZE, ui->hot == id ? FRAME_HOT : FRAME);
    if (*value)
        fill(ui, r.x + 3, r.y + 3, CHECK_SIZE - 6, CHECK_SIZE - 6, ui->hot == id ? ACCENT_HOT : ACCENT);
    draw_label(ui, r.x + CHECK_SIZE + 8, r.y + (CHECK_SIZE - GUI_CHAR_HEIGHT) / 2, label, TEXT);
    if (focused)
        draw_focus(ui, (struct gui_rect){r.x, r.y, CHECK_SIZE, CHECK_SIZE});
    return toggled;
}

// The frame stretches, with the label after it as ImGui puts it.
static void framed(struct gui_ui *ui, const char *label, int height, struct gui_rect *frame) {
    int label_width = shown_length(label) * GUI_CHAR_WIDTH;
    int width = room(ui);
    struct gui_rect r = place(ui, width, height);

    *frame = (struct gui_rect){r.x, r.y, width - (label_width ? label_width + SPACING : 0), height};
    draw_label(ui, frame->x + frame->width + SPACING, r.y + PADDING, label, TEXT);
}

bool gui_slider(struct gui_ui *ui, const char *label, int *value, int min, int max) {
    uint32_t id = id_of(label);
    struct gui_rect frame;
    int before = *value, span = max > min ? max - min : 1;

    framed(ui, label, FRAME_HEIGHT, &frame);
    interact(ui, id, frame);

    bool focused = focusable(ui, id);
    int step = 1;

    for (int i = 0; focused && i < ui->key_count; i++) {
        step = ui->keys[i].modifiers & KEY_MOD_SHIFT ? 10 : 1;
        if (ui->keys[i].code == KEY_LEFT)
            *value -= step;
        else if (ui->keys[i].code == KEY_RIGHT)
            *value += step;
    }

    // Dragging sets it from where the mouse is along the frame.
    int track = frame.width - GRAB_WIDTH;

    if (ui->active == id && ui->mouse_down && track > 0)
        *value = min + ((ui->mouse_x - frame.x - GRAB_WIDTH / 2) * span + track / 2) / track;
    *value = *value < min ? min : *value > max ? max : *value;

    char s[32];
    int grab = frame.x + (int)((long)(*value - min) * track / span);

    snprintf(s, sizeof(s), "%d", *value);
    fill(ui, frame.x, frame.y, frame.width, frame.height, ui->hot == id ? FRAME_HOT : FRAME);
    fill(ui, grab, frame.y + 2, GRAB_WIDTH, frame.height - 4, ui->active == id ? ACCENT_HOT : ACCENT);
    text(ui, frame.x + (frame.width - gui_text_width(s, 1)) / 2, frame.y + PADDING, s, TEXT, 1);
    if (focused)
        draw_focus(ui, frame);
    return *value != before;
}

// The cursor starts at the end when a text widget gets the keyboard, with nothing
// selected.
static void own_cursor(struct gui_ui *ui, uint32_t id, size_t len) {
    if (ui->focus == id && (ui->text_owner != id || ui->text_cursor > len || ui->text_anchor > len)) {
        ui->text_cursor = ui->text_anchor = len;
        ui->text_owner = id;
    }
}

static size_t line_start(const char *s, size_t at) {
    while (at > 0 && s[at - 1] != '\n')
        at--;
    return at;
}

static size_t line_end(const char *s, size_t at) {
    while (s[at] && s[at] != '\n')
        at++;
    return at;
}

static bool is_word(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// A column on a line of length, or its end if the line is shorter.
static size_t column_in(size_t column, size_t length) {
    return column < length ? column : length;
}

// Text being edited, with the cursor at *at and the other end of what's selected
// at *anchor, the same when nothing is.
struct editing {
    char *s;
    size_t size, *at, *anchor;
    bool multiline;
};

static void selected_range(const struct editing *e, size_t *low, size_t *high) {
    *low = *e->at < *e->anchor ? *e->at : *e->anchor;
    *high = *e->at < *e->anchor ? *e->anchor : *e->at;
}

static bool delete_selection(struct editing *e) {
    size_t low, high;

    selected_range(e, &low, &high);
    if (low == high)
        return false;
    memmove(e->s + low, e->s + high, strlen(e->s) - high + 1);
    *e->at = *e->anchor = low;
    return true;
}

// Puts text in place of what's selected, as much as fits. One line takes line
// breaks as spaces, and what has no glyph shows as ?.
static bool insert(struct editing *e, const char *text, size_t n) {
    bool changed = delete_selection(e);
    size_t len = strlen(e->s), at = *e->at, room = e->size - 1 - len;

    n = n < room ? n : room;
    memmove(e->s + at + n, e->s + at, len - at + 1);
    for (size_t i = 0; i < n; i++) {
        char c = text[i];

        e->s[at + i] = c == '\n' ? (e->multiline ? '\n' : ' ') : c == '\t' ? ' ' : c >= ' ' && c < 127 ? c : '?';
    }
    *e->at = *e->anchor = at + n;
    return changed || n;
}

static void copy_selection(const struct editing *e) {
    size_t low, high;

    selected_range(e, &low, &high);
    if (low < high)
        gui_clipboard_set(e->s + low, high - low);
}

// Selects the word at, or the run of other characters there.
static void select_word(struct editing *e, size_t at) {
    size_t start = at, end = at;
    bool word = is_word(e->s[at]);

    while (start > 0 && is_word(e->s[start - 1]) == word && e->s[start - 1] != '\n')
        start--;
    while (e->s[end] && is_word(e->s[end]) == word && e->s[end] != '\n')
        end++;
    *e->anchor = start;
    *e->at = end;
}

static bool ctrl_key(const struct key_event *k, char letter) {
    return (k->modifiers & KEY_MOD_CTRL) && (k->text == (uint32_t)(letter & 0x1F) || k->text == (uint32_t)letter);
}

bool gui_shortcut(const struct gui_ui *ui, char letter) {
    bool shift = letter >= 'A' && letter <= 'Z';

    for (int i = 0; i < ui->key_count; i++) {
        const struct key_event *k = &ui->keys[i];

        if (ctrl_key(k, shift ? letter - 'A' + 'a' : letter) && !(k->modifiers & KEY_MOD_SHIFT) == !shift)
            return true;
    }
    return false;
}

bool gui_key(const struct gui_ui *ui, uint16_t code) {
    return key_pressed(ui, code);
}

bool gui_focused(const struct gui_ui *ui, const char *label) {
    return ui->focus == id_of(label);
}

void gui_set_focus(struct gui_ui *ui, const char *label, const char *text) {
    ui->focus = ui->reveal = id_of(label);
    if (text) {
        ui->text_owner = ui->focus;
        ui->text_anchor = 0;
        ui->text_cursor = strlen(text);
    }
}

// Editing that's the same on one line or many. Shift with the keys that move the
// cursor selects. Returns whether the text changed.
static bool edit(const struct key_event *k, struct editing *e) {
    char *s = e->s, typed = k->code == KEY_ENTER && e->multiline ? '\n' : (char)k->text;
    size_t len = strlen(s), at = *e->at, start = line_start(s, at), end = line_end(s, at), low, high;
    bool shift = k->modifiers & KEY_MOD_SHIFT, moved = true;

    selected_range(e, &low, &high);
    if (ctrl_key(k, 'a')) {
        *e->anchor = 0;
        *e->at = len;
        return false;
    }
    if (ctrl_key(k, 'c') || ctrl_key(k, 'x')) {
        copy_selection(e);
        return ctrl_key(k, 'x') && delete_selection(e);
    }
    if (ctrl_key(k, 'v')) {
        size_t n;
        char *pasted = gui_clipboard_get(&n);
        bool changed = pasted && insert(e, pasted, n);

        free(pasted);
        return changed;
    }

    // Left and Right on their own go to that end of what's selected.
    if ((k->code == KEY_LEFT || k->code == KEY_RIGHT) && !shift && low != high)
        at = k->code == KEY_LEFT ? low : high;
    else if (k->code == KEY_LEFT && at > 0)
        at--;
    else if (k->code == KEY_RIGHT && at < len)
        at++;
    else if (k->code == KEY_HOME)
        at = start;
    else if (k->code == KEY_END)
        at = end;
    else if (k->code == KEY_UP && e->multiline && start > 0)
        at = line_start(s, start - 1) + column_in(at - start, start - 1 - line_start(s, start - 1));
    else if (k->code == KEY_DOWN && e->multiline && s[end])
        at = end + 1 + column_in(at - start, line_end(s, end + 1) - end - 1);
    else
        moved = false;
    if (moved) {
        *e->at = at;
        if (!shift)
            *e->anchor = at;
        return false;
    }

    if ((k->code == KEY_BACKSPACE || k->code == KEY_DELETE) && low != high)
        return delete_selection(e);
    if (k->code == KEY_BACKSPACE && at > 0) {
        memmove(s + at - 1, s + at, len - at + 1);
        *e->at = *e->anchor = at - 1;
        return true;
    }
    if (k->code == KEY_DELETE && at < len) {
        memmove(s + at, s + at + 1, len - at);
        return true;
    }
    if (((typed >= ' ' && typed < 127) || (typed == '\n' && e->multiline)) && !(k->modifiers & (KEY_MOD_CTRL | KEY_MOD_ALT)))
        return insert(e, &typed, 1);
    return false;
}

// The mouse in text: a press puts the cursor there, or selects the word with a
// double click, and dragging selects from there. Returns whether it moved.
static bool click_text(struct gui_ui *ui, uint32_t id, struct editing *e, size_t at) {
    if (ui->active != id || !ui->mouse_down)
        return false;
    if (ui->mouse_pressed && ui->double_click)
        select_word(e, at);
    else if (ui->mouse_pressed)
        *e->at = *e->anchor = at;
    else
        *e->at = at;
    return true;
}

// The selected part of text drawn from x, from index from and count characters long.
static void draw_selection(struct gui_ui *ui, const struct editing *e, int x, int y, size_t from, size_t count) {
    size_t low, high;

    selected_range(e, &low, &high);
    low = low > from ? low : from;
    high = high < from + count ? high : from + count;
    if (low < high)
        fill(ui, x + (int)(low - from) * GUI_CHAR_WIDTH, y - 1, (int)(high - low) * GUI_CHAR_WIDTH, GUI_CHAR_HEIGHT + 2,
             SELECTION);
}

bool gui_textbox(struct gui_ui *ui, const char *label, char *s, size_t size) {
    uint32_t id = id_of(label);
    struct gui_rect frame;
    struct editing e = {s, size, &ui->text_cursor, &ui->text_anchor, false};
    bool changed = false;

    framed(ui, label, FRAME_HEIGHT, &frame);
    interact(ui, id, frame);
    if (over(ui, frame))
        ui->cursor = GUI_CURSOR_TEXT;

    bool focused = focusable(ui, id);
    int fits = (frame.width - 2 * PADDING) / GUI_CHAR_WIDTH;
    size_t len = strlen(s);

    // Long text scrolls to keep the cursor in sight.
    own_cursor(ui, id, len);

    size_t first = focused && (int)ui->text_cursor >= fits ? ui->text_cursor - fits + 1 : 0;
    long column = first + (ui->mouse_x - frame.x - PADDING + GUI_CHAR_WIDTH / 2) / GUI_CHAR_WIDTH;

    click_text(ui, id, &e, column < 0 ? 0 : (size_t)column > len ? len : (size_t)column);
    for (int i = 0; focused && i < ui->key_count; i++)
        changed |= edit(&ui->keys[i], &e);
    first = focused && (int)ui->text_cursor >= fits ? ui->text_cursor - fits + 1 : 0;

    fill(ui, frame.x, frame.y, frame.width, frame.height, ui->hot == id || focused ? FRAME_HOT : FRAME);
    if (focused)
        draw_selection(ui, &e, frame.x + PADDING, frame.y + PADDING, first, fits);
    draw_fitted(ui, frame.x + PADDING, frame.y + PADDING, frame.width - 2 * PADDING, s + first, 255, TEXT);
    if (focused) {
        int caret = frame.x + PADDING + (int)(ui->text_cursor - first) * GUI_CHAR_WIDTH;

        fill(ui, caret, frame.y + PADDING - 2, 1, GUI_CHAR_HEIGHT + 4, TEXT);
        draw_focus(ui, frame);
    } else if (!*s) {
        text(ui, frame.x + PADDING, frame.y + PADDING, "...", DIM_TEXT, 1);
    }
    return changed;
}

static struct gui_scroll *scroll_of(struct gui_ui *ui, uint32_t id) {
    struct gui_scroll *free_slot = NULL;

    for (int i = 0; i < GUI_UI_MAX_SCROLLS; i++) {
        if (ui->scrolls[i].id == id)
            return &ui->scrolls[i];
        if (!ui->scrolls[i].id && !free_slot)
            free_slot = &ui->scrolls[i];
    }
    if (!free_slot)
        free_slot = &ui->scrolls[id % GUI_UI_MAX_SCROLLS];
    *free_slot = (struct gui_scroll){.id = id};
    return free_slot;
}

static void clamp_scroll(struct gui_scroll *s, int view) {
    int most = s->content > view ? s->content - view : 0;

    s->offset = s->offset < 0 ? 0 : s->offset > most ? most : s->offset;
}

// The wheel and a scroll bar on the right of view, for content s->content tall.
// Returns what's left of view for the content.
static struct gui_rect scrolling(struct gui_ui *ui, uint32_t id, struct gui_scroll *s, struct gui_rect view) {
    uint32_t bar_id = id ^ 0x9E3779B9u;

    // Inner ones come later, so win, unless there's nothing for them to scroll.
    if (over(ui, view) && s->content > view.height)
        ui->next_wheel_owner = id;
    if (ui->wheel && ui->wheel_owner == id)
        s->offset += ui->wheel * WHEEL_STEP;
    clamp_scroll(s, view.height);
    if (s->content <= view.height)
        return view;

    struct gui_rect track = {view.x + view.width - SCROLLBAR, view.y, SCROLLBAR, view.height};
    int thumb_height = view.height * view.height / s->content, most = s->content - view.height;

    thumb_height = thumb_height < MIN_THUMB ? MIN_THUMB : thumb_height;

    int room_left = track.height - thumb_height;
    struct gui_rect thumb = {track.x, track.y + (int)((long)s->offset * room_left / most), SCROLLBAR, thumb_height};

    // Pressing on the thumb drags it, and on the track either side moves a page.
    if (over(ui, track) && ui->mouse_pressed) {
        bool on_thumb = contains(thumb, ui->mouse_x, ui->mouse_y);

        ui->active = bar_id;
        ui->drag_offset = on_thumb ? ui->mouse_y - thumb.y : -1;
        if (!on_thumb)
            s->offset += ui->mouse_y < thumb.y ? -view.height : view.height;
    }
    if (ui->active == bar_id && ui->mouse_down && ui->drag_offset >= 0 && room_left > 0)
        s->offset = (int)((long)(ui->mouse_y - ui->drag_offset - track.y) * most / room_left);
    clamp_scroll(s, view.height);
    thumb.y = track.y + (int)((long)s->offset * room_left / most);

    fill(ui, track.x, track.y, track.width, track.height, PANEL);
    fill(ui, thumb.x + 2, thumb.y + 2, thumb.width - 4, thumb.height - 4, ui->active == bar_id ? ACCENT : THUMB);
    view.width -= SCROLLBAR;
    return view;
}

static void push_clip(struct gui_ui *ui, struct gui_rect r, struct gui_rect *was) {
    *was = ui->layout.clip;
    ui->layout.clip = intersect(ui->layout.clip, r);
}

// Which line and column a point in text is at, for clicks.
static size_t index_at(const char *s, int line, int column) {
    size_t at = 0;

    for (; line > 0 && s[line_end(s, at)]; line--)
        at = line_end(s, at) + 1;

    size_t end = line_end(s, at);

    return at + (column < 0 ? 0 : (size_t)column < end - at ? (size_t)column : end - at);
}

bool gui_textarea(struct gui_ui *ui, const char *label, char *s, size_t size, int lines) {
    uint32_t id = id_of(label);
    struct gui_scroll *scroll = scroll_of(ui, id);
    struct gui_rect frame, was;
    struct editing e = {s, size, &ui->text_cursor, &ui->text_anchor, true};
    bool changed = false, moved = false;
    int line_count = 1;

    framed(ui, label, lines > 0 ? lines * GUI_CHAR_HEIGHT + 2 * PADDING : height_left(ui, FRAME_HEIGHT), &frame);
    for (const char *p = s; *p; p++)
        line_count += *p == '\n';
    scroll->content = line_count * GUI_CHAR_HEIGHT + 2 * PADDING;
    interact(ui, id, frame);

    bool focused = focusable(ui, id);

    fill(ui, frame.x, frame.y, frame.width, frame.height, ui->hot == id || focused ? FRAME_HOT : FRAME);

    struct gui_rect inner = scrolling(ui, id, scroll, frame);
    int fits = (inner.width - 2 * PADDING) / GUI_CHAR_WIDTH;

    if (over(ui, inner))
        ui->cursor = GUI_CURSOR_TEXT;
    own_cursor(ui, id, strlen(s));

    // Long lines scroll sideways, all together, to keep the cursor in sight.
    size_t column = ui->text_cursor - line_start(s, ui->text_cursor);
    int first = focused && (int)column >= fits ? (int)column - fits + 1 : 0;

    // Dragging past the top or bottom scrolls, as the cursor goes out of sight.
    int line = (ui->mouse_y - inner.y - PADDING + scroll->offset) / GUI_CHAR_HEIGHT;

    if (ui->mouse_y < inner.y + PADDING)
        line = (ui->mouse_y - inner.y - PADDING + scroll->offset - GUI_CHAR_HEIGHT + 1) / GUI_CHAR_HEIGHT;
    if (!(ui->mouse_pressed && !over(ui, inner)))
        moved = click_text(ui, id, &e,
                           index_at(s, line, first + (ui->mouse_x - inner.x - PADDING + GUI_CHAR_WIDTH / 2) / GUI_CHAR_WIDTH));
    for (int i = 0; focused && i < ui->key_count; i++) {
        changed |= edit(&ui->keys[i], &e);
        moved = true;
    }

    // And down, when it moves with the keyboard.
    int cursor_line = 0;

    for (size_t i = 0; i < ui->text_cursor; i++)
        cursor_line += s[i] == '\n';
    if (changed) {
        line_count = 1;
        for (const char *p = s; *p; p++)
            line_count += *p == '\n';
        scroll->content = line_count * GUI_CHAR_HEIGHT + 2 * PADDING;
    }
    if (moved) {
        int top = cursor_line * GUI_CHAR_HEIGHT, bottom = top + GUI_CHAR_HEIGHT + 2 * PADDING;

        scroll->offset = top < scroll->offset ? top : bottom > scroll->offset + inner.height ? bottom - inner.height
                                                                                              : scroll->offset;
        clamp_scroll(scroll, inner.height);
        column = ui->text_cursor - line_start(s, ui->text_cursor);
        first = (int)column >= fits ? (int)column - fits + 1 : 0;
    }

    push_clip(ui, inner, &was);
    size_t at = 0;

    for (int line = 0; line < line_count; line++) {
        int y = inner.y + PADDING + line * GUI_CHAR_HEIGHT - scroll->offset;
        size_t end = line_end(s, at);

        bool seen = y + GUI_CHAR_HEIGHT > inner.y && y < inner.y + inner.height;

        // A selected line break shows as a space at the end of its line.
        if (seen && focused && (int)(end - at) + 1 > first)
            draw_selection(ui, &e, inner.x + PADDING, y, at + first, end - at + 1 - first);
        if (seen && (int)(end - at) > first)
            draw_fitted(ui, inner.x + PADDING, y, fits * GUI_CHAR_WIDTH, s + at + first, (int)(end - at) - first, TEXT);
        at = end + 1;
    }
    if (focused) {
        int x = inner.x + PADDING + ((int)column - first) * GUI_CHAR_WIDTH;
        int y = inner.y + PADDING + cursor_line * GUI_CHAR_HEIGHT - scroll->offset;

        fill(ui, x, y - 1, 1, GUI_CHAR_HEIGHT + 2, TEXT);
    } else if (!*s) {
        text(ui, inner.x + PADDING, inner.y + PADDING, "...", DIM_TEXT, 1);
    }
    ui->layout.clip = was;
    if (focused)
        draw_focus(ui, frame);
    return changed;
}

static bool menu_key(const struct gui_ui *ui, uint16_t code) {
    for (int i = 0; i < ui->menu_key_count; i++) {
        if (ui->menu_keys[i].code == code)
            return true;
    }
    return false;
}

// Popups are drawn as they come, then kept and put back at the end of the frame,
// so nothing after covers them. Their items fill their own rows.
static void popup_begin(struct gui_ui *ui, struct gui_rect r) {
    struct gui_rect all = whole(ui);

    r.x = r.x + r.width > all.width ? all.width - r.width : r.x;
    r.x = r.x < 0 ? 0 : r.x;
    ui->popup_drawn = ui->in_popup = true;
    ui->popup_rect = r;
    ui->outside_popup = ui->layout.clip;
    ui->layout.clip = all;
    ui->item_y = r.y + 4;
    ui->item_width = r.width;
    ui->item_index = 0;
    fill(ui, r.x, r.y, r.width, 4, POPUP);
}

// The items end at bottom.
static void popup_end(struct gui_ui *ui, int bottom) {
    struct gui_rect *r = &ui->popup_rect;
    struct gui_canvas *w = &ui->window->canvas;

    ui->layout.clip = whole(ui);
    fill(ui, r->x, bottom, r->width, 4, POPUP);
    r->height = bottom + 4 - r->y;
    draw_border(ui, *r, SEPARATOR);
    *r = intersect(*r, whole(ui));
    free(ui->popup_pixels);
    ui->popup_pixels = malloc((size_t)r->width * r->height * sizeof(uint32_t));
    for (int row = 0; ui->popup_pixels && row < r->height; row++)
        memcpy(ui->popup_pixels + (long)row * r->width, w->pixels + (long)(r->y + row) * w->stride + r->x,
               r->width * sizeof(uint32_t));
    ui->layout.clip = ui->outside_popup;
    ui->in_popup = false;
}

// A row in a popup, highlighted when the mouse or the keyboard is on it, or
// marked. Returns whether it was chosen: let go over, or Enter on it.
static bool popup_item(struct gui_ui *ui, const char *label, bool keyed, bool marked) {
    struct gui_rect r = {ui->popup_rect.x, ui->item_y, ui->item_width, ROW_HEIGHT};
    bool is_over = over(ui, r);
    bool chosen = (is_over && ui->mouse_released) || (keyed && ui->menu_key_count && menu_key(ui, KEY_ENTER));

    ui->item_y += ROW_HEIGHT;
    ui->item_index++;
    fill(ui, r.x, r.y, r.width, r.height, is_over || keyed ? ACCENT : marked ? FRAME_HOT : POPUP);
    draw_fitted(ui, r.x + 8, r.y + (ROW_HEIGHT - GUI_CHAR_HEIGHT) / 2, r.width - 16, label, shown_length(label),
                is_over || keyed ? ON_ACCENT : TEXT);

    // What was drawn before it may show what it changes, so the frame goes again.
    ui->again |= chosen;
    return chosen;
}

static void toggle_popup(struct gui_ui *ui, uint32_t id, bool menu) {
    ui->popup = ui->popup == id ? 0 : id;
    ui->popup_is_menu = menu;
}

// Under the frame, or over it if there's more room there, as many rows as fit up to
// DROPDOWN_ROWS, scrolling to show the selected one when it opens or changes.
static void dropdown_popup(struct gui_ui *ui, uint32_t id, struct gui_rect frame, const char *const *items, int count,
                           int *selected, bool opened, bool moved) {
    uint32_t list_id = id ^ 0x85EBCA6Bu;
    struct gui_scroll *scroll = scroll_of(ui, list_id);
    int below = whole(ui).height - frame.y - frame.height, above = frame.y;
    int fit = ((below > above ? below : above) - 8) / ROW_HEIGHT;
    int rows = count < DROPDOWN_ROWS ? count : DROPDOWN_ROWS;

    rows = rows < fit ? rows : fit > 1 ? fit : 1;

    int height = rows * ROW_HEIGHT + 8, top = *selected * ROW_HEIGHT;
    bool under = height <= below || below >= above;
    struct gui_rect view;

    scroll->content = count * ROW_HEIGHT;
    if (opened)
        scroll->offset = top - rows / 2 * ROW_HEIGHT;
    else if (moved)
        scroll->offset = top < scroll->offset ? top : top + ROW_HEIGHT > scroll->offset + rows * ROW_HEIGHT
                                                          ? top + ROW_HEIGHT - rows * ROW_HEIGHT
                                                          : scroll->offset;
    popup_begin(ui, (struct gui_rect){frame.x, under ? frame.y + frame.height : frame.y - height, frame.width, height});
    view = (struct gui_rect){ui->popup_rect.x, ui->popup_rect.y + 4, frame.width, rows * ROW_HEIGHT};

    struct gui_rect inner = scrolling(ui, list_id, scroll, view);

    ui->layout.clip = inner;
    ui->item_y = inner.y - scroll->offset;
    ui->item_width = inner.width;
    for (int i = 0; i < count; i++) {
        if (popup_item(ui, items[i], false, i == *selected)) {
            *selected = i;
            ui->popup = 0;
        }
    }
    popup_end(ui, view.y + view.height);
}

bool gui_dropdown(struct gui_ui *ui, const char *label, const char *const *items, int count, int *selected) {
    uint32_t id = id_of(label);
    struct gui_rect frame;
    int before = *selected;
    bool was_open = ui->popup == id, moved = false;

    framed(ui, label, FRAME_HEIGHT, &frame);

    bool is_over = over(ui, frame);

    if (is_over)
        ui->hot = id;
    if (is_over && ui->mouse_pressed) {
        toggle_popup(ui, id, false);
        ui->focus = id;
    }

    bool focused = focusable(ui, id);

    for (int i = 0; focused && i < ui->key_count; i++) {
        uint16_t code = ui->keys[i].code;

        if (code == KEY_UP || code == KEY_DOWN) {
            *selected += code == KEY_DOWN ? 1 : -1;
            moved = true;
        } else if (code == KEY_ENTER || code == KEY_SPACE) {
            toggle_popup(ui, id, false);
        }
    }
    *selected = *selected >= count ? count - 1 : *selected < 0 ? 0 : *selected;

    fill(ui, frame.x, frame.y, frame.width, frame.height, ui->hot == id || ui->popup == id ? FRAME_HOT : FRAME);
    if (count)
        draw_fitted(ui, frame.x + PADDING, frame.y + PADDING, frame.width - 2 * PADDING - 16, items[*selected], 255, TEXT);
    text(ui, frame.x + frame.width - PADDING - GUI_CHAR_WIDTH, frame.y + PADDING, "v", DIM_TEXT, 1);
    if (focused)
        draw_focus(ui, frame);

    if (ui->popup == id)
        dropdown_popup(ui, id, frame, items, count, selected, !was_open, moved);
    return *selected != before;
}

int gui_list(struct gui_ui *ui, const char *label, const char *const *items, int count, int *selected, int rows) {
    return gui_list_icons(ui, label, items, NULL, count, selected, rows);
}

int gui_list_icons(struct gui_ui *ui, const char *label, const char *const *items, const struct gui_image *const *icons,
                   int count, int *selected, int rows) {
    uint32_t id = id_of(label);
    struct gui_scroll *scroll = scroll_of(ui, id);
    struct gui_rect frame, was;
    int before = *selected;
    bool activated = false;

    framed(ui, label, rows > 0 ? rows * ROW_HEIGHT + 4 : height_left(ui, ROW_HEIGHT + 4), &frame);
    rows = (frame.height - 4) / ROW_HEIGHT;
    scroll->content = count * ROW_HEIGHT + 4;
    if (over(ui, frame) && (ui->mouse_pressed || ui->right_pressed)) {
        ui->focus = id;
        ui->clicked_item = -1;
    }

    bool focused = focusable(ui, id);
    bool moved = false;

    for (int i = 0; focused && i < ui->key_count; i++) {
        uint16_t code = ui->keys[i].code;
        int page = rows - 1 > 1 ? rows - 1 : 1;

        moved = true;
        if (code == KEY_UP || code == KEY_DOWN)
            *selected += code == KEY_DOWN ? 1 : -1;
        else if (code == KEY_PAGE_UP || code == KEY_PAGE_DOWN)
            *selected += code == KEY_PAGE_DOWN ? page : -page;
        else if (code == KEY_HOME || code == KEY_END)
            *selected = code == KEY_HOME ? 0 : count - 1;
        else
            moved = false;
        activated |= code == KEY_ENTER && count;
    }
    *selected = *selected >= count ? count - 1 : *selected < 0 ? 0 : *selected;

    fill(ui, frame.x, frame.y, frame.width, frame.height, PANEL);

    // Choosing with the keyboard scrolls to keep the choice in sight.
    if (moved) {
        int top = *selected * ROW_HEIGHT, bottom = top + ROW_HEIGHT + 4;

        scroll->offset = top < scroll->offset ? top : bottom > scroll->offset + frame.height ? bottom - frame.height
                                                                                              : scroll->offset;
    }

    struct gui_rect inner = scrolling(ui, id, scroll, frame);

    push_clip(ui, inner, &was);
    for (int i = scroll->offset / ROW_HEIGHT; i < count; i++) {
        struct gui_rect r = {inner.x + 2, inner.y + 2 + i * ROW_HEIGHT - scroll->offset, inner.width - 4, ROW_HEIGHT};

        if (r.y >= inner.y + inner.height)
            break;
        if (over(ui, r) && (ui->mouse_pressed || ui->right_pressed)) {
            activated |= ui->mouse_pressed && ui->double_click && i == *selected;
            *selected = ui->clicked_item = i;
        }
        int text_x = icons ? 12 + GUI_ICON_SIZE : 6;

        if (i == *selected || over(ui, r))
            fill(ui, r.x, r.y, r.width, r.height, i == *selected ? ACCENT : FRAME_HOT);
        if (icons && icons[i])
            draw_image(ui, r.x + 5, r.y + (ROW_HEIGHT - GUI_ICON_SIZE) / 2, icons[i]);
        draw_fitted(ui, r.x + text_x, r.y + (ROW_HEIGHT - GUI_CHAR_HEIGHT) / 2, r.width - text_x - 6, items[i], 255,
                    i == *selected ? ON_ACCENT : TEXT);
    }
    ui->layout.clip = was;
    if (focused)
        draw_focus(ui, frame);
    return activated ? GUI_LIST_ACTIVATED : *selected != before ? GUI_LIST_CHANGED : 0;
}

// Up and Down go through the open menu's items, and Left and Right to the menus
// beside it. One to the left has been drawn already, so the frame goes again.
static void menu_keys(struct gui_ui *ui, uint32_t id) {
    int at = -1;

    for (int i = 0; i < ui->menu_count; i++) {
        if (ui->menus[i] == id)
            at = i;
    }
    for (int i = 0; i < ui->menu_key_count; i++) {
        uint16_t code = ui->menu_keys[i].code;
        int items = ui->menu_items > 0 ? ui->menu_items : 1;

        if (code == KEY_DOWN)
            ui->menu_choice = (ui->menu_choice + 1) % items;
        else if (code == KEY_UP)
            ui->menu_choice = ui->menu_choice <= 0 ? items - 1 : ui->menu_choice - 1;
        if ((code == KEY_LEFT || code == KEY_RIGHT) && at >= 0) {
            at = (at + (code == KEY_RIGHT ? 1 : ui->menu_count - 1)) % ui->menu_count;
            ui->popup = ui->menus[at];
            ui->menu_choice = 0;
            ui->menu_key_count = 0; // used up, not for the menu it goes to
            ui->again = true;
            return;
        }
    }
}

void gui_menu_bar(struct gui_ui *ui) {
    int width = whole(ui).width;

    fill(ui, 0, 0, width, FRAME_HEIGHT, FRAME);
    fill(ui, 0, FRAME_HEIGHT, width, 1, SEPARATOR);
    ui->bar_x = 4;
    ui->layout.y = FRAME_HEIGHT + 1 + MARGIN;
}

bool gui_menu(struct gui_ui *ui, const char *label) {
    uint32_t id = id_of(label);
    struct gui_rect r = {ui->bar_x, 0, shown_length(label) * GUI_CHAR_WIDTH + 2 * (PADDING + 3), FRAME_HEIGHT};
    bool is_over = over(ui, r);

    ui->bar_x += r.width;
    if (ui->next_menu_count < GUI_UI_MAX_MENUS)
        ui->next_menus[ui->next_menu_count++] = id;

    // Opens on a press, and while one menu is open, the mouse moving over another opens that.
    if (is_over && ui->mouse_pressed) {
        toggle_popup(ui, id, true);
        ui->menu_choice = -1;
    } else if (is_over && ui->popup && ui->popup_is_menu && ui->popup != id) {
        ui->popup = id;
        ui->menu_choice = -1;
    }
    if (ui->popup == id)
        menu_keys(ui, id);
    fill(ui, r.x, r.y, r.width, r.height, ui->popup == id ? ACCENT : is_over ? FRAME_HOT : FRAME);
    draw_label(ui, r.x + PADDING + 3, r.y + PADDING, label, ui->popup == id ? ON_ACCENT : TEXT);
    if (ui->popup != id)
        return false;
    popup_begin(ui, (struct gui_rect){r.x, r.y + r.height, MENU_WIDTH, 0});
    return true;
}

bool gui_menu_item(struct gui_ui *ui, const char *label) {
    struct gui_rect r = {ui->popup_rect.x, ui->item_y, ui->item_width, ROW_HEIGHT};
    int index = ui->item_index;

    // The keyboard goes on from wherever the mouse was.
    if (over(ui, r))
        ui->menu_choice = index;

    bool chosen = popup_item(ui, label, index == ui->menu_choice, false);

    if (chosen)
        ui->popup = 0;
    return chosen;
}

void gui_menu_separator(struct gui_ui *ui) {
    struct gui_rect r = {ui->popup_rect.x, ui->item_y, ui->item_width, SEPARATOR_HEIGHT};

    fill(ui, r.x, r.y, r.width, r.height, POPUP);
    fill(ui, r.x + 8, r.y + SEPARATOR_HEIGHT / 2, r.width - 16, 1, SEPARATOR);
    ui->item_y += SEPARATOR_HEIGHT;
}

void gui_menu_end(struct gui_ui *ui) {
    ui->menu_items = ui->item_index;
    ui->menu_height = ui->item_y + 4 - ui->popup_rect.y;
    popup_end(ui, ui->item_y);
}

bool gui_context_menu(struct gui_ui *ui, const char *label, struct gui_rect area) {
    uint32_t id = id_of(label);

    if (ui->right_pressed && over(ui, area)) {
        ui->popup = id;
        ui->popup_is_menu = true;
        ui->menu_choice = -1;
        ui->menu_x = ui->mouse_x;
        ui->menu_y = ui->mouse_y;
        ui->menu_height = 0;
        ui->right_pressed = false; // one menu for each click
        ui->again = true;          // once more, to know how tall it is
    }
    if (ui->popup != id)
        return false;
    menu_keys(ui, id);

    // Under the mouse, or over it if there isn't room.
    int y = ui->menu_y + ui->menu_height > whole(ui).height ? ui->menu_y - ui->menu_height : ui->menu_y;

    popup_begin(ui, (struct gui_rect){ui->menu_x, y < 0 ? 0 : y, MENU_WIDTH, 0});
    return true;
}

int gui_tabs(struct gui_ui *ui, const char *const *names, int count, int *active, int *closing) {
    int width = whole(ui).width, plus = closing ? FRAME_HEIGHT : 0, result = 0;
    int tab = count ? (width - 8 - (closing ? 4 + plus : 0)) / count : TAB_WIDTH;
    uint32_t id = id_of("##tabs");

    tab = tab < TAB_WIDTH ? tab : TAB_WIDTH;
    fill(ui, 0, 0, width, TAB_BAR, FRAME);
    fill(ui, 0, TAB_BAR, width, 1, SEPARATOR);
    for (int i = 0; i < count; i++) {
        struct gui_rect r = {4 + i * tab, 4, tab - 2, TAB_BAR - 4};
        struct gui_rect x = {r.x + r.width - TAB_CLOSE - 4, r.y + (r.height - TAB_CLOSE) / 2, closing ? TAB_CLOSE : 0,
                             TAB_CLOSE};
        bool on_close = closing && over(ui, x), on_tab = over(ui, r) && !on_close;

        if (closing && interact(ui, id + 2 * i + 1, x)) {
            *closing = i;
            result = GUI_TABS_CLOSE;
        }
        if (on_tab && ui->mouse_pressed && *active != i) {
            *active = i;
            result = GUI_TABS_CHANGED;
        }
        if (i == *active) {
            fill(ui, r.x, r.y, r.width, r.height + 1, BACKGROUND);
            fill(ui, r.x, r.y, r.width, 2, ACCENT);
        } else {
            fill(ui, r.x, r.y, r.width, r.height, on_tab ? FRAME_HOT : FRAME);
            fill(ui, r.x + r.width + 1, r.y + 4, 1, r.height - 8, SEPARATOR);
        }
        draw_fitted(ui, r.x + 8, r.y + (r.height - GUI_CHAR_HEIGHT) / 2 + 1, r.width - 16 - x.width, names[i], 255,
                    i == *active ? TEXT : DIM_TEXT);
        if (on_close)
            fill(ui, x.x, x.y, x.width, x.height, i == *active ? FRAME : SEPARATOR);
        if (closing)
            text(ui, x.x + (TAB_CLOSE - GUI_CHAR_WIDTH) / 2, x.y, "x", on_close ? TEXT : DIM_TEXT, 1);
    }

    struct gui_rect add = {4 + count * tab + 2, 4 + (TAB_BAR - plus) / 2, plus, plus - 4};

    if (closing && interact(ui, id, add))
        result = GUI_TABS_NEW;
    if (closing && ui->hot == id)
        fill(ui, add.x, add.y, add.width, add.height, FRAME_HOT);
    if (closing)
        text(ui, add.x + (plus - GUI_CHAR_WIDTH) / 2, add.y + (add.height - GUI_CHAR_HEIGHT) / 2, "+", TEXT, 1);

    for (int i = 0; i < ui->key_count; i++) {
        const struct key_event *k = &ui->keys[i];

        if (k->code == KEY_TAB && (k->modifiers & KEY_MOD_CTRL) && count) {
            *active = (*active + (k->modifiers & KEY_MOD_SHIFT ? count - 1 : 1)) % count;
            result = GUI_TABS_CHANGED;
        }
    }
    if (closing && gui_shortcut(ui, 't'))
        result = GUI_TABS_NEW;
    if (closing && gui_shortcut(ui, 'w') && count) {
        *closing = *active;
        result = GUI_TABS_CLOSE;
    }
    ui->layout.y = TAB_BAR + 1 + MARGIN;
    ui->again |= result != 0; // what's under the tabs is another folder now
    return result;
}

void gui_scroll_begin(struct gui_ui *ui, const char *label, int height) {
    uint32_t id = id_of(label);
    struct gui_scroll *scroll = scroll_of(ui, id);
    struct gui_rect area = place(ui, room(ui), height > 0 ? height : height_left(ui, FRAME_HEIGHT));

    fill(ui, area.x, area.y, area.width, area.height, PANEL);

    struct gui_rect inner = scrolling(ui, id, scroll, area);

    if (ui->depth == GUI_UI_MAX_DEPTH)
        return; // too deep: what's in it goes on as if it weren't there
    ui->saved[ui->depth].layout = ui->layout;
    ui->saved[ui->depth].area = area;
    ui->saved[ui->depth].view = inner;
    ui->saved[ui->depth].id = id;
    ui->saved[ui->depth].top = inner.y - scroll->offset;
    ui->depth++;
    ui->layout = (struct gui_layout){
        .left = inner.x + PADDING, .right = inner.x + inner.width - PADDING,
        .x = inner.x + PADDING, .y = inner.y + PADDING - scroll->offset,
        .clip = intersect(ui->layout.clip, inner),
    };
}

void gui_scroll_end(struct gui_ui *ui) {
    if (ui->depth == 0)
        return;
    ui->depth--;

    struct gui_scroll *scroll = scroll_of(ui, ui->saved[ui->depth].id);
    int bottom = ui->layout.y + ui->layout.line_height + PADDING;

    scroll->content = bottom - ui->saved[ui->depth].top;
    ui->layout = ui->saved[ui->depth].layout;
    ui->last = ui->saved[ui->depth].area;
}
