#include <gui.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

#define BACKGROUND 0x1E2129
#define PANEL 0x181B21 // behind scrolling areas and lists
#define POPUP 0x2B303B
#define TEXT 0xE6E8EE
#define DIM_TEXT 0x8A93A6
#define FRAME 0x2B303B
#define FRAME_HOT 0x363D4B
#define ACCENT 0x3D63DD
#define ACCENT_HOT 0x5578E8
#define FOCUS_RING 0x8FA8F5
#define SEPARATOR 0x3A4150
#define THUMB 0x4A5263

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

void gui_ui_init(struct gui_ui *ui, struct gui_window *window) {
    *ui = (struct gui_ui){.window = window, .window_focused = true, .mouse_x = -1, .mouse_y = -1};
}

bool gui_ui_event(struct gui_ui *ui, const struct gui_message *e) {
    switch (e->type) {
    case GUI_MOUSE_MOVE:
    case GUI_MOUSE_BUTTON:
    case GUI_MOUSE_WHEEL:
        ui->mouse_x = e->x;
        ui->mouse_y = e->y;
        if (e->type == GUI_MOUSE_BUTTON && (e->changed & 1)) {
            ui->mouse_down = e->buttons & 1;
            ui->mouse_pressed |= ui->mouse_down;
            ui->mouse_released |= !ui->mouse_down;
        }
        if (e->type == GUI_MOUSE_WHEEL)
            ui->wheel += e->wheel;
        return true;
    case GUI_KEY:
        if (e->key.pressed && ui->key_count < GUI_UI_MAX_KEYS)
            ui->keys[ui->key_count++] = e->key;
        return e->key.pressed;
    case GUI_FOCUS:
        ui->window_focused = e->focused;
        return true;
    case GUI_RESIZE:
        return gui_resize(ui->window, e->width, e->height);
    case GUI_REDRAW:
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
    ui->next_count = 0;
    ui->next_wheel_owner = 0;
    ui->popup_drawn = false;

    // An open popup has the mouse when it's over it. A click anywhere else, or
    // Escape, closes it, and does nothing more.
    ui->mouse_in_popup = ui->popup && contains(ui->popup_rect, ui->mouse_x, ui->mouse_y);
    if (ui->popup && ((ui->mouse_pressed && !ui->mouse_in_popup) || key_pressed(ui, KEY_ESCAPE))) {
        ui->popup = 0;
        ui->mouse_pressed = false;
    }

    // Clicking where there's no widget takes the keyboard away, and one there gives it back.
    if (ui->mouse_pressed && !ui->mouse_in_popup)
        ui->focus = 0;

    // Tab goes to the next widget that can have the keyboard, and Shift+Tab back,
    // in the order they came last time, going round at the ends.
    for (int i = 0; i < ui->key_count && ui->order_count; i++) {
        int at = -1;

        if (ui->keys[i].code != KEY_TAB)
            continue;
        for (int j = 0; j < ui->order_count; j++) {
            if (ui->order[j] == ui->focus)
                at = j;
        }

        bool back = ui->keys[i].modifiers & KEY_MOD_SHIFT;

        at = at < 0 ? (back ? ui->order_count - 1 : 0) : (at + (back ? ui->order_count - 1 : 1)) % ui->order_count;
        ui->focus = ui->order[at];
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
    if (!ui->popup_drawn)
        ui->popup = 0;

    memcpy(ui->order, ui->next_order, ui->next_count * sizeof(*ui->order));
    ui->order_count = ui->next_count;
    ui->wheel_owner = ui->next_wheel_owner;
    if (ui->mouse_released)
        ui->active = 0;
    ui->mouse_pressed = ui->mouse_released = false;
    ui->wheel = 0;
    ui->key_count = 0;
    gui_show(ui->window);
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

// Takes part in Tab's order, returning whether it has the keyboard.
static bool focusable(struct gui_ui *ui, uint32_t id) {
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
    fill(ui, r.x, r.y, r.width, r.height, ui->active == id && ui->hot == id ? ACCENT : ui->hot == id ? FRAME_HOT : FRAME);
    draw_label(ui, r.x + PADDING + 2, r.y + PADDING, label, TEXT);
    if (focused)
        draw_focus(ui, r);
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

// The cursor starts at the end when a text widget gets the keyboard.
static void own_cursor(struct gui_ui *ui, uint32_t id, size_t len) {
    if (ui->focus == id && (ui->text_owner != id || ui->text_cursor > len)) {
        ui->text_cursor = len;
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

// Editing that's the same on one line or many. Returns whether the text changed.
static bool edit(const struct key_event *k, char *s, size_t size, size_t *at, bool multiline) {
    size_t len = strlen(s), start = line_start(s, *at), end = line_end(s, *at);
    char typed = k->code == KEY_ENTER && multiline ? '\n' : (char)k->text;

    if (k->code == KEY_LEFT && *at > 0) {
        (*at)--;
    } else if (k->code == KEY_RIGHT && *at < len) {
        (*at)++;
    } else if (k->code == KEY_HOME) {
        *at = start;
    } else if (k->code == KEY_END) {
        *at = end;
    } else if (k->code == KEY_UP && multiline && start > 0) {
        size_t above = line_start(s, start - 1);

        *at = above + (*at - start < start - 1 - above ? *at - start : start - 1 - above);
    } else if (k->code == KEY_DOWN && multiline && s[end]) {
        size_t below = end + 1, below_end = line_end(s, below);

        *at = below + (*at - start < below_end - below ? *at - start : below_end - below);
    } else if (k->code == KEY_BACKSPACE && *at > 0) {
        memmove(s + *at - 1, s + *at, len - *at + 1);
        (*at)--;
        return true;
    } else if (k->code == KEY_DELETE && *at < len) {
        memmove(s + *at, s + *at + 1, len - *at);
        return true;
    } else if (((typed >= ' ' && typed < 127) || typed == '\n') && !(k->modifiers & (KEY_MOD_CTRL | KEY_MOD_ALT))
               && len + 1 < size) {
        memmove(s + *at + 1, s + *at, len - *at + 1);
        s[(*at)++] = typed;
        return true;
    }
    return false;
}

bool gui_textbox(struct gui_ui *ui, const char *label, char *s, size_t size) {
    uint32_t id = id_of(label);
    struct gui_rect frame;
    bool changed = false;

    framed(ui, label, FRAME_HEIGHT, &frame);
    interact(ui, id, frame);

    bool focused = focusable(ui, id);

    own_cursor(ui, id, strlen(s));
    for (int i = 0; focused && i < ui->key_count; i++)
        changed |= edit(&ui->keys[i], s, size, &ui->text_cursor, false);

    // Long text scrolls to keep the cursor in sight.
    int fits = (frame.width - 2 * PADDING) / GUI_CHAR_WIDTH;
    size_t first = focused && (int)ui->text_cursor >= fits ? ui->text_cursor - fits + 1 : 0;

    fill(ui, frame.x, frame.y, frame.width, frame.height, ui->hot == id || focused ? FRAME_HOT : FRAME);
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
    bool changed = false, moved = false;
    int line_count = 1;

    framed(ui, label, lines * GUI_CHAR_HEIGHT + 2 * PADDING, &frame);
    for (const char *p = s; *p; p++)
        line_count += *p == '\n';
    scroll->content = line_count * GUI_CHAR_HEIGHT + 2 * PADDING;
    interact(ui, id, frame);

    bool focused = focusable(ui, id);

    fill(ui, frame.x, frame.y, frame.width, frame.height, ui->hot == id || focused ? FRAME_HOT : FRAME);

    struct gui_rect inner = scrolling(ui, id, scroll, frame);
    int fits = (inner.width - 2 * PADDING) / GUI_CHAR_WIDTH;

    own_cursor(ui, id, strlen(s));

    // Long lines scroll sideways, all together, to keep the cursor in sight.
    size_t column = ui->text_cursor - line_start(s, ui->text_cursor);
    int first = focused && (int)column >= fits ? (int)column - fits + 1 : 0;

    if (ui->active == id && ui->mouse_pressed && over(ui, inner)) {
        int line = (ui->mouse_y - inner.y - PADDING + scroll->offset) / GUI_CHAR_HEIGHT;

        ui->text_cursor = index_at(s, line, first + (ui->mouse_x - inner.x - PADDING + GUI_CHAR_WIDTH / 2) / GUI_CHAR_WIDTH);
    }
    for (int i = 0; focused && i < ui->key_count; i++) {
        changed |= edit(&ui->keys[i], s, size, &ui->text_cursor, true);
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

        if (y + GUI_CHAR_HEIGHT > inner.y && y < inner.y + inner.height && (int)(end - at) > first)
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
    fill(ui, r.x, r.y, r.width, 4, POPUP);
}

static void popup_end(struct gui_ui *ui) {
    struct gui_rect *r = &ui->popup_rect;
    struct gui_canvas *w = &ui->window->canvas;

    fill(ui, r->x, ui->item_y, r->width, 4, POPUP);
    r->height = ui->item_y + 4 - r->y;
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

// A row in a popup, returning whether it was chosen: let go over.
static bool popup_item(struct gui_ui *ui, const char *label, bool marked) {
    struct gui_rect r = {ui->popup_rect.x, ui->item_y, ui->popup_rect.width, ROW_HEIGHT};
    bool is_over = over(ui, r);

    ui->item_y += ROW_HEIGHT;
    fill(ui, r.x, r.y, r.width, r.height, is_over ? ACCENT : marked ? FRAME_HOT : POPUP);
    draw_fitted(ui, r.x + 8, r.y + (ROW_HEIGHT - GUI_CHAR_HEIGHT) / 2, r.width - 16, label, shown_length(label), TEXT);

    // What was drawn before it may show what it changes, so the frame goes again.
    ui->again |= is_over && ui->mouse_released;
    return is_over && ui->mouse_released;
}

static void toggle_popup(struct gui_ui *ui, uint32_t id, bool menu) {
    ui->popup = ui->popup == id ? 0 : id;
    ui->popup_is_menu = menu;
}

bool gui_dropdown(struct gui_ui *ui, const char *label, const char *const *items, int count, int *selected) {
    uint32_t id = id_of(label);
    struct gui_rect frame;
    int before = *selected;

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

        if (code == KEY_UP || code == KEY_DOWN)
            *selected += code == KEY_DOWN ? 1 : -1;
        else if (code == KEY_ENTER || code == KEY_SPACE)
            toggle_popup(ui, id, false);
    }
    *selected = *selected >= count ? count - 1 : *selected < 0 ? 0 : *selected;

    fill(ui, frame.x, frame.y, frame.width, frame.height, ui->hot == id || ui->popup == id ? FRAME_HOT : FRAME);
    if (count)
        draw_fitted(ui, frame.x + PADDING, frame.y + PADDING, frame.width - 2 * PADDING - 16, items[*selected], 255, TEXT);
    text(ui, frame.x + frame.width - PADDING - GUI_CHAR_WIDTH, frame.y + PADDING, "v", DIM_TEXT, 1);
    if (focused)
        draw_focus(ui, frame);

    // Under the frame, or over it if there's no room under.
    if (ui->popup == id) {
        int height = count * ROW_HEIGHT + 8;
        bool under = frame.y + frame.height + height <= whole(ui).height || frame.y < height;

        popup_begin(ui, (struct gui_rect){frame.x, under ? frame.y + frame.height : frame.y - height, frame.width, 0});
        for (int i = 0; i < count; i++) {
            if (popup_item(ui, items[i], i == *selected)) {
                *selected = i;
                ui->popup = 0;
            }
        }
        popup_end(ui);
    }
    return *selected != before;
}

bool gui_list(struct gui_ui *ui, const char *label, const char *const *items, int count, int *selected, int rows) {
    uint32_t id = id_of(label);
    struct gui_scroll *scroll = scroll_of(ui, id);
    struct gui_rect frame, was;
    int before = *selected;

    framed(ui, label, rows * ROW_HEIGHT + 4, &frame);
    scroll->content = count * ROW_HEIGHT + 4;
    if (over(ui, frame) && ui->mouse_pressed)
        ui->focus = id;

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
        if (over(ui, r) && ui->mouse_pressed)
            *selected = i;
        if (i == *selected || over(ui, r))
            fill(ui, r.x, r.y, r.width, r.height, i == *selected ? ACCENT : FRAME_HOT);
        draw_fitted(ui, r.x + 6, r.y + (ROW_HEIGHT - GUI_CHAR_HEIGHT) / 2, r.width - 12, items[i], 255, TEXT);
    }
    ui->layout.clip = was;
    if (focused)
        draw_focus(ui, frame);
    return *selected != before;
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

    // Opens on a press, and while one menu is open, the mouse moving over another opens that.
    if (is_over && ui->mouse_pressed)
        toggle_popup(ui, id, true);
    else if (is_over && ui->popup && ui->popup_is_menu)
        ui->popup = id;
    fill(ui, r.x, r.y, r.width, r.height, ui->popup == id ? ACCENT : is_over ? FRAME_HOT : FRAME);
    draw_label(ui, r.x + PADDING + 3, r.y + PADDING, label, TEXT);
    if (ui->popup != id)
        return false;
    popup_begin(ui, (struct gui_rect){r.x, r.y + r.height, MENU_WIDTH, 0});
    return true;
}

bool gui_menu_item(struct gui_ui *ui, const char *label) {
    bool chosen = popup_item(ui, label, false);

    if (chosen)
        ui->popup = 0;
    return chosen;
}

void gui_menu_end(struct gui_ui *ui) {
    popup_end(ui);
}

void gui_scroll_begin(struct gui_ui *ui, const char *label, int height) {
    uint32_t id = id_of(label);
    struct gui_scroll *scroll = scroll_of(ui, id);
    int x, y;

    next_spot(ui, &x, &y);
    if (height <= 0)
        height = whole(ui).height - MARGIN - y;

    struct gui_rect area = place(ui, room(ui), height > FRAME_HEIGHT ? height : FRAME_HEIGHT);

    fill(ui, area.x, area.y, area.width, area.height, PANEL);

    struct gui_rect inner = scrolling(ui, id, scroll, area);

    if (ui->depth == GUI_UI_MAX_DEPTH)
        return; // too deep: what's in it goes on as if it weren't there
    ui->saved[ui->depth].layout = ui->layout;
    ui->saved[ui->depth].area = area;
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
