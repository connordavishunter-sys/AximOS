#pragma once

#include <stdint.h>

// Shared by the kernel and libc. Pixels are always 32 bits, 0x00RRGGBB.

struct fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;       // bytes from the start of one row to the next
    uint32_t can_set_mode;
    uint64_t memory_size; // how much can be mapped with mmap
};

// Part of the pixels in a program's memory, and where on the screen they go. It
// can hang over the edges of the screen. stride is the bytes from one row of
// pixels to the next, with 0 meaning that rows follow straight after each other.
struct fb_rect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
    uint32_t stride;
    const uint32_t *pixels;
};
