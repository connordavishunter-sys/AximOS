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
#define MIN_ZOOM (1.0 / 16)
#define MAX_ZOOM 32.0
#define KEY_EQUALS 0x0D
#define KEY_MINUS 0x0C
#define KEY_0 0x0B
#define KEY_1 0x02

static struct gui_window *window;
static uint32_t background, text_colour; // from the theme
static struct gui_image image, fitted; // as it is, and as it's shown fitted to the window
static char title[GUI_TITLE_MAX] = "Viewer";

// Zoomed, the image is shown zoom times its size, with the point centre_x,
// centre_y of it in the middle of the window, as far as it'll go. Otherwise it's
// fitted to the window.
static double zoom, centre_x, centre_y;
static int drag_x, drag_y; // where the mouse was, dragging the image about

static double fitted_zoom(void) {
    struct gui_canvas *c = &window->canvas;
    double across = (double)c->width / image.width, down = (double)c->height / image.height;
    double fits = across < down ? across : down;

    return fits < 1 ? fits : 1;
}

// Where the image's top left corner goes, keeping it on the window when it's
// bigger, and in the middle when it's smaller.
static void place(double *x, double *y) {
    struct gui_canvas *c = &window->canvas;
    double w = image.width * zoom, h = image.height * zoom, half_w = c->width / 2.0 / zoom, half_h = c->height / 2.0 / zoom;

    centre_x = w <= c->width ? image.width / 2.0 : centre_x < half_w ? half_w : centre_x > image.width - half_w ? image.width - half_w : centre_x;
    centre_y = h <= c->height ? image.height / 2.0 : centre_y < half_h ? half_h : centre_y > image.height - half_h ? image.height - half_h : centre_y;
    *x = c->width / 2.0 - centre_x * zoom;
    *y = c->height / 2.0 - centre_y * zoom;
}

static uint32_t check_at(int x, int y) {
    return (x / CHECK + y / CHECK) % 2 ? CHECK_DARK : CHECK_LIGHT;
}

// A pixel of 0xAARRGGBB over one of the window's, in 256ths so each colour shifts
// into place on its own: dividing red and blue together would spill one into the other.
static uint32_t over(uint32_t p, uint32_t under) {
    uint32_t a = p >> 24, keep;

    a += a >> 7;
    keep = 256 - a;
    return ((((p & 0xFF00FF) * a + (under & 0xFF00FF) * keep) >> 8) & 0xFF00FF)
         | ((((p & 0x00FF00) * a + (under & 0x00FF00) * keep) >> 8) & 0x00FF00);
}

// Each pixel of the window from the nearest of the image, over the checks.
static void draw_zoomed(struct gui_canvas *c) {
    double x0, y0;

    place(&x0, &y0);

    int left = x0 > 0 ? (int)x0 : 0, top = y0 > 0 ? (int)y0 : 0;
    int right = x0 + image.width * zoom < c->width ? (int)(x0 + image.width * zoom) : c->width;
    int bottom = y0 + image.height * zoom < c->height ? (int)(y0 + image.height * zoom) : c->height;

    for (int y = top; y < bottom; y++) {
        int sy = (int)((y - y0) / zoom);
        const uint32_t *from = image.pixels + (long)(sy < image.height ? sy : image.height - 1) * image.width;
        uint32_t *to = c->pixels + (long)y * c->stride;

        for (int x = left; x < right; x++) {
            int sx = (int)((x - x0) / zoom);
            to[x] = over(from[sx < image.width ? sx : image.width - 1], check_at(x - left, y - top));
        }
    }
}

static void draw(void) {
    struct gui_canvas *c = &window->canvas;
    int w = image.width, h = image.height;

    gui_fill(c, 0, 0, c->width, c->height, background);
    if (!image.pixels) {
        gui_text(c, 16, 16, "No image: open one from Files.", text_colour, 1);
        gui_show(window);
        return;
    }
    if (zoom) {
        draw_zoomed(c);
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

// Zooms to a new size, or back to fitting the window with 0, keeping the point of
// the image at x, y in the window where it is.
static void zoom_to(double to, int x, int y) {
    double x0, y0;

    if (!image.pixels)
        return;
    if (!zoom) {
        zoom = fitted_zoom();
        centre_x = image.width / 2.0;
        centre_y = image.height / 2.0;
    }
    place(&x0, &y0);

    double at_x = (x - x0) / zoom, at_y = (y - y0) / zoom;

    to = to && to < MIN_ZOOM ? MIN_ZOOM : to > MAX_ZOOM ? MAX_ZOOM : to;
    zoom = to;
    if (to) {
        centre_x = at_x - (x - window->canvas.width / 2.0) / to;
        centre_y = at_y - (y - window->canvas.height / 2.0) / to;
        printf("viewer: zoom %d%%\n", (int)(to * 100 + 0.5));
    } else {
        printf("viewer: zoom to fit\n");
    }
    draw();
}

static void key(const struct key_event *k) {
    int middle_x = window->canvas.width / 2, middle_y = window->canvas.height / 2;
    double now = zoom ? zoom : image.pixels ? fitted_zoom() : 1;

    if (k->code == KEY_EQUALS || k->text == '+')
        zoom_to(now * 2, middle_x, middle_y);
    else if (k->code == KEY_MINUS)
        zoom_to(now / 2, middle_x, middle_y);
    else if (k->code == KEY_0)
        zoom_to(0, middle_x, middle_y);
    else if (k->code == KEY_1)
        zoom_to(1, middle_x, middle_y);
    else if ((k->modifiers & KEY_MOD_CTRL) && (k->text == 'c' || k->text == ('c' & 0x1F)) && image.pixels) {
        struct gui_canvas all = {image.pixels, image.width, image.height, image.width};

        if (gui_clipboard_set_image(&all))
            printf("viewer: copied %dx%d\n", image.width, image.height);
    }
}

// The wheel zooms where the mouse is, and dragging moves a zoomed image about.
static void mouse(const struct gui_message *e) {
    double now = zoom ? zoom : image.pixels ? fitted_zoom() : 1;

    if (e->type == GUI_MOUSE_WHEEL && e->wheel)
        zoom_to(e->wheel < 0 ? now * 2 : now / 2, e->x, e->y);
    if (e->type == GUI_MOUSE_MOVE && (e->buttons & 1) && zoom) {
        centre_x -= (e->x - drag_x) / zoom;
        centre_y -= (e->y - drag_y) / zoom;
        draw();
    }
    drag_x = e->x;
    drag_y = e->y;
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
    zoom = 0;
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
        if (e.type == GUI_KEY && e.key.pressed)
            key(&e.key);
        if (e.type == GUI_MOUSE_WHEEL || e.type == GUI_MOUSE_MOVE || e.type == GUI_MOUSE_BUTTON)
            mouse(&e);
        fflush(stdout);
        if (e.type == GUI_RESIZE && gui_resize(window, e.width, e.height))
            draw();
    }
    gui_destroy_window(window);
    return 0;
}
