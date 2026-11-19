#include <gui.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int gui_recent_list(char (*paths)[GUI_PATH_MAX], int max) {
    FILE *f = fopen(GUI_RECENT, "r");
    int count = 0;

    while (f && count < max && fgets(paths[count], GUI_PATH_MAX, f)) {
        paths[count][strcspn(paths[count], "\n")] = '\0';
        count += paths[count][0] == '/';
    }
    if (f)
        fclose(f);
    return count;
}

// It goes first, and any line for it already goes, as does the oldest past GUI_RECENT_MAX.
void gui_recent_add(const char *path) {
    char paths[GUI_RECENT_MAX][GUI_PATH_MAX];
    int count = gui_recent_list(paths, GUI_RECENT_MAX);
    FILE *f;

    if (path[0] != '/' || strchr(path, '\n') || !(f = fopen(GUI_RECENT ".new", "w")))
        return;
    fprintf(f, "%s\n", path);
    for (int i = 0, kept = 1; i < count && kept < GUI_RECENT_MAX; i++) {
        if (strcmp(paths[i], path)) {
            fprintf(f, "%s\n", paths[i]);
            kept++;
        }
    }
    if (fclose(f) == 0)
        rename(GUI_RECENT ".new", GUI_RECENT);
}

void gui_recent_clear(void) {
    unlink(GUI_RECENT);
}
