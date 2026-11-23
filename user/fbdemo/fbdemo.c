#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fb.h>
#include <time.h>
#include <unistd.h>

#define BOX_SIZE 96
#define FRAME_NS 16666667L

static long now_ns(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000000L + t.tv_nsec;
}

static uint32_t gradient(int x, int y, int w, int h) {
    return (uint32_t)(x * 255 / w) << 16 | (uint32_t)(y * 255 / h) << 8 | 0x40;
}

static void draw_frame(struct gui_canvas *c, int x, int y) {
    for (int row = 0; row < c->height; row++) {
        for (int col = 0; col < c->width; col++)
            c->pixels[row * c->stride + col] = gradient(col, row, c->width, c->height);
    }
    gui_fill(c, x, y, BOX_SIZE, BOX_SIZE, 0xFFFFFF);
}

// The same in a window, which can be resized, until it's closed.
static int bounce_in_window(void) {
    struct gui_window *window = gui_create_window("fbdemo", 480, 320);
    int x = 0, y = 0, dx = 5, dy = 3;

    if (!window) {
        dprintf(STDERR_FILENO, "fbdemo: couldn't make a window\n");
        return 1;
    }
    for (;;) {
        struct gui_canvas *c = &window->canvas;
        struct pollfd p = {.fd = gui_fd(), .events = POLLIN};
        struct gui_message e;

        if (x + dx < 0 || x + dx + BOX_SIZE > c->width)
            dx = -dx;
        if (y + dy < 0 || y + dy + BOX_SIZE > c->height)
            dy = -dy;
        x += dx;
        y += dy;
        draw_frame(c, x, y);
        gui_show(window);

        while (poll(&p, 1, FRAME_NS / 1000000) > 0) {
            if (!gui_next_event(&e) || e.type == GUI_CLOSE || (e.type == GUI_KEY && e.key.code == KEY_ESCAPE))
                return 0;
            if (e.type == GUI_RESIZE && gui_resize(window, e.width, e.height)) {
                x = x + BOX_SIZE > e.width ? 0 : x;
                y = y + BOX_SIZE > e.height ? 0 : y;
            }
        }
    }
}

// Bounces a box over a colour gradient until Ctrl+C, or in a window under wm.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: fbdemo\n"
                            "Bounces a box across the screen until Ctrl+C, or in a window if wm is running.\n");
    if (gui_connect())
        return bounce_in_window();

    int fd = open("/dev/fb0", O_RDWR);
    struct fb_info info;

    if (fd < 0 || fb_info(fd, &info) < 0) {
        dprintf(STDERR_FILENO, "fbdemo: /dev/fb0: %s\n", strerror(errno));
        return 1;
    }

    int w = info.width, h = info.height;
    uint32_t *back = malloc((size_t)w * h * sizeof(uint32_t));
    uint32_t *box = malloc(BOX_SIZE * BOX_SIZE * sizeof(uint32_t));

    if (!back || !box) {
        dprintf(STDERR_FILENO, "fbdemo: out of memory\n");
        return 1;
    }

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++)
            back[y * w + x] = gradient(x, y, w, h);
    }
    for (int i = 0; i < BOX_SIZE * BOX_SIZE; i++)
        box[i] = 0xFFFFFF;

    struct fb_rect whole = {.width = w, .height = h, .pixels = back};

    fb_draw(fd, &whole);

    int x = 0, y = 0, dx = 5, dy = 3;

    for (;;) {
        long frame_start = now_ns();

        // Puts back what the box covered, then draws it in its new place.
        struct fb_rect old = {x, y, BOX_SIZE, BOX_SIZE, w * sizeof(uint32_t), &back[y * w + x]};

        fb_draw(fd, &old);

        if (x + dx < 0 || x + dx + BOX_SIZE > w)
            dx = -dx;
        if (y + dy < 0 || y + dy + BOX_SIZE > h)
            dy = -dy;
        x += dx;
        y += dy;

        struct fb_rect moved = {x, y, BOX_SIZE, BOX_SIZE, 0, box};

        fb_draw(fd, &moved);

        long left = FRAME_NS - (now_ns() - frame_start);

        if (left > 0)
            nanosleep(&(struct timespec){0, left}, NULL);
    }
}
