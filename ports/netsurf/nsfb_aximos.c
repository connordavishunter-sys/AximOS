/* A libnsfb surface that's an AximOS window, drawn through libgui. */
#include <stdbool.h>
#include <stdlib.h>

#include <gui.h>
#include <mouse_event.h>

#include "libnsfb.h"
#include "libnsfb_event.h"
#include "libnsfb_plot.h"
#include "nsfb.h"
#include "plot.h"
#include "surface.h"
#include "cursor.h"

static struct gui_window *window;

// The wheel gives a press and a release of a mouse button, which go out one at a time.
static nsfb_event_t waiting;
static bool is_waiting;

// Scancode set 1, unshifted, as libnsfb's keys are SDL's: the frontend adds Shift.
static const unsigned short keys[0x60] = {
    [0x01] = NSFB_KEY_ESCAPE, [0x02] = '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=',
    NSFB_KEY_BACKSPACE, NSFB_KEY_TAB, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']',
    NSFB_KEY_RETURN, NSFB_KEY_LCTRL, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    NSFB_KEY_LSHIFT, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', NSFB_KEY_RSHIFT,
    [0x38] = NSFB_KEY_LALT, [0x39] = ' ', [0x3B] = NSFB_KEY_F1, NSFB_KEY_F2, NSFB_KEY_F3, NSFB_KEY_F4,
    NSFB_KEY_F5, NSFB_KEY_F6, NSFB_KEY_F7, NSFB_KEY_F8, NSFB_KEY_F9, NSFB_KEY_F10,
    [0x57] = NSFB_KEY_F11, NSFB_KEY_F12,
};

static int key_of(unsigned code) {
    switch (code) {
    case KEY_RIGHT_CTRL: return NSFB_KEY_RCTRL;
    case KEY_RIGHT_ALT: return NSFB_KEY_RALT;
    case KEY_HOME: return NSFB_KEY_HOME;
    case KEY_END: return NSFB_KEY_END;
    case KEY_UP: return NSFB_KEY_UP;
    case KEY_DOWN: return NSFB_KEY_DOWN;
    case KEY_LEFT: return NSFB_KEY_LEFT;
    case KEY_RIGHT: return NSFB_KEY_RIGHT;
    case KEY_PAGE_UP: return NSFB_KEY_PAGEUP;
    case KEY_PAGE_DOWN: return NSFB_KEY_PAGEDOWN;
    case KEY_INSERT: return NSFB_KEY_INSERT;
    case KEY_DELETE: return NSFB_KEY_DELETE;
    case (KEY_EXTENDED | 0x1C): return NSFB_KEY_RETURN; // the keypad's Enter
    default: return code < 0x60 && keys[code] ? keys[code] : NSFB_KEY_UNKNOWN;
    }
}

static void point_at_canvas(nsfb_t *nsfb) {
    nsfb->ptr = (uint8_t *)window->canvas.pixels;
    nsfb->width = window->canvas.width;
    nsfb->height = window->canvas.height;
    nsfb->linelen = window->canvas.stride * 4;
}

static int aximos_defaults(nsfb_t *nsfb) {
    nsfb->width = 1000;
    nsfb->height = 700;
    nsfb->format = NSFB_FMT_XRGB8888; // 0x00RRGGBB, as wm's pixels are
    select_plotters(nsfb);
    return 0;
}

static int aximos_initialise(nsfb_t *nsfb) {
    if (!gui_connect() || !(window = gui_create_window("NetSurf", nsfb->width, nsfb->height)))
        return -1;
    point_at_canvas(nsfb);
    return 0;
}

static int aximos_geometry(nsfb_t *nsfb, int width, int height, enum nsfb_format_e format) {
    (void)format;
    nsfb->format = NSFB_FMT_XRGB8888;
    select_plotters(nsfb);
    if (!window) { // not made yet: this is the size it'll have
        nsfb->width = width > 0 ? width : nsfb->width;
        nsfb->height = height > 0 ? height : nsfb->height;
        return 0;
    }
    if (!gui_resize(window, width, height))
        return -1;
    point_at_canvas(nsfb);
    return 0;
}

static int aximos_finalise(nsfb_t *nsfb) {
    (void)nsfb;
    if (window)
        gui_destroy_window(window);
    window = NULL;
    return 0;
}

static bool button(nsfb_event_t *event, uint32_t changed, uint32_t held) {
    event->type = changed & held ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
    event->value.keycode = changed & MOUSE_BUTTON_LEFT ? NSFB_KEY_MOUSE_1
                         : changed & MOUSE_BUTTON_MIDDLE ? NSFB_KEY_MOUSE_2 : NSFB_KEY_MOUSE_3;
    return true;
}

static bool aximos_input(nsfb_t *nsfb, nsfb_event_t *event, int timeout) {
    struct gui_message m;

    (void)nsfb;
    if (is_waiting) {
        *event = waiting;
        is_waiting = false;
        return true;
    }
    switch (gui_wait_event(&m, timeout)) {
    case 0:
        return false;
    case -1:
        m.type = GUI_CLOSE;
        break;
    }
    event->type = NSFB_EVENT_NONE;
    switch (m.type) {
    case GUI_KEY:
        event->type = m.key.pressed ? NSFB_EVENT_KEY_DOWN : NSFB_EVENT_KEY_UP;
        event->value.keycode = key_of(m.key.code);
        break;
    case GUI_MOUSE_MOVE:
        event->type = NSFB_EVENT_MOVE_ABSOLUTE;
        event->value.vector.x = m.x;
        event->value.vector.y = m.y;
        event->value.vector.z = 0;
        break;
    case GUI_MOUSE_BUTTON:
        return button(event, m.changed, m.buttons);
    case GUI_MOUSE_WHEEL:
        if (!m.wheel)
            break;
        event->type = NSFB_EVENT_KEY_DOWN;
        event->value.keycode = m.wheel < 0 ? NSFB_KEY_MOUSE_4 : NSFB_KEY_MOUSE_5;
        waiting = *event;
        waiting.type = NSFB_EVENT_KEY_UP;
        is_waiting = true;
        break;
    case GUI_RESIZE:
        event->type = NSFB_EVENT_RESIZE;
        event->value.resize.w = m.width;
        event->value.resize.h = m.height;
        break;
    case GUI_CLOSE:
        event->type = NSFB_EVENT_CONTROL;
        event->value.controlcode = NSFB_CONTROL_QUIT;
        break;
    case GUI_REDRAW:
        gui_show(window);
        break;
    }
    return true;
}

// wm draws the mouse cursor, so libnsfb's own is never plotted.
static int aximos_cursor(nsfb_t *nsfb, struct nsfb_cursor_s *cursor) {
    (void)nsfb, (void)cursor;
    return true;
}

static int aximos_claim(nsfb_t *nsfb, nsfb_bbox_t *box) {
    (void)nsfb, (void)box;
    return 0;
}

static int aximos_update(nsfb_t *nsfb, nsfb_bbox_t *box) {
    (void)nsfb;
    gui_update(window, box->x0, box->y0, box->x1 - box->x0, box->y1 - box->y0);
    return 0;
}

const nsfb_surface_rtns_t aximos_rtns = {
    .defaults = aximos_defaults,
    .initialise = aximos_initialise,
    .finalise = aximos_finalise,
    .geometry = aximos_geometry,
    .input = aximos_input,
    .claim = aximos_claim,
    .update = aximos_update,
    .cursor = aximos_cursor,
};

NSFB_SURFACE_DEF(aximos, NSFB_SURFACE_AXIMOS, &aximos_rtns)
