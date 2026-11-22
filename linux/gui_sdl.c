#include "../user/libgui/backend.h"
#include <SDL.h>
#include <errno.h>
#include <key_text.h>
#include <key_usage.h>
#include <mouse_event.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// libgui on Linux: each window is an SDL window, whose texture gets what changed on
// the canvas, and SDL's events become the messages wm would send. What only the
// desktop does, such as notifications, is printed, or left undone.

#define MAX_WINDOWS 16

static struct gui_window *windows[MAX_WINDOWS];
static SDL_Window *sdl_windows[MAX_WINDOWS];
static SDL_Renderer *renderers[MAX_WINDOWS];
static SDL_Texture *textures[MAX_WINDOWS];
static char dropped[GUI_PATH_MAX];

static int slot_of(const struct gui_window *window) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i] == window)
            return i;
    }
    return -1;
}

static int slot_of_sdl(uint32_t id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (sdl_windows[i] && SDL_GetWindowID(sdl_windows[i]) == id)
            return i;
    }
    return -1;
}

bool gui_connect(void) {
    return SDL_Init(SDL_INIT_VIDEO) == 0;
}

int gui_fd(void) {
    return -1;
}

bool gui_send(int fd, const struct gui_message *message) {
    (void)fd, (void)message;
    return true;
}

bool gui_receive(int fd, struct gui_message *message) {
    (void)fd, (void)message;
    return false;
}

static bool new_texture(int slot, int width, int height) {
    SDL_Texture *texture = SDL_CreateTexture(renderers[slot], SDL_PIXELFORMAT_RGB888, SDL_TEXTUREACCESS_STREAMING,
                                             width, height);

    if (!texture)
        return false;
    if (textures[slot])
        SDL_DestroyTexture(textures[slot]);
    textures[slot] = texture;
    return true;
}

static bool new_canvas(struct gui_window *window, int width, int height) {
    uint32_t *pixels = calloc((size_t)width * height, sizeof(uint32_t));

    if (!pixels)
        return false;
    free(window->canvas.pixels);
    window->canvas = (struct gui_canvas){.pixels = pixels, .width = width, .height = height, .stride = width};
    return true;
}

struct gui_window *gui_create_window(const char *title, int width, int height) {
    int slot = slot_of(NULL);
    struct gui_window *window = slot < 0 ? NULL : calloc(1, sizeof(*window));
    SDL_DisplayMode mode;

    if (!window || !new_canvas(window, width, height)
        || !(sdl_windows[slot] = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, width, height,
                                                  SDL_WINDOW_RESIZABLE))
        || !(renderers[slot] = SDL_CreateRenderer(sdl_windows[slot], -1, 0)) || !new_texture(slot, width, height)) {
        fprintf(stderr, "gui: can't open a window: %s\n", SDL_GetError());
        if (window)
            free(window->canvas.pixels);
        free(window);
        return NULL;
    }
    window->id = slot + 1;
    windows[slot] = window;
    gui_set_icon(window, program_invocation_short_name);

    // What wm says after making a window.
    SDL_GetCurrentDisplayMode(0, &mode);
    gui_post(&(struct gui_message){.type = GUI_SCREEN, .window = window->id, .width = mode.w, .height = mode.h, .y = 1});
    return window;
}

bool gui_resize(struct gui_window *window, int width, int height) {
    return new_canvas(window, width, height) && new_texture(slot_of(window), width, height);
}

void gui_destroy_window(struct gui_window *window) {
    int slot = slot_of(window);

    SDL_DestroyTexture(textures[slot]);
    SDL_DestroyRenderer(renderers[slot]);
    SDL_DestroyWindow(sdl_windows[slot]);
    windows[slot] = NULL;
    sdl_windows[slot] = NULL;
    textures[slot] = NULL;
    free(window->canvas.pixels);
    free(window);
}

static void present(int slot) {
    SDL_RenderClear(renderers[slot]);
    SDL_RenderCopy(renderers[slot], textures[slot], NULL, NULL);
    SDL_RenderPresent(renderers[slot]);
}

void gui_update(struct gui_window *window, int x, int y, int width, int height) {
    const struct gui_canvas *c = &window->canvas;
    int slot = slot_of(window);
    SDL_Rect r = {x < 0 ? 0 : x, y < 0 ? 0 : y, 0, 0};

    r.w = (x + width < c->width ? x + width : c->width) - r.x;
    r.h = (y + height < c->height ? y + height : c->height) - r.y;
    if (r.w > 0 && r.h > 0)
        SDL_UpdateTexture(textures[slot], &r, c->pixels + (long)r.y * c->stride + r.x, c->stride * sizeof(uint32_t));
    present(slot);
}

void gui_show(struct gui_window *window) {
    gui_update(window, 0, 0, window->canvas.width, window->canvas.height);
}

void gui_set_title(struct gui_window *window, const char *title) {
    SDL_SetWindowTitle(sdl_windows[slot_of(window)], title);
}

void gui_set_icon(struct gui_window *window, const char *name) {
    char path[GUI_PATH_MAX];
    struct gui_image icon;

    snprintf(path, sizeof(path), GUI_ROOT "/system/share/icons/apps/%s.png", name);
    if (!gui_image_load(path, &icon))
        return;

    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormatFrom(icon.pixels, icon.width, icon.height, 32,
                                                              icon.width * 4, SDL_PIXELFORMAT_ARGB8888);

    if (surface) {
        SDL_SetWindowIcon(sdl_windows[slot_of(window)], surface);
        SDL_FreeSurface(surface);
    }
    gui_image_free(&icon);
}

void gui_set_cursor(struct gui_window *window, enum gui_cursor cursor) {
    static const SDL_SystemCursor shapes[] = {
        [GUI_CURSOR_ARROW] = SDL_SYSTEM_CURSOR_ARROW,         [GUI_CURSOR_TEXT] = SDL_SYSTEM_CURSOR_IBEAM,
        [GUI_CURSOR_RESIZE_H] = SDL_SYSTEM_CURSOR_SIZEWE,     [GUI_CURSOR_RESIZE_V] = SDL_SYSTEM_CURSOR_SIZENS,
        [GUI_CURSOR_RESIZE_NWSE] = SDL_SYSTEM_CURSOR_SIZENWSE, [GUI_CURSOR_RESIZE_NESW] = SDL_SYSTEM_CURSOR_SIZENESW,
    };
    static SDL_Cursor *made[sizeof(shapes) / sizeof(shapes[0])];

    (void)window;
    if (cursor >= sizeof(shapes) / sizeof(shapes[0]))
        return;
    if (!made[cursor])
        made[cursor] = SDL_CreateSystemCursor(shapes[cursor]);
    SDL_SetCursor(made[cursor]);
}

void gui_set_translucent(struct gui_window *window, bool on) {
    (void)window, (void)on;
}

bool gui_notify_action(struct gui_window *window, const char *title, const char *text, const char *label,
                       const char *command) {
    (void)window, (void)label, (void)command;
    fprintf(stderr, "%s: %s%s%s\n", program_invocation_short_name, title, text ? ": " : "", text ? text : "");
    return true;
}

bool gui_notify(struct gui_window *window, const char *title, const char *text) {
    return gui_notify_action(window, title, text, NULL, NULL);
}

bool gui_clipboard_set(const char *text, size_t length) {
    char *copy = strndup(text, length);
    bool ok = copy && SDL_SetClipboardText(copy) == 0;

    free(copy);
    return ok;
}

char *gui_clipboard_get(size_t *length) {
    char *sdl = SDL_HasClipboardText() ? SDL_GetClipboardText() : NULL;
    char *text = sdl && *sdl ? strdup(sdl) : NULL;

    SDL_free(sdl);
    if (length)
        *length = text ? strlen(text) : 0;
    return text;
}

bool gui_clipboard_set_image(const struct gui_canvas *from) {
    (void)from;
    return false;
}

bool gui_clipboard_get_image(struct gui_image *image) {
    (void)image;
    return false;
}

bool gui_drag_file(const char *path) {
    (void)path;
    return false;
}

bool gui_dropped_file(const struct gui_message *drop, char path[GUI_PATH_MAX]) {
    if (drop->type != GUI_DROP || !dropped[0])
        return false;
    strcpy(path, dropped);
    return true;
}

static uint32_t buttons_of(uint32_t sdl) {
    return (sdl & SDL_BUTTON_LMASK ? MOUSE_BUTTON_LEFT : 0) | (sdl & SDL_BUTTON_RMASK ? MOUSE_BUTTON_RIGHT : 0)
         | (sdl & SDL_BUTTON_MMASK ? MOUSE_BUTTON_MIDDLE : 0);
}

static uint8_t modifiers_of(SDL_Keymod mod) {
    return (mod & KMOD_SHIFT ? KEY_MOD_SHIFT : 0) | (mod & KMOD_CTRL ? KEY_MOD_CTRL : 0)
         | (mod & KMOD_ALT ? KEY_MOD_ALT : 0) | (mod & KMOD_GUI ? KEY_MOD_SUPER : 0)
         | (mod & KMOD_CAPS ? KEY_MOD_CAPS_LOCK : 0);
}

// The message wm would send for an SDL event, or false for one it wouldn't.
static bool translate(const SDL_Event *e, struct gui_message *m) {
    int slot = -1, x, y;

    switch (e->type) {
    case SDL_WINDOWEVENT:
        slot = slot_of_sdl(e->window.windowID);
        if (e->window.event == SDL_WINDOWEVENT_CLOSE)
            *m = (struct gui_message){.type = GUI_CLOSE};
        else if (e->window.event == SDL_WINDOWEVENT_SIZE_CHANGED)
            *m = (struct gui_message){.type = GUI_RESIZE, .width = e->window.data1, .height = e->window.data2};
        else if (e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED || e->window.event == SDL_WINDOWEVENT_FOCUS_LOST)
            *m = (struct gui_message){.type = GUI_FOCUS, .focused = e->window.event == SDL_WINDOWEVENT_FOCUS_GAINED};
        else {
            if (e->window.event == SDL_WINDOWEVENT_EXPOSED && slot >= 0)
                present(slot);
            return false;
        }
        break;
    case SDL_MOUSEMOTION:
        slot = slot_of_sdl(e->motion.windowID);
        *m = (struct gui_message){.type = GUI_MOUSE_MOVE, .x = e->motion.x, .y = e->motion.y,
                                  .buttons = buttons_of(e->motion.state)};
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        slot = slot_of_sdl(e->button.windowID);
        *m = (struct gui_message){.type = GUI_MOUSE_BUTTON, .x = e->button.x, .y = e->button.y,
                                  .buttons = buttons_of(SDL_GetMouseState(NULL, NULL)),
                                  .changed = buttons_of(SDL_BUTTON(e->button.button))};
        break;
    case SDL_MOUSEWHEEL: // towards you is down, and positive, as on AximOS
        slot = slot_of_sdl(e->wheel.windowID);
        SDL_GetMouseState(&x, &y);
        *m = (struct gui_message){.type = GUI_MOUSE_WHEEL, .x = x, .y = y, .wheel = -e->wheel.y};
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        SDL_Scancode usage = e->key.keysym.scancode;
        uint16_t code = usage < sizeof(key_usages) / sizeof(key_usages[0]) ? key_usages[usage] : 0;
        uint8_t modifiers = modifiers_of(e->key.keysym.mod);
        bool pressed = e->type == SDL_KEYDOWN;

        if (!code)
            return false;
        slot = slot_of_sdl(e->key.windowID);
        *m = (struct gui_message){.type = GUI_KEY, .key = {.code = code, .pressed = pressed, .modifiers = modifiers,
                                                           .text = pressed ? key_event_text(code, modifiers) : 0}};
        break;
    }
    case SDL_DROPFILE:
        slot = slot_of_sdl(e->drop.windowID);
        snprintf(dropped, sizeof(dropped), "%s", e->drop.file);
        SDL_free(e->drop.file);
        SDL_GetMouseState(&x, &y);
        *m = (struct gui_message){.type = GUI_DROP, .x = x, .y = y};
        break;
    default:
        return false;
    }
    if (slot < 0)
        return false;
    m->window = windows[slot]->id;
    return true;
}

bool gui_backend_wait(int64_t due) {
    SDL_Event e;
    struct gui_message m;
    int64_t wait = due - gui_now_ms();

    if (!(due < 0 ? SDL_WaitEvent(&e) : SDL_WaitEventTimeout(&e, wait > 0 ? (int)wait : 0)))
        return true;
    do {
        // Being told to quit, by SIGTERM or Ctrl+C, closes every window.
        for (int i = 0; e.type == SDL_QUIT && i < MAX_WINDOWS; i++) {
            if (windows[i])
                gui_post(&(struct gui_message){.type = GUI_CLOSE, .window = windows[i]->id});
        }
        if (translate(&e, &m))
            gui_post(&m);
    } while (SDL_PollEvent(&e));
    return true;
}

void help_option(int argc, char **argv, const char *text) {
    if (argc > 1 && strcmp(argv[1], "-h") == 0) {
        fputs(text, stdout);
        exit(0);
    }
}
