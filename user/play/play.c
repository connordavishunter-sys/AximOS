#include <aximos.h>
#include <audio.h>
#include <stdio.h>
#include <unistd.h>

#define CHUNK 4096 // frames read at a time

// Plays a file to its end, returning false if it couldn't.
static bool play(const char *path) {
    static int16_t samples[CHUNK * 2];
    struct audio_info info;
    struct audio_file *f = audio_open(path, &info);
    struct audio_stream *out = f ? audio_play(info.rate, info.channels) : NULL;
    long n = 0;

    if (!out) {
        dprintf(STDERR_FILENO, "play: %s: %s\n", path, audio_error());
        if (f)
            audio_close(f);
        return false;
    }
    while ((n = audio_read(f, samples, CHUNK)) > 0 && audio_stream_write(out, samples, n))
        ;
    if (n < 0)
        dprintf(STDERR_FILENO, "play: %s: %s\n", path, audio_error());
    while (audio_stream_delay(out) > 0) // until it's been heard
        usleep(10000);
    audio_stream_close(out);
    audio_close(f);
    return n == 0;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: play FILE...\n"
                            "Plays sound files, one after another, through audiod.\n");

    bool ok = argc > 1;

    for (int i = 1; i < argc; i++)
        ok = play(argv[i]) && ok;
    return ok ? 0 : 1;
}
