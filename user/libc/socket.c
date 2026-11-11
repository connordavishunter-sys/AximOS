#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "syscall.h"

static long checked(long result) {
    if (result < 0) {
        errno = -result;
        return -1;
    }
    return result;
}

int socket(int domain, int type, int protocol) {
    return checked(syscall3(SYS_SOCKET, domain, type, protocol));
}

int socketpair(int domain, int type, int protocol, int fds[2]) {
    return checked(syscall6(SYS_SOCKETPAIR, domain, type, protocol, (long)fds, 0, 0));
}

int bind(int fd, const struct sockaddr *address, socklen_t length) {
    return checked(syscall3(SYS_BIND, fd, (long)address, length));
}

int listen(int fd, int backlog) {
    return checked(syscall3(SYS_LISTEN, fd, backlog, 0));
}

int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags) {
    return checked(syscall6(SYS_ACCEPT, fd, (long)address, (long)length, flags, 0, 0));
}

int accept(int fd, struct sockaddr *address, socklen_t *length) {
    return accept4(fd, address, length, 0);
}

int connect(int fd, const struct sockaddr *address, socklen_t length) {
    return checked(syscall3(SYS_CONNECT, fd, (long)address, length));
}

int shutdown(int fd, int how) {
    return checked(syscall3(SYS_SHUTDOWN, fd, how, 0));
}

int getsockname(int fd, struct sockaddr *address, socklen_t *length) {
    return checked(syscall6(SYS_SOCKET_NAME, fd, (long)address, (long)length, 0, 0, 0));
}

int getpeername(int fd, struct sockaddr *address, socklen_t *length) {
    return checked(syscall6(SYS_SOCKET_NAME, fd, (long)address, (long)length, 1, 0, 0));
}

ssize_t sendmsg(int fd, const struct msghdr *message, int flags) {
    return checked(syscall3(SYS_SENDMSG, fd, (long)message, flags));
}

ssize_t recvmsg(int fd, struct msghdr *message, int flags) {
    return checked(syscall3(SYS_RECVMSG, fd, (long)message, flags));
}

ssize_t send(int fd, const void *buf, size_t len, int flags) {
    struct iovec part = {(void *)buf, len};
    struct msghdr message = {.msg_iov = &part, .msg_iovlen = 1};

    return sendmsg(fd, &message, flags);
}

ssize_t recv(int fd, void *buf, size_t len, int flags) {
    struct iovec part = {buf, len};
    struct msghdr message = {.msg_iov = &part, .msg_iovlen = 1};

    return recvmsg(fd, &message, flags);
}

ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t to_length) {
    struct iovec part = {(void *)buf, len};
    struct msghdr message = {.msg_iov = &part, .msg_iovlen = 1, .msg_name = (void *)to, .msg_namelen = to_length};

    return sendmsg(fd, &message, flags);
}

// A local stream's messages say nothing of who sent them, so that's asked for separately.
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *from_length) {
    struct iovec part = {buf, len};
    struct msghdr message = {.msg_iov = &part, .msg_iovlen = 1, .msg_name = from, .msg_namelen = from_length ? *from_length : 0};
    ssize_t count = recvmsg(fd, &message, flags);

    if (count >= 0 && from && from_length) {
        int saved = errno;

        if (message.msg_namelen)
            *from_length = message.msg_namelen;
        else if (getpeername(fd, from, from_length) < 0)
            *from_length = 0;
        errno = saved;
    }
    return count;
}

static bool is_socket(int fd) {
    struct stat st;

    if (fstat(fd, &st) < 0)
        return false;
    if (S_ISSOCK(st.st_mode))
        return true;
    errno = ENOTSOCK;
    return false;
}

int getsockopt(int fd, int level, int option, void *value, socklen_t *length) {
    if (!is_socket(fd))
        return -1;
    if (level != SOL_SOCKET || *length < sizeof(int)) {
        errno = level != SOL_SOCKET ? ENOPROTOOPT : EINVAL;
        return -1;
    }

    switch (option) {
    case SO_TYPE:
    case SO_ERROR:
        *(int *)value = checked(syscall3(SYS_SOCKET_INFO, fd, option, 0));
        if (*(int *)value < 0)
            return -1;
        break;
    case SO_SNDBUF:
    case SO_RCVBUF:
        *(int *)value = 4096;
        break;
    case SO_ACCEPTCONN: {
        // Reading from one that's listening is what gives it away.
        char c;
        struct sockaddr_un peer;
        socklen_t peer_length = sizeof(peer);
        (void)c;
        *(int *)value = getpeername(fd, (struct sockaddr *)&peer, &peer_length) < 0 && recv(fd, &c, 0, MSG_DONTWAIT) < 0 && errno != ENOTCONN;
        errno = 0;
        break;
    }
    default:
        errno = ENOPROTOOPT;
        return -1;
    }
    *length = sizeof(int);
    return 0;
}

int setsockopt(int fd, int level, int option, const void *value, socklen_t length) {
    (void)option;
    (void)value;
    (void)length;
    if (!is_socket(fd))
        return -1;
    if (level != SOL_SOCKET && level != IPPROTO_TCP) {
        errno = ENOPROTOOPT;
        return -1;
    }
    return 0;
}

ssize_t readv(int fd, const struct iovec *parts, int count) {
    ssize_t total = 0;

    for (int i = 0; i < count; i++) {
        ssize_t got = read(fd, parts[i].iov_base, parts[i].iov_len);

        if (got < 0)
            return total ? total : -1;
        total += got;
        if ((size_t)got < parts[i].iov_len)
            break;
    }
    return total;
}

ssize_t writev(int fd, const struct iovec *parts, int count) {
    ssize_t total = 0;

    for (int i = 0; i < count; i++) {
        ssize_t wrote = write(fd, parts[i].iov_base, parts[i].iov_len);

        if (wrote < 0)
            return total ? total : -1;
        total += wrote;
        if ((size_t)wrote < parts[i].iov_len)
            break;
    }
    return total;
}
