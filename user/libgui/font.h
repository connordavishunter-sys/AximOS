#pragma once

#include <gui.h>

// Whether text is drawn smoothly, from the settings, which is the default.
extern bool font_smooth;

// Draws text smoothly, returning false if it can't, when the bitmap font is used.
bool font_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale);
