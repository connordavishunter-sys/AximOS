#pragma once

#include <gui.h>

// What events.c needs from whatever shows the windows: wm on AximOS, or SDL on
// Linux. It waits until something arrives, or due, in milliseconds by gui_now_ms,
// if that isn't -1, and gives what arrives to gui_post. False means it's gone.
bool gui_backend_wait(int64_t due);

int64_t gui_now_ms(void);
