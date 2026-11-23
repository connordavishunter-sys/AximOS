#include "../user/wm/display.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// wm runs on the screen itself through DRM. On another Linux, it runs in a window
// when there's a desktop to make one on, unless --window or --drm says.

extern const struct display wm_drm;
#ifdef GUI_HOSTED
extern const struct display wm_sdl;
#else
static const struct display wm_sdl;
#endif

const struct display *display_choose(int *argc, char ***argv) {
#ifdef GUI_HOSTED
    bool window = getenv("WAYLAND_DISPLAY") || getenv("DISPLAY");
#else
    bool window = false;
#endif
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    char path[108];

    while (*argc > 1 && (!strcmp((*argv)[1], "--window") || !strcmp((*argv)[1], "--drm"))) {
        window = !strcmp((*argv)[1], "--window") && wm_sdl.open;
        (*argv)[1] = (*argv)[0];
        (*argv)++, (*argc)--;
    }

    // Its programs find it here, and the desktop's own don't, as they've no GUI_SOCKET.
    snprintf(path, sizeof(path), "%s/aximos-wm-%d.sock", runtime ? runtime : "/tmp", getpid());
    setenv("GUI_SOCKET", path, 1);
    return window ? &wm_sdl : &wm_drm;
}
