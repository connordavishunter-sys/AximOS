#include "display.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/fb.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// AximOS's own: /dev/fb0, mapped and drawn on, and /dev/mouse and /dev/keyboard.

static int fb = -1, mouse = -1, keyboard = -1;

static bool map(struct screen *screen) {
    struct fb_info info;
    void *pixels;

    if (fb_info(fb, &info) < 0
        || (pixels = mmap(NULL, (size_t)info.pitch * info.height, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0))
               == MAP_FAILED)
        return false;
    *screen = (struct screen){pixels, info.width, info.height, info.pitch, info.can_set_mode};
    return true;
}

static bool open_display(struct screen *screen) {
    fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    mouse = open("/dev/mouse", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    keyboard = open("/dev/keyboard", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fb < 0 || mouse < 0 || keyboard < 0) {
        perror(fb < 0 ? "wm: /dev/fb0" : mouse < 0 ? "wm: /dev/mouse" : "wm: /dev/keyboard");
        return false;
    }
    return map(screen);
}

static bool set_mode(struct screen *screen, int width, int height) {
    struct screen was = *screen;

    if (fb_set_mode(fb, width, height) < 0)
        return false;
    munmap(was.pixels, (size_t)was.pitch * was.height);
    return map(screen);
}

static void changed(int x, int y, int width, int height) {
    (void)x, (void)y, (void)width, (void)height;
}

static void show(void) {
}

static int poll_fds(struct pollfd *fds) {
    fds[0] = (struct pollfd){.fd = mouse, .events = POLLIN};
    fds[1] = (struct pollfd){.fd = keyboard, .events = POLLIN};
    return 2;
}

static bool read_input(const struct pollfd *fds, struct screen *screen, const struct input *input) {
    struct mouse_event mice[32];
    struct key_event keys[32];
    ssize_t n = fds[0].revents & POLLIN ? read(mouse, mice, sizeof(mice)) : 0;

    (void)screen;
    for (ssize_t i = 0; i < n / (ssize_t)sizeof(*mice); i++)
        input->mouse(&mice[i]);
    n = fds[1].revents & POLLIN ? read(keyboard, keys, sizeof(keys)) : 0;
    for (ssize_t i = 0; i < n / (ssize_t)sizeof(*keys); i++)
        input->key(&keys[i]);
    return false;
}

static void set_repeat(int delay, int rate) {
    ioctl(keyboard, KEYBOARD_SET_REPEAT_DELAY, &delay);
    ioctl(keyboard, KEYBOARD_SET_REPEAT_RATE, &rate);
}

const struct display *display_choose(int *argc, char ***argv) {
    static const struct display aximos = {open_display, set_mode, changed, show, poll_fds, read_input, set_repeat, -1};

    (void)argc, (void)argv;
    return &aximos;
}
