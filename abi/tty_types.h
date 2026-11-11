#pragma once

#include <stdint.h>

// Shared by the kernel and libc. The layout and numbers match Linux.

#define NCCS 19

struct termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[NCCS];
};

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

// c_iflag. Only the carriage return ones change anything here.
#define IGNBRK 0000001
#define BRKINT 0000002
#define IGNPAR 0000004
#define PARMRK 0000010
#define INPCK 0000020
#define ISTRIP 0000040
#define INLCR 0000100
#define IGNCR 0000200
#define ICRNL 0000400
#define IXON 0002000
#define IXOFF 0010000
#define IMAXBEL 0020000

// c_oflag. The console always treats a newline as starting a new line.
#define OPOST 0000001
#define ONLCR 0000004

// c_cflag, which means nothing to a screen and keyboard.
#define CSIZE 0000060
#define CS8 0000060
#define CSTOPB 0000100
#define CREAD 0000200
#define PARENB 0000400
#define HUPCL 0002000
#define CLOCAL 0004000

// c_lflag.
#define ISIG 0000001
#define ICANON 0000002
#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define NOFLSH 0000200
#define TOSTOP 0000400
#define IEXTEN 0100000

// c_cc.
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6

#define TCGETS 0x5401
#define TCSETS 0x5402  // tcsetattr with TCSANOW
#define TCSETSW 0x5403 // with TCSADRAIN, the same here since output never waits
#define TCSETSF 0x5404 // with TCSAFLUSH, which also discards unread input
#define TIOCGPGRP 0x540F
#define TIOCSPGRP 0x5410
#define TIOCSCTTY 0x540E   // makes it the terminal of a process that has none
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define FIONREAD 0x541B
#define TIOCGPTPEER 0x5441 // on a pseudo-terminal's master, opens the slave with these flags
