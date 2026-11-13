// Counts a repeating timer and a one-shot one for a second, while the mouse moves.
#include <gui.h>
#include <stdio.h>
#include <time.h>

static long ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

int main(void) {
    struct gui_message e;

    if (!gui_connect() || !gui_create_window("Timers", 200, 100))
        return 1;

    int every = gui_timer_start(100, true), once = gui_timer_start(250, false), end = gui_timer_start(1000, false);
    int stopped = gui_timer_start(50, true), repeats = 0, onces = 0, stops = 0, others = 0;
    long start = ms(), once_at = 0;

    gui_timer_stop(stopped);
    while (gui_next_event(&e)) {
        if (e.type != GUI_TIMER)
            others++;
        else if (e.x == every)
            repeats++;
        else if (e.x == once && !onces++)
            once_at = ms() - start;
        else if (e.x == stopped)
            stops++;
        else if (e.x == end)
            break;
    }
    printf("timers: %d repeats, %d once at %ld ms, %d stopped, %d others, %ld ms\n", repeats, onces, once_at, stops,
           others, ms() - start);
    return 0;
}
