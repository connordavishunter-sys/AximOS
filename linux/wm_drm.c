#include "../user/wm/display.h"
#include "evdev.h"
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <errno.h>
#include <key_text.h>
#include <libseat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

// The screen itself, through DRM: a buffer the graphics card shows as it is, with
// the keyboards and mice from evdev. They come from libseat, through seatd, or
// logind on another Linux, which gives them to the session whose console is showing
// and takes them away while another's is, putting each console back as it was.

#define MAX_CARDS 8
#define MAX_ITEMS 64
#define MAX_MODES 128

static struct libseat *seat;
static bool active, reopen; // whether the seat's ours, and the input has to be opened again
static int card = -1;
static uint32_t connector, crtc, fb, handle;
static struct drm_mode_modeinfo mode;
static uint8_t *buffer, *drawn; // what's shown, which is slow to touch, and what wm draws on
static size_t size, pitch;
static struct drm_clip_rect dirty;
static uint8_t modifiers;
static const struct input *taking;

static int drm(unsigned long request, void *argument) {
    int result;

    while ((result = ioctl(card, request, argument)) < 0 && (errno == EINTR || errno == EAGAIN))
        ;
    return result;
}

// A connector's modes and encoders, asking once for how many, then for them.
static bool get_connector(uint32_t id, struct drm_mode_get_connector *c, struct drm_mode_modeinfo *modes,
                          uint32_t *encoders) {
    *c = (struct drm_mode_get_connector){.connector_id = id};
    if (drm(DRM_IOCTL_MODE_GETCONNECTOR, c) < 0)
        return false;
    c->count_modes = c->count_modes < MAX_MODES ? c->count_modes : MAX_MODES;
    c->count_encoders = c->count_encoders < MAX_ITEMS ? c->count_encoders : MAX_ITEMS;
    c->count_props = 0;
    c->modes_ptr = (uintptr_t)modes;
    c->encoders_ptr = (uintptr_t)encoders;
    return drm(DRM_IOCTL_MODE_GETCONNECTOR, c) == 0;
}

static void free_buffer(void) {
    if (buffer)
        munmap(buffer, size);
    free(drawn);
    if (fb)
        drm(DRM_IOCTL_MODE_RMFB, &fb);
    if (handle)
        drm(DRM_IOCTL_MODE_DESTROY_DUMB, &(struct drm_mode_destroy_dumb){.handle = handle});
    buffer = drawn = NULL, fb = handle = 0;
}

static bool set_crtc(void) {
    struct drm_mode_crtc c = {.crtc_id = crtc, .fb_id = fb, .set_connectors_ptr = (uintptr_t)&connector,
                              .count_connectors = 1, .mode = mode, .mode_valid = 1};

    return drm(DRM_IOCTL_MODE_SETCRTC, &c) == 0;
}

// A buffer the mode's size, shown on the screen. wm draws in ordinary memory, and
// what changes is copied to it, as the card's memory is slow to write and far
// slower to read.
static bool show_mode(const struct drm_mode_modeinfo *m, struct screen *screen) {
    struct drm_mode_create_dumb create = {.width = m->hdisplay, .height = m->vdisplay, .bpp = 32};
    struct drm_mode_map_dumb map = {0};
    struct drm_mode_fb_cmd add = {.width = m->hdisplay, .height = m->vdisplay, .bpp = 32, .depth = 24};

    free_buffer();
    if (drm(DRM_IOCTL_MODE_CREATE_DUMB, &create) < 0)
        return false;
    handle = map.handle = add.handle = create.handle;
    size = create.size;
    pitch = add.pitch = create.pitch;
    if (drm(DRM_IOCTL_MODE_ADDFB, &add) < 0 || (fb = add.fb_id, drm(DRM_IOCTL_MODE_MAP_DUMB, &map) < 0)
        || (buffer = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, card, map.offset)) == MAP_FAILED
        || !(drawn = calloc(1, size))) {
        buffer = buffer == MAP_FAILED ? NULL : buffer;
        free_buffer();
        return false;
    }
    memset(buffer, 0, size);
    mode = *m;
    if (!set_crtc()) {
        perror("wm: can't show the screen");
        free_buffer();
        return false;
    }
    *screen = (struct screen){drawn, create.width, create.height, create.pitch, true};
    return true;
}

// The mode a size asks for at the fastest refresh, or with no size, the screen's own.
static bool find_mode(int width, int height, struct drm_mode_modeinfo *out) {
    struct drm_mode_get_connector c;
    struct drm_mode_modeinfo modes[MAX_MODES];
    uint32_t encoders[MAX_ITEMS];
    int best = -1;

    if (!get_connector(connector, &c, modes, encoders))
        return false;
    for (int i = 0; i < (int)c.count_modes; i++) {
        bool fits = width ? modes[i].hdisplay == width && modes[i].vdisplay == height
                          : modes[i].type & DRM_MODE_TYPE_PREFERRED;

        if (fits && (best < 0 || modes[i].vrefresh > modes[best].vrefresh))
            best = i;
    }
    best = best < 0 && !width && c.count_modes ? 0 : best;
    if (best >= 0)
        *out = modes[best];
    return best >= 0;
}

// A CRTC that can drive a connector with a screen plugged in, on this card.
static bool find_crtc(void) {
    struct drm_mode_card_res res = {0};
    uint32_t connectors[MAX_ITEMS], crtcs[MAX_ITEMS];

    if (drm(DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return false;
    res.count_connectors = res.count_connectors < MAX_ITEMS ? res.count_connectors : MAX_ITEMS;
    res.count_crtcs = res.count_crtcs < MAX_ITEMS ? res.count_crtcs : MAX_ITEMS;
    res.count_fbs = res.count_encoders = 0;
    res.connector_id_ptr = (uintptr_t)connectors;
    res.crtc_id_ptr = (uintptr_t)crtcs;
    if (drm(DRM_IOCTL_MODE_GETRESOURCES, &res) < 0)
        return false;
    for (uint32_t i = 0; i < res.count_connectors; i++) {
        struct drm_mode_get_connector c;
        struct drm_mode_modeinfo modes[MAX_MODES];
        uint32_t encoders[MAX_ITEMS];

        if (!get_connector(connectors[i], &c, modes, encoders) || c.connection != 1 || !c.count_modes)
            continue; // 1 is connected
        for (uint32_t e = 0; e < c.count_encoders; e++) {
            struct drm_mode_get_encoder encoder = {.encoder_id = encoders[e]};

            for (uint32_t k = 0; drm(DRM_IOCTL_MODE_GETENCODER, &encoder) == 0 && k < res.count_crtcs; k++) {
                if (encoder.possible_crtcs & 1 << k) {
                    crtc = crtcs[k], connector = c.connector_id;
                    return true;
                }
            }
        }
    }
    return false;
}

static int open_device(const char *path, int *id) {
    int fd = -1;

    *id = libseat_open_device(seat, path, &fd);
    return *id < 0 ? -1 : fd;
}

static void close_device(int id) {
    libseat_close_device(seat, id);
}

static bool find_screen(void) {
    char path[32];
    int id;

    for (int n = 0; n < MAX_CARDS; n++) {
        snprintf(path, sizeof(path), "/dev/dri/card%d", n);
        if ((card = open_device(path, &id)) < 0)
            continue;
        if (find_crtc())
            return true;
        close_device(id);
    }
    return false;
}

// Coming back, the screen's shown as it was, and the input's opened again, as it
// was taken away.
static void enable_seat(struct libseat *s, void *unused) {
    (void)s, (void)unused;
    active = reopen = true;
    if (buffer && set_crtc())
        memcpy(buffer, drawn, size);
}

static void disable_seat(struct libseat *s, void *unused) {
    (void)unused;
    active = false;
    evdev_close();
    libseat_disable_seat(s);
}

static bool open_display(struct screen *screen) {
    static struct libseat_seat_listener listener = {enable_seat, disable_seat};
    struct drm_mode_modeinfo m;

    libseat_set_log_level(LIBSEAT_LOG_LEVEL_ERROR);
    if (!(seat = libseat_open_seat(&listener, NULL))) {
        fprintf(stderr, "wm: no seat to show it on, so seatd isn't running, or it's not ours\n");
        return false;
    }
    while (!active && libseat_dispatch(seat, -1) >= 0)
        ;
    if (!find_screen()) {
        fprintf(stderr, "wm: no screen in /dev/dri to show it on\n");
        return false;
    }
    if (!find_mode(0, 0, &m) || !show_mode(&m, screen))
        return false;
    reopen = false;
    if (!evdev_open(open_device, close_device))
        fprintf(stderr, "wm: no keyboard or mouse in /dev/input\n");
    return true;
}

static bool set_mode(struct screen *screen, int width, int height) {
    struct drm_mode_modeinfo m;

    return active && find_mode(width, height, &m) && show_mode(&m, screen);
}

static void changed(int x, int y, int width, int height) {
    if (dirty.x2 <= dirty.x1)
        dirty = (struct drm_clip_rect){x, y, x + width, y + height};
    dirty = (struct drm_clip_rect){x < dirty.x1 ? x : dirty.x1, y < dirty.y1 ? y : dirty.y1,
                                   x + width > dirty.x2 ? x + width : dirty.x2, y + height > dirty.y2 ? y + height : dirty.y2};
}

// Most cards show the buffer as it's written, but some need telling.
static void show(void) {
    struct drm_mode_fb_dirty_cmd tell = {.fb_id = fb, .num_clips = 1, .clips_ptr = (uintptr_t)&dirty};

    for (int y = dirty.y1; active && y < dirty.y2; y++)
        memcpy(buffer + y * pitch + dirty.x1 * 4, drawn + y * pitch + dirty.x1 * 4, (dirty.x2 - dirty.x1) * 4);
    if (active)
        drm(DRM_IOCTL_MODE_DIRTYFB, &tell);
    dirty = (struct drm_clip_rect){0};
}

// The seat's own file comes first, then the input's, while the seat's ours.
static int poll_fds(struct pollfd *fds) {
    fds[0] = (struct pollfd){.fd = libseat_get_fd(seat), .events = POLLIN};
    return 1 + (active && !reopen ? evdev_poll_fds(fds + 1, DISPLAY_FDS - 1) : 0);
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
    bool was_reopening = reopen;

    (void)screen;
    taking = input;
    if (fds[0].revents & POLLIN)
        libseat_dispatch(seat, 0);
    if (active && reopen) {
        modifiers = 0; // what was held went with the seat
        evdev_open(open_device, close_device);
        reopen = false;
    } else if (active && !was_reopening) {
        evdev_read(fds + 1, &(struct evdev_input){input->mouse, key});
    }
    return false;
}

static void switch_session(int number) {
    libseat_switch_session(seat, number);
}

const struct display wm_drm = {open_display, set_mode, changed, show, poll_fds, read_input, evdev_set_repeat, -1,
                               switch_session};
