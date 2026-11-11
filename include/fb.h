#pragma once

#include <fb_types.h>
#include <stdint.h>

// Finds the bootloader's framebuffer and starts the console on it. Early enough
// that it can't use mm yet.
void fb_init(void);

// The rest of fb_init, for once mm is up.
void fb_map(void);

// Whether programs can use the screen: it has to be in their pixel format.
bool fb_drawable(void);

// Takes the screen from the console, or fails with EBUSY if it's already taken.
// The resolution stays as it is when it's given back.
int fb_acquire(void);
void fb_release(void);

void fb_get_info(struct fb_info *out);
int fb_set_mode(uint64_t width, uint64_t height);

// Copies pixels to the screen. rect is in user memory, and has been checked.
int fb_draw(const struct fb_rect *rect);

uint64_t fb_memory_address(void);
uint64_t fb_memory_size(void);
