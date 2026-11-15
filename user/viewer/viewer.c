#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define CHECK 8 // squares behind what's transparent
#define CHECK_LIGHT 0x3A3F4B
#define CHECK_DARK 0x2E323C
#define MAX_WIDTH 1000
#define MAX_HEIGHT 680
#define MIN_SIZE 240

static struct gui_window *window;
static uint32_t background, text_colour; // from the theme
static struct gui_image image, fitted; // as it is, and as it's shown
static char title[GUI_TITLE_MAX] = "Viewer";

// The image fitted into the window, never made bigger than it is, in the middle,
// with checks under it.
static void draw(void) {
    struct gui_canvas *c = &window->canvas;
    int w = image.width, h = image.height;

    gui_fill(c, 0, 0, c->width, c->height, background);
    if (!image.pixels) {
        gui_text(c, 16, 16, "No image: open one from Files.", text_colour, 1);
        gui_show(window);
        return;
    }
    if (w > c->width || h > c->height) {
        bool wider = (long)w * c->height > (long)h * c->width;

        h = wider ? (int)((long)h * c->width / w) : c->height;
        w = wider ? c->width : (int)((long)image.width * c->height / image.height);
        w = w > 0 ? w : 1;
        h = h > 0 ? h : 1;
    }
    if (fitted.width != w || fitted.height != h) {
        gui_image_free(&fitted);
        if (w == image.width && h == image.height)
            fitted = (struct gui_image){0};
        else if (!gui_image_scale(&image, w, h, &fitted))
            fitted = (struct gui_image){0};
    }

    const struct gui_image *shown = fitted.pixels ? &fitted : &image;
    int x = (c->width - shown->width) / 2, y = (c->height - shown->height) / 2;

    for (int cy = 0; cy < shown->height; cy += CHECK) {
        for (int cx = 0; cx < shown->width; cx += CHECK)
            gui_fill(c, x + cx, y + cy, CHECK < shown->width - cx ? CHECK : shown->width - cx,
                     CHECK < shown->height - cy ? CHECK : shown->height - cy, (cx / CHECK + cy / CHECK) % 2 ? CHECK_DARK : CHECK_LIGHT);
    }
    gui_draw_image(c, x, y, shown);
    gui_show(window);
}

// Shows a file in place of what was shown, with its name and size in the title.
static void show_file(const char *path) {
    const char *name = strrchr(path, '/');
    struct gui_image loaded;

    if (!gui_image_load(path, &loaded)) {
        printf("viewer: %s: %s\n", path, gui_image_error());
        return;
    }
    gui_image_free(&image);
    gui_image_free(&fitted);
    image = loaded;
    snprintf(title, sizeof(title), "%s (%dx%d) - Viewer", name ? name + 1 : path, image.width, image.height);
    printf("viewer: showing %s %dx%d\n", path, image.width, image.height);
    if (window)
        gui_set_title(window, title);
}

static void use_theme(void) {
    struct gui_settings settings;

    gui_settings_load(&settings);
    background = settings.light ? 0xF3F4F7 : 0x1E2129;
    text_colour = settings.light ? 0x1E2129 : 0xE6E8EE;
}

// Shows a PNG, JPEG or BMP file.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: viewer [image]\n"
                            "Opens a window showing a PNG, JPEG or BMP image.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "viewer: wm isn't running\n");
        return 1;
    }

    if (argc > 1)
        show_file(argv[1]);
    fflush(stdout);

    int w = image.width < MIN_SIZE ? MIN_SIZE : image.width > MAX_WIDTH ? MAX_WIDTH : image.width;
    int h = image.height < MIN_SIZE ? MIN_SIZE : image.height > MAX_HEIGHT ? MAX_HEIGHT : image.height;
    struct gui_message e;

    window = gui_create_window(title, w, h);
    if (!window) {
        dprintf(STDERR_FILENO, "viewer: couldn't make a window\n");
        return 1;
    }
    use_theme();
    draw();
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        char path[GUI_PATH_MAX];

        if (gui_dropped_file(&e, path)) {
            show_file(path);
            draw();
            fflush(stdout);
        }
        if (e.type == GUI_SETTINGS) {
            use_theme();
            draw();
        }
        if (e.type == GUI_KEY && e.key.pressed && e.key.code == KEY_ESCAPE)
            break;
        if (e.type == GUI_RESIZE && gui_resize(window, e.width, e.height))
            draw();
    }
    gui_destroy_window(window);
    return 0;
}
