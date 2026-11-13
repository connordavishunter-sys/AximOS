#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Intel High Definition Audio: /dev/audio plays 16-bit stereo, written as it's to
// be heard, at the rate AUDIO_SET_RATE picks, 48000 to start with.

// Finds a controller and a codec with somewhere to play, returning false if none.
bool hda_init(void);

// Only one file can have it open. These return a negative errno.
int hda_open(void);
void hda_close(void); // after what's been written has played
int64_t hda_write(const void *buf, size_t len, bool nonblocking);
int hda_ioctl(uint64_t request, void *arg);
bool hda_writable(void);

// Called on every tick, to follow where playing has got to.
void hda_tick(void);
