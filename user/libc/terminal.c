#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#include "syscall.h"

static long checked(long result) {
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

int ioctl(int fd, unsigned long request, ...) {
    va_list args;

    va_start(args, request);

    void *arg = va_arg(args, void *);

    va_end(args);
    return checked(syscall3(SYS_IOCTL, fd, request, (long)arg));
}

int tcgetattr(int fd, struct termios *out) {
    return ioctl(fd, TCGETS, out);
}

int tcsetattr(int fd, int when, const struct termios *settings) {
    if (when < TCSANOW || when > TCSAFLUSH) {
        errno = EINVAL;
        return -1;
    }
    return ioctl(fd, when == TCSANOW ? TCSETS : when == TCSADRAIN ? TCSETSW : TCSETSF, settings);
}

// Output is never queued, so only input can be discarded.
int tcflush(int fd, int queue) {
    struct termios settings;

    if (tcgetattr(fd, &settings) < 0)
        return -1;
    return queue == TCOFLUSH ? 0 : tcsetattr(fd, TCSAFLUSH, &settings);
}

int tcdrain(int fd) {
    return isatty(fd) ? 0 : -1;
}

int tcflow(int fd, int action) {
    (void)action;
    return tcdrain(fd);
}

int tcsendbreak(int fd, int duration) {
    (void)duration;
    return tcdrain(fd);
}

void cfmakeraw(struct termios *t) {
    t->c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag = (t->c_cflag & ~(CSIZE | PARENB)) | CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

speed_t cfgetispeed(const struct termios *settings) {
    (void)settings;
    return B38400;
}

speed_t cfgetospeed(const struct termios *settings) {
    (void)settings;
    return B38400;
}

int cfsetispeed(struct termios *settings, speed_t speed) {
    (void)settings;
    (void)speed;
    return 0;
}

int cfsetospeed(struct termios *settings, speed_t speed) {
    (void)settings;
    (void)speed;
    return 0;
}

int cfsetspeed(struct termios *settings, speed_t speed) {
    (void)settings;
    (void)speed;
    return 0;
}

int isatty(int fd) {
    struct termios settings;

    return tcgetattr(fd, &settings) == 0;
}

int ttyname_r(int fd, char *out, size_t size) {
    const char *name = ttyname(fd);

    if (!name)
        return errno;
    if (strlen(name) >= size)
        return ERANGE;
    strcpy(out, name);
    return 0;
}

char *ttyname(int fd) {
    return isatty(fd) ? "/dev/tty" : NULL;
}

char *ctermid(char *out) {
    return out ? strcpy(out, "/dev/tty") : "/dev/tty";
}

pid_t tcgetpgrp(int fd) {
    int pgid;

    return ioctl(fd, TIOCGPGRP, &pgid) < 0 ? -1 : pgid;
}

int tcsetpgrp(int fd, pid_t pgid) {
    return ioctl(fd, TIOCSPGRP, &pgid);
}

int poll(struct pollfd *fds, nfds_t count, int timeout) {
    return checked(syscall3(SYS_POLL, (long)fds, count, timeout));
}

int select(int count, fd_set *read_set, fd_set *write_set, fd_set *error_set, struct timeval *timeout) {
    struct pollfd fds[FD_SETSIZE];
    int used = 0;

    if (count < 0 || count > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    for (int fd = 0; fd < count; fd++) {
        short events = 0;

        if (read_set && FD_ISSET(fd, read_set))
            events |= POLLIN;
        if (write_set && FD_ISSET(fd, write_set))
            events |= POLLOUT;
        if (events || (error_set && FD_ISSET(fd, error_set)))
            fds[used++] = (struct pollfd){.fd = fd, .events = events};
    }

    // Rounded up, so that it never wakes before what was asked for.
    int ms = timeout ? timeout->tv_sec * 1000 + (timeout->tv_usec + 999) / 1000 : -1;
    int found = poll(fds, used, ms);

    if (found < 0)
        return -1;

    for (int i = 0; i < FD_SETSIZE && read_set; i++)
        FD_CLR(i, read_set);
    for (int i = 0; i < FD_SETSIZE && write_set; i++)
        FD_CLR(i, write_set);
    for (int i = 0; i < FD_SETSIZE && error_set; i++)
        FD_CLR(i, error_set);

    int bits = 0;

    for (int i = 0; i < used; i++) {
        short ready = fds[i].revents;

        // A closed pipe counts as readable, as the read says so at once.
        if (read_set && (ready & (POLLIN | POLLHUP)) && (fds[i].events & POLLIN)) {
            FD_SET(fds[i].fd, read_set);
            bits++;
        }
        if (write_set && (ready & (POLLOUT | POLLERR)) && (fds[i].events & POLLOUT)) {
            FD_SET(fds[i].fd, write_set);
            bits++;
        }
        if (ready & POLLNVAL) {
            errno = EBADF;
            return -1;
        }
    }
    return bits;
}
