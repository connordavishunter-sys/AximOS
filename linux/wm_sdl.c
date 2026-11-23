#include "../user/wm/display.h"
#include <SDL.h>
#include <key_text.h>
#include <key_usage.h>
#include <stdlib.h>

// A window on the Linux desktop, through SDL: wm draws into memory, and what changed
// goes to the window's texture. Its pointer is wm's own, where the desktop's is.

#define START_WIDTH 1280
#define START_HEIGHT 800
#define EVENT_WAIT_MS 8 // SDL's events can't be polled for, so wm looks this often

static SDL_Window *window;
static SDL_Renderer *renderer;
static SDL_Texture *texture;
static uint32_t *pixels;
static int width;
static SDL_Rect dirty;

// Pixels as many as the window has, which is more than its size on a HiDPI screen.
static bool fit(struct screen *screen) {
    int w, h;
    SDL_Texture *t;
    uint32_t *p;

    SDL_GetRendererOutputSize(renderer, &w, &h);
    if (!(p = calloc((size_t)w * h, sizeof(*p))))
        return false;
    if (!(t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING, w, h))) {
        free(p);
        return false;
    }
    if (texture)
        SDL_DestroyTexture(texture);
    free(pixels);
    texture = t, pixels = p, width = w;
    *screen = (struct screen){(uint8_t *)p, w, h, w * (int)sizeof(*p), true};
    return true;
}

static bool open_display(struct screen *screen) {
    if (SDL_Init(SDL_INIT_VIDEO) < 0
        || !(window = SDL_CreateWindow("AximOS", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, START_WIDTH,
                                       START_HEIGHT, SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI))
        || !(renderer = SDL_CreateRenderer(window, -1, 0))) {
        fprintf(stderr, "wm: can't make a window: %s\n", SDL_GetError());
        return false;
    }
    SDL_ShowCursor(SDL_DISABLE); // wm draws its own
    return fit(screen);
}

static bool set_mode(struct screen *screen, int w, int h) {
    SDL_SetWindowSize(window, w, h);
    return fit(screen);
}

static void changed(int x, int y, int w, int h) {
    SDL_Rect r = {x, y, w, h};

    if (dirty.w)
        SDL_UnionRect(&dirty, &r, &r);
    dirty = r;
}

static void show(void) {
    if (dirty.w)
        SDL_UpdateTexture(texture, &dirty, pixels + (long)dirty.y * width + dirty.x, width * sizeof(*pixels));
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
    dirty = (SDL_Rect){0};
}

static int poll_fds(struct pollfd *fds) {
    (void)fds;
    return 0;
}

static uint8_t modifiers_of(SDL_Keymod mod) {
    return (mod & KMOD_SHIFT ? KEY_MOD_SHIFT : 0) | (mod & KMOD_CTRL ? KEY_MOD_CTRL : 0)
         | (mod & KMOD_ALT ? KEY_MOD_ALT : 0) | (mod & KMOD_GUI ? KEY_MOD_SUPER : 0)
         | (mod & KMOD_CAPS ? KEY_MOD_CAPS_LOCK : 0);
}

// Where the pointer is, as an absolute mouse says, with the buttons held and how
// far the wheel turned.
static void mouse(const struct input *input, int wheel) {
    int x, y, w, h;
    uint32_t held = SDL_GetMouseState(&x, &y);

    SDL_GetWindowSize(window, &w, &h);
    x = x < 0 ? 0 : x >= w ? w - 1 : x;
    y = y < 0 ? 0 : y >= h ? h - 1 : y;
    input->mouse(&(struct mouse_event){
        .wheel = wheel, .absolute = 1, .x = (int64_t)x * MOUSE_ABSOLUTE_MAX / w, .y = (int64_t)y * MOUSE_ABSOLUTE_MAX / h,
        .buttons = (held & SDL_BUTTON_LMASK ? MOUSE_BUTTON_LEFT : 0) | (held & SDL_BUTTON_RMASK ? MOUSE_BUTTON_RIGHT : 0)
                 | (held & SDL_BUTTON_MMASK ? MOUSE_BUTTON_MIDDLE : 0),
    });
}

static bool read_input(const struct pollfd *fds, struct screen *screen, const struct input *input) {
    SDL_Event e;
    bool resized = false;

    (void)fds;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT)
            input->quit();
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            resized |= fit(screen);
        else if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_EXPOSED)
            show();
        else if (e.type == SDL_MOUSEMOTION || e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP)
            mouse(input, 0);
        else if (e.type == SDL_MOUSEWHEEL)
            mouse(input, -e.wheel.y); // towards you is down, and positive, as on AximOS
        else if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP) {
            SDL_Scancode usage = e.key.keysym.scancode;
            uint16_t code = usage < sizeof(key_usages) / sizeof(key_usages[0]) ? key_usages[usage] : 0;
            uint8_t modifiers = modifiers_of(e.key.keysym.mod);
            bool pressed = e.type == SDL_KEYDOWN;

            if (code)
                input->key(&(struct key_event){.code = code, .pressed = pressed, .modifiers = modifiers,
                                                .text = pressed ? key_event_text(code, modifiers) : 0});
        }
    }
    return resized;
}

static void set_repeat(int delay, int rate) {
    (void)delay, (void)rate; // the desktop's own repeat
}

const struct display wm_sdl = {open_display, set_mode, changed, show, poll_fds, read_input, set_repeat, EVENT_WAIT_MS,
                               NULL};
