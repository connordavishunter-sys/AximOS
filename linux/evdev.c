#include "evdev.h"
#include <dirent.h>
#include <errno.h>
#include <linux/input.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_DEVICES 16
#define E(code) (0x100 | (code)) // KEY_EXTENDED, which this can't include beside Linux's names
#define PIXELS_PER_MM 10         // how far a finger on a touchpad moves the pointer
#define BITS(n) ((n) / 8 + 1)
#define HAS(bits, n) ((bits)[(n) / 8] & 1 << (n) % 8)

static struct device {
    int fd, id;
    bool keyboard, touchpad, absolute; // a touchpad moves the pointer, and a tablet places it
    int resolution;           // a touchpad's, in units a millimetre
    int last[2], rest[2], held; // where the finger was, what's left of a pixel, and whether it's down
    int min[2], range[2]; // an absolute pointer's axes
    struct mouse_event pending;
    bool moved;
} devices[MAX_DEVICES];
static int count, watch = -1; // watching /dev/input for what's plugged in later
static bool repeats = true;
static unsigned int repeat_times[2] = {500, 33};
static int (*open_device)(const char *path, int *id);
static void (*close_device)(int id);

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
    struct input_absinfo axes[2] = {0};
    int id, fd = open_device(path, &id);

    if (fd < 0)
        return;
    ioctl(fd, EVIOCGBIT(0, sizeof(types)), types);
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);
    ioctl(fd, EVIOCGBIT(EV_REL, sizeof(rel)), rel);
    ioctl(fd, EVIOCGBIT(EV_ABS, sizeof(abs)), abs);
    ioctl(fd, EVIOCGPROP(sizeof(props)), props);

    // A touchpad says where the finger is, but the pointer moves as far as it does.
    // A tablet, or a touchscreen, says where the pointer is.
    bool pointing = HAS(types, EV_ABS) && HAS(abs, ABS_X) && HAS(abs, ABS_Y);
    bool keyboard = HAS(types, EV_KEY) && HAS(keys, KEY_A) && HAS(keys, KEY_Z);
    bool mouse = HAS(types, EV_REL) && HAS(rel, REL_X) && HAS(keys, BTN_LEFT);
    bool touchpad = pointing && HAS(keys, BTN_TOUCH) && !HAS(props, INPUT_PROP_DIRECT);
    bool absolute = pointing && !touchpad && (HAS(keys, BTN_LEFT) || HAS(keys, BTN_TOUCH));

    if ((!keyboard && !mouse && !touchpad && !absolute) || count == MAX_DEVICES) {
        close_device(id);
        return;
    }
    ioctl(fd, EVIOCGABS(ABS_X), &axes[0]);
    ioctl(fd, EVIOCGABS(ABS_Y), &axes[1]);
    devices[count] = (struct device){.fd = fd, .id = id, .keyboard = keyboard, .touchpad = touchpad, .absolute = absolute,
                                     .resolution = axes[0].resolution > 0 ? axes[0].resolution : 40};
    for (int axis = 0; axis < 2; axis++) {
        devices[count].min[axis] = axes[axis].minimum;
        devices[count].range[axis] = axes[axis].maximum > axes[axis].minimum ? axes[axis].maximum - axes[axis].minimum : 1;
    }
    if (keyboard)
        ioctl(fd, EVIOCSREP, repeat_times);
    count++;
}

static void add_named(const char *name) {
    char path[300];

    if (!strncmp(name, "event", 5)) {
        snprintf(path, sizeof(path), "/dev/input/%s", name);
        add(path);
    }
}

int evdev_open(int (*open)(const char *path, int *id), void (*close)(int id)) {
    DIR *dir;
    struct dirent *d;

    open_device = open, close_device = close;
    watch = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (watch >= 0)
        inotify_add_watch(watch, "/dev/input", IN_CREATE);
    if ((dir = opendir("/dev/input"))) {
        while ((d = readdir(dir)))
            add_named(d->d_name);
        closedir(dir);
    }
    return count;
}

void evdev_close(void) {
    while (count)
        close_device(devices[--count].id);
    if (watch >= 0)
        close(watch);
    watch = -1;
}

// What's been plugged in since.
static void added(void) {
    char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n;

    while ((n = read(watch, buffer, sizeof(buffer))) > 0) {
        for (char *at = buffer; at < buffer + n; at += sizeof(struct inotify_event) + ((struct inotify_event *)at)->len) {
            struct inotify_event *e = (struct inotify_event *)at;

            if (e->len)
                add_named(e->name);
        }
    }
}

// The watch on /dev/input first, then each device.
int evdev_poll_fds(struct pollfd *fds, int max) {
    int n = count < max - 1 ? count : max - 1;

    fds[0] = (struct pollfd){.fd = watch, .events = POLLIN};
    for (int i = 0; i < n; i++)
        fds[1 + i] = (struct pollfd){.fd = devices[i].fd, .events = POLLIN};
    return 1 + n;
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

    if (e->type == EV_KEY && e->code == BTN_TOUCH && !d->absolute) {
        d->held = e->value ? 1 : 0;
        return;
    }
    if (e->type == EV_KEY && e->code == BTN_TOUCH) { // a touchscreen's touch is a click
        d->pending.buttons = e->value ? d->pending.buttons | MOUSE_BUTTON_LEFT : d->pending.buttons & ~MOUSE_BUTTON_LEFT;
        d->moved = true;
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
    } else if (e->type == EV_ABS && d->touchpad && (e->code == ABS_X || e->code == ABS_Y)) {
        touch(d, e->code, e->value);
    } else if (e->type == EV_ABS && d->absolute && (e->code == ABS_X || e->code == ABS_Y)) {
        *(e->code == ABS_X ? &d->pending.x : &d->pending.y) =
            (int64_t)(e->value - d->min[e->code]) * MOUSE_ABSOLUTE_MAX / d->range[e->code];
        d->pending.absolute = 1;
        d->moved = true;
    }
    else if (e->type == EV_SYN && e->code == SYN_REPORT) {
        d->held += d->held == 1;
        if (d->moved)
            input->mouse(&d->pending);
        d->pending.dx = d->pending.dy = d->pending.wheel = 0;
        d->moved = false;
    }
}

// A device that's been unplugged has gone, and the last takes its place.
void evdev_read(const struct pollfd *fds, const struct evdev_input *input) {
    struct input_event events[64];
    int polled = count;

    for (int i = polled - 1; i >= 0; i--) {
        ssize_t n = fds[1 + i].revents & (POLLIN | POLLERR | POLLHUP) ? read(devices[i].fd, events, sizeof(events)) : 0;

        for (ssize_t e = 0; e < n / (ssize_t)sizeof(*events); e++)
            event(&devices[i], &events[e], input);
        if (n < 0 && errno == ENODEV) {
            close_device(devices[i].id);
            devices[i] = devices[--count];
        }
    }
    if (fds[0].revents & POLLIN)
        added();
}

void evdev_set_repeat(int delay, int rate) {
    repeat_times[0] = delay, repeat_times[1] = rate ? 1000 / rate : 1000;
    repeats = rate;
    for (int i = 0; i < count; i++) {
        if (devices[i].keyboard)
            ioctl(devices[i].fd, EVIOCSREP, repeat_times);
    }
}
