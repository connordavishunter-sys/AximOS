#include <gui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define WIDTH 640 // of a new picture
#define HEIGHT 400
#define MAX_SIZE 4096
#define UNDO 8
#define SWATCH 22
#define WHITE 0xFFFFFF
#define ACCENT 0x3D63DD
#define AROUND 0x2A2E37 // the window, round a picture smaller than it
#define BAR 12          // scroll bars
#define BAR_TRACK 0x22252D
#define BAR_THUMB 0x4A5263
#define WHEEL_STEP 48
#define KEY_EQUALS 0x0D
#define KEY_MINUS 0x0C

// Pictures drawn with the mouse, saved and opened as PNG files.
enum tool { PENCIL, LINE, BOX, OVAL, FILL, ERASER, PICKER, TOOLS };
static const char *const tool_names[] = {"Pencil", "Line", "Box", "Oval", "Fill", "Eraser", "Picker"};
static const uint32_t palette[] = {
    0x000000, 0x5A5F6B, 0xA0A6B2, 0xFFFFFF, 0xE5484D, 0xF2994A, 0xF2C94C, 0x6FCF97,
    0x27AE60, 0x56CCF2, 0x2F80ED, 0x3D63DD, 0x9B51E0, 0xEB5FA8, 0x8B5A2B, 0x1E2430,
};
#define COLOURS ((int)(sizeof(palette) / sizeof(palette[0])))

static struct gui_window *window;
static struct gui_file_picker picker;
static struct gui_canvas picture, preview; // the picture, and a copy with the shape being drawn
static struct gui_canvas undo[UNDO];
static int undo_count;

// The picture's shown zoom times its size, scrolled by scroll_x and scroll_y of
// those pixels, in view, with bars to scroll it when it doesn't fit.
static const double zooms[] = {0.25, 0.5, 1, 2, 4, 8};
#define ZOOMS ((int)(sizeof(zooms) / sizeof(zooms[0])))
#define ACTUAL_SIZE 2
static int zoom_index = ACTUAL_SIZE, scroll_x, scroll_y;
static struct gui_rect view, bars[2]; // across the bottom and down the side, if they're shown
static int dragging_bar = -1, bar_grab;
static enum tool tool;
static uint32_t colour;
static int size = 3;
static char path[GUI_PATH_MAX], status[GUI_PATH_MAX + 64];
static bool modified, closing, quitting;

// A stroke under way: where it started, and where the mouse was last.
static bool drawing;
static int start_x, start_y, last_x, last_y;

static bool new_picture(int width, int height) {
    uint32_t *pixels = malloc((size_t)width * height * sizeof(uint32_t));

    if (!pixels)
        return false;
    free(picture.pixels);
    picture = (struct gui_canvas){pixels, width, height, width};
    gui_fill(&picture, 0, 0, width, height, WHITE);
    for (; undo_count > 0; undo_count--)
        free(undo[undo_count - 1].pixels);
    scroll_x = scroll_y = 0;
    return true;
}

static const char *file_name(void) {
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

static void show_title(void) {
    char title[GUI_TITLE_MAX];

    snprintf(title, sizeof(title), "%s%s - Paint", modified ? "*" : "", path[0] ? file_name() : "Untitled");
    gui_set_title(window, title);
}

static void set_modified(bool now) {
    if (modified != now) {
        modified = now;
        show_title();
    }
    closing = false;
}

// Keeps the picture as it is, and its size, to go back to.
static void save_undo(void) {
    size_t bytes = (size_t)picture.width * picture.height * sizeof(uint32_t);
    uint32_t *copy = malloc(bytes);

    if (!copy)
        return;
    memcpy(copy, picture.pixels, bytes);
    if (undo_count == UNDO) {
        free(undo[0].pixels);
        memmove(undo, undo + 1, (UNDO - 1) * sizeof(*undo));
        undo_count--;
    }
    undo[undo_count++] = (struct gui_canvas){copy, picture.width, picture.height, picture.width};
}

static void undo_last(void) {
    if (!undo_count)
        return;
    free(picture.pixels);
    picture = undo[--undo_count];
    set_modified(true);
    printf("paint: undone\n");
}

static void copy_picture(void) {
    if (gui_clipboard_set_image(&picture))
        printf("paint: copied %dx%d\n", picture.width, picture.height);
}

// A picture from the clipboard goes at the top left, making the picture bigger if
// it's smaller.
static void paste_picture(void) {
    struct gui_image pasted;

    if (!gui_clipboard_get_image(&pasted)) {
        snprintf(status, sizeof(status), "There's no picture to paste");
        return;
    }

    int width = pasted.width > picture.width ? pasted.width : picture.width;
    int height = pasted.height > picture.height ? pasted.height : picture.height;
    uint32_t *pixels = width <= MAX_SIZE && height <= MAX_SIZE ? malloc((size_t)width * height * sizeof(uint32_t)) : NULL;

    if (pixels) {
        struct gui_canvas bigger = {pixels, width, height, width};

        save_undo();
        gui_fill(&bigger, 0, 0, width, height, WHITE);
        gui_copy(&bigger, 0, 0, &picture, 0, 0, picture.width, picture.height);
        gui_draw_image(&bigger, 0, 0, &pasted);
        free(picture.pixels);
        picture = bigger;
        set_modified(true);
        printf("paint: pasted %dx%d\n", pasted.width, pasted.height);
    }
    gui_image_free(&pasted);
}

static int zoomed(int pixels) {
    return (int)(pixels * zooms[zoom_index]);
}

// Zooms in or out, keeping what's in the middle of the view there.
static void zoom_by(int steps) {
    int to = zoom_index + steps < 0 ? 0 : zoom_index + steps >= ZOOMS ? ZOOMS - 1 : zoom_index + steps;
    double middle_x = (scroll_x + view.width / 2.0) / zooms[zoom_index];
    double middle_y = (scroll_y + view.height / 2.0) / zooms[zoom_index];

    zoom_index = steps ? to : ACTUAL_SIZE;
    scroll_x = (int)(middle_x * zooms[zoom_index] - view.width / 2.0);
    scroll_y = (int)(middle_y * zooms[zoom_index] - view.height / 2.0);
    printf("paint: zoom %d%%\n", (int)(zooms[zoom_index] * 100));
}

// What's partly see-through goes on white, as there's nothing under a picture.
static void open_file(const char *chosen) {
    struct gui_image image;

    if (!gui_image_load(chosen, &image)) {
        snprintf(status, sizeof(status), "Can't open %s: %s", chosen, gui_image_error());
        return;
    }
    if (image.width <= MAX_SIZE && image.height <= MAX_SIZE && new_picture(image.width, image.height)) {
        gui_draw_image(&picture, 0, 0, &image);
        snprintf(path, sizeof(path), "%s", chosen);
        snprintf(status, sizeof(status), "Opened %s, %dx%d", chosen, image.width, image.height);
        printf("paint: opened %s %dx%d\n", chosen, image.width, image.height);
        modified = true;
        set_modified(false);
    } else {
        snprintf(status, sizeof(status), "%s is too big", chosen);
    }
    gui_image_free(&image);
}

static void save_file(const char *to) {
    if (!gui_image_save(to, &picture)) {
        snprintf(status, sizeof(status), "Can't save %s: %s", to, gui_image_error());
        return;
    }
    snprintf(path, sizeof(path), "%s", to);
    snprintf(status, sizeof(status), "Saved %s", to);
    printf("paint: saved %s %dx%d\n", to, picture.width, picture.height);
    modified = true;
    set_modified(false);
}

static void pick(bool saving) {
    gui_picker_open(&picker, "/", saving ? (path[0] ? file_name() : "picture.png") : "", saving);
    gui_post(&(struct gui_message){.type = GUI_REDRAW});
}

static void save(void) {
    if (path[0])
        save_file(path);
    else
        pick(true);
}

// Drawing, on the picture or a preview over it.

static void dot(struct gui_canvas *c, int x, int y, uint32_t with) {
    int r = size / 2;

    for (int dy = -r; dy <= r; dy++) {
        int half = (int)sqrt((double)(r * r - dy * dy) + 0.5);

        gui_fill(c, x - half, y + dy, 2 * half + 1, 1, with);
    }
}

static void line(struct gui_canvas *c, int x0, int y0, int x1, int y1, uint32_t with) {
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, error = dx + dy;

    for (;;) {
        int twice = 2 * error; // before either step changes it

        dot(c, x0, y0, with);
        if (x0 == x1 && y0 == y1)
            return;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static void box(struct gui_canvas *c, int x0, int y0, int x1, int y1, uint32_t with) {
    line(c, x0, y0, x1, y0, with);
    line(c, x1, y0, x1, y1, with);
    line(c, x1, y1, x0, y1, with);
    line(c, x0, y1, x0, y0, with);
}

static void oval(struct gui_canvas *c, int x0, int y0, int x1, int y1, uint32_t with) {
    double cx = (x0 + x1) / 2.0, cy = (y0 + y1) / 2.0, rx = abs(x1 - x0) / 2.0, ry = abs(y1 - y0) / 2.0;
    int steps = 8 + (int)(2 * (rx + ry)), px = (int)(cx + rx), py = (int)cy;

    for (int i = 1; i <= steps; i++) {
        double a = 2 * M_PI * i / steps;
        int x = (int)lround(cx + rx * cos(a)), y = (int)lround(cy + ry * sin(a));

        line(c, px, py, x, y, with);
        px = x;
        py = y;
    }
}

// Fills the run of the colour at x, y and what touches it, a line at a time, with
// where the runs above and below start kept in a stack that grows as it needs to.
static void fill(int x, int y, uint32_t with) {
    struct gui_canvas *c = &picture;
    uint32_t was = c->pixels[(long)y * c->stride + x];
    size_t top = 0, room = 4096;
    int *stack = was == with ? NULL : malloc(room * sizeof(int));

    if (!stack)
        return;
    stack[top++] = x;
    stack[top++] = y;
    while (top) {
        int sy = stack[--top], sx = stack[--top], left = sx, right = sx;
        uint32_t *row = c->pixels + (long)sy * c->stride;

        if (row[sx] != was)
            continue;
        while (left > 0 && row[left - 1] == was)
            left--;
        while (right < c->width - 1 && row[right + 1] == was)
            right++;
        for (int i = left; i <= right; i++) {
            row[i] = with;
            for (int ny = sy - 1; ny <= sy + 1; ny += 2) {
                if (ny >= 0 && ny < c->height && c->pixels[(long)ny * c->stride + i] == was
                    && (i == left || c->pixels[(long)ny * c->stride + i - 1] != was)) {
                    if (top + 2 > room) {
                        int *bigger = realloc(stack, 2 * room * sizeof(int));

                        if (!bigger) {
                            free(stack);
                            return;
                        }
                        stack = bigger;
                        room *= 2;
                    }
                    stack[top++] = i;
                    stack[top++] = ny;
                }
            }
        }
    }
    free(stack);
}

// A line, box or oval goes from where the mouse was pressed to where it is.
static void shape(struct gui_canvas *c, int x, int y) {
    if (tool == LINE)
        line(c, start_x, start_y, x, y, colour);
    else if (tool == BOX)
        box(c, start_x, start_y, x, y, colour);
    else if (tool == OVAL)
        oval(c, start_x, start_y, x, y, colour);
}

// Says where something is when it moves, for tests.
static void report(int index, const char *what, struct gui_rect r) {
    static struct gui_rect places[TOOLS + 2];

    if (memcmp(&places[index], &r, sizeof(r)))
        printf("paint: at %s %d %d %d %d\n", what, r.x, r.y, r.width, r.height);
    places[index] = r;
}

static void choose_colour(uint32_t to) {
    colour = to;
    printf("paint: colour %06X\n", to);
}

static bool inside(struct gui_rect r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.width && y < r.y + r.height;
}

// Which pixel of the picture is at a point in the window, rounding down.
static int picture_x(int x) {
    return (int)floor((x - view.x + scroll_x) / zooms[zoom_index]);
}

static int picture_y(int y) {
    return (int)floor((y - view.y + scroll_y) / zooms[zoom_index]);
}

// The mouse on the picture uses the tool.
static void use_tool(struct gui_ui *ui) {
    int x = picture_x(ui->mouse_x), y = picture_y(ui->mouse_y);
    bool on = x >= 0 && y >= 0 && x < picture.width && y < picture.height && inside(view, ui->mouse_x, ui->mouse_y);

    if (ui->mouse_pressed && on && !ui->mouse_in_popup) {
        if (tool == PICKER) {
            choose_colour(picture.pixels[(long)y * picture.stride + x]);
            return;
        }
        save_undo();
        set_modified(true);
        if (tool == FILL) {
            fill(x, y, colour);
            printf("paint: filled %d %d\n", x, y);
            return;
        }
        drawing = true;
        start_x = last_x = x;
        start_y = last_y = y;
    }
    if (!drawing)
        return;
    if (tool == PENCIL || tool == ERASER) {
        line(&picture, last_x, last_y, x, y, tool == ERASER ? WHITE : colour);
        last_x = x;
        last_y = y;
    }
    if (!ui->mouse_down) {
        shape(&picture, x, y);
        printf("paint: %s %d %d to %d %d\n", tool_names[tool], start_x, start_y, x, y);
        drawing = false;
    }
}

// Scroll bars across the bottom and down the side, when the picture's wider or
// taller than the view, which the wheel scrolls up and down too.
static void scroll_bars(struct gui_ui *ui) {
    int content[2] = {zoomed(picture.width), zoomed(picture.height)}, *scroll[2] = {&scroll_x, &scroll_y};
    int length[2] = {view.width, view.height};

    scroll_y += ui->wheel * WHEEL_STEP;
    for (int i = 0; i < 2; i++) {
        int most = content[i] - length[i] > 0 ? content[i] - length[i] : 0;
        struct gui_rect track = bars[i];
        int along = i ? track.height : track.width, thumb = most ? along * length[i] / content[i] : along;
        int mouse = i ? ui->mouse_y - track.y : ui->mouse_x - track.x;

        thumb = thumb < 16 ? 16 : thumb;
        if (most && ui->mouse_pressed && inside(track, ui->mouse_x, ui->mouse_y)) {
            int at = *scroll[i] * (along - thumb) / most;

            dragging_bar = i;
            bar_grab = mouse >= at && mouse < at + thumb ? mouse - at : thumb / 2;
        }
        if (dragging_bar == i && along > thumb)
            *scroll[i] = (mouse - bar_grab) * most / (along - thumb);
        *scroll[i] = *scroll[i] < 0 ? 0 : *scroll[i] > most ? most : *scroll[i];
        if (most) {
            int at = *scroll[i] * (along - thumb) / most;

            gui_fill(&window->canvas, track.x, track.y, track.width, track.height, BAR_TRACK);
            gui_fill(&window->canvas, track.x + (i ? 2 : at + 2), track.y + (i ? at + 2 : 2), i ? BAR - 4 : thumb - 4,
                     i ? thumb - 4 : BAR - 4, dragging_bar == i ? ACCENT : BAR_THUMB);
        }
    }
    if (!ui->mouse_down)
        dragging_bar = -1;
}

// What's to be shown, the picture or the preview, zoomed in view, each pixel from
// the nearest of the picture's.
static void draw_zoomed(const struct gui_canvas *from) {
    struct gui_canvas *c = &window->canvas;

    for (int y = view.y; y < view.y + view.height; y++) {
        int sy = picture_y(y);
        uint32_t *row = c->pixels + (long)y * c->stride;

        for (int x = view.x; x < view.x + view.width; x++) {
            int sx = picture_x(x);

            row[x] = sx < from->width && sy < from->height ? from->pixels[(long)sy * from->stride + sx] : AROUND;
        }
    }
}

// The picture in area, with room taken from it for scroll bars when it doesn't fit.
static void show_picture(struct gui_ui *ui, struct gui_rect area) {
    bool across = zoomed(picture.width) > area.width, down = zoomed(picture.height) > area.height;

    across |= down && zoomed(picture.width) > area.width - BAR;
    down |= across && zoomed(picture.height) > area.height - BAR;
    view = (struct gui_rect){area.x, area.y, area.width - (down ? BAR : 0), area.height - (across ? BAR : 0)};
    if (view.width <= 0 || view.height <= 0)
        return;
    bars[0] = across ? (struct gui_rect){view.x, view.y + view.height, view.width, BAR} : (struct gui_rect){0};
    bars[1] = down ? (struct gui_rect){view.x + view.width, view.y, BAR, view.height} : (struct gui_rect){0};
    scroll_bars(ui);

    // Where the picture is, and how far it's scrolled, for tests, as far as it's shown.
    static int was_x = -1, was_y = -1;

    if (scroll_x != was_x || scroll_y != was_y)
        printf("paint: scrolled %d %d\n", scroll_x, scroll_y);
    was_x = scroll_x;
    was_y = scroll_y;
    int shown_width = zoomed(picture.width) - scroll_x, shown_height = zoomed(picture.height) - scroll_y;

    report(TOOLS + 1, "picture",
           (struct gui_rect){view.x, view.y, shown_width < view.width ? shown_width : view.width,
                             shown_height < view.height ? shown_height : view.height});
    if (dragging_bar < 0)
        use_tool(ui);
    if (drawing && tool != PENCIL && tool != ERASER) {
        size_t bytes = (size_t)picture.width * picture.height * sizeof(uint32_t);

        if (preview.width != picture.width || preview.height != picture.height) {
            free(preview.pixels);
            preview = (struct gui_canvas){malloc(bytes), picture.width, picture.height, picture.width};
        }
        if (preview.pixels) {
            memcpy(preview.pixels, picture.pixels, bytes);
            shape(&preview, picture_x(ui->mouse_x), picture_y(ui->mouse_y));
            draw_zoomed(&preview);
            return;
        }
    }
    draw_zoomed(&picture);
}

static void painting(struct gui_ui *ui) {
    gui_menu_bar(ui);
    if (gui_menu(ui, "File")) {
        if (gui_menu_item(ui, "New") && new_picture(WIDTH, HEIGHT)) {
            path[0] = '\0';
            modified = true;
            set_modified(false);
        }
        if (gui_menu_item(ui, "Open..."))
            pick(false);
        if (gui_menu_item(ui, "Save"))
            save();
        if (gui_menu_item(ui, "Save as..."))
            pick(true);
        gui_menu_end(ui);
    }
    if (gui_menu(ui, "Edit")) {
        if (gui_menu_item(ui, "Undo"))
            undo_last();
        if (gui_menu_item(ui, "Copy"))
            copy_picture();
        if (gui_menu_item(ui, "Paste"))
            paste_picture();
        gui_menu_end(ui);
    }
    if (gui_menu(ui, "View")) {
        if (gui_menu_item(ui, "Zoom in"))
            zoom_by(1);
        if (gui_menu_item(ui, "Zoom out"))
            zoom_by(-1);
        if (gui_menu_item(ui, "Actual size"))
            zoom_by(0);
        gui_menu_end(ui);
    }
    for (int i = 0; i < ui->key_count; i++) {
        const struct key_event *k = &ui->keys[i];

        if ((k->modifiers & KEY_MOD_CTRL) && (k->code == KEY_EQUALS || k->code == KEY_MINUS))
            zoom_by(k->code == KEY_EQUALS ? 1 : -1);
    }
    if (gui_shortcut(ui, 'z'))
        undo_last();
    if (gui_shortcut(ui, 'c'))
        copy_picture();
    if (gui_shortcut(ui, 'v'))
        paste_picture();
    if (gui_shortcut(ui, 's'))
        save();
    if (gui_shortcut(ui, 'o'))
        pick(false);

    for (int i = 0; i < TOOLS; i++) {
        if (i)
            gui_same_line(ui);
        if (gui_button(ui, tool_names[i])) {
            tool = i;
            printf("paint: tool %s\n", tool_names[i]);
        }
        report(i, tool_names[i], gui_last_rect(ui));
        if (tool == (enum tool)i) {
            struct gui_rect r = gui_last_rect(ui);

            gui_fill(&window->canvas, r.x, r.y + r.height - 3, r.width, 3, ACCENT);
        }
    }
    gui_same_line(ui);
    gui_slider(ui, "Size", &size, 1, 24);

    for (int i = 0; i < COLOURS; i++) {
        char label[8];

        snprintf(label, sizeof(label), "##%d", i);
        if (i)
            gui_same_line(ui);
        if (gui_button_sized(ui, label, SWATCH, SWATCH))
            choose_colour(palette[i]);

        struct gui_rect r = gui_last_rect(ui);

        if (!i)
            report(TOOLS, "colours", r);
        gui_fill(&window->canvas, r.x + 3, r.y + 3, r.width - 6, r.height - 6, palette[i]);
        if (palette[i] == colour)
            gui_fill(&window->canvas, r.x, r.y + r.height + 2, r.width, 3, ACCENT);
    }
    gui_same_line(ui);
    gui_label(ui, "%s", status);

    struct gui_rect last = gui_last_rect(ui);

    show_picture(ui, (struct gui_rect){10, last.y + last.height + 14, window->canvas.width - 20,
                                       window->canvas.height - 24 - last.y - last.height});
}

static void frame(struct gui_ui *ui) {
    gui_ui_begin(ui);
    if (!picker.open) {
        painting(ui);
    } else {
        bool saving = picker.saving;
        int result = gui_file_picker(ui, &picker);

        if (result == GUI_PICKED && saving)
            save_file(picker.path);
        else if (result == GUI_PICKED)
            open_file(picker.path);
        if (result)
            gui_post(&(struct gui_message){.type = GUI_REDRAW});
    }
    gui_ui_end(ui);
    fflush(stdout);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: paint [image]\n"
                            "Opens a window to draw a picture in, or the image given, saved as PNG.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "paint: wm isn't running\n");
        return 1;
    }

    struct gui_ui ui;
    struct gui_message e;

    window = gui_create_window("Paint", WIDTH + 20, HEIGHT + 120);
    if (!window || !new_picture(WIDTH, HEIGHT)) {
        dprintf(STDERR_FILENO, "paint: couldn't make a window\n");
        return 1;
    }
    gui_ui_init(&ui, window);
    if (argc > 1)
        open_file(argv[1]);
    show_title();
    frame(&ui);
    while (!quitting && gui_next_event(&e)) {
        char dropped[GUI_PATH_MAX];
        bool opening = gui_dropped_file(&e, dropped);

        if (opening)
            open_file(dropped);
        if (e.type == GUI_CLOSE) {
            quitting = !modified || closing;
            closing = true;
            snprintf(status, sizeof(status), "Not saved: close again to lose it");
        }
        if (gui_ui_event(&ui, &e) || opening || e.type == GUI_CLOSE)
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
