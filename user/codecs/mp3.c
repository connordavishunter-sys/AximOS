// MP3: read with minimp3, written with shine.
#include <audio_codec.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_STDIO
#include <minimp3_ex.h>
#include <layer3.h>

#define DEFAULT_BITRATE 128

struct reader {
    int fd;
    mp3dec_io_t io;
    mp3dec_ex_t dec;
};

static size_t read_cb(void *buf, size_t size, void *user) {
    ssize_t n = read(*(int *)user, buf, size);

    return n > 0 ? (size_t)n : 0;
}

static int seek_cb(uint64_t position, void *user) {
    return lseek(*(int *)user, (off_t)position, SEEK_SET) < 0;
}

static void *mp3_open(const char *path, struct audio_info *info) {
    struct reader *r = calloc(1, sizeof(*r));

    if (!r)
        return audio_codec_error("out of memory");
    r->fd = open(path, O_RDONLY);
    r->io = (mp3dec_io_t){read_cb, &r->fd, seek_cb, &r->fd};
    if (r->fd < 0 || mp3dec_ex_open_cb(&r->dec, &r->io, MP3D_SEEK_TO_SAMPLE) || !r->dec.info.hz) {
        if (r->fd >= 0) {
            mp3dec_ex_close(&r->dec);
            close(r->fd);
        }
        free(r);
        return audio_codec_error("%s isn't an MP3 this can read", path);
    }
    info->rate = r->dec.info.hz;
    info->channels = r->dec.info.channels;
    info->frames = r->dec.samples / r->dec.info.channels;
    info->bitrate = r->dec.info.bitrate_kbps;
    return r;
}

static long mp3_read(void *state, int16_t *samples, long frames) {
    struct reader *r = state;
    size_t got = mp3dec_ex_read(&r->dec, samples, (size_t)frames * r->dec.info.channels);

    if (got == 0 && r->dec.last_error)
        return audio_codec_error("the MP3 is damaged"), -1;
    return (long)(got / r->dec.info.channels);
}

static bool mp3_seek(void *state, int64_t frame) {
    struct reader *r = state;

    return mp3dec_ex_seek(&r->dec, (uint64_t)frame * r->dec.info.channels) == 0;
}

static void mp3_close(void *state) {
    struct reader *r = state;

    mp3dec_ex_close(&r->dec);
    close(r->fd);
    free(r);
}

// shine takes a fixed number of samples each time, so they're gathered up.
struct writer {
    int fd, channels, per_pass, have;
    shine_t shine;
    int16_t *pending;
};

static bool put(struct writer *w, const unsigned char *data, int length) {
    return length == 0 || write(w->fd, data, length) == length;
}

static void *mp3_create(const char *path, const struct audio_info *info) {
    int bitrate = info->bitrate ? info->bitrate : DEFAULT_BITRATE;
    shine_config_t config = {{info->channels == 1 ? PCM_MONO : PCM_STEREO, info->rate}, {0}};
    struct writer *w;

    shine_set_config_mpeg_defaults(&config.mpeg);
    config.mpeg.bitr = bitrate;
    config.mpeg.mode = info->channels == 1 ? MONO : JOINT_STEREO;
    if (shine_check_config(info->rate, bitrate) < 0)
        return audio_codec_error("MP3 can't be %d samples a second at %d kbit/s", info->rate, bitrate);
    if (!(w = calloc(1, sizeof(*w))))
        return audio_codec_error("out of memory");
    w->fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    w->channels = info->channels;
    w->shine = w->fd < 0 ? NULL : shine_initialise(&config);
    w->per_pass = w->shine ? shine_samples_per_pass(w->shine) : 0;
    w->pending = malloc(sizeof(int16_t) * 2 * (w->per_pass ? w->per_pass : 1));
    if (!w->shine || !w->pending) {
        if (w->shine)
            shine_close(w->shine);
        if (w->fd >= 0)
            close(w->fd);
        free(w->pending);
        free(w);
        return audio_codec_error("can't write %s", path);
    }
    return w;
}

static bool mp3_write(void *state, const int16_t *samples, long frames) {
    struct writer *w = state;

    for (long i = 0; i < frames;) {
        long n = w->per_pass - w->have < frames - i ? w->per_pass - w->have : frames - i;
        int length;

        memcpy(w->pending + w->have * w->channels, samples + i * w->channels, n * w->channels * sizeof(int16_t));
        w->have += n;
        i += n;
        if (w->have == w->per_pass) {
            unsigned char *data = shine_encode_buffer_interleaved(w->shine, w->pending, &length);

            w->have = 0;
            if (!put(w, data, length))
                return audio_codec_error("can't write the MP3") != NULL;
        }
    }
    return true;
}

// What's left is padded out with silence to a whole pass.
static bool mp3_finish(void *state) {
    struct writer *w = state;
    unsigned char *data;
    int length;
    bool ok = true;

    if (w->have) {
        memset(w->pending + w->have * w->channels, 0, (w->per_pass - w->have) * w->channels * sizeof(int16_t));
        data = shine_encode_buffer_interleaved(w->shine, w->pending, &length);
        ok = put(w, data, length);
    }
    data = shine_flush(w->shine, &length);
    ok = put(w, data, length) && ok;
    ok = close(w->fd) == 0 && ok;
    shine_close(w->shine);
    free(w->pending);
    free(w);
    return ok ? true : audio_codec_error("couldn't finish the MP3") != NULL;
}

const struct audio_codec audio_codec = {
    mp3_open, mp3_read, mp3_seek, mp3_close, mp3_create, mp3_write, mp3_finish,
};
