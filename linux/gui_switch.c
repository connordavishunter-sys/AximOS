#include "../user/libgui/backend.h"
#include <stdlib.h>

// A program wm started, or one started from one of those, talks to wm, which says
// where it is in GUI_FD or GUI_SOCKET. Any other gets SDL windows on the desktop.

#define FUNCTIONS(X)                                                                                             \
    X(int, fd, (void), ())                                                                                       \
    X(bool, send, (int fd, const struct gui_message *m), (fd, m))                                                \
    X(bool, receive, (int fd, struct gui_message *m), (fd, m))                                                   \
    X(struct gui_window *, create_window, (const char *title, int width, int height), (title, width, height))    \
    X(bool, resize, (struct gui_window * w, int width, int height), (w, width, height))                          \
    X(void, destroy_window, (struct gui_window * w), (w))                                                        \
    X(void, update, (struct gui_window * w, int x, int y, int width, int height), (w, x, y, width, height))     \
    X(void, show, (struct gui_window * w), (w))                                                                  \
    X(void, set_title, (struct gui_window * w, const char *title), (w, title))                                   \
    X(void, set_icon, (struct gui_window * w, const char *name), (w, name))                                      \
    X(void, set_cursor, (struct gui_window * w, enum gui_cursor cursor), (w, cursor))                            \
    X(void, set_translucent, (struct gui_window * w, bool on), (w, on))                                          \
    X(bool, notify, (struct gui_window * w, const char *title, const char *text), (w, title, text))             \
    X(bool, notify_action,                                                                                       \
      (struct gui_window * w, const char *title, const char *text, const char *label, const char *command),     \
      (w, title, text, label, command))                                                                          \
    X(bool, clipboard_set, (const char *text, size_t length), (text, length))                                    \
    X(char *, clipboard_get, (size_t *length), (length))                                                         \
    X(bool, clipboard_set_image, (const struct gui_canvas *from), (from))                                        \
    X(bool, clipboard_get_image, (struct gui_image *out), (out))                                                 \
    X(bool, drag_file, (const char *path), (path))                                                               \
    X(bool, dropped_file, (const struct gui_message *drop, char path[GUI_PATH_MAX]), (drop, path))              \
    X(bool, backend_wait, (int64_t due), (due))

#define DECLARE(type, name, params, args) type wm_gui_##name params; type sdl_gui_##name params;
#define DEFINE(type, name, params, args) \
    type gui_##name params {             \
        return use_wm ? wm_gui_##name args : sdl_gui_##name args; \
    }

static bool use_wm;

FUNCTIONS(DECLARE)
bool wm_gui_connect(void), sdl_gui_connect(void);

bool gui_connect(void) {
    use_wm = (getenv("GUI_FD") || getenv("GUI_SOCKET")) && wm_gui_connect();
    return use_wm || sdl_gui_connect();
}

FUNCTIONS(DEFINE)
