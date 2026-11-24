#pragma once

#include <stdint.h>

// What libaudio and audiod say to each other, as packets on a SOCK_SEQPACKET socket.
// Each is a message, and a play's followed by its samples, interleaved 16-bit. Those
// that ask something are answered with a message of the same type.

#define AUDIOD_SOCKET "/run/audiod.sock"
#define AUDIOD_RATE 48000          // what everything's mixed at
#define AUDIOD_PLAY_FRAMES 4096    // the most a play carries
#define AUDIOD_MAX_PACKET (sizeof(struct audiod_message) + AUDIOD_PLAY_FRAMES * 2 * sizeof(int16_t))

enum audiod_type {
    AUDIOD_OPEN = 1,  // a stream: a is its rate, b its channels; answered with a 0 when it can
    AUDIOD_PLAY,      // samples to play
    AUDIOD_DELAY,     // answered with a as how many frames written haven't been heard, at its rate
    AUDIOD_FLUSH,     // forgets what hasn't been heard
    AUDIOD_MIXER,     // answered with a as the volume, b as flags
    AUDIOD_VOLUME,    // a is 0 to 100
    AUDIOD_MUTE,      // a is 1 to mute
    AUDIOD_LATENCY,   // a is 1 for low latency
};

#define AUDIOD_MUTED 1
#define AUDIOD_LOW_LATENCY 2
#define AUDIOD_CARD 4 // there's a sound card

struct audiod_message {
    uint32_t type;
    int32_t a, b;
};
