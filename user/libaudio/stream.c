#include "audiod.h"
#include <audio.h>
#include <audio_codec.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct audio_stream {
    int fd, channels;
};

static int connect_audiod(void) {
    struct sockaddr_un at = {.sun_family = AF_UNIX, .sun_path = AUDIOD_SOCKET};
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

    if (fd >= 0 && connect(fd, (struct sockaddr *)&at, sizeof(at)) == 0)
        return fd;
    if (fd >= 0)
        close(fd);
    audio_codec_error("audiod isn't running");
    return -1;
}

static bool say(int fd, uint32_t type, int32_t a, int32_t b) {
    struct audiod_message m = {type, a, b};

    return send(fd, &m, sizeof(m), MSG_NOSIGNAL) == sizeof(m);
}

// Asks something, giving the answer's a, and b if wanted, or false.
static bool ask(int fd, uint32_t type, int32_t a, int32_t b, int32_t *answer, int32_t *flags) {
    struct audiod_message m;

    if (!say(fd, type, a, b) || recv(fd, &m, sizeof(m), 0) != sizeof(m) || m.type != type)
        return false;
    *answer = m.a;
    if (flags)
        *flags = m.b;
    return true;
}

struct audio_stream *audio_play(int rate, int channels) {
    struct audio_stream *s = malloc(sizeof(*s));
    int32_t ok = -1;

    if (!s || (s->fd = connect_audiod()) < 0) {
        free(s);
        return NULL;
    }
    s->channels = channels;
    if (!ask(s->fd, AUDIOD_OPEN, rate, channels, &ok, NULL) || ok != 0) {
        audio_codec_error("there's no sound card that plays %d channels at %d", channels, rate);
        close(s->fd);
        free(s);
        return NULL;
    }
    return s;
}

bool audio_stream_write(struct audio_stream *s, const int16_t *samples, long frames) {
    static __thread unsigned char packet[AUDIOD_MAX_PACKET];
    struct audiod_message m = {AUDIOD_PLAY, 0, 0};

    while (frames > 0) {
        long n = frames < AUDIOD_PLAY_FRAMES ? frames : AUDIOD_PLAY_FRAMES;
        size_t bytes = n * s->channels * sizeof(int16_t);

        memcpy(packet, &m, sizeof(m));
        memcpy(packet + sizeof(m), samples, bytes);
        if (send(s->fd, packet, sizeof(m) + bytes, MSG_NOSIGNAL) < 0)
            return false;
        samples += n * s->channels;
        frames -= n;
    }
    return true;
}

long audio_stream_delay(struct audio_stream *s) {
    int32_t frames = 0;

    return ask(s->fd, AUDIOD_DELAY, 0, 0, &frames, NULL) ? frames : 0;
}

void audio_stream_flush(struct audio_stream *s) {
    say(s->fd, AUDIOD_FLUSH, 0, 0);
}

void audio_stream_close(struct audio_stream *s) {
    if (s) {
        close(s->fd);
        free(s);
    }
}

// One connection for the mixer, kept, and made again if audiod's restarted.
static int mixer_fd = -1;

static bool mixer_ask(uint32_t type, int32_t a, int32_t *answer, int32_t *flags) {
    for (int tries = 0; tries < 2; tries++) {
        if (mixer_fd < 0 && (mixer_fd = connect_audiod()) < 0)
            return false;
        if (ask(mixer_fd, type, a, 0, answer, flags))
            return true;
        close(mixer_fd);
        mixer_fd = -1;
    }
    return false;
}

int audio_outputs(struct audio_output *outputs, int max) {
    for (int tries = 0; tries < 2; tries++) {
        int32_t count, playing;

        if (mixer_fd < 0 && (mixer_fd = connect_audiod()) < 0)
            return -1;
        if (ask(mixer_fd, AUDIOD_OUTPUTS, 0, 0, &count, &playing)) {
            for (int i = 0; i < count; i++) {
                struct audiod_output o;

                if (recv(mixer_fd, &o, sizeof(o), 0) != sizeof(o))
                    return -1;
                if (i < max) {
                    o.key[sizeof(o.key) - 1] = o.name[sizeof(o.name) - 1] = '\0';
                    snprintf(outputs[i].key, sizeof(outputs[i].key), "%s", o.key);
                    snprintf(outputs[i].name, sizeof(outputs[i].name), "%s", o.name);
                    outputs[i].current = i == playing;
                }
            }
            return count;
        }
        close(mixer_fd);
        mixer_fd = -1;
    }
    return -1;
}

bool audio_set_output(const char *key) {
    unsigned char packet[sizeof(struct audiod_message) + 64];
    struct audiod_message m = {AUDIOD_OUTPUT, 0, 0}, answer;
    size_t len = strnlen(key, 63);

    if (mixer_fd < 0 && (mixer_fd = connect_audiod()) < 0)
        return false;
    memcpy(packet, &m, sizeof(m));
    memcpy(packet + sizeof(m), key, len);
    return send(mixer_fd, packet, sizeof(m) + len, MSG_NOSIGNAL) > 0
           && recv(mixer_fd, &answer, sizeof(answer), 0) == sizeof(answer) && answer.type == AUDIOD_OUTPUT && answer.a == 0;
}

static struct audio_mixer mixer_of(int32_t volume, int32_t flags) {
    return (struct audio_mixer){volume, flags & AUDIOD_MUTED, flags & AUDIOD_LOW_LATENCY, flags & AUDIOD_CARD};
}

bool audio_mixer_get(struct audio_mixer *mixer) {
    int32_t volume, flags;

    if (!mixer_ask(AUDIOD_MIXER, 0, &volume, &flags))
        return false;
    *mixer = mixer_of(volume, flags);
    return true;
}

int audio_mixer_watch(struct audio_mixer *mixer) {
    int fd = connect_audiod();
    int32_t volume, flags;

    if (fd >= 0 && !ask(fd, AUDIOD_WATCH, 0, 0, &volume, &flags)) {
        close(fd);
        return -1;
    }
    if (fd >= 0)
        *mixer = mixer_of(volume, flags);
    return fd;
}

bool audio_mixer_changed(int fd, struct audio_mixer *mixer) {
    struct audiod_message m;

    if (recv(fd, &m, sizeof(m), MSG_DONTWAIT) != sizeof(m) || m.type != AUDIOD_WATCH)
        return false;
    *mixer = mixer_of(m.a, m.b);
    return true;
}

static bool mixer_set(uint32_t type, int32_t value) {
    int32_t ok;

    return mixer_ask(type, value, &ok, NULL) && ok == 0;
}

bool audio_set_volume(int volume) {
    return mixer_set(AUDIOD_VOLUME, volume);
}

bool audio_set_muted(bool muted) {
    return mixer_set(AUDIOD_MUTE, muted);
}

bool audio_set_low_latency(bool low) {
    return mixer_set(AUDIOD_LATENCY, low);
}
