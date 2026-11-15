#include "font.h"
#include <font8x16.h>
#include <gui.h>
#include <string.h>

// Shrinks a rectangle to what's on the canvas, returning false if nothing is.
static bool clip(const struct gui_canvas *canvas, int *x, int *y, int *width, int *height) {
    if (*x < 0) {
        *width += *x;
        *x = 0;
    }
    if (*y < 0) {
        *height += *y;
        *y = 0;
    }
    if (*width > canvas->width - *x)
        *width = canvas->width - *x;
    if (*height > canvas->height - *y)
        *height = canvas->height - *y;
    return *width > 0 && *height > 0;
}

void gui_fill(struct gui_canvas *canvas, int x, int y, int width, int height, uint32_t color) {
    if (!clip(canvas, &x, &y, &width, &height))
        return;

    for (int row = y; row < y + height; row++) {
        uint32_t *p = canvas->pixels + (long)row * canvas->stride + x;

        for (int i = 0; i < width; i++)
            p[i] = color;
    }
}

void gui_copy(struct gui_canvas *canvas, int x, int y, const struct gui_canvas *from, int from_x, int from_y,
              int width, int height) {
    // Clip to the source first, then the destination, moving the other to match.
    int sx = from_x, sy = from_y;

    if (!clip(from, &sx, &sy, &width, &height))
        return;
    x += sx - from_x;
    y += sy - from_y;

    int dx = x, dy = y;

    if (!clip(canvas, &dx, &dy, &width, &height))
        return;
    sx += dx - x;
    sy += dy - y;

    for (int row = 0; row < height; row++)
        memcpy(canvas->pixels + (long)(dy + row) * canvas->stride + dx,
               from->pixels + (long)(sy + row) * from->stride + sx, width * sizeof(uint32_t));
}

// Each glyph that's on the canvas, and of it only the rows and columns that are,
// pixel by pixel when it isn't scaled.
void gui_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale) {
    if (font_text(canvas, x, y, text, color, scale))
        return;

    int width = GUI_CHAR_WIDTH * scale, height = GUI_CHAR_HEIGHT * scale;
    int first_row = y < 0 ? -y / scale : 0;
    int last_row = (canvas->height - y + scale - 1) / scale;

    if (y >= canvas->height || y + height <= 0)
        return;
    last_row = last_row < GUI_CHAR_HEIGHT ? last_row : GUI_CHAR_HEIGHT;
    for (; *text && x < canvas->width; text++, x += width) {
        unsigned char c = (unsigned char)*text < 128 ? (unsigned char)*text : '?';

        if (x + width <= 0)
            continue;
        for (int row = first_row; row < last_row; row++) {
            unsigned char bits = font8x16[c][row];

            for (int col = 0; bits && col < GUI_CHAR_WIDTH; col++, bits <<= 1) {
                int px = x + col * scale, py = y + row * scale;

                if (!(bits & 0x80))
                    continue;
                if (scale == 1 && px >= 0 && px < canvas->width)
                    canvas->pixels[(long)py * canvas->stride + px] = color;
                else if (scale > 1)
                    gui_fill(canvas, px, py, scale, scale, color);
            }
        }
    }
}

int gui_text_width(const char *text, int scale) {
    return (int)strlen(text) * GUI_CHAR_WIDTH * scale;
}
