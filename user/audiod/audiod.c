#include "../libaudio/audiod.h"
#include <aximos.h>
#include <audio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <sound/asound.h>
#include <sound/tlv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// The sound server: it plays every program's sound on the card, mixed, through the
// kernel's own ALSA interface. Streams come in at their own rates, are changed to
// 48 kHz and mixed as floats, and the card's given what it takes. Only root, and
// the account whose console is showing, are heard: anyone else's sound is taken
// and dropped, as if it played.

#define CONF "/system/etc/audiod.conf"
#define SEAT_DIR "/run/seat" // the account logged in on each console, by name
#define ACTIVE_CONSOLE "/sys/class/tty/tty0/active"
#define MAX_CLIENTS 32
#define RING_FRAMES 65536 // what each stream holds, at 48 kHz: more than a play at 8 kHz becomes
#define IDLE_MS 2000      // how long the card keeps playing silence once there's nothing
#define PERIOD_NORMAL 480 // frames: 10 ms, twice over, so about 20 ms
#define PERIOD_LOW 120    // and 2.5 ms, so about 5 ms

struct client {
    int fd;
    uid_t uid;
    bool stream, closed;         // whether it's a stream, and whether it's gone, with sound still to play
    bool watching;               // whether it's told of each change to the mixer
    int rate, channels;
    struct audio_resampler *resampler;
    float ring[RING_FRAMES][2];
    long head, count;            // where the next frame to mix is, and how many there are
    int64_t end;                 // where its last frame mixed is in what the card's been given
};

static struct client *clients[MAX_CLIENTS];
static int volume = 100;
static bool muted, low_latency;

static int pcm = -1;                   // the card's playback device
static snd_pcm_format_t format;
static long period, buffer;            // in frames, as the card took them
static bool running;                   // whether it's playing
static int64_t idle_since = -1;
static int64_t given;                  // frames given the card since it started
static uid_t seat_uid = (uid_t)-1;     // who's heard, besides root

static int64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void load_conf(void) {
    FILE *f = fopen(CONF, "r");
    char key[32];
    int value;

    while (f && fscanf(f, "%31s %d", key, &value) == 2) {
        if (!strcmp(key, "volume"))
            volume = value < 0 ? 0 : value > 100 ? 100 : value;
        else if (!strcmp(key, "muted"))
            muted = value;
        else if (!strcmp(key, "low-latency"))
            low_latency = value;
    }
    if (f)
        fclose(f);
}

static void save_conf(void) {
    FILE *f = fopen(CONF ".new", "w");

    if (!f)
        return;
    fprintf(f, "volume %d\nmuted %d\nlow-latency %d\n", volume, muted, low_latency);
    if (fclose(f) == 0)
        rename(CONF ".new", CONF);
}

// Who's at the screen: the account logged in on the console that's showing.
static void read_seat(void) {
    char console[32] = "", path[64 + sizeof(console)];
    FILE *f = fopen(ACTIVE_CONSOLE, "r");

    seat_uid = (uid_t)-1;
    if (!f)
        return;
    if (fscanf(f, "%31s", console) != 1)
        console[0] = '\0';
    fclose(f);
    snprintf(path, sizeof(path), SEAT_DIR "/%s", console);
    if (console[0] && (f = fopen(path, "r"))) {
        unsigned uid;

        if (fscanf(f, "%u", &uid) == 1)
            seat_uid = uid;
        fclose(f);
    }
}

static bool heard(const struct client *c) {
    return c->uid == 0 || c->uid == seat_uid;
}

// The ALSA hardware parameters, with every value allowed, narrowed by the setters.
static void params_any(struct snd_pcm_hw_params *p) {
    memset(p, 0, sizeof(*p));
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_MASK - SNDRV_PCM_HW_PARAM_FIRST_MASK; i++)
        memset(&p->masks[i], 0xff, sizeof(p->masks[i]));
    for (int i = 0; i <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; i++)
        p->intervals[i] = (struct snd_interval){.min = 0, .max = UINT_MAX};
    p->rmask = ~0u;
    p->info = ~0u;
}

static void param_mask(struct snd_pcm_hw_params *p, int param, unsigned value) {
    struct snd_mask *m = &p->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];

    memset(m, 0, sizeof(*m));
    m->bits[value / 32] = 1u << (value % 32);
}

static void param_range(struct snd_pcm_hw_params *p, int param, unsigned min, unsigned max) {
    p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL] = (struct snd_interval){.min = min, .max = max, .integer = 1};
}

static unsigned param_value(const struct snd_pcm_hw_params *p, int param) {
    return p->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL].min;
}

// Sets the card up: 32-bit where it takes it, else 16, stereo at 48 kHz, in two periods
// of the latency asked for, or the nearest it can do above that.
static bool configure(void) {
    static const snd_pcm_format_t formats[] = {SNDRV_PCM_FORMAT_S32_LE, SNDRV_PCM_FORMAT_S16_LE};

    for (long want = low_latency ? PERIOD_LOW : PERIOD_NORMAL; want <= 4096; want *= 2) {
        for (size_t f = 0; f < sizeof(formats) / sizeof(*formats); f++) {
            struct snd_pcm_hw_params p;

            params_any(&p);
            param_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
            param_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, formats[f]);
            param_mask(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
            param_range(&p, SNDRV_PCM_HW_PARAM_CHANNELS, 2, 2);
            param_range(&p, SNDRV_PCM_HW_PARAM_RATE, AUDIOD_RATE, AUDIOD_RATE);
            param_range(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, want, want);
            param_range(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, 2 * want, 2 * want);
            if (ioctl(pcm, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0)
                continue;
            format = formats[f];
            period = param_value(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
            buffer = param_value(&p, SNDRV_PCM_HW_PARAM_BUFFER_SIZE);

            struct snd_pcm_sw_params s = {
                .tstamp_mode = SNDRV_PCM_TSTAMP_NONE,
                .period_step = 1,
                .avail_min = period,
                .start_threshold = period,
                .stop_threshold = buffer,
                .boundary = buffer,
            };

            while (s.boundary * 2 <= (snd_pcm_uframes_t)(LONG_MAX - buffer))
                s.boundary *= 2;
            if (ioctl(pcm, SNDRV_PCM_IOCTL_SW_PARAMS, &s) < 0)
                return false;
            printf("audiod: playing %d-bit at %d Hz, %ld frames a period, %ld in all\n",
                   format == SNDRV_PCM_FORMAT_S32_LE ? 32 : 16, AUDIOD_RATE, period, buffer);
            fflush(stdout);
            return true;
        }
    }
    return false;
}

// The card's own volume, where it has one: its main playback control, which the
// volume's set on in decibels, as far down as it goes, with its switch for muting.
// What's quieter than that is audiod's, as is all of it on a card without one.
struct control {
    struct snd_ctl_elem_id id;
    unsigned count;
    long min, max;
    bool scaled;          // whether its steps are in decibels, from db_min, db_step apart
    int db_min, db_step;  // in hundredths of a decibel
};

static int ctl = -1;
static struct control card_volume, card_switch;
static bool has_volume, has_switch;
static float soft_gain = 1; // what audiod scales by, besides the card

static bool find(const char *name, struct control *c) {
    struct snd_ctl_elem_info info = {.id = {.iface = SNDRV_CTL_ELEM_IFACE_MIXER}};
    unsigned tlv[16] = {0};
    struct snd_ctl_tlv *t = (struct snd_ctl_tlv *)tlv;

    snprintf((char *)info.id.name, sizeof(info.id.name), "%s", name);
    if (ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_INFO, &info) < 0 || !(info.access & SNDRV_CTL_ELEM_ACCESS_WRITE))
        return false;
    *c = (struct control){.id = info.id, .count = info.count < 128 ? info.count : 128, .min = info.value.integer.min,
                          .max = info.type == SNDRV_CTL_ELEM_TYPE_BOOLEAN ? 1 : info.value.integer.max};
    t->numid = info.id.numid;
    t->length = sizeof(tlv) - sizeof(*t);
    if ((info.access & SNDRV_CTL_ELEM_ACCESS_TLV_READ) && ioctl(ctl, SNDRV_CTL_IOCTL_TLV_READ, t) == 0) {
        unsigned *v = t->tlv;

        if (v[0] == SNDRV_CTL_TLVT_DB_SCALE) {
            c->scaled = true;
            c->db_min = (int)v[2];
            c->db_step = v[3] & 0xffff;
        } else if ((v[0] == SNDRV_CTL_TLVT_DB_MINMAX || v[0] == SNDRV_CTL_TLVT_DB_MINMAX_MUTE) && c->max > c->min) {
            c->scaled = true;
            c->db_min = (int)v[2];
            c->db_step = ((int)v[3] - (int)v[2]) / (c->max - c->min);
        }
    }
    return true;
}

static void write_control(const struct control *c, long value) {
    struct snd_ctl_elem_value v = {.id = c->id};

    for (unsigned i = 0; i < c->count; i++)
        v.value.integer.value[i] = value;
    ioctl(ctl, SNDRV_CTL_IOCTL_ELEM_WRITE, &v);
}

// The step for a level in hundredths of a decibel, at or above it, so audiod only ever
// takes away the rest.
static long step_for(const struct control *c, int db) {
    long step = db <= c->db_min ? 0 : (db - c->db_min + c->db_step - 1) / (c->db_step ? c->db_step : 1);

    return c->min + step > c->max ? c->max : c->min + step;
}

// Sets the volume on the card and in audiod: its square, in loudness, which sounds even.
static void apply_volume(void) {
    float wanted = volume ? 40 * log10f(volume / 100.0f) : -1000; // in decibels

    if (has_switch)
        write_control(&card_switch, !muted);
    if (!has_volume)
        soft_gain = muted ? 0 : (float)(volume * volume) / 10000;
    else if (card_volume.scaled) {
        long step = step_for(&card_volume, (int)lroundf(wanted * 100));
        float got = (card_volume.db_min + (step - card_volume.min) * card_volume.db_step) / 100.0f;

        write_control(&card_volume, step);
        soft_gain = muted || !volume ? 0 : powf(10, (wanted - got) / 20);
        printf("audiod: volume %d, the card at %.2f dB and %.2f dB more here\n", volume, got, wanted > got ? 0 : wanted - got);
        fflush(stdout);
    } else { // steps of unknown size, taken as even in loudness
        write_control(&card_volume, card_volume.min + lroundf((card_volume.max - card_volume.min) * volume * volume / 10000.0f));
        soft_gain = muted || !volume ? 0 : 1;
    }
    if (muted && !has_switch)
        soft_gain = 0;
}

// Opens the card's controls: its main volume, and the others it plays through at no
// gain, switched on, as Linux starts them muted.
static void open_controls(int card) {
    static const char *const mains[] = {"Master", "PCM", "Speaker", "Headphone", "Line Out"};
    static const char *const others[] = {"PCM", "Speaker", "Headphone", "Line Out", "Front", "Bass Speaker"};
    char path[64], name[64];
    struct control c;

    snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
    if (ctl >= 0)
        close(ctl);
    has_volume = has_switch = false;
    if ((ctl = open(path, O_RDWR | O_CLOEXEC)) < 0)
        return;
    for (size_t i = 0; !has_volume && i < sizeof(mains) / sizeof(*mains); i++) {
        snprintf(name, sizeof(name), "%s Playback Volume", mains[i]);
        if ((has_volume = find(name, &card_volume))) {
            snprintf(name, sizeof(name), "%s Playback Switch", mains[i]);
            has_switch = find(name, &card_switch);
            printf("audiod: the volume's %s, %s\n", mains[i], card_volume.scaled ? "in decibels" : "in steps");
            fflush(stdout);
        }
    }
    for (size_t i = 0; i < sizeof(others) / sizeof(*others); i++) {
        snprintf(name, sizeof(name), "%s Playback Volume", others[i]);
        if (find(name, &c) && (!has_volume || c.id.numid != card_volume.id.numid))
            write_control(&c, c.scaled ? step_for(&c, 0) : c.max);
        snprintf(name, sizeof(name), "%s Playback Switch", others[i]);
        if (find(name, &c) && (!has_switch || c.id.numid != card_switch.id.numid))
            write_control(&c, 1);
    }
    apply_volume();
}

// The first card that plays anything but HDMI, which is the computer's own speakers
// and headphones.
static bool open_card(void) {
    for (int card = 0; card < 8; card++) {
        for (int device = 0; device < 8; device++) {
            char path[64];
            struct snd_pcm_info info = {0};
            int fd;

            snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dp", card, device);
            if ((fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0)
                continue;
            if (ioctl(fd, SNDRV_PCM_IOCTL_INFO, &info) == 0 && !strstr((char *)info.name, "HDMI")
                && !strstr((char *)info.id, "HDMI")) {
                pcm = fd;
                printf("audiod: card %d, %s\n", card, (char *)info.name);
                fflush(stdout);
                open_controls(card);
                if (configure())
                    return true;
                pcm = -1;
            }
            close(fd);
        }
    }
    return false;
}

static void start(void) {
    if (!running && pcm >= 0 && ioctl(pcm, SNDRV_PCM_IOCTL_PREPARE) == 0) {
        running = true;
        given = 0;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i])
                clients[i]->end = 0;
        }
    }
}

static void stop(void) {
    if (running)
        ioctl(pcm, SNDRV_PCM_IOCTL_DROP);
    running = false;
}

static void free_client(int i) {
    struct client *c = clients[i];

    close(c->fd);
    if (c->resampler)
        audio_resampler_free(c->resampler);
    free(c);
    clients[i] = NULL;
}

// How many of a stream's frames are still to be heard, at 48 kHz: what's waiting to
// be mixed, and what the card's been given that it hasn't played.
static long stream_delay(const struct client *c) {
    snd_pcm_sframes_t d = 0;
    int64_t heard_up_to = given - (running && ioctl(pcm, SNDRV_PCM_IOCTL_DELAY, &d) == 0 && d > 0 ? d : 0);

    return c->count + (running && c->end > heard_up_to ? c->end - heard_up_to : 0);
}

// A period of everything, mixed, at the volume, onto the card. Returns false if it
// hasn't room for one yet.
static bool mix(void) {
    static float out[4096][2];
    static int32_t wide[4096 * 2];
    static int16_t narrow[4096 * 2];
    float scale = soft_gain;
    struct snd_pcm_status status = {0};
    bool any = false;

    if (ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status) < 0)
        return false;
    if (status.state == SNDRV_PCM_STATE_XRUN) { // it ran out, so it starts again
        printf("audiod: the card ran out\n");
        fflush(stdout);
        running = false;
        start();
    } else if ((long)status.avail < period)
        return false;
    memset(out, 0, period * sizeof(*out));
    for (int i = 0; i < MAX_CLIENTS; i++) {
        struct client *c = clients[i];
        long n = c && c->stream ? (c->count < period ? c->count : period) : 0;

        for (long f = 0; f < n; f++) {
            float *in = c->ring[(c->head + f) % RING_FRAMES];

            if (heard(c)) {
                out[f][0] += in[0];
                out[f][1] += in[1];
            }
        }
        if (n) {
            c->head = (c->head + n) % RING_FRAMES;
            c->count -= n;
            c->end = given + n;
            any = true;
        }
    }
    for (long f = 0; f < period; f++) {
        for (int ch = 0; ch < 2; ch++) {
            float v = out[f][ch] * scale;

            v = v > 1 ? 1 : v < -1 ? -1 : v;
            if (format == SNDRV_PCM_FORMAT_S32_LE)
                wide[2 * f + ch] = (int32_t)(v * 2147483520.0f);
            else
                narrow[2 * f + ch] = (int16_t)(v * 32767);
        }
    }

    struct snd_xferi x = {.buf = format == SNDRV_PCM_FORMAT_S32_LE ? (void *)wide : (void *)narrow, .frames = period};

    if (ioctl(pcm, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
        if (errno == EPIPE) {
            printf("audiod: the card ran out\n");
            fflush(stdout);
            running = false;
            start();
        }
        return false;
    }
    given += period;
    idle_since = any ? -1 : idle_since < 0 ? now_ms() : idle_since;
    return true;
}

static bool reply(struct client *c, uint32_t type, int32_t a, int32_t b) {
    struct audiod_message m = {type, a, b};

    return send(c->fd, &m, sizeof(m), MSG_NOSIGNAL) == sizeof(m);
}

static int32_t mixer_flags(void) {
    return (muted ? AUDIOD_MUTED : 0) | (low_latency ? AUDIOD_LOW_LATENCY : 0) | (pcm >= 0 ? AUDIOD_CARD : 0);
}

// Tells those watching the mixer how it is now.
static void tell_watchers(void) {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i] && clients[i]->watching && !clients[i]->closed) {
            struct audiod_message m = {AUDIOD_WATCH, volume, mixer_flags()};

            send(clients[i]->fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
        }
    }
}

// Takes samples into a stream's ring, changed to 48 kHz stereo.
static void take(struct client *c, const int16_t *samples, long frames) {
    static int16_t stereo[AUDIOD_PLAY_FRAMES * 2], changed[(AUDIOD_PLAY_FRAMES * AUDIOD_RATE / 8000 + 2) * 2];
    const int16_t *in = samples;
    long n = frames;

    if (c->channels == 1) {
        for (long f = 0; f < frames; f++)
            stereo[2 * f] = stereo[2 * f + 1] = samples[f];
        in = stereo;
    }
    if (c->resampler) {
        n = audio_resample(c->resampler, in, frames, changed, sizeof(changed) / sizeof(*changed) / 2);
        in = changed;
    }
    for (long f = 0; f < n && c->count < RING_FRAMES; f++, c->count++) {
        float *at = c->ring[(c->head + c->count) % RING_FRAMES];

        at[0] = in[2 * f] / 32768.0f;
        at[1] = in[2 * f + 1] / 32768.0f;
    }
}

// What a client says. Returns false when it's gone.
static bool serve(struct client *c) {
    static unsigned char packet[AUDIOD_MAX_PACKET];
    ssize_t n = recv(c->fd, packet, sizeof(packet), MSG_DONTWAIT);
    struct audiod_message m;
    bool allowed;

    if (n <= 0)
        return n < 0 && errno == EAGAIN;
    if ((size_t)n < sizeof(m))
        return false;
    memcpy(&m, packet, sizeof(m));
    if (m.type == AUDIOD_VOLUME || m.type == AUDIOD_MUTE || m.type == AUDIOD_LATENCY)
        read_seat();
    allowed = heard(c);
    switch (m.type) {
    case AUDIOD_OPEN:
        read_seat(); // as someone may have just logged in, and this be their first sound
        if (c->stream || m.a < 8000 || m.a > 192000 || (m.b != 1 && m.b != 2) || pcm < 0)
            return reply(c, AUDIOD_OPEN, -1, 0);
        c->stream = true;
        c->rate = m.a;
        c->channels = m.b;
        c->resampler = m.a != AUDIOD_RATE ? audio_resampler_new(m.a, AUDIOD_RATE, 2) : NULL;
        start();
        return reply(c, AUDIOD_OPEN, 0, 0);
    case AUDIOD_PLAY:
        if (!c->stream)
            return false;
        take(c, (const int16_t *)(packet + sizeof(m)), (n - sizeof(m)) / (2 * c->channels));
        start();
        return true;
    case AUDIOD_DELAY:
        return reply(c, AUDIOD_DELAY, (int32_t)(stream_delay(c) * (int64_t)c->rate / AUDIOD_RATE), 0);
    case AUDIOD_FLUSH:
        c->count = 0;
        return true;
    case AUDIOD_MIXER:
        return reply(c, AUDIOD_MIXER, volume, mixer_flags());
    case AUDIOD_WATCH:
        c->watching = true;
        return reply(c, AUDIOD_WATCH, volume, mixer_flags());
    case AUDIOD_VOLUME:
    case AUDIOD_MUTE:
    case AUDIOD_LATENCY:
        if (allowed) {
            if (m.type == AUDIOD_VOLUME)
                volume = m.a < 0 ? 0 : m.a > 100 ? 100 : m.a;
            else if (m.type == AUDIOD_MUTE)
                muted = m.a;
            else if (low_latency != (m.a != 0) && pcm >= 0) {
                low_latency = m.a;
                stop();
                ioctl(pcm, SNDRV_PCM_IOCTL_HW_FREE);
                configure();
            }
            apply_volume();
            save_conf();
            tell_watchers();
        }
        return reply(c, m.type, allowed ? 0 : -1, 0);
    }
    return false;
}

static void accept_client(int listener) {
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    struct ucred who;
    socklen_t size = sizeof(who);

    if (fd < 0)
        return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!clients[i] && getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &who, &size) == 0
            && (clients[i] = calloc(1, sizeof(struct client)))) {
            clients[i]->fd = fd;
            clients[i]->uid = who.uid;
            return;
        }
    }
    close(fd);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: audiod\n"
                            "Plays every program's sound on the sound card, mixed, at the volume set,\n"
                            "for root and the account whose console is showing.\n");

    struct sockaddr_un at = {.sun_family = AF_UNIX, .sun_path = AUDIOD_SOCKET};
    int listener = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);

    signal(SIGPIPE, SIG_IGN);
    load_conf();
    mkdir(SEAT_DIR, 0755);
    unlink(AUDIOD_SOCKET);
    if (listener < 0 || bind(listener, (struct sockaddr *)&at, sizeof(at)) < 0 || chmod(AUDIOD_SOCKET, 0666) < 0
        || listen(listener, 16) < 0) {
        perror("audiod");
        return 1;
    }
    if (!open_card()) {
        puts("audiod: there's no sound card yet");
        fflush(stdout);
    }
    for (int64_t seat_read = 0, card_tried = now_ms();;) {
        struct pollfd fds[MAX_CLIENTS + 2];
        int slot[MAX_CLIENTS + 2], n = 0, waiting = running ? -1 : 1000;

        if (now_ms() - seat_read >= 1000) {
            read_seat();
            seat_read = now_ms();
        }
        if (pcm < 0 && now_ms() - card_tried >= 2000) { // as its driver may still be starting
            if (open_card())
                tell_watchers();
            card_tried = now_ms();
        }
        fds[n] = (struct pollfd){listener, POLLIN, 0};
        slot[n++] = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            struct client *c = clients[i];

            // Heard from only while there's room for a play at 8 kHz, so it waits meanwhile
            if (c && !c->closed && (!c->stream || RING_FRAMES - c->count > AUDIOD_PLAY_FRAMES * AUDIOD_RATE / 8000 + 2)) {
                fds[n] = (struct pollfd){c->fd, POLLIN, 0};
                slot[n++] = i;
            }
        }
        if (running) {
            fds[n] = (struct pollfd){pcm, POLLOUT, 0};
            slot[n++] = -2;
            waiting = 1000;
        }
        if (poll(fds, n, waiting) < 0 && errno != EINTR)
            return 1;
        for (int k = 0; k < n; k++) {
            if (!fds[k].revents)
                continue;
            if (slot[k] == -1)
                accept_client(listener);
            else if (slot[k] == -2)
                while (mix())
                    ;
            else if (!serve(clients[slot[k]]))
                clients[slot[k]]->closed = true; // what it wrote still plays
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i] && clients[i]->closed && (!clients[i]->stream || !clients[i]->count))
                free_client(i);
        }
        if (running && idle_since >= 0 && now_ms() - idle_since > IDLE_MS) {
            bool streams = false;

            for (int i = 0; i < MAX_CLIENTS; i++)
                streams = streams || (clients[i] && clients[i]->stream);
            if (!streams) {
                stop();
                idle_since = -1;
            }
        }
    }
}
