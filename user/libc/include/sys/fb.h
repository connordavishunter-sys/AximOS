#pragma once

#include <fb_types.h>

// The screen is /dev/fb0. Opening it for writing takes the screen away from the
// text console until it's closed, which also happens when a program dies. Only
// one open file can have it at a time, and opening it read-only just allows
// fb_info. The memory can also be mapped with mmap and MAP_SHARED, after which
// each pixel is at pitch * y + 4 * x.

int fb_info(int fd, struct fb_info *out);

// Changes the resolution, which stays changed after the program ends, and blanks
// the screen. Fails with ENOTSUP if the graphics card isn't one that can be
// driven, or EINVAL if it can't do that size. Width has to be a multiple of 8.
int fb_set_mode(int fd, int width, int height);

// Copies pixels from the program's memory to the screen.
int fb_draw(int fd, const struct fb_rect *rect);
