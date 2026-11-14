#include <gui.h>
#include <stdio.h>
#include <unistd.h>

// Shows a notification from the shell, as programs do.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: notify title [text]\n"
                            "Shows a notification in the corner of the screen for a few seconds.\n");
    if (argc < 2 || argc > 3) {
        dprintf(STDERR_FILENO, "usage: notify title [text]\n");
        return 2;
    }
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "notify: wm isn't running\n");
        return 1;
    }
    return gui_notify(NULL, argv[1], argc > 2 ? argv[2] : "") ? 0 : 1;
}
