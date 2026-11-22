#include "evdev.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_DEVICES 16
#define E(code) (0x100 | (code)) // KEY_EXTENDED, which this can't include beside Linux's names
#define PIXELS_PER_MM 10         // how far a finger on a touchpad moves the pointer
#define BITS(n) ((n) / 8 + 1)
#define HAS(bits, n) ((bits)[(n) / 8] & 1 << (n) % 8)

static struct device {
    int fd;
    bool keyboard, touchpad;
    int resolution;           // a touchpad's, in units a millimetre
    int last[2], rest[2], held; // where the finger was, what's left of a pixel, and whether it's down
    struct mouse_event pending;
    bool moved;
} devices[MAX_DEVICES];
static int count;
static bool repeats = true;

// Linux's codes are scancode set 1's up to F12, and these others have 0xE0 first.
static uint16_t scancode(int key) {
    static const struct { uint16_t evdev, code; } extended[] = {
        {KEY_KPENTER, E(0x1C)}, {KEY_RIGHTCTRL, E(0x1D)}, {KEY_KPSLASH, E(0x35)}, {KEY_SYSRQ, E(0x37)},
        {KEY_RIGHTALT, E(0x38)}, {KEY_HOME, E(0x47)}, {KEY_UP, E(0x48)}, {KEY_PAGEUP, E(0x49)},
        {KEY_LEFT, E(0x4B)}, {KEY_RIGHT, E(0x4D)}, {KEY_END, E(0x4F)}, {KEY_DOWN, E(0x50)},
        {KEY_PAGEDOWN, E(0x51)}, {KEY_INSERT, E(0x52)}, {KEY_DELETE, E(0x53)}, {KEY_LEFTMETA, E(0x5B)},
        {KEY_RIGHTMETA, E(0x5C)}, {KEY_COMPOSE, E(0x5D)},
    };

    if (key <= KEY_F12)
        return key;
    for (size_t i = 0; i < sizeof(extended) / sizeof(extended[0]); i++) {
        if (extended[i].evdev == key)
            return extended[i].code;
    }
    return 0;
}

static void add(const char *path) {
    uint8_t types[BITS(EV_MAX)] = {0}, keys[BITS(KEY_MAX)] = {0}, rel[BITS(REL_MAX)] = {0}, abs[BITS(ABS_MAX)] = {0};
    uint8_t props[BITS(INPUT_PROP_MAX)] = {0};
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct input_absinfo x = {0};

    if (fd < 0)
        return;
    ioctl(fd, EVIOCGBIT(0, sizeof(types)), types);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
    ioctl(fd, EVIOCGPROP(sizeof(props)), props);

    bool keyboard = HAS(types, EV_KEY) && HAS(keys, KEY_A) && HAS(keys, KEY_Z);
    bool mouse = HAS(types, EV_REL) && HAS(rel, REL_X) && HAS(keys, BTN_LEFT);
    bool touchpad = HAS(types, EV_ABS) && HAS(abs, ABS_X) && HAS(keys, BTN_TOUCH) && !HAS(props, INPUT_PROP_DIRECT);

    if ((!keyboard && !mouse && !touchpad) || count == MAX_DEVICES || ioctl(fd, EVIOCGRAB, 1) < 0) {
        close(fd);
        return;
    }
    if (touchpad)
        ioctl(fd, EVIOCGABS(ABS_X), &x);
    devices[count++] = (struct device){.fd = fd, .keyboard = keyboard, .touchpad = touchpad,
                                       .resolution = x.resolution > 0 ? x.resolution : 40};
}

int evdev_open(void) {
    DIR *dir = opendir("/dev/input");
    struct dirent *d;
    char path[300];

    while (dir && (d = readdir(dir))) {
        if (!strncmp(d->d_name, "event", 5)) {
            snprintf(path, sizeof(path), "/dev/input/%s", d->d_name);
            add(path);
        }
    }
    if (dir)
        closedir(dir);
    return count;
}

int evdev_poll_fds(struct pollfd *fds, int max) {
    int n = count < max ? count : max;

    for (int i = 0; i < n; i++)
        fds[i] = (struct pollfd){.fd = devices[i].fd, .events = POLLIN};
    return n;
}

// A touchpad moves the pointer as far as the finger moves while it's down.
static void touch(struct device *d, int axis, int value) {
    int delta = value - d->last[axis], move;

    d->last[axis] = value;
    if (d->held < 2) // the first report after the finger lands is where it landed
        return;
    d->rest[axis] += delta * PIXELS_PER_MM;
    move = d->rest[axis] / d->resolution;
    d->rest[axis] -= move * d->resolution;
    *(axis == ABS_X ? &d->pending.dx : &d->pending.dy) += move;
    d->moved = true;
}

static void event(struct device *d, const struct input_event *e, const struct evdev_input *input) {
    static const struct { int evdev; uint32_t button; } buttons[] = {
        {BTN_LEFT, MOUSE_BUTTON_LEFT}, {BTN_RIGHT, MOUSE_BUTTON_RIGHT}, {BTN_MIDDLE, MOUSE_BUTTON_MIDDLE},
    };

    if (e->type == EV_KEY && e->code == BTN_TOUCH) {
        d->held = e->value ? 1 : 0;
        return;
    }
    for (size_t i = 0; e->type == EV_KEY && i < sizeof(buttons) / sizeof(buttons[0]); i++) {
        if (e->code == buttons[i].evdev) {
            d->pending.buttons = e->value ? d->pending.buttons | buttons[i].button : d->pending.buttons & ~buttons[i].button;
            d->moved = true;
            return;
        }
    }
    if (e->type == EV_KEY && d->keyboard && scancode(e->code) && (e->value < 2 || repeats))
        input->key(scancode(e->code), e->value != 0);
    else if (e->type == EV_REL && (e->code == REL_X || e->code == REL_Y)) {
        *(e->code == REL_X ? &d->pending.dx : &d->pending.dy) += e->value;
        d->moved = true;
    } else if (e->type == EV_REL && e->code == REL_WHEEL) {
        d->pending.wheel -= e->value; // up the wheel is away from the user
        d->moved = true;
    } else if (e->type == EV_ABS && d->touchpad && (e->code == ABS_X || e->code == ABS_Y))
        touch(d, e->code, e->value);
    else if (e->type == EV_SYN && e->code == SYN_REPORT) {
        d->held += d->held == 1;
        if (d->moved)
            input->mouse(&d->pending);
        d->pending.dx = d->pending.dy = d->pending.wheel = 0;
        d->moved = false;
    }
}

void evdev_read(const struct pollfd *fds, const struct evdev_input *input) {
    struct input_event events[64];

    for (int i = 0; i < count; i++) {
        ssize_t n = fds[i].revents & POLLIN ? read(devices[i].fd, events, sizeof(events)) : 0;

        for (ssize_t e = 0; e < n / (ssize_t)sizeof(*events); e++)
            event(&devices[i], &events[e], input);
    }
}

void evdev_set_repeat(int delay, int rate) {
    unsigned int times[2] = {delay, rate ? 1000 / rate : 1000};

    repeats = rate;
    for (int i = 0; i < count; i++) {
        if (devices[i].keyboard)
            ioctl(devices[i].fd, EVIOCSREP, times);
    }
}
