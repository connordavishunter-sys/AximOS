#include <aximos.h>
#include <audio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Says or sets the volume of everything that plays.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: volume [0-100] [mute | unmute] [low-latency on|off]\n"
                            "Says how loud everything plays, or sets it, from 0 to 100, and mutes it or not.\n"
                            "Low latency plays sound in about 5 ms rather than 20, but it can crackle while\n"
                            "the computer's busy.\n");

    struct audio_mixer m;

    if (!audio_mixer_get(&m)) {
        dprintf(STDERR_FILENO, "volume: %s\n", audio_error());
        return 1;
    }
    if (!m.card) {
        dprintf(STDERR_FILENO, "volume: there's no sound card\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        bool mute = !strcmp(argv[i], "mute"), ok;
        char *end;
        long value = strtol(argv[i], &end, 10);

        if (mute || !strcmp(argv[i], "unmute"))
            ok = audio_set_muted(mute);
        else if (!strcmp(argv[i], "low-latency") && i + 1 < argc && (!strcmp(argv[i + 1], "on") || !strcmp(argv[i + 1], "off")))
            ok = audio_set_low_latency(!strcmp(argv[++i], "on"));
        else if (*end == '\0' && end != argv[i] && value >= 0 && value <= 100)
            ok = audio_set_volume((int)value);
        else {
            dprintf(STDERR_FILENO, "volume: %s isn't 0 to 100, mute, unmute or low-latency on|off\n", argv[i]);
            return 2;
        }
        if (!ok) {
            dprintf(STDERR_FILENO, "volume: only whoever's at the screen can change it\n");
            return 1;
        }
    }
    audio_mixer_get(&m);
    printf("volume %d%s%s\n", m.volume, m.muted ? ", muted" : "", m.low_latency ? ", low latency" : "");
    return 0;
}
