/*
 * Smooth text for NetSurf on AximOS: the DejaVu fonts, drawn by stb_truetype,
 * which libgui uses too, in place of FreeType.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <stb_truetype.h>

#include "utils/utf8.h"
#include "utils/log.h"
#include "utils/nsoption.h"
#include "netsurf/layout.h"
#include "netsurf/plotters.h"
#include "netsurf/browser.h"
#include "framebuffer/font.h"

enum face { SANS, SANS_BOLD, SANS_ITALIC, SANS_BOLD_ITALIC, SERIF, SERIF_BOLD, MONO, MONO_BOLD, FACES };

static const char *const files[FACES] = {
	NETSURF_FB_FONT_SANS_SERIF, NETSURF_FB_FONT_SANS_SERIF_BOLD, NETSURF_FB_FONT_SANS_SERIF_ITALIC,
	NETSURF_FB_FONT_SANS_SERIF_ITALIC_BOLD, NETSURF_FB_FONT_SERIF, NETSURF_FB_FONT_SERIF_BOLD,
	NETSURF_FB_FONT_MONOSPACE, NETSURF_FB_FONT_MONOSPACE_BOLD,
};

static struct {
	uint8_t *data;
	stbtt_fontinfo info;
	bool loaded;
} faces[FACES];

/* Glyphs drawn so far, each slot taken by the last one hashed to it. */
#define CACHE 4096

struct cached {
	enum face face;
	int px;
	uint32_t ucs4;
	bool used;
	struct fb_aximos_glyph glyph;
};

static struct cached cache[CACHE];

static bool load(enum face face)
{
	char path[512];
	FILE *f;
	long size;

	snprintf(path, sizeof(path), "%s/%s", NETSURF_FB_FONTPATH, files[face]);
	if ((f = fopen(path, "rb")) == NULL)
		return false;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	faces[face].data = size > 0 ? malloc(size) : NULL;
	faces[face].loaded = faces[face].data && fread(faces[face].data, 1, size, f) == (size_t)size
		&& stbtt_InitFont(&faces[face].info, faces[face].data, stbtt_GetFontOffsetForIndex(faces[face].data, 0));
	fclose(f);
	if (!faces[face].loaded) {
		free(faces[face].data);
		faces[face].data = NULL;
	}
	return faces[face].loaded;
}

bool fb_font_init(void)
{
	for (int face = 0; face < FACES; face++) {
		if (!load(face))
			NSLOG(netsurf, INFO, "no font %s", files[face]);
	}
	return faces[SANS].loaded;
}

bool fb_font_finalise(void)
{
	for (int i = 0; i < CACHE; i++)
		free(cache[i].glyph.coverage);
	for (int face = 0; face < FACES; face++)
		free(faces[face].data);
	return true;
}

/* The face for a style, or the nearest that's there. */
static enum face face_of(const plot_font_style_t *fstyle)
{
	bool bold = fstyle->weight >= 700, italic = fstyle->flags & (FONTF_ITALIC | FONTF_OBLIQUE);
	enum face face;

	switch (fstyle->family) {
	case PLOT_FONT_FAMILY_SERIF:
		face = bold ? SERIF_BOLD : SERIF;
		break;
	case PLOT_FONT_FAMILY_MONOSPACE:
		face = bold ? MONO_BOLD : MONO;
		break;
	default:
		face = bold ? (italic ? SANS_BOLD_ITALIC : SANS_BOLD) : (italic ? SANS_ITALIC : SANS);
		break;
	}
	if (!faces[face].loaded)
		face = face == SANS_BOLD_ITALIC && faces[SANS_BOLD].loaded ? SANS_BOLD
		     : face == SERIF_BOLD && faces[SERIF].loaded ? SERIF
		     : face == MONO_BOLD && faces[MONO].loaded ? MONO
		     : bold && faces[SANS_BOLD].loaded ? SANS_BOLD : SANS;
	return face;
}

/* Points, scaled by PLOT_STYLE_SCALE, at the screen's DPI, as pixels per em. */
static int pixels_of(const plot_font_style_t *fstyle)
{
	int px = (int)(((long long)fstyle->size * browser_get_dpi() + PLOT_STYLE_SCALE * 36) / (PLOT_STYLE_SCALE * 72));

	return px < 4 ? 4 : px > 256 ? 256 : px;
}

const struct fb_aximos_glyph *fb_aximos_glyph(const plot_font_style_t *fstyle, uint32_t ucs4)
{
	enum face face = face_of(fstyle);
	int px = pixels_of(fstyle);
	struct cached *c = &cache[(ucs4 * 2654435761u ^ (uint32_t)px * 40503u ^ (uint32_t)face * 97u) % CACHE];
	stbtt_fontinfo *info = &faces[face].info;
	float scale;
	int index, advance, unused;

	if (c->used && c->face == face && c->px == px && c->ucs4 == ucs4)
		return &c->glyph;
	free(c->glyph.coverage);
	*c = (struct cached){.face = face, .px = px, .ucs4 = ucs4, .used = true};

	/* A character the face doesn't have comes from sans, if that does. */
	if ((index = stbtt_FindGlyphIndex(info, ucs4)) == 0 && face != SANS
			&& stbtt_FindGlyphIndex(&faces[SANS].info, ucs4) != 0) {
		info = &faces[SANS].info;
		index = stbtt_FindGlyphIndex(info, ucs4);
	}
	scale = stbtt_ScaleForMappingEmToPixels(info, px);
	stbtt_GetGlyphHMetrics(info, index, &advance, &unused);
	c->glyph.advance = (int)(advance * scale + 0.5f);
	c->glyph.coverage = stbtt_GetGlyphBitmap(info, scale, scale, index, &c->glyph.width, &c->glyph.height,
			&c->glyph.left, &c->glyph.top);
	c->glyph.top = -c->glyph.top;
	return &c->glyph;
}

static int advance_of(const plot_font_style_t *fstyle, const char *string, size_t length, size_t at)
{
	return fb_aximos_glyph(fstyle, utf8_to_ucs4(string + at, length - at))->advance;
}

nserror fb_font_width(const plot_font_style_t *fstyle, const char *string, size_t length, int *width)
{
	*width = 0;
	for (size_t at = 0; at < length; at = utf8_next(string, length, at))
		*width += advance_of(fstyle, string, length, at);
	return NSERROR_OK;
}

nserror fb_font_position(const plot_font_style_t *fstyle, const char *string, size_t length, int x,
		size_t *char_offset, int *actual_x)
{
	size_t at = 0;
	int before = 0;

	*actual_x = 0;
	while (at < length) {
		*actual_x += advance_of(fstyle, string, length, at);
		if (*actual_x > x)
			break;
		before = *actual_x;
		at = utf8_next(string, length, at);
	}
	/* the nearer of the edges either side */
	if (abs(*actual_x - x) > abs(before - x))
		*actual_x = before;
	*char_offset = at;
	return NSERROR_OK;
}

static nserror fb_font_split(const plot_font_style_t *fstyle, const char *string, size_t length, int x,
		size_t *char_offset, int *actual_x)
{
	size_t at = 0, space = 0;
	int space_x = 0;

	*actual_x = 0;
	while (at < length) {
		if (string[at] == ' ') {
			space = at;
			space_x = *actual_x;
		}
		*actual_x += advance_of(fstyle, string, length, at);
		if (*actual_x > x && space != 0) {
			*actual_x = space_x;
			*char_offset = space;
			return NSERROR_OK;
		}
		at = utf8_next(string, length, at);
	}
	*char_offset = at;
	return NSERROR_OK;
}

static struct gui_layout_table layout_table = {
	.width = fb_font_width,
	.position = fb_font_position,
	.split = fb_font_split,
};

struct gui_layout_table *framebuffer_layout_table = &layout_table;
struct gui_utf8_table *framebuffer_utf8_table = NULL;
