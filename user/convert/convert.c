#include <aximos.h>
#include <audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHUNK 4096

// Reads a sound file and writes it as another kind, by the new name's extension.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: convert [-b kbit/s] [-s seconds] from to\n"
                            "       convert -i file\n"
                            "Writes a sound file as another kind, by the name it's given: .wav, .mp3, .flac,\n"
                            ".ogg, .opus or .aac, at the bitrate given or a usual one, from the start or so\n"
                            "many seconds in. -i says what a file is.\n");

    struct audio_info info;
    int bitrate = 0, opt;
    double start = 0;
    bool describe = false;

    while ((opt = getopt(argc, argv, "b:is:")) != -1) {
        if (opt == 'b')
            bitrate = atoi(optarg);
        else if (opt == 's')
            start = atof(optarg);
        else if (opt == 'i')
            describe = true;
        else
            return 2;
    }
    if (argc - optind != (describe ? 1 : 2)) {
        dprintf(STDERR_FILENO, "usage: convert [-b kbit/s] [-s seconds] from to, or convert -i file\n");
        return 2;
    }

    struct audio_file *in = audio_open(argv[optind], &info);

    if (!in) {
        dprintf(STDERR_FILENO, "convert: %s\n", audio_error());
        return 1;
    }
    if (describe) {
        printf("%s: %d Hz, %d channel%s, %lld samples, %d kbit/s\n", argv[optind], info.rate, info.channels,
               info.channels == 1 ? "" : "s", (long long)info.frames, info.bitrate);
        audio_close(in);
        return 0;
    }
    info.bitrate = bitrate;
    if (start > 0 && !audio_seek(in, (int64_t)(start * info.rate))) {
        dprintf(STDERR_FILENO, "convert: can't start %g seconds in: %s\n", start, audio_error());
        audio_close(in);
        return 1;
    }

    struct audio_file *out = audio_create(argv[optind + 1], &info);
    static int16_t samples[CHUNK * 2];
    long n, total = 0;

    if (!out) {
        dprintf(STDERR_FILENO, "convert: %s\n", audio_error());
        audio_close(in);
        return 1;
    }
    while ((n = audio_read(in, samples, CHUNK)) > 0 && audio_write(out, samples, n))
        total += n;
    audio_close(in);
    if (n != 0 || !audio_close(out)) {
        dprintf(STDERR_FILENO, "convert: %s\n", audio_error());
        return 1;
    }
    printf("convert: wrote %ld samples to %s\n", total, argv[optind + 1]);
    return 0;
}
