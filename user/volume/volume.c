#include <audio_types.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

// Says or sets the volume of everything that plays.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: volume [0-100] [mute | unmute]\n"
                            "Says how loud everything plays, or sets it, from 0 to 100, and mutes it or not.\n");

    int fd = open("/dev/mixer", O_RDONLY), volume, muted;

    if (fd < 0) {
        dprintf(STDERR_FILENO, "volume: there's no sound card\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        int on = !strcmp(argv[i], "mute");
        char *end;
        long value = strtol(argv[i], &end, 10);

        if (on || !strcmp(argv[i], "unmute")) {
            ioctl(fd, MIXER_SET_MUTE, &on);
        } else if (*end == '\0' && end != argv[i] && value >= 0 && value <= 100) {
            volume = (int)value;
            ioctl(fd, MIXER_SET_VOLUME, &volume);
        } else {
            dprintf(STDERR_FILENO, "volume: %s isn't 0 to 100, mute or unmute\n", argv[i]);
            return 2;
        }
    }
    ioctl(fd, MIXER_GET_VOLUME, &volume);
    ioctl(fd, MIXER_GET_MUTE, &muted);
    printf("volume %d%s\n", volume, muted ? ", muted" : "");
    return 0;
}
