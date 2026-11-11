#include <gui.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define MARGIN 10
#define SPACING 8
#define PADDING 7 // around text in a frame
#define FRAME_HEIGHT (GUI_CHAR_HEIGHT + 2 * PADDING)
#define CHECK_SIZE 14
#define GRAB_WIDTH 10

#define BACKGROUND 0x1E2129
#define TEXT 0xE6E8EE
#define DIM_TEXT 0x8A93A6
#define FRAME 0x2B303B
#define FRAME_HOT 0x363D4B
#define ACCENT 0x3D63DD
#define ACCENT_HOT 0x5578E8
#define FOCUS_RING 0x8FA8F5
#define SEPARATOR 0x3A4150

static struct gui_canvas *canvas(struct gui_ui *ui) {
    return &ui->window->canvas;
}

// What a label shows: everything before any "##".
static int shown_length(const char *label) {
    const char *hidden = strstr(label, "##");

    return hidden ? (int)(hidden - label) : (int)strlen(label);
}

static void draw_label(struct gui_ui *ui, int x, int y, const char *label, uint32_t color) {
    char shown[256];

    snprintf(shown, sizeof(shown), "%.*s", shown_length(label), label);
    gui_text(canvas(ui), x, y, shown, color, 1);
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
        ui->mouse_x = e->x;
        ui->mouse_y = e->y;
        if (e->type == GUI_MOUSE_BUTTON && (e->changed & 1)) {
            ui->mouse_down = e->buttons & 1;
            ui->mouse_pressed |= ui->mouse_down;
            ui->mouse_released |= !ui->mouse_down;
        }
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
    }
    return false;
}

void gui_ui_begin(struct gui_ui *ui) {
    struct gui_canvas *c = canvas(ui);

    gui_fill(c, 0, 0, c->width, c->height, BACKGROUND);
    ui->x = ui->line_end = MARGIN;
    ui->y = MARGIN;
    ui->line_height = 0;
    ui->same_line = false;
    ui->hot = 0;
    ui->next_count = 0;

    // Clicking where there's no widget takes the keyboard away, and one there gives it back.
    if (ui->mouse_pressed)
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
    memcpy(ui->order, ui->next_order, ui->next_count * sizeof(*ui->order));
    ui->order_count = ui->next_count;
    if (ui->mouse_released)
        ui->active = 0;
    ui->mouse_pressed = ui->mouse_released = false;
    ui->key_count = 0;
    gui_show(ui->window);
}

void gui_same_line(struct gui_ui *ui) {
    ui->same_line = true;
}

struct gui_rect gui_last_rect(const struct gui_ui *ui) {
    return ui->last;
}

// Room for a widget, under the last or beside it.
static struct gui_rect place(struct gui_ui *ui, int width, int height) {
    if (ui->same_line && ui->line_height) {
        ui->x = ui->line_end + SPACING;
    } else {
        ui->y += ui->line_height ? ui->line_height + SPACING : 0;
        ui->x = MARGIN;
        ui->line_height = 0;
    }
    ui->same_line = false;
    ui->last = (struct gui_rect){ui->x, ui->y, width, height};
    ui->line_end = ui->x + width;
    if (height > ui->line_height)
        ui->line_height = height;
    return ui->last;
}

// What's left of the line, for widgets that stretch.
static int room(const struct gui_ui *ui) {
    int x = ui->same_line && ui->line_height ? ui->line_end + SPACING : MARGIN;

    return canvas((struct gui_ui *)ui)->width - MARGIN - x;
}

static bool contains(struct gui_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

// The mouse's part in a widget: whether it's over it, and pressing makes it
// active and gives it the keyboard. Returns whether it was clicked: let go over it.
static bool interact(struct gui_ui *ui, uint32_t id, struct gui_rect r) {
    bool over = contains(r, ui->mouse_x, ui->mouse_y);

    if (over && (!ui->active || ui->active == id))
        ui->hot = id;
    if (over && ui->mouse_pressed) {
        ui->active = id;
        ui->focus = id;
    }
    return ui->mouse_released && ui->active == id && over;
}

// Takes part in Tab's order, returning whether it has the keyboard.
static bool focusable(struct gui_ui *ui, uint32_t id) {
    if (ui->next_count < GUI_UI_MAX_FOCUSABLE)
        ui->next_order[ui->next_count++] = id;
    return ui->focus == id && ui->window_focused;
}

static bool key_pressed(const struct gui_ui *ui, uint16_t code) {
    for (int i = 0; i < ui->key_count; i++) {
        if (ui->keys[i].code == code)
            return true;
    }
    return false;
}

static void draw_focus(struct gui_ui *ui, struct gui_rect r) {
    struct gui_canvas *c = canvas(ui);

    gui_fill(c, r.x - 2, r.y - 2, r.width + 4, 1, FOCUS_RING);
    gui_fill(c, r.x - 2, r.y + r.height + 1, r.width + 4, 1, FOCUS_RING);
    gui_fill(c, r.x - 2, r.y - 2, 1, r.height + 4, FOCUS_RING);
    gui_fill(c, r.x + r.width + 1, r.y - 2, 1, r.height + 4, FOCUS_RING);
}

void gui_heading(struct gui_ui *ui, const char *text) {
    struct gui_rect r = place(ui, gui_text_width(text, 2), 2 * GUI_CHAR_HEIGHT);

    gui_text(canvas(ui), r.x, r.y, text, TEXT, 2);
}

void gui_label(struct gui_ui *ui, const char *format, ...) {
    char text[256];
    va_list args;

    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);

    // Beside a framed widget, it lines up with that widget's text.
    struct gui_rect r = place(ui, gui_text_width(text, 1), ui->same_line && ui->line_height ? ui->line_height : GUI_CHAR_HEIGHT);

    gui_text(canvas(ui), r.x, r.y + (r.height - GUI_CHAR_HEIGHT) / 2, text, TEXT, 1);
}

void gui_separator(struct gui_ui *ui) {
    struct gui_rect r = place(ui, room(ui), 1);

    gui_fill(canvas(ui), r.x, r.y, r.width, 1, SEPARATOR);
}

void gui_progress(struct gui_ui *ui, float fraction) {
    struct gui_rect r = place(ui, room(ui), FRAME_HEIGHT);
    char text[16];

    fraction = fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
    snprintf(text, sizeof(text), "%d%%", (int)(fraction * 100 + 0.5f));
    gui_fill(canvas(ui), r.x, r.y, r.width, r.height, FRAME);
    gui_fill(canvas(ui), r.x, r.y, (int)(r.width * fraction), r.height, ACCENT);
    gui_text(canvas(ui), r.x + (r.width - gui_text_width(text, 1)) / 2, r.y + PADDING, text, TEXT, 1);
}

bool gui_button(struct gui_ui *ui, const char *label) {
    uint32_t id = id_of(label);
    struct gui_rect r = place(ui, shown_length(label) * GUI_CHAR_WIDTH + 2 * PADDING + 4, FRAME_HEIGHT);
    bool clicked = interact(ui, id, r);
    bool focused = focusable(ui, id);

    clicked |= focused && (key_pressed(ui, KEY_ENTER) || key_pressed(ui, KEY_SPACE));
    gui_fill(canvas(ui), r.x, r.y, r.width, r.height,
             ui->active == id && ui->hot == id ? ACCENT : ui->hot == id ? FRAME_HOT : FRAME);
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
    gui_fill(canvas(ui), r.x, r.y, CHECK_SIZE, CHECK_SIZE, ui->hot == id ? FRAME_HOT : FRAME);
    if (*value)
        gui_fill(canvas(ui), r.x + 3, r.y + 3, CHECK_SIZE - 6, CHECK_SIZE - 6, ui->hot == id ? ACCENT_HOT : ACCENT);
    draw_label(ui, r.x + CHECK_SIZE + 8, r.y + (CHECK_SIZE - GUI_CHAR_HEIGHT) / 2, label, TEXT);
    if (focused)
        draw_focus(ui, (struct gui_rect){r.x, r.y, CHECK_SIZE, CHECK_SIZE});
    return toggled;
}

// The frame stretches, with the label after it as ImGui puts it.
static struct gui_rect framed(struct gui_ui *ui, const char *label, struct gui_rect *frame) {
    int label_width = shown_length(label) * GUI_CHAR_WIDTH;
    int width = room(ui);
    struct gui_rect r = place(ui, width, FRAME_HEIGHT);

    *frame = (struct gui_rect){r.x, r.y, width - (label_width ? label_width + SPACING : 0), FRAME_HEIGHT};
    draw_label(ui, frame->x + frame->width + SPACING, r.y + PADDING, label, TEXT);
    return r;
}

bool gui_slider(struct gui_ui *ui, const char *label, int *value, int min, int max) {
    uint32_t id = id_of(label);
    struct gui_rect frame;
    int before = *value, span = max > min ? max - min : 1;

    framed(ui, label, &frame);
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

    char text[32];
    int grab = frame.x + (int)((long)(*value - min) * track / span);

    snprintf(text, sizeof(text), "%d", *value);
    gui_fill(canvas(ui), frame.x, frame.y, frame.width, frame.height, ui->hot == id ? FRAME_HOT : FRAME);
    gui_fill(canvas(ui), grab, frame.y + 2, GRAB_WIDTH, frame.height - 4, ui->active == id ? ACCENT_HOT : ACCENT);
    gui_text(canvas(ui), frame.x + (frame.width - gui_text_width(text, 1)) / 2, frame.y + PADDING, text, TEXT, 1);
    if (focused)
        draw_focus(ui, frame);
    return *value != before;
}

bool gui_textbox(struct gui_ui *ui, const char *label, char *text, size_t size) {
    uint32_t id = id_of(label);
    struct gui_rect frame;
    bool changed = false;

    framed(ui, label, &frame);
    interact(ui, id, frame);

    bool focused = focusable(ui, id);
    size_t len = strlen(text);

    // The cursor starts at the end when the box gets the keyboard.
    if (ui->focus == id && (ui->text_owner != id || ui->text_cursor > len)) {
        ui->text_cursor = len;
        ui->text_owner = id;
    }

    for (int i = 0; focused && i < ui->key_count; i++) {
        const struct key_event *k = &ui->keys[i];
        size_t *at = &ui->text_cursor;

        if (k->code == KEY_LEFT && *at > 0) {
            (*at)--;
        } else if (k->code == KEY_RIGHT && *at < len) {
            (*at)++;
        } else if (k->code == KEY_HOME) {
            *at = 0;
        } else if (k->code == KEY_END) {
            *at = len;
        } else if (k->code == KEY_BACKSPACE && *at > 0) {
            memmove(text + *at - 1, text + *at, len - *at + 1);
            (*at)--;
            len--;
            changed = true;
        } else if (k->code == KEY_DELETE && *at < len) {
            memmove(text + *at, text + *at + 1, len - *at);
            len--;
            changed = true;
        } else if (k->text >= ' ' && k->text < 127 && !(k->modifiers & (KEY_MOD_CTRL | KEY_MOD_ALT))
                   && len + 1 < size) {
            memmove(text + *at + 1, text + *at, len - *at + 1);
            text[(*at)++] = (char)k->text;
            len++;
            changed = true;
        }
    }

    // Long text scrolls to keep the cursor in sight.
    int fits = (frame.width - 2 * PADDING) / GUI_CHAR_WIDTH;
    size_t first = focused && (int)ui->text_cursor >= fits ? ui->text_cursor - fits + 1 : 0;
    char shown[256];

    snprintf(shown, sizeof(shown), "%.*s", fits > 0 ? fits : 0, text + first);
    gui_fill(canvas(ui), frame.x, frame.y, frame.width, frame.height, ui->hot == id || focused ? FRAME_HOT : FRAME);
    gui_text(canvas(ui), frame.x + PADDING, frame.y + PADDING, shown, TEXT, 1);
    if (focused) {
        int caret = frame.x + PADDING + (int)(ui->text_cursor - first) * GUI_CHAR_WIDTH;

        gui_fill(canvas(ui), caret, frame.y + PADDING - 2, 1, GUI_CHAR_HEIGHT + 4, TEXT);
        draw_focus(ui, frame);
    } else if (!*text) {
        gui_text(canvas(ui), frame.x + PADDING, frame.y + PADDING, "...", DIM_TEXT, 1);
    }
    return changed;
}
