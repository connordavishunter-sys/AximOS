#pragma once

#include <stdbool.h>
#include <stdint.h>

// Sound files, read and written as 16-bit samples, interleaved when there are two
// channels. Each kind is done by its own library in /system/lib, loaded when a
// file of that kind is first opened:
//
//     .wav            libwav.so
//     .mp3            libmp3.so      minimp3 and shine
//     .flac           libflac.so     libFLAC
//     .ogg .oga       libvorbis.so   libvorbis, or libopus.so for Opus in Ogg
//     .opus           libopus.so     libopus
//     .aac            libaac.so      faad2 and faac, as ADTS

struct audio_info {
    int rate;       // samples a second
    int channels;   // 1 or 2; files with more are mixed down to 2
    int64_t frames; // how many samples each channel has, or -1 when it isn't known
    int bitrate;    // kilobits a second: what to encode at, or what a file averages, or 0
};

struct audio_file;

// Opens a file to read, filling in info.
struct audio_file *audio_open(const char *path, struct audio_info *info);

// Reads up to frames samples a channel, returning how many it read, 0 at the end
// and -1 if the file is damaged.
long audio_read(struct audio_file *file, int16_t *samples, long frames);

// Goes to a sample, counting from the start of each channel.
bool audio_seek(struct audio_file *file, int64_t frame);

// Makes a file to write, encoded at info's rate and bitrate, or a default one when
// that's 0. Lossless kinds ignore the bitrate.
struct audio_file *audio_create(const char *path, const struct audio_info *info);
bool audio_write(struct audio_file *file, const int16_t *samples, long frames);

// Finishes writing, and frees either kind. Returns false if the file couldn't be finished.
bool audio_close(struct audio_file *file);

// What went wrong last.
const char *audio_error(void);

// Whether a file's name says it's one of the kinds above.
bool audio_known(const char *path);

// Changes the rate of interleaved samples, carrying on from one call to the next.
// Each call takes all of in, so out must have room for in_frames * to / from + 2.
// Returns how many it wrote.
struct audio_resampler;
struct audio_resampler *audio_resampler_new(int from, int to, int channels);
long audio_resample(struct audio_resampler *r, const int16_t *in, long in_frames, int16_t *out, long out_frames);
void audio_resampler_free(struct audio_resampler *r);
