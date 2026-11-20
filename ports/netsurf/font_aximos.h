#ifndef NETSURF_FB_FONT_AXIMOS_H
#define NETSURF_FB_FONT_AXIMOS_H

#include <stdint.h>

/* A glyph drawn smoothly from a TrueType font, by stb_truetype. */
struct fb_aximos_glyph {
	uint8_t *coverage; /* how much of each pixel it covers, out of 255 */
	int width, height;
	int left, top;     /* from the pen to its top left, up being positive */
	int advance;       /* how far the pen goes after it */
};

const struct fb_aximos_glyph *fb_aximos_glyph(const plot_font_style_t *fstyle, uint32_t ucs4);

#endif
