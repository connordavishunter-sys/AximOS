#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// /dev/audio, which mixes what every open file writes, and /dev/mixer, which sets
// the volume of it all. These return a negative errno.

struct audio_stream;

// Finds the sound card.
void audio_init(void);
bool audio_present(void);

int audio_open(struct audio_stream **out);
void audio_close(struct audio_stream *s); // after what it's written has been mixed
int64_t audio_write(struct audio_stream *s, const void *buf, size_t len, bool nonblocking);
int audio_ioctl(struct audio_stream *s, uint64_t request, void *arg);
bool audio_writable(const struct audio_stream *s);

int audio_mixer_ioctl(uint64_t request, void *arg);

// Called on every tick, to mix what's waiting.
void audio_tick(void);
