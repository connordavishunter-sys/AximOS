#pragma once

#include <socket_types.h>
#include <sys/types.h>
#include <sys/uio.h>

// Local stream sockets, and Internet sockets of SOCK_STREAM and SOCK_DGRAM. A local
// socket's name only exists in a table in the
// kernel, not on the disk, and it goes away when the socket closes. Linux would
// leave a file there for unlink to remove. With SCM_RIGHTS descriptors are passed
// between processes, arriving with the next recvmsg however much data is between.
int socket(int domain, int type, int protocol);
int socketpair(int domain, int type, int protocol, int fds[2]);
int bind(int fd, const struct sockaddr *address, socklen_t length);
int listen(int fd, int backlog);
int accept(int fd, struct sockaddr *address, socklen_t *length);
int accept4(int fd, struct sockaddr *address, socklen_t *length, int flags);
int connect(int fd, const struct sockaddr *address, socklen_t length);
int shutdown(int fd, int how);
int getsockname(int fd, struct sockaddr *address, socklen_t *length);
int getpeername(int fd, struct sockaddr *address, socklen_t *length);
ssize_t send(int fd, const void *buf, size_t len, int flags);
ssize_t recv(int fd, void *buf, size_t len, int flags);
ssize_t sendto(int fd, const void *buf, size_t len, int flags, const struct sockaddr *to, socklen_t to_length);
ssize_t recvfrom(int fd, void *buf, size_t len, int flags, struct sockaddr *from, socklen_t *from_length);
ssize_t sendmsg(int fd, const struct msghdr *message, int flags);
ssize_t recvmsg(int fd, struct msghdr *message, int flags);

// SO_TYPE, SO_ERROR and SO_ACCEPTCONN can be read. The buffer and address options are
// accepted and do nothing.
int getsockopt(int fd, int level, int option, void *value, socklen_t *length);
int setsockopt(int fd, int level, int option, const void *value, socklen_t length);
