/*
 * PNG, JPEG and WebP images for NetSurf on AximOS, decoded by libgui, instead of
 * libpng, libjpeg and libwebp of NetSurf's own.
 */
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <gui.h>

#include "utils/errors.h"
#include "utils/utils.h"
#include "netsurf/bitmap.h"
#include "content/llcache.h"
#include "content/content_protected.h"
#include "content/content_factory.h"
#include "desktop/gui_internal.h"
#include "desktop/bitmap.h"
#include "image/image_cache.h"
#include "aximos_image.h"

static unsigned be16(const uint8_t *p) {
	return p[0] << 8 | p[1];
}

static unsigned le24(const uint8_t *p) {
	return p[0] | p[1] << 8 | p[2] << 16;
}

/* A WebP's size, from its lossy, lossless or extended header. */
static bool webp_size(const uint8_t *d, size_t n, int *width, int *height)
{
	if (n >= 30 && memcmp(d + 12, "VP8 ", 4) == 0) {
		*width = (d[26] | d[27] << 8) & 0x3FFF;
		*height = (d[28] | d[29] << 8) & 0x3FFF;
	} else if (n >= 25 && memcmp(d + 12, "VP8L", 4) == 0) {
		uint32_t bits = d[21] | d[22] << 8 | d[23] << 16 | (uint32_t)d[24] << 24;

		*width = (bits & 0x3FFF) + 1;
		*height = (bits >> 14 & 0x3FFF) + 1;
	} else if (n >= 30 && memcmp(d + 12, "VP8X", 4) == 0) {
		*width = le24(d + 24) + 1;
		*height = le24(d + 27) + 1;
	} else {
		return false;
	}
	return *width > 0 && *height > 0;
}

/* The size from a PNG's or JPEG's header, without decoding it. */
static bool image_size(const uint8_t *d, size_t n, int *width, int *height)
{
	if (n >= 24 && memcmp(d, "\x89PNG\r\n\x1a\n", 8) == 0) {
		*width = be16(d + 18);
		*height = be16(d + 22);
		return *width > 0 && *height > 0 && be16(d + 16) == 0 && be16(d + 20) == 0;
	}
	if (n >= 12 && memcmp(d, "RIFF", 4) == 0 && memcmp(d + 8, "WEBP", 4) == 0)
		return webp_size(d, n, width, height);
	if (n < 4 || d[0] != 0xFF || d[1] != 0xD8)
		return false;
	for (size_t at = 2; at + 9 <= n;) {
		uint8_t marker;

		if (d[at] != 0xFF)
			return false;
		while (at < n && d[at] == 0xFF)
			at++;
		if (at + 8 > n)
			return false;
		marker = d[at++];
		/* Start of frame: any of 0xC0 to 0xCF but the tables 0xC4, 0xC8 and 0xCC. */
		if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
			*height = be16(d + at + 3);
			*width = be16(d + at + 5);
			return *width > 0 && *height > 0;
		}
		at += be16(d + at);
	}
	return false;
}

static nserror image_create(const content_handler *handler, lwc_string *imime_type,
		const struct http_parameter *params, llcache_handle *llcache,
		const char *fallback_charset, bool quirks, struct content **c)
{
	struct content *image = calloc(1, sizeof(struct content));
	nserror res;

	if (image == NULL)
		return NSERROR_NOMEM;
	res = content__init(image, handler, imime_type, params, llcache, fallback_charset, quirks);
	if (res != NSERROR_OK) {
		free(image);
		return res;
	}
	*c = image;
	return NSERROR_OK;
}

static struct bitmap *image_cache_convert(struct content *c)
{
	bitmap_fmt_t format = {.layout = BITMAP_LAYOUT_B8G8R8A8, .pma = false}; /* 0xAARRGGBB */
	struct gui_image decoded;
	struct bitmap *bitmap;
	const uint8_t *data;
	uint8_t *pixels;
	size_t size, stride;

	data = content__get_source_data(c, &size);
	if (!gui_image_decode(data, size, &decoded))
		return NULL;
	bitmap = guit->bitmap->create(decoded.width, decoded.height, BITMAP_NONE);
	if (bitmap == NULL || (pixels = guit->bitmap->get_buffer(bitmap)) == NULL) {
		if (bitmap)
			guit->bitmap->destroy(bitmap);
		gui_image_free(&decoded);
		return NULL;
	}
	stride = guit->bitmap->get_rowstride(bitmap);
	for (int y = 0; y < decoded.height; y++)
		memcpy(pixels + y * stride, decoded.pixels + (size_t)y * decoded.width, decoded.width * 4);
	gui_image_free(&decoded);
	bitmap_format_to_client(bitmap, &format);
	guit->bitmap->modified(bitmap);
	return bitmap;
}

static bool image_convert(struct content *c)
{
	const uint8_t *data;
	size_t size;
	int width, height;

	data = content__get_source_data(c, &size);
	if (!image_size(data, size, &width, &height))
		return false;
	c->width = width;
	c->height = height;
	c->size = width * height * 4;
	image_cache_add(c, NULL, image_cache_convert);
	content_set_ready(c);
	content_set_done(c);
	return true;
}

static nserror image_clone(const struct content *old, struct content **new_c)
{
	struct content *image = calloc(1, sizeof(struct content));
	nserror res;

	if (image == NULL)
		return NSERROR_NOMEM;
	res = content__clone(old, image);
	if (res != NSERROR_OK) {
		content_destroy(image);
		return res;
	}
	if ((old->status == CONTENT_STATUS_READY || old->status == CONTENT_STATUS_DONE) && !image_convert(image)) {
		content_destroy(image);
		return NSERROR_CLONE_FAILED;
	}
	*new_c = image;
	return NSERROR_OK;
}

static const content_handler aximos_image_handler = {
	.create = image_create,
	.data_complete = image_convert,
	.destroy = image_cache_destroy,
	.redraw = image_cache_redraw,
	.clone = image_clone,
	.get_internal = image_cache_get_internal,
	.type = image_cache_content_type,
	.is_opaque = image_cache_is_opaque,
	.no_share = false,
};

static const char *aximos_image_types[] = {
	"image/png", "image/x-png", "image/jpeg", "image/jpg", "image/pjpeg", "image/webp",
};

CONTENT_FACTORY_REGISTER_TYPES(nsaximos_image, aximos_image_types, aximos_image_handler);
