#include <audio.h>
#include <audio_types.h>
#include <dirent.h>
#include <fcntl.h>
#include <gui.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_TRACKS 256
#define CHUNK 1024 // samples a channel decoded at a time, about 20 ms
#define TICK_MS 200
#define SEEK_STEP 5 // seconds, for Left and Right
#define PLAYLIST "##playlist"
#define POSITION "##position"
#define VOLUME "Volume##volume"

// What the window and the thread that plays share, under lock. The window asks
// for things, and the thread says how it's going.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static struct {
    char path[GUI_PATH_MAX]; // what to play, or nothing to stop
    unsigned asked;          // goes up each time path is set
    bool paused;
    int64_t seek; // where to go, in samples, or -1
    int volume;   // 0 to 100

    unsigned playing;              // the ask what follows is about
    int64_t position, length;      // in samples
    int rate, channels, bitrate;
    bool finished;                 // it played to the end
    char error[GUI_PATH_MAX + 64];
} shared = {.seek = -1, .volume = 80};

static char tracks[MAX_TRACKS][GUI_PATH_MAX];
static const char *names[MAX_TRACKS];
static int track_count, current = -1, selected;
static struct gui_window *window;
static struct gui_file_picker picker;

// The volume follows the square of where it's set, which sounds more even.
static void apply_volume(int16_t *samples, long count, int volume) {
    int scale = volume * volume; // out of 10000

    for (long i = 0; volume < 100 && i < count; i++)
        samples[i] = (int16_t)(samples[i] * scale / 10000);
}

// How much of what's been written is still to be heard, in the file's samples.
static int64_t delay(int fd, int rate, int out_rate) {
    int bytes = 0;

    ioctl(fd, AUDIO_GET_DELAY, &bytes);
    return (int64_t)bytes / AUDIO_FRAME_BYTES * rate / out_rate;
}

// Waits while paused, returning false if something else was asked for meanwhile.
// Pausing forgets what's waiting to be heard, so it goes back to where it stopped.
static bool wait_while_paused(unsigned ask, int fd, struct audio_file *f, int64_t *base, int64_t *written, int rate,
                              int out_rate) {
    int64_t at = *base + *written - delay(fd, rate, out_rate);

    ioctl(fd, AUDIO_FLUSH, NULL);
    shared.position = at;
    while (shared.paused && shared.asked == ask)
        pthread_cond_wait(&changed, &lock);
    audio_seek(f, at);
    *base = at;
    *written = 0;
    return shared.asked == ask;
}

static void play(const char *path, unsigned ask) {
    struct audio_info info;
    struct audio_file *f = audio_open(path, &info);
    int fd = f ? open("/dev/audio", O_WRONLY) : -1, out_rate;
    struct audio_resampler *resampler = NULL;
    int64_t base = 0, written = 0;
    static int16_t in[CHUNK * 2], stereo[CHUNK * 2], out[(CHUNK * 48000 / 8000 + 2) * 2];

    pthread_mutex_lock(&lock);
    if (!f || fd < 0) {
        snprintf(shared.error, sizeof(shared.error), "Can't play it: %s", f ? "there's no sound card" : audio_error());
        shared.finished = true;
        pthread_mutex_unlock(&lock);
        if (f)
            audio_close(f);
        return;
    }
    shared.length = info.frames;
    shared.rate = info.rate;
    shared.channels = info.channels;
    shared.bitrate = info.bitrate;
    pthread_mutex_unlock(&lock);

    // The sound card plays most rates, and anything else is changed to one it does.
    out_rate = info.rate;
    if (ioctl(fd, AUDIO_SET_RATE, &out_rate) < 0) {
        out_rate = AUDIO_RATE_DEFAULT;
        resampler = audio_resampler_new(info.rate, out_rate, 2);
    }
    for (;;) {
        pthread_mutex_lock(&lock);
        bool going = shared.asked == ask
                     && (!shared.paused || wait_while_paused(ask, fd, f, &base, &written, info.rate, out_rate));

        if (!going) {
            pthread_mutex_unlock(&lock);
            ioctl(fd, AUDIO_FLUSH, NULL); // it's stopped, or something else is to play
            break;
        }
        if (shared.seek >= 0) {
            ioctl(fd, AUDIO_FLUSH, NULL);
            audio_seek(f, shared.seek);
            base = shared.seek;
            written = 0;
            shared.seek = -1;
        }
        shared.position = base + written - delay(fd, info.rate, out_rate);

        int volume = shared.volume;

        pthread_mutex_unlock(&lock);

        long n = audio_read(f, in, CHUNK);

        if (n <= 0) {
            // The end: once what's waiting has been heard, it's finished.
            if (delay(fd, info.rate, out_rate) > 0) {
                usleep(20000);
                continue;
            }
            pthread_mutex_lock(&lock);
            if (n < 0)
                snprintf(shared.error, sizeof(shared.error), "%s", audio_error());
            shared.finished = shared.asked == ask;
            shared.position = base + written;
            pthread_mutex_unlock(&lock);
            break;
        }
        written += n;
        if (info.channels == 1) {
            for (long i = 0; i < n; i++)
                stereo[2 * i] = stereo[2 * i + 1] = in[i];
        } else {
            memcpy(stereo, in, n * 4);
        }
        apply_volume(stereo, n * 2, volume);

        const int16_t *samples = stereo;

        if (resampler) {
            n = audio_resample(resampler, stereo, n, out, sizeof(out) / 4);
            samples = out;
        }
        write(fd, samples, n * AUDIO_FRAME_BYTES);
    }
    audio_resampler_free(resampler);
    audio_close(f);
    close(fd); // waits for what's been written, unless it was flushed
}

// Plays whatever it's asked to, one at a time.
static void *player(void *unused) {
    unsigned done = 0;

    (void)unused;
    for (;;) {
        char path[GUI_PATH_MAX];
        unsigned ask;

        pthread_mutex_lock(&lock);
        while (shared.asked == done || !shared.path[0]) {
            done = shared.asked;
            pthread_cond_wait(&changed, &lock);
        }
        ask = done = shared.asked;
        snprintf(path, sizeof(path), "%s", shared.path);
        shared.playing = ask;
        shared.position = 0;
        shared.length = -1;
        shared.finished = false;
        shared.error[0] = '\0';
        pthread_mutex_unlock(&lock);
        printf("player: playing %s\n", path);
        fflush(stdout);
        play(path, ask);
    }
    return NULL;
}

// Asks for a track to be played, or for it to stop with -1.
static void choose(int track) {
    pthread_mutex_lock(&lock);
    current = track;
    snprintf(shared.path, sizeof(shared.path), "%s", track >= 0 ? tracks[track] : "");
    shared.asked++;
    shared.paused = false;
    shared.seek = -1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    if (track >= 0) {
        char title[GUI_TITLE_MAX];

        selected = track;
        snprintf(title, sizeof(title), "%s - Player", names[track]);
        gui_set_title(window, title);
    }
}

static void add_track(const char *path) {
    for (int i = 0; i < track_count; i++) {
        if (!strcmp(tracks[i], path))
            return;
    }
    if (track_count == MAX_TRACKS)
        return;
    snprintf(tracks[track_count], GUI_PATH_MAX, "%s", path);
    const char *slash = strrchr(tracks[track_count], '/');

    names[track_count] = slash ? slash + 1 : tracks[track_count];
    track_count++;
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Opening a file puts the other sounds in its folder in the list too, in order.
// Returns where it is in the list.
static int open_file(const char *path) {
    char folder[GUI_PATH_MAX], *found[MAX_TRACKS];
    const char *slash = strrchr(path, '/');
    int count = 0;
    DIR *dir;
    struct dirent *d;

    snprintf(folder, sizeof(folder), "%.*s", slash ? (int)(slash - path) : 1, slash ? path : ".");
    if ((dir = opendir(folder[0] ? folder : "/"))) {
        while ((d = readdir(dir)) && count < MAX_TRACKS) {
            if (audio_known(d->d_name) && (found[count] = malloc(GUI_PATH_MAX)))
                snprintf(found[count++], GUI_PATH_MAX, "%s/%s", folder, d->d_name);
        }
        closedir(dir);
    }
    qsort(found, count, sizeof(*found), by_name);
    for (int i = 0; i < count; i++) {
        add_track(found[i]);
        free(found[i]);
    }
    add_track(path);
    for (int i = 0; i < track_count; i++) {
        if (!strcmp(tracks[i], path))
            return i;
    }
    return -1;
}

static void format_time(char *out, size_t size, int64_t samples, int rate) {
    long seconds = rate > 0 && samples > 0 ? (long)(samples / rate) : 0;

    snprintf(out, size, "%ld:%02ld", seconds / 60, seconds % 60);
}

static void seek_to(int64_t sample) {
    pthread_mutex_lock(&lock);
    shared.seek = sample < 0 ? 0 : shared.length > 0 && sample > shared.length ? shared.length : sample;
    shared.position = shared.seek;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

static void toggle_pause(void) {
    if (current < 0) {
        if (track_count)
            choose(selected);
        return;
    }
    pthread_mutex_lock(&lock);
    shared.paused = !shared.paused;
    printf("player: %s\n", shared.paused ? "paused" : "playing again");
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
}

// Says where the controls are, and what's playing, when they change, for the tests.
static void report(struct gui_ui *ui, const char *what) {
    static char last[16][96];
    static const char *whats[16];
    struct gui_rect r = gui_last_rect(ui);
    char now[96];
    int i = 0;

    while (i < 16 && whats[i] && strcmp(whats[i], what))
        i++;
    if (i == 16)
        return;
    whats[i] = what;
    snprintf(now, sizeof(now), "%d %d %d %d", r.x, r.y, r.width, r.height);
    if (strcmp(last[i], now))
        printf("player: at %s %s\n", what, now);
    snprintf(last[i], sizeof(last[i]), "%s", now);
}

static void frame(struct gui_ui *ui) {
    char at[16], length[16];

    gui_ui_begin(ui);
    if (picker.open) {
        int picked = gui_file_picker(ui, &picker);

        if (picked == GUI_PICKED && (picked = open_file(picker.path)) >= 0)
            choose(picked);
        if (picked)
            ui->again = true;
        gui_ui_end(ui);
        return;
    }

    pthread_mutex_lock(&lock);
    bool paused = shared.paused, playing = current >= 0 && !shared.finished;
    int64_t position = shared.position, total = shared.length;
    int rate = shared.rate, channels = shared.channels, bitrate = shared.bitrate, volume = shared.volume;
    char error[sizeof(shared.error)];

    snprintf(error, sizeof(error), "%s", shared.error);
    pthread_mutex_unlock(&lock);

    gui_heading(ui, current >= 0 ? names[current] : "Nothing playing");
    format_time(at, sizeof(at), position, rate);
    format_time(length, sizeof(length), total, rate);
    if (current >= 0) {
        char about[64];

        snprintf(about, sizeof(about), "%d Hz, %s", rate, channels == 1 ? "mono" : "stereo");
        if (bitrate)
            snprintf(about + strlen(about), sizeof(about) - strlen(about), ", %d kbit/s", bitrate);
        gui_label(ui, "%s / %s    %s", at, length, about);
    } else {
        gui_label(ui, "Open a sound file to play it.");
    }

    int seconds = rate ? (int)(position / rate) : 0;

    if (gui_slider(ui, POSITION, &seconds, 0, total > 0 && rate ? (int)(total / rate) : 0) && current >= 0)
        seek_to((int64_t)seconds * rate);
    report(ui, "position");

    if (gui_button(ui, "Previous") && track_count)
        choose(current > 0 ? current - 1 : 0);
    report(ui, "previous");
    gui_same_line(ui);
    if (gui_button(ui, playing && !paused ? "Pause" : "Play"))
        toggle_pause();
    report(ui, "play");
    gui_same_line(ui);
    if (gui_button(ui, "Stop"))
        choose(-1);
    report(ui, "stop");
    gui_same_line(ui);
    if (gui_button(ui, "Next") && current + 1 < track_count)
        choose(current + 1);
    report(ui, "next");
    gui_same_line(ui);
    if (gui_button(ui, "Open...")) {
        char folder[GUI_PATH_MAX] = "/";

        const char *slash = current >= 0 ? strrchr(tracks[current], '/') : NULL;

        if (slash) // where what's playing is
            snprintf(folder, sizeof(folder), "%.*s", (int)(slash - tracks[current]), tracks[current]);
        gui_picker_open(&picker, folder, NULL, false);
        ui->again = true;
    }
    report(ui, "open");
    if (gui_slider(ui, VOLUME, &volume, 0, 100)) {
        pthread_mutex_lock(&lock);
        shared.volume = volume;
        pthread_mutex_unlock(&lock);
    }
    report(ui, "volume");
    if (error[0])
        gui_label(ui, "%s", error);

    int result = gui_list(ui, PLAYLIST, names, track_count, &selected, 0);

    report(ui, "playlist");
    if (result == GUI_LIST_ACTIVATED)
        choose(selected);

    // Space pauses, and Left and Right go back and on, when nothing else takes them.
    if (!gui_focused(ui, POSITION) && !gui_focused(ui, VOLUME)) {
        if (gui_key(ui, KEY_SPACE) && !gui_focused(ui, PLAYLIST))
            toggle_pause();
        if (gui_key(ui, KEY_LEFT) && current >= 0)
            seek_to(position - (int64_t)SEEK_STEP * rate);
        if (gui_key(ui, KEY_RIGHT) && current >= 0)
            seek_to(position + (int64_t)SEEK_STEP * rate);
    }
    gui_ui_end(ui);
    fflush(stdout);
}

// What's playing has finished: on to the next, or stopping after the last.
static void next_if_finished(void) {
    pthread_mutex_lock(&lock);
    bool finished = shared.finished && shared.playing == shared.asked && current >= 0 && !shared.error[0];

    pthread_mutex_unlock(&lock);
    if (!finished)
        return;
    printf("player: finished %s\n", names[current]);
    if (current + 1 < track_count) {
        choose(current + 1);
    } else {
        pthread_mutex_lock(&lock);
        shared.path[0] = '\0';
        shared.asked++;
        pthread_mutex_unlock(&lock);
        current = -1;
    }
}

// A window that plays sound files, one after another, from a list.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: player [file]...\n"
                            "Opens a window that plays sound files: WAV, MP3, FLAC, Ogg Vorbis, Opus and AAC.\n"
                            "Each file given goes in its list, with the others in its folder, and the first plays.\n");
    if (!gui_connect()) {
        dprintf(STDERR_FILENO, "player: wm isn't running\n");
        return 1;
    }

    struct gui_ui ui;
    struct gui_message e;
    pthread_t thread;

    window = gui_create_window("Player", 520, 400);
    if (!window || pthread_create(&thread, NULL, player, NULL)) {
        dprintf(STDERR_FILENO, "player: couldn't start\n");
        return 1;
    }
    gui_ui_init(&ui, window);

    int first = -1;

    for (int i = 1; i < argc; i++) {
        int at = open_file(argv[i]);

        first = first < 0 ? at : first;
    }
    if (first >= 0)
        choose(first);
    gui_timer_start(TICK_MS, true);
    frame(&ui);
    printf("player: ready\n");
    fflush(stdout);
    while (gui_next_event(&e) && e.type != GUI_CLOSE) {
        if (e.type == GUI_TIMER)
            next_if_finished();
        if (gui_ui_event(&ui, &e))
            frame(&ui);
    }
    gui_destroy_window(window);
    return 0;
}
