#pragma once

#include <sys/types.h>
#include <tty_types.h>

typedef uint32_t tcflag_t;
typedef uint8_t cc_t;
typedef uint32_t speed_t;

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2

#define TCOOFF 0
#define TCOON 1
#define TCIOFF 2
#define TCION 3

// The console has no line speed, so this one is all that's ever reported.
#define B0 0
#define B50 0000001
#define B75 0000002
#define B110 0000003
#define B134 0000004
#define B150 0000005
#define B200 0000006
#define B300 0000007
#define B600 0000010
#define B1200 0000011
#define B1800 0000012
#define B2400 0000013
#define B4800 0000014
#define B9600 0000015
#define B19200 0000016
#define B38400 0000017
#define B115200 0010002

int tcgetattr(int fd, struct termios *out);
int tcsetattr(int fd, int when, const struct termios *settings);
int tcflush(int fd, int queue);
int tcdrain(int fd);
int tcflow(int fd, int action);
int tcsendbreak(int fd, int duration);
void cfmakeraw(struct termios *settings);
speed_t cfgetispeed(const struct termios *settings);
speed_t cfgetospeed(const struct termios *settings);
int cfsetispeed(struct termios *settings, speed_t speed);
int cfsetospeed(struct termios *settings, speed_t speed);
int cfsetspeed(struct termios *settings, speed_t speed);
