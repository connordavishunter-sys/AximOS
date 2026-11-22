#include "../user/wm/display.h"
#include "evdev.h"
#include <errno.h>
#include <fcntl.h>
#include <key_text.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

// The screen itself, through DRM: a buffer the graphics card shows as it is, with
// the keyboards and mice from evdev. It runs from a text console, which it keeps
// from drawing over it, and puts back as it was when it stops.

#define MAX_CARDS 8

static int card = -1;
static uint32_t connector, crtc, fb, handle;
static drmModeCrtc *before; // what was on the screen, to put back
static uint8_t *buffer, *drawn; // what's shown, which is slow to touch, and what wm draws on
static size_t size, pitch;
static drmModeClip dirty;
static bool console;
static uint8_t modifiers;
static const struct input *taking;

static void put_back(void) {
    if (before)
        drmModeSetCrtc(card, before->crtc_id, before->buffer_id, before->x, before->y, &connector, 1, &before->mode);
    if (console)
        ioctl(STDIN_FILENO, KDSETMODE, KD_TEXT);
    before = NULL;
    console = false;
}

static void put_back_and_die(int signal) {
    put_back();
    raise(signal); // as SA_RESETHAND left it
}

static void free_buffer(void) {
    if (buffer)
        munmap(buffer, size);
    free(drawn);
    drawn = NULL;
    if (fb)
        drmModeRmFB(card, fb);
    if (handle)
        drmIoctl(card, DRM_IOCTL_MODE_DESTROY_DUMB, &(struct drm_mode_destroy_dumb){.handle = handle});
    buffer = NULL, fb = handle = 0;
}

// A buffer the mode's size, shown on the screen. wm draws in ordinary memory, and
// what changes is copied to it, as the card's memory is slow to write and far
// slower to read.
static bool show_mode(drmModeModeInfo *mode, struct screen *screen) {
    struct drm_mode_create_dumb create = {.width = mode->hdisplay, .height = mode->vdisplay, .bpp = 32};
    struct drm_mode_map_dumb map = {0};

    free_buffer();
    if (drmIoctl(card, DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        return false;
    handle = map.handle = create.handle;
    size = create.size;
    pitch = create.pitch;
    if (drmModeAddFB(card, create.width, create.height, 24, 32, create.pitch, handle, &fb) < 0
        || drmIoctl(card, DRM_IOCTL_MODE_MAP_DUMB, &map) < 0
        || (buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, card, map.offset)) == MAP_FAILED
        || !(drawn = calloc(1, size))) {
        buffer = buffer == MAP_FAILED ? NULL : buffer;
        free_buffer();
        return false;
    }
    memset(buffer, 0, size);
    if (drmModeSetCrtc(card, crtc, fb, 0, 0, &connector, 1, mode) < 0) {
        perror("wm: can't take the screen, which something else may have");
        free_buffer();
        return false;
    }
    *screen = (struct screen){drawn, create.width, create.height, create.pitch, true};
    return true;
}

// The mode a size asks for at the fastest refresh, or with no size, the screen's own.
static bool find_mode(int width, int height, drmModeModeInfo *out) {
    drmModeConnector *c = drmModeGetConnector(card, connector);
    int best = -1;

    for (int i = 0; c && i < c->count_modes; i++) {
        drmModeModeInfo *m = &c->modes[i];
        bool fits = width ? m->hdisplay == width && m->vdisplay == height : m->type & DRM_MODE_TYPE_PREFERRED;

        if (fits && (best < 0 || m->vrefresh > c->modes[best].vrefresh))
            best = i;
    }
    if (c && best < 0 && !width && c->count_modes)
        best = 0;
    if (best >= 0)
        *out = c->modes[best];
    drmModeFreeConnector(c);
    return best >= 0;
}

// The first card with a screen plugged in, and a CRTC that can drive it.
static bool find_screen(void) {
    char path[32];

    for (int n = 0; n < MAX_CARDS && !crtc; n++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", n);
        if ((card = open(path, O_RDWR | O_CLOEXEC)) < 0)
            continue;

        drmModeRes *res = drmModeGetResources(card);

        for (int i = 0; res && i < res->count_connectors && !crtc; i++) {
            drmModeConnector *c = drmModeGetConnector(card, res->connectors[i]);

            for (int e = 0; c && c->connection == DRM_MODE_CONNECTED && c->count_modes && e < c->count_encoders && !crtc;
                 e++) {
                drmModeEncoder *encoder = drmModeGetEncoder(card, c->encoders[e]);

                for (int k = 0; encoder && k < res->count_crtcs && !crtc; k++) {
                    if (encoder->possible_crtcs & 1 << k)
                        crtc = res->crtcs[k], connector = c->connector_id;
                }
                drmModeFreeEncoder(encoder);
            }
            drmModeFreeConnector(c);
        }
        drmModeFreeResources(res);
        if (!crtc)
            close(card);
    }
    return crtc;
}

static bool open_display(struct screen *screen) {
    drmModeModeInfo mode;

    if (!find_screen()) {
        fprintf(stderr, "wm: no screen in /dev/dri to show it on\n");
        return false;
    }
    before = drmModeGetCrtc(card, crtc);
    atexit(put_back);
    for (int s = 0; s < 4; s++) {
        int signals[] = {SIGINT, SIGTERM, SIGSEGV, SIGABRT};

        sigaction(signals[s], &(struct sigaction){.sa_handler = put_back_and_die, .sa_flags = SA_RESETHAND}, NULL);
    }
    console = isatty(STDIN_FILENO) && ioctl(STDIN_FILENO, KDSETMODE, KD_GRAPHICS) == 0;
    if (!find_mode(0, 0, &mode) || !show_mode(&mode, screen))
        return false;

    // Only once it has the screen, so it can't take the keyboard from whatever has it.
    if (!evdev_open()) {
        fprintf(stderr, "wm: can't read /dev/input, which needs root or the input group\n");
        return false;
    }
    return true;
}

static bool set_mode(struct screen *screen, int width, int height) {
    drmModeModeInfo mode;

    return find_mode(width, height, &mode) && show_mode(&mode, screen);
}

static void changed(int x, int y, int width, int height) {
    if (dirty.x2 <= dirty.x1)
        dirty = (drmModeClip){x, y, x + width, y + height};
    dirty = (drmModeClip){x < dirty.x1 ? x : dirty.x1, y < dirty.y1 ? y : dirty.y1,
                          x + width > dirty.x2 ? x + width : dirty.x2, y + height > dirty.y2 ? y + height : dirty.y2};
}

// Most cards show the buffer as it's written, but some need telling.
static void show(void) {
    for (int y = dirty.y1; y < dirty.y2; y++)
        memcpy(buffer + y * pitch + dirty.x1 * 4, drawn + y * pitch + dirty.x1 * 4, (dirty.x2 - dirty.x1) * 4);
    drmModeDirtyFB(card, fb, &dirty, 1);
    dirty = (drmModeClip){0};
}

static int poll_fds(struct pollfd *fds) {
    return evdev_poll_fds(fds, DISPLAY_FDS);
}

static void key(uint16_t code, bool pressed) {
    static const struct { uint16_t code; uint8_t bit; } held[] = {
        {KEY_LEFT_SHIFT, KEY_MOD_SHIFT}, {KEY_RIGHT_SHIFT, KEY_MOD_SHIFT}, {KEY_LEFT_CTRL, KEY_MOD_CTRL},
        {KEY_RIGHT_CTRL, KEY_MOD_CTRL}, {KEY_LEFT_ALT, KEY_MOD_ALT}, {KEY_RIGHT_ALT, KEY_MOD_ALT},
        {KEY_LEFT_SUPER, KEY_MOD_SUPER}, {KEY_RIGHT_SUPER, KEY_MOD_SUPER},
    };

    for (size_t i = 0; i < sizeof(held) / sizeof(held[0]); i++) {
        if (held[i].code == code)
            modifiers = pressed ? modifiers | held[i].bit : modifiers & ~held[i].bit;
    }
    if (code == KEY_CAPS_LOCK && pressed)
        modifiers ^= KEY_MOD_CAPS_LOCK;
    if (code == KEY_PRINT_SCREEN && modifiers & KEY_MOD_ALT) // as AximOS's keyboards send it
        code = KEY_SYSRQ;
    taking->key(&(struct key_event){.code = code, .pressed = pressed, .modifiers = modifiers,
                                    .text = pressed ? key_event_text(code, modifiers) : 0});
}

static bool read_input(const struct pollfd *fds, struct screen *screen, const struct input *input) {
    (void)screen;
    taking = input;
    evdev_read(fds, &(struct evdev_input){input->mouse, key});
    return false;
}

const struct display wm_drm = {open_display, set_mode, changed, show, poll_fds, read_input, evdev_set_repeat, -1};
