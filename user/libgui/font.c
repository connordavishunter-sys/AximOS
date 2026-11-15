#include "font.h"
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

// Text drawn smoothly from a TrueType font, sized so each character fills the
// same 8x16 cell, times scale, as the bitmap font's, so nothing else moves. Only
// monospaced fonts will do: one whose characters aren't all as wide is refused,
// and the bitmap font used instead.
#define FONT "/system/share/fonts/mono.ttf"
#define MAX_FONT (4 * 1024 * 1024)
#define MAX_SCALE 4

struct glyph {
    uint8_t *coverage; // how much of each pixel the glyph covers, out of 255
    int width, height, x, y; // where it goes from the cell's left edge and baseline
    bool made;
};

struct size {
    float scale;  // from font units to pixels
    int baseline; // down from the top of the cell
    struct glyph glyphs[128];
};

bool font_smooth = true;
static enum { UNTRIED, LOADED, FAILED } state;
static stbtt_fontinfo font;
static uint8_t *data;
static struct size sizes[MAX_SCALE + 1];

static bool load(void) {
    int fd = open(FONT, O_RDONLY | O_CLOEXEC), advance, other, unused;
    struct stat st;
    bool ok = fd >= 0 && fstat(fd, &st) == 0 && st.st_size > 0 && st.st_size <= MAX_FONT
           && (data = malloc(st.st_size)) && read(fd, data, st.st_size) == st.st_size
           && stbtt_InitFont(&font, data, stbtt_GetFontOffsetForIndex(data, 0));

    if (fd >= 0)
        close(fd);
    if (ok) {
        stbtt_GetCodepointHMetrics(&font, 'M', &advance, &unused);
        stbtt_GetCodepointHMetrics(&font, 'i', &other, &unused);
        ok = advance > 0 && advance == other;
    }
    if (!ok) {
        free(data);
        data = NULL;
    }
    return ok;
}

static struct size *size_of(int scale) {
    struct size *s = &sizes[scale];
    int advance, unused, ascent, descent;

    if (s->scale)
        return s;
    stbtt_GetCodepointHMetrics(&font, 'M', &advance, &unused);
    stbtt_GetFontVMetrics(&font, &ascent, &descent, &unused);
    s->scale = (float)(GUI_CHAR_WIDTH * scale) / advance;
    // Centred in the cell's height, from the top of the tallest to the bottom of
    // the lowest.
    s->baseline = (int)((GUI_CHAR_HEIGHT * scale - (ascent - descent) * s->scale) / 2 + ascent * s->scale + 0.5f);
    return s;
}

static const struct glyph *glyph_of(struct size *s, unsigned char c) {
    struct glyph *g = &s->glyphs[c];

    if (!g->made) {
        g->made = true;
        g->coverage = stbtt_GetCodepointBitmap(&font, s->scale, s->scale, c, &g->width, &g->height, &g->x, &g->y);
    }
    return g;
}

// Mixes color over a pixel by a out of 255, as 256ths so it's a shift.
static uint32_t blend(uint32_t under, uint32_t color, uint32_t a) {
    uint32_t keep;

    a += a >> 7;
    keep = 256 - a;
    return ((((color & 0xFF00FF) * a + (under & 0xFF00FF) * keep) >> 8) & 0xFF00FF)
         | ((((color & 0x00FF00) * a + (under & 0x00FF00) * keep) >> 8) & 0x00FF00);
}

// The first time, the font is loaded, and the settings, which say whether to use
// it, for programs that don't load them themselves.
bool font_text(struct gui_canvas *canvas, int x, int y, const char *text, uint32_t color, int scale) {
    if (state == UNTRIED) {
        struct gui_settings settings;

        gui_settings_load(&settings);
        state = load() ? LOADED : FAILED;
    }
    if (!font_smooth || state == FAILED || scale < 1 || scale > MAX_SCALE)
        return false;

    struct size *s = size_of(scale);
    int width = GUI_CHAR_WIDTH * scale;

    if (y >= canvas->height || y + GUI_CHAR_HEIGHT * scale <= 0)
        return true;
    for (; *text && x < canvas->width; text++, x += width) {
        unsigned char c = (unsigned char)*text < 128 ? (unsigned char)*text : '?';
        const struct glyph *g = c > ' ' ? glyph_of(s, c) : NULL;

        if (!g || !g->coverage || x + width <= 0)
            continue;
        for (int row = 0; row < g->height; row++) {
            int py = y + s->baseline + g->y + row;

            if (py < 0 || py >= canvas->height)
                continue;

            uint32_t *line = canvas->pixels + (long)py * canvas->stride;
            const uint8_t *cover = g->coverage + row * g->width;

            for (int col = 0; col < g->width; col++) {
                int px = x + g->x + col;

                if (cover[col] && px >= 0 && px < canvas->width)
                    line[px] = cover[col] == 255 ? color : blend(line[px], color, cover[col]);
            }
        }
    }
    return true;
}
