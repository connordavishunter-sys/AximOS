#include <font8x8_basic.h>
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

void gui_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale) {
    for (; *text; text++, x += GUI_CHAR_WIDTH * scale) {
        unsigned char c = (unsigned char)*text < 128 ? (unsigned char)*text : '?';

        for (int row = 0; row < GUI_CHAR_HEIGHT; row++) {
            for (int col = 0; col < GUI_CHAR_WIDTH; col++) {
                if (font8x8_basic[c][row] >> col & 1)
                    gui_fill(canvas, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

int gui_text_width(const char *text, int scale) {
    return (int)strlen(text) * GUI_CHAR_WIDTH * scale;
}
