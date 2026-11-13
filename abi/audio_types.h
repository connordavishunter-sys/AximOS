#pragma once

// Shared by the kernel and libc. /dev/audio plays what's written to it: 16-bit
// stereo samples, left then right, at 48000 a second unless set otherwise.

#define AUDIO_RATE_DEFAULT 48000
#define AUDIO_FRAME_BYTES 4

// ioctl requests on /dev/audio.
#define AUDIO_SET_RATE 0x4101  // int: samples a second, which the hardware must have
#define AUDIO_GET_DELAY 0x4102 // int: bytes written that haven't been heard yet
#define AUDIO_FLUSH 0x4103     // forgets what hasn't been heard, at once
