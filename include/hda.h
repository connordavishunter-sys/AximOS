#pragma once

#include <stdbool.h>
#include <stdint.h>

// Intel High Definition Audio: a sound card that plays 16-bit stereo, round and
// round a buffer, at a rate set while it's stopped. audio.c mixes into it.

#define HDA_BUFFER_BYTES (16 * 4096)

// Finds a controller and a codec with somewhere to play, returning false if none.
bool hda_init(void);

bool hda_supports(int rate);
void hda_set_rate(int rate);

// Starting plays from the beginning of the buffer, and stopping silences all of it.
void hda_start(void);
void hda_stop(void);

// Where in the buffer it's playing, and writing there.
uint32_t hda_played(void);
void hda_put(uint32_t offset, const void *data, uint32_t len);
