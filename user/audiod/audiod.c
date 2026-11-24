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
#include <sys/inotify.h>
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
#define MAX_OUTPUTS 16
#define MAX_LEVELS 32     // the outputs whose volumes are kept, plugged in or not
#define SCAN_MS 5000      // how often the cards are looked at again, for HDMI monitors coming and going

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
static int volume = 100; // the output's that's playing
static bool muted, low_latency;

// What can be played through, and which is: the one chosen, or the one plugged in
// last, or else the computer's own.
struct output {
    char key[64], name[80];
    int card, device;
    bool usb;
};

static struct output outputs[MAX_OUTPUTS];
static int output_count, current = -1;
static char chosen[64];

// Each output's volume, kept whether it's plugged in or not.
struct level {
    char key[64];
    int volume;
    bool muted;
};

static struct level levels[MAX_LEVELS];
static int level_count;

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

// An output's volume, made from the one that's playing if it hasn't one yet.
static struct level *level_of(const char *key) {
    for (int i = 0; i < level_count; i++) {
        if (!strcmp(levels[i].key, key))
            return &levels[i];
    }

    struct level *l = &levels[level_count < MAX_LEVELS ? level_count++ : MAX_LEVELS - 1];

    *l = (struct level){.volume = volume, .muted = muted};
    snprintf(l->key, sizeof(l->key), "%s", key);
    return l;
}

static void keep_level(void) {
    if (current >= 0) {
        struct level *l = level_of(outputs[current].key);

        l->volume = volume;
        l->muted = muted;
    }
}

static void load_conf(void) {
    FILE *f = fopen(CONF, "r");
    char line[128], key[64];
    int a, b;

    while (f && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "level %63s %d %d", key, &a, &b) == 3) {
            struct level *l = level_of(key);

            l->volume = a < 0 ? 0 : a > 100 ? 100 : a;
            l->muted = b;
        } else if (sscanf(line, "chosen %63s", chosen) == 1)
            ;
        else if (sscanf(line, "low-latency %d", &a) == 1)
            low_latency = a;
        else if (sscanf(line, "volume %d", &a) == 1) // for outputs not seen before
            volume = a < 0 ? 0 : a > 100 ? 100 : a;
        else if (sscanf(line, "muted %d", &a) == 1)
            muted = a;
    }
    if (f)
        fclose(f);
}

static void save_conf(void) {
    FILE *f = fopen(CONF ".new", "w");

    keep_level();
    if (!f)
        return;
    fprintf(f, "volume %d\nmuted %d\nlow-latency %d\n", volume, muted, low_latency);
    if (chosen[0])
        fprintf(f, "chosen %s\n", chosen);
    for (int i = 0; i < level_count; i++)
        fprintf(f, "level %s %d %d\n", levels[i].key, levels[i].volume, levels[i].muted);
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

// Whether an HDMI or DisplayPort output has a monitor on it, as its jack says.
static bool monitor_on(int fd, int device) {
    struct snd_ctl_elem_value v = {.id = {.iface = SNDRV_CTL_ELEM_IFACE_CARD}};

    snprintf((char *)v.id.name, sizeof(v.id.name), "HDMI/DP,pcm=%d Jack", device);
    return ioctl(fd, SNDRV_CTL_IOCTL_ELEM_READ, &v) == 0 && v.value.integer.value[0];
}

// Every card's playback devices, from its control device, which can be asked while
// one's playing: the computer's own, USB cards by their names, and HDMI only with a
// monitor on it.
static int scan(struct output *found) {
    int n = 0;

    for (int card = 0; card < 32 && n < MAX_OUTPUTS; card++) {
        char path[64];
        struct snd_ctl_card_info info = {0};
        int fd;

        snprintf(path, sizeof(path), "/dev/snd/controlC%d", card);
        if ((fd = open(path, O_RDONLY | O_CLOEXEC)) < 0)
            continue;
        for (int device = -1; n < MAX_OUTPUTS && ioctl(fd, SNDRV_CTL_IOCTL_CARD_INFO, &info) == 0
                              && ioctl(fd, SNDRV_CTL_IOCTL_PCM_NEXT_DEVICE, &device) == 0 && device >= 0;) {
            struct snd_pcm_info p = {.device = device, .stream = SNDRV_PCM_STREAM_PLAYBACK};

            if (ioctl(fd, SNDRV_CTL_IOCTL_PCM_INFO, &p) < 0)
                continue;
            bool hdmi = strstr((char *)p.id, "HDMI") || strstr((char *)p.name, "HDMI");
            bool usb = !strcmp((char *)info.driver, "USB-Audio");

            if (hdmi && !monitor_on(fd, device))
                continue;
            found[n] = (struct output){.card = card, .device = device, .usb = usb};
            snprintf(found[n].key, sizeof(found[n].key), "%s,%d", (char *)info.id, device);
            snprintf(found[n].name, sizeof(found[n].name), "%s", usb ? (char *)info.name : hdmi ? "HDMI" : "Built-in");
            n++;
        }
        close(fd);
    }
    for (int i = 0; i < n; i++) { // the same names told apart
        for (int j = i + 1; j < n; j++) {
            if (!strcmp(found[i].name, found[j].name)) {
                size_t len = strlen(found[j].name);

                snprintf(found[j].name + len, sizeof(found[j].name) - len, " %d", j - i + 1);
            }
        }
    }
    return n;
}

static void close_card(void) {
    if (running)
        ioctl(pcm, SNDRV_PCM_IOCTL_DROP);
    running = false;
    if (pcm >= 0)
        close(pcm);
    if (ctl >= 0)
        close(ctl);
    pcm = ctl = -1;
    has_volume = has_switch = false;
}

// Plays through an output from now on, at its own volume.
static void play_through(int i) {
    char path[64];

    keep_level();
    close_card();
    current = i;
    if (i < 0) {
        puts("audiod: there's no sound card");
        fflush(stdout);
        return;
    }

    struct level *l = level_of(outputs[i].key);

    volume = l->volume;
    muted = l->muted;
    snprintf(path, sizeof(path), "/dev/snd/pcmC%dD%dp", outputs[i].card, outputs[i].device);
    if ((pcm = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC)) < 0 || !configure()) {
        printf("audiod: can't play through %s\n", outputs[i].name);
        fflush(stdout);
        close_card();
        return;
    }
    printf("audiod: playing through %s\n", outputs[i].name);
    fflush(stdout);
    open_controls(outputs[i].card);
}

static void tell_watchers(void);

// Looks at the cards again: one plugged in is played through, and when the one that
// played's gone, the one chosen, or the computer's own.
static void update_outputs(bool first) {
    struct output found[MAX_OUTPUTS];
    int n = scan(found), plugged = -1, want = -1;
    char was[64] = "";
    bool changed = n != output_count;

    if (current >= 0)
        snprintf(was, sizeof(was), "%s", outputs[current].key);
    for (int i = 0; i < n; i++) {
        bool known = false;

        for (int j = 0; j < output_count; j++)
            known = known || !strcmp(found[i].key, outputs[j].key);
        changed = changed || !known;
        if (!first && !known && found[i].usb)
            plugged = i;
    }
    memcpy(outputs, found, sizeof(found));
    output_count = n;
    current = -1;
    for (int i = 0; i < n; i++) {
        if (!strcmp(outputs[i].key, was))
            current = want = i;
    }
    if (plugged >= 0) {
        want = plugged;
        snprintf(chosen, sizeof(chosen), "%s", outputs[plugged].key);
        save_conf();
    }
    for (int i = 0; want < 0 && i < n; i++) { // the one chosen
        if (!strcmp(outputs[i].key, chosen))
            want = i;
    }
    for (int i = 0; want < 0 && i < n; i++) { // or the computer's own
        if (!outputs[i].usb)
            want = i;
    }
    want = want < 0 && n ? 0 : want;
    if (want != current || (want >= 0 && pcm < 0)) {
        play_through(want);
        changed = true;
    }
    if (changed)
        tell_watchers();
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

    if (ioctl(pcm, SNDRV_PCM_IOCTL_STATUS, &status) < 0) {
        if (errno == ENODEV) // unplugged, so another plays
            update_outputs(false);
        return false;
    }
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
        if (errno == ENODEV)
            update_outputs(false);
        else if (errno == EPIPE) {
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
    case AUDIOD_OUTPUTS:
        if (!reply(c, AUDIOD_OUTPUTS, output_count, current))
            return false;
        for (int i = 0; i < output_count; i++) {
            struct audiod_output o;

            snprintf(o.key, sizeof(o.key), "%s", outputs[i].key);
            snprintf(o.name, sizeof(o.name), "%s", outputs[i].name);
            if (send(c->fd, &o, sizeof(o), MSG_NOSIGNAL) != sizeof(o))
                return false;
        }
        return true;
    case AUDIOD_OUTPUT: {
        char key[64] = "";
        int which = -1;

        read_seat();
        memcpy(key, packet + sizeof(m), (size_t)n - sizeof(m) < sizeof(key) - 1 ? (size_t)n - sizeof(m) : sizeof(key) - 1);
        for (int i = 0; i < output_count; i++) {
            if (!strcmp(outputs[i].key, key))
                which = i;
        }
        if (!heard(c) || which < 0)
            return reply(c, AUDIOD_OUTPUT, -1, 0);
        snprintf(chosen, sizeof(chosen), "%s", key);
        if (which != current)
            play_through(which);
        save_conf();
        tell_watchers();
        return reply(c, AUDIOD_OUTPUT, 0, 0);
    }
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
    // Cards come and go as their files in /dev/snd do, and are looked at again a moment
    // after, once the kernel's made the rest of them.
    int notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC), watched = -1;

    update_outputs(true);
    for (int64_t seat_read = 0, scanned = now_ms(), scan_due = -1;;) {
        struct pollfd fds[MAX_CLIENTS + 3];
        int slot[MAX_CLIENTS + 3], n = 0, waiting = 1000;

        if (now_ms() - seat_read >= 1000) {
            read_seat();
            seat_read = now_ms();
        }
        if (watched < 0)
            watched = inotify_add_watch(notify, "/dev/snd", IN_CREATE | IN_DELETE);
        if ((scan_due >= 0 && now_ms() >= scan_due) || now_ms() - scanned >= SCAN_MS) {
            update_outputs(false);
            scanned = now_ms();
            scan_due = -1;
        }
        if (scan_due >= 0)
            waiting = (int)(scan_due - now_ms() > 0 ? scan_due - now_ms() : 0);
        fds[n] = (struct pollfd){listener, POLLIN, 0};
        slot[n++] = -1;
        fds[n] = (struct pollfd){notify, POLLIN, 0};
        slot[n++] = -3;
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
        }
        if (poll(fds, n, waiting) < 0 && errno != EINTR)
            return 1;
        for (int k = 0; k < n; k++) {
            if (!fds[k].revents)
                continue;
            if (slot[k] == -1)
                accept_client(listener);
            else if (slot[k] == -3) {
                char events[4096];

                while (read(notify, events, sizeof(events)) > 0)
                    ;
                scan_due = now_ms() + 300;
            }
            else if (slot[k] == -2)
                while (pcm >= 0 && mix())
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
