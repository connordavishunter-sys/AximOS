#include <audio.h>
#include <audio_types.h>
#include <errno_values.h>
#include <hda.h>
#include <irq.h>
#include <mm.h>
#include <process.h>
#include <serial.h>
#include <string.h>

// Each stream is what one open file has written, waiting to be mixed. On every
// tick they're mixed into the sound card's buffer, keeping AHEAD_MS ahead of
// where it's playing, at the card's rate: the rate of the stream that started it,
// if the card has that, so one program on its own is played as it was written.

#define STREAM_PAGES 8
#define STREAM_BYTES (STREAM_PAGES * PAGE_SIZE)
#define AHEAD_MS 60
#define GUARD 512
#define MIX_FRAMES 256
#define IDLE_TICKS 50  // with nothing to play for this long, the card stops
#define DRAIN_TICKS 200 // the longest close waits for what's left to be mixed
#define ONE (1 << 16)   // in the fixed point the rates are changed with

struct audio_stream {
    bool used;
    int rate;
    uint8_t *pages[STREAM_PAGES];
    uint64_t phys[STREAM_PAGES];
    uint32_t head, fill; // where the oldest byte is, and how many are waiting
    uint32_t step, frac; // input samples an output apart, and between which two it is
    int16_t now[2], next[2];
    bool primed;    // now and next are the two it's between
    bool mixed;     // its last sample's been mixed, to end at until in the card's buffer
    uint32_t until;
};

static struct audio_stream streams[AUDIO_STREAMS];
static bool present, running;
static int card_rate = AUDIO_RATE_DEFAULT, volume = 100;
static bool muted;
static uint32_t write_at, play_at, ahead, idle;

void audio_init(void) {
    present = hda_init();
}

bool audio_present(void) {
    return present;
}

static bool take_frame(struct audio_stream *s, int16_t frame[2]) {
    if (s->fill < AUDIO_FRAME_BYTES)
        return false;
    memcpy(frame, s->pages[s->head / PAGE_SIZE] + s->head % PAGE_SIZE, AUDIO_FRAME_BYTES);
    s->head = (s->head + AUDIO_FRAME_BYTES) % STREAM_BYTES;
    s->fill -= AUDIO_FRAME_BYTES;
    return true;
}

// Adds one output sample of a stream, between two of its own when the rates
// differ, returning whether it had one.
static bool add_sample(struct audio_stream *s, int32_t sum[2]) {
    int16_t frame[2];

    if (s->rate == card_rate) {
        if (!take_frame(s, frame))
            return false;
        sum[0] += frame[0];
        sum[1] += frame[1];
        return true;
    }
    if (!s->primed) {
        if (s->fill < 2 * AUDIO_FRAME_BYTES)
            return false;
        take_frame(s, s->now);
        take_frame(s, s->next);
        s->frac = 0;
        s->primed = true;
    }
    for (int c = 0; c < 2; c++)
        sum[c] += s->now[c] + (int32_t)(((int64_t)(s->next[c] - s->now[c]) * s->frac) >> 16);
    for (s->frac += s->step; s->primed && s->frac >= ONE; s->frac -= ONE) {
        memcpy(s->now, s->next, sizeof(s->now));
        s->primed = take_frame(s, s->next);
    }
    return true;
}

// Volume follows its square, which sounds more even, as a fraction of ONE.
static int32_t gain(void) {
    return muted ? 0 : volume * volume * ONE / 10000;
}

static void mix(uint32_t frames) {
    int16_t out[MIX_FRAMES * 2];
    int32_t g = gain();

    for (uint32_t i = 0; i < frames; i++) {
        int32_t sum[2] = {0, 0};

        for (int k = 0; k < AUDIO_STREAMS; k++) {
            struct audio_stream *s = &streams[k];

            if (s->used && add_sample(s, sum)) {
                s->mixed = true;
                s->until = (write_at + (i + 1) * AUDIO_FRAME_BYTES) % HDA_BUFFER_BYTES;
            }
        }
        for (int c = 0; c < 2; c++) {
            int32_t v = (int32_t)(((int64_t)sum[c] * g) >> 16);

            out[2 * i + c] = v < -32768 ? -32768 : v > 32767 ? 32767 : v;
        }
    }
    hda_put(write_at, out, frames * AUDIO_FRAME_BYTES);
    write_at = (write_at + frames * AUDIO_FRAME_BYTES) % HDA_BUFFER_BYTES;
    ahead += frames * AUDIO_FRAME_BYTES;
}

static void fill_ahead(void) {
    uint32_t target = card_rate * AHEAD_MS / 1000 * AUDIO_FRAME_BYTES;

    while (ahead < target) {
        uint32_t frames = (target - ahead) / AUDIO_FRAME_BYTES;

        mix(frames < MIX_FRAMES ? frames : MIX_FRAMES);
    }
    process_wakeup(streams);
}

static bool anything_waiting(void) {
    for (int k = 0; k < AUDIO_STREAMS; k++) {
        if (streams[k].used && streams[k].fill >= AUDIO_FRAME_BYTES)
            return true;
    }
    return false;
}

static void set_step(struct audio_stream *s) {
    s->step = (uint32_t)(((uint64_t)s->rate << 16) / card_rate);
    s->primed = false;
}

static void stop(void) {
    hda_stop();
    running = false;
    for (int k = 0; k < AUDIO_STREAMS; k++)
        streams[k].mixed = false;
    process_wakeup(streams);
}

// The card runs at the rate of what starts it, when it can.
static void start(int rate) {
    card_rate = hda_supports(rate) ? rate : AUDIO_RATE_DEFAULT;
    for (int k = 0; k < AUDIO_STREAMS; k++)
        set_step(&streams[k]);
    hda_set_rate(card_rate);
    write_at = play_at = ahead = idle = 0;
    fill_ahead();
    hda_start();
    running = true;
}

void audio_tick(void) {
    if (!running)
        return;

    uint32_t at = hda_played(), played = (at + HDA_BUFFER_BYTES - play_at) % HDA_BUFFER_BYTES;

    play_at = at;
    if (played >= ahead) { // fallen behind, so it carries on just after where it's playing
        if (anything_waiting())
            kprintf("audio: fell %u ms behind\n", (played - ahead) / AUDIO_FRAME_BYTES * 1000 / card_rate);
        ahead = 0;
        write_at = (at + GUARD) % HDA_BUFFER_BYTES;
    } else {
        ahead -= played;
    }
    idle = anything_waiting() ? 0 : idle + 1;
    if (idle >= IDLE_TICKS)
        stop();
    else
        fill_ahead();
}

int audio_open(struct audio_stream **out) {
    struct audio_stream *s = NULL;

    if (!present)
        return -ENODEV;
    for (int k = 0; k < AUDIO_STREAMS && !s; k++)
        s = streams[k].used ? NULL : &streams[k];
    if (!s)
        return -EBUSY;
    *s = (struct audio_stream){.rate = AUDIO_RATE_DEFAULT};
    for (int i = 0; i < STREAM_PAGES; i++) {
        s->phys[i] = mm_alloc_kernel_page();
        if (!s->phys[i]) {
            while (i-- > 0)
                mm_free_kernel_page(s->phys[i]);
            return -ENOMEM;
        }
        s->pages[i] = mm_phys_to_virt(s->phys[i]);
    }
    set_step(s);
    s->used = true;
    *out = s;
    return 0;
}

// What's been written is mixed first.
void audio_close(struct audio_stream *s) {
    for (int i = 0; i < DRAIN_TICKS && running && s->fill >= AUDIO_FRAME_BYTES; i++)
        process_sleep_uninterruptible(streams, 1);
    s->used = false;
    for (int i = 0; i < STREAM_PAGES; i++)
        mm_free_kernel_page(s->phys[i]);
}

static uint32_t room(const struct audio_stream *s) {
    return (STREAM_BYTES - s->fill) & ~(AUDIO_FRAME_BYTES - 1);
}

bool audio_writable(const struct audio_stream *s) {
    return room(s) > 0;
}

int64_t audio_write(struct audio_stream *s, const void *buf, size_t len, bool nonblocking) {
    const uint8_t *from = buf;
    size_t done = 0;

    len &= ~(size_t)(AUDIO_FRAME_BYTES - 1);
    while (done < len) {
        uint32_t n = room(s);

        if (n == 0) {
            if (nonblocking)
                break;
            if (!process_sleep(streams))
                return done ? (int64_t)done : -EINTR;
            continue;
        }
        n = n < len - done ? n : len - done;
        for (uint32_t copied = 0; copied < n;) {
            uint32_t at = (s->head + s->fill) % STREAM_BYTES, part = PAGE_SIZE - at % PAGE_SIZE;

            part = part < n - copied ? part : n - copied;
            memcpy(s->pages[at / PAGE_SIZE] + at % PAGE_SIZE, from + done + copied, part);
            copied += part;
            s->fill += part;
        }
        done += n;
        if (!running)
            start(s->rate);
    }
    return done ? (int64_t)done : -EAGAIN;
}

int audio_ioctl(struct audio_stream *s, uint64_t request, void *arg) {
    switch (request) {
    case AUDIO_SET_RATE: {
        int rate = *(int *)arg;

        if (rate < 8000 || rate > 192000)
            return -EINVAL;
        s->rate = rate;
        set_step(s);
        if (running && !anything_waiting())
            stop(); // so the next to play picks the card's rate
        return 0;
    }
    case AUDIO_GET_DELAY: {
        // What's waiting, and what of it's been mixed but not heard, in its own samples.
        uint32_t heard_in = s->mixed && running ? (s->until + HDA_BUFFER_BYTES - play_at) % HDA_BUFFER_BYTES : 0;

        heard_in = heard_in <= ahead ? heard_in : 0; // or it's been played already
        *(int *)arg = (int)(s->fill + (uint64_t)heard_in / AUDIO_FRAME_BYTES * s->rate / card_rate * AUDIO_FRAME_BYTES);
        return 0;
    }
    case AUDIO_FLUSH:
        s->head = s->fill = 0;
        s->primed = false;
        return 0;
    }
    return -ENOTTY;
}

int audio_mixer_ioctl(uint64_t request, void *arg) {
    int value = *(int *)arg;

    switch (request) {
    case MIXER_GET_VOLUME:
        *(int *)arg = volume;
        return 0;
    case MIXER_SET_VOLUME:
        volume = value < 0 ? 0 : value > 100 ? 100 : value;
        return 0;
    case MIXER_GET_MUTE:
        *(int *)arg = muted;
        return 0;
    case MIXER_SET_MUTE:
        muted = value != 0;
        return 0;
    }
    return -ENOTTY;
}
