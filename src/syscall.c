#include <block.h>
#include <clock.h>
#include <console.h>
#include <errno_values.h>
#include <fb.h>
#include <file.h>
#include <irq.h>
#include <mm.h>
#include <power.h>
#include <process.h>
#include <shm.h>
#include <inet.h>
#include <net.h>
#include <random.h>
#include <socket.h>
#include <string.h>
#include <syscall.h>
#include <syscall_numbers.h>
#include <signal_numbers.h>
#include <time_types.h>
#include <tty_types.h>
#include <audio_types.h>
#include <vfs.h>

#define NS_PER_TICK (NS_PER_SECOND / TIMER_HZ)
#define MAX_SLEEP_SECONDS (1LL << 33)
#define MAX_POLL_FDS 1024
#define MAX_IOVECS 1024
#define SYSCALL_INSTRUCTION_SIZE 2

// Every call returns its result, or a negative errno.

static struct file *fd_lookup(uint64_t fd) {
    return fd < MAX_FDS ? process_files()[fd] : NULL;
}

static int fd_find_free(int from) {
    struct file **files = process_files();

    for (int fd = from; fd < MAX_FDS; fd++) {
        if (!files[fd])
            return fd;
    }
    return -EMFILE;
}

// Gives an open file a descriptor, closing it if there's none free. Opening can
// sleep, and another thread of the process can take a descriptor meanwhile, so
// the descriptor is only chosen once the file is open.
static int fd_install(int status, struct file *file, bool close_on_exec) {
    int fd = status < 0 ? status : fd_find_free(0);

    if (fd < 0) {
        if (status >= 0)
            file_close(file);
        return fd;
    }
    process_files()[fd] = file;
    if (close_on_exec)
        *process_close_on_exec() |= 1ull << fd;
    return fd;
}

static int64_t sys_write(uint64_t fd, uint64_t buf, uint64_t len) {
    struct file *file = fd_lookup(fd);

    if (!file)
        return -EBADF;
    if (!mm_user_range_ok(buf, len, false))
        return -EFAULT;
    return file_write(file, (const void *)buf, len);
}

static int64_t sys_read(uint64_t fd, uint64_t buf, uint64_t len) {
    struct file *file = fd_lookup(fd);

    if (!file)
        return -EBADF;
    if (!mm_user_range_ok(buf, len, true))
        return -EFAULT;
    return file_read(file, (void *)buf, len);
}

static int64_t sys_open(uint64_t path_ptr, uint64_t flags) {
    char path[PATH_MAX];
    uint64_t known = O_ACCMODE | O_CREAT | O_EXCL | O_NOCTTY | O_TRUNC | O_APPEND | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC;

    if ((flags & ~known) || (flags & O_ACCMODE) == O_ACCMODE)
        return -EINVAL;

    int64_t len = mm_copy_user_string(path, path_ptr, PATH_MAX);
    if (len < 0)
        return len;

    struct file *file;
    int status = file_open(process_cwd(), path, flags & ~(O_CLOEXEC | O_NOCTTY), &file);

    return fd_install(status, file, flags & O_CLOEXEC);
}

static int64_t sys_pipe(uint64_t fds_ptr) {
    if (!mm_user_range_ok(fds_ptr, 2 * sizeof(int), true))
        return -EFAULT;

    int read_fd = fd_find_free(0);
    if (read_fd < 0)
        return read_fd;

    int write_fd = fd_find_free(read_fd + 1);
    if (write_fd < 0)
        return write_fd;

    struct file **files = process_files();
    int status = file_pipe(&files[read_fd], &files[write_fd]);
    if (status < 0)
        return status;

    int *fds = (int *)fds_ptr;
    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

static int64_t sys_dup(uint64_t fd) {
    struct file *file = fd_lookup(fd);

    if (!file)
        return -EBADF;

    int target = fd_find_free(0);
    if (target < 0)
        return target;

    process_files()[target] = file_ref(file);
    return target;
}

static int64_t sys_dup2(uint64_t fd, uint64_t target) {
    struct file *file = fd_lookup(fd);

    if (!file || target >= MAX_FDS)
        return -EBADF;
    if (fd == target)
        return target;

    struct file **files = process_files();
    if (files[target])
        file_close(files[target]);
    files[target] = file_ref(file);
    *process_close_on_exec() &= ~(1ull << target);
    return target;
}

static int64_t sys_fcntl(uint64_t fd, uint64_t command, uint64_t value) {
    struct file *file = fd_lookup(fd);
    uint64_t *close_on_exec = process_close_on_exec();

    if (!file)
        return -EBADF;

    switch (command) {
    case F_DUPFD: {
        int target = value < MAX_FDS ? fd_find_free(value) : -EINVAL;

        if (target >= 0)
            process_files()[target] = file_ref(file);
        return target;
    }
    case F_GETFD:
        return *close_on_exec >> fd & 1 ? FD_CLOEXEC : 0;
    case F_SETFD:
        *close_on_exec = (*close_on_exec & ~(1ull << fd)) | (uint64_t)(value & FD_CLOEXEC) << fd;
        return 0;
    case F_GETFL:
        return file_status_flags(file);
    case F_SETFL:
        file_set_status_flags(file, value);
        return 0;
    }
    return -EINVAL;
}

static size_t ioctl_arg_size(uint64_t request) {
    switch (request) {
    case TCGETS:
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        return sizeof(struct termios);
    case TIOCGWINSZ:
    case TIOCSWINSZ:
        return sizeof(struct winsize);
    case TIOCSCTTY:
    case AUDIO_FLUSH:
        return 0;
    }
    return sizeof(int);
}

static int64_t sys_ioctl(uint64_t fd, uint64_t request, uint64_t arg) {
    struct file *file = fd_lookup(fd);

    if (!file)
        return -EBADF;
    // Its argument is the flags themselves, and it makes a descriptor.
    if (request == TIOCGPTPEER) {
        struct file *peer;
        int status = file_open_peer(file, arg, &peer);

        return fd_install(status, peer, arg & O_CLOEXEC);
    }
    if (!mm_user_range_ok(arg, ioctl_arg_size(request), true))
        return -EFAULT;
    return file_ioctl(file, request, (void *)arg);
}

static int64_t sys_poll(uint64_t fds_ptr, uint64_t count, int64_t timeout_ms) {
    if (count > MAX_POLL_FDS)
        return -EINVAL;
    if (!mm_user_range_ok(fds_ptr, count * sizeof(struct pollfd), true))
        return -EFAULT;

    struct pollfd *fds = (struct pollfd *)fds_ptr;
    // One tick more, as the current one is already partly over.
    uint64_t deadline = timeout_ms > 0 ? irq_ticks() + (timeout_ms * TIMER_HZ + 999) / 1000 + 1 : 0;

    for (;;) {
        int found = 0;

        for (uint64_t i = 0; i < count; i++) {
            struct file *file = fds[i].fd >= 0 ? fd_lookup(fds[i].fd) : NULL;

            if (fds[i].fd < 0)
                fds[i].revents = 0;
            else
                fds[i].revents = file ? file_poll(file, fds[i].events) : POLLNVAL;
            found += fds[i].revents != 0;
        }
        if (found || timeout_ms == 0)
            return found;

        uint64_t now = irq_ticks();

        if (timeout_ms > 0 && now >= deadline)
            return 0;
        if (!process_sleep_any(timeout_ms > 0 ? deadline - now : 0))
            return -EINTR;
    }
}

static int64_t sys_fstat(uint64_t fd, uint64_t out) {
    struct file *file = fd_lookup(fd);

    if (!file)
        return -EBADF;
    if (!mm_user_range_ok(out, sizeof(struct stat), true))
        return -EFAULT;
    return file_stat(file, (struct stat *)out);
}

static int64_t sys_sigaction(uint64_t signal, uint64_t action, uint64_t previous) {
    if ((action && !mm_user_range_ok(action, sizeof(struct sigaction), false))
        || (previous && !mm_user_range_ok(previous, sizeof(struct sigaction), true)))
        return -EFAULT;
    return process_sigaction(signal, (const struct sigaction *)action, (struct sigaction *)previous);
}

static int64_t sys_sigprocmask(uint64_t how, uint64_t set, uint64_t previous) {
    if ((set && !mm_user_range_ok(set, sizeof(sigset_t), false))
        || (previous && !mm_user_range_ok(previous, sizeof(sigset_t), true)))
        return -EFAULT;
    return process_sigprocmask(how, (const sigset_t *)set, (sigset_t *)previous);
}

static int64_t sys_sigpending(uint64_t out) {
    if (!mm_user_range_ok(out, sizeof(sigset_t), true))
        return -EFAULT;
    *(sigset_t *)out = process_signals_pending();
    return 0;
}

static int64_t sys_futex(uint64_t address, uint64_t operation, uint64_t value, uint64_t timeout_ns) {
    if (address % sizeof(uint32_t) || !mm_user_range_ok(address, sizeof(uint32_t), false))
        return -EFAULT;

    void *channel = (void *)mm_user_physical(address);

    if (operation == FUTEX_WAKE)
        return process_wakeup_some(channel, value > INT32_MAX ? INT32_MAX : value);
    if (operation != FUTEX_WAIT)
        return -EINVAL;
    if (*(const volatile uint32_t *)address != (uint32_t)value)
        return -EAGAIN;

    uint64_t ticks = timeout_ns ? timeout_ns / NS_PER_TICK + 1 : 0;
    uint64_t deadline = irq_ticks() + ticks;

    if (!process_sleep_timeout(channel, ticks))
        return -EINTR;
    return ticks && irq_ticks() >= deadline ? -ETIMEDOUT : 0;
}

static int64_t sys_exe_path(uint64_t buf, uint64_t len) {
    const char *path = process_exe();
    size_t size = strlen(path) + 1;

    if (size > len)
        return -ERANGE;
    if (!mm_user_range_ok(buf, size, true))
        return -EFAULT;
    memcpy((void *)buf, path, size);
    return size - 1;
}

// Turns an address into a socket's name: a normalized path, or @ and the rest for a
// name starting with a zero byte, which is in no directory.
static int socket_name_from_user(uint64_t address, uint64_t length, char out[PATH_MAX]) {
    if (length < sizeof(sa_family_t) + 1 || length > sizeof(struct sockaddr_un)
        || !mm_user_range_ok(address, length, false))
        return length < sizeof(sa_family_t) + 1 || length > sizeof(struct sockaddr_un) ? -EINVAL : -EFAULT;

    const struct sockaddr_un *in = (const struct sockaddr_un *)address;
    size_t len = length - sizeof(sa_family_t);
    char name[UNIX_PATH_MAX + 2];

    if (in->sun_family != AF_UNIX)
        return -EAFNOSUPPORT;

    memcpy(name, in->sun_path, len);
    name[len] = '\0';

    if (name[0] == '\0') {
        out[0] = '@';
        memcpy(out + 1, name + 1, len);
        out[len] = '\0';
        return 0;
    }
    return vfs_normalize(process_cwd(), name, out) ? 0 : -ENAMETOOLONG;
}

static struct inet_socket *inet_of(uint64_t fd) {
    struct file *file = fd_lookup(fd);

    return file ? file_inet(file) : NULL;
}

// Copies an Internet address in, or fails with a negative errno.
static int inet_address_from_user(uint64_t address, uint64_t length, struct sockaddr_in *out) {
    if (length < sizeof(sa_family_t))
        return -EINVAL;
    if (!mm_user_range_ok(address, length < sizeof(*out) ? length : sizeof(*out), false))
        return -EFAULT;

    *out = (struct sockaddr_in){0};
    memcpy(out, (const void *)address, length < sizeof(*out) ? length : sizeof(*out));
    if (out->sin_family == AF_INET && length < sizeof(*out))
        return -EINVAL;
    return 0;
}

// Writes an Internet address the way the calls that report them do.
static int inet_address_to_user(const struct sockaddr_in *in, uint64_t address, uint64_t length_ptr) {
    if (!address)
        return 0;
    if (!mm_user_range_ok(length_ptr, sizeof(socklen_t), true))
        return -EFAULT;

    socklen_t room = *(socklen_t *)length_ptr;
    size_t copied = sizeof(*in) < room ? sizeof(*in) : room;

    if (!mm_user_range_ok(address, copied, true))
        return -EFAULT;
    memcpy((void *)address, in, copied);
    *(socklen_t *)length_ptr = sizeof(*in);
    return 0;
}

static int inet_install(struct inet_socket *socket, int flags) {
    int fd = fd_find_free(0);

    if (fd < 0)
        return fd;

    int status = file_from_inet(socket, flags & SOCK_NONBLOCK ? O_NONBLOCK : 0, &process_files()[fd]);

    if (status < 0)
        return status;
    if (flags & SOCK_CLOEXEC)
        *process_close_on_exec() |= 1ull << fd;
    return fd;
}

static int64_t sys_socket_inet(uint64_t type, uint64_t protocol) {
    struct inet_socket *socket;
    int status = inet_create(type & ~(uint64_t)(SOCK_NONBLOCK | SOCK_CLOEXEC), protocol, &socket);

    if (status < 0)
        return status;

    int fd = inet_install(socket, type);

    if (fd < 0)
        inet_close(socket);
    return fd;
}

static int socket_install(struct socket *socket, int flags, bool *installed) {
    int fd = fd_find_free(0);

    if (fd < 0)
        return fd;

    int status = file_from_socket(socket, flags & SOCK_NONBLOCK ? O_NONBLOCK : 0, &process_files()[fd]);

    if (status < 0)
        return status;
    if (flags & SOCK_CLOEXEC)
        *process_close_on_exec() |= 1ull << fd;
    *installed = true;
    return fd;
}

static int check_socket_kind(uint64_t domain, uint64_t type, uint64_t protocol) {
    if (domain != AF_UNIX)
        return -EAFNOSUPPORT;
    if ((type & ~(uint64_t)(SOCK_NONBLOCK | SOCK_CLOEXEC)) != SOCK_STREAM)
        return -ESOCKTNOSUPPORT;
    return protocol == 0 ? 0 : -EPROTONOSUPPORT;
}

static int64_t sys_socket(uint64_t domain, uint64_t type, uint64_t protocol) {
    if (domain == AF_INET)
        return sys_socket_inet(type, protocol);

    int status = check_socket_kind(domain, type, protocol);

    if (status < 0)
        return status;

    struct socket *socket = socket_create();
    bool installed = false;

    if (!socket)
        return -ENOMEM;

    int fd = socket_install(socket, type, &installed);

    if (!installed)
        socket_close(socket);
    return fd;
}

static int64_t sys_socketpair(uint64_t domain, uint64_t type, uint64_t protocol, uint64_t fds_ptr) {
    int status = check_socket_kind(domain, type, protocol);

    if (status < 0)
        return status;
    if (!mm_user_range_ok(fds_ptr, 2 * sizeof(int), true))
        return -EFAULT;

    struct socket *a, *b;

    status = socket_create_pair(&a, &b);
    if (status < 0)
        return status;

    bool a_in = false, b_in = false;
    int fd_a = socket_install(a, type, &a_in);
    int fd_b = a_in ? socket_install(b, type, &b_in) : -EMFILE;

    if (!a_in)
        socket_close(a);
    if (!b_in) {
        socket_close(b);
        if (a_in) {
            file_close(process_files()[fd_a]);
            process_files()[fd_a] = NULL;
        }
        return a_in ? fd_b : fd_a;
    }

    int *fds = (int *)fds_ptr;

    fds[0] = fd_a;
    fds[1] = fd_b;
    return 0;
}

static struct socket *socket_of(uint64_t fd, int *error) {
    struct file *file = fd_lookup(fd);
    struct socket *socket = file ? file_socket(file) : NULL;

    *error = !file ? -EBADF : -ENOTSOCK;
    return socket;
}

static int64_t sys_bind(uint64_t fd, uint64_t address, uint64_t length) {
    struct inet_socket *inet = inet_of(fd);

    if (inet) {
        struct sockaddr_in in;
        int status = inet_address_from_user(address, length, &in);

        return status < 0 ? status : inet_bind(inet, &in);
    }

    char name[PATH_MAX];
    int error;
    struct socket *socket = socket_of(fd, &error);

    if (!socket)
        return error;

    int status = socket_name_from_user(address, length, name);

    return status < 0 ? status : socket_bind(socket, name);
}

static int64_t sys_listen(uint64_t fd, int64_t backlog) {
    struct inet_socket *inet = inet_of(fd);

    if (inet)
        return inet_listen(inet, backlog);

    int error;
    struct socket *socket = socket_of(fd, &error);

    return socket ? socket_listen(socket, backlog) : error;
}

static bool file_is_nonblocking(uint64_t fd) {
    return file_status_flags(fd_lookup(fd)) & O_NONBLOCK;
}

static int64_t sys_connect(uint64_t fd, uint64_t address, uint64_t length) {
    struct inet_socket *inet = inet_of(fd);

    if (inet) {
        struct sockaddr_in in;
        int status = inet_address_from_user(address, length, &in);

        return status < 0 ? status : inet_connect(inet, &in, file_is_nonblocking(fd));
    }

    char name[PATH_MAX];
    int error;
    struct socket *socket = socket_of(fd, &error);

    if (!socket)
        return error;

    int status = socket_name_from_user(address, length, name);

    return status < 0 ? status : socket_connect(socket, name, file_is_nonblocking(fd));
}

// Writes a name the way accept and getsockname report one, cut short if it must be.
static int64_t copy_name_to_user(const char *name, uint64_t address, uint64_t length_ptr) {
    if (!address)
        return 0;
    if (!mm_user_range_ok(length_ptr, sizeof(socklen_t), true))
        return -EFAULT;

    socklen_t room = *(socklen_t *)length_ptr;
    struct sockaddr_un out = {.sun_family = AF_UNIX};
    size_t len = strlen(name);

    if (name[0] == '@') {
        memcpy(out.sun_path + 1, name + 1, len - 1);
    } else {
        memcpy(out.sun_path, name, len);
        if (len)
            len++; // the terminator counts
    }

    size_t full = sizeof(sa_family_t) + len;
    size_t copied = full < room ? full : room;

    if (!mm_user_range_ok(address, copied, true))
        return -EFAULT;
    memcpy((void *)address, &out, copied);
    *(socklen_t *)length_ptr = full;
    return 0;
}

static int64_t sys_accept(uint64_t fd, uint64_t address, uint64_t length_ptr, uint64_t flags) {
    int error;
    struct socket *socket = socket_of(fd, &error);
    struct inet_socket *inet = inet_of(fd);

    if (!socket && !inet)
        return error;
    if (flags & ~(uint64_t)(SOCK_NONBLOCK | SOCK_CLOEXEC))
        return -EINVAL;
    if (fd_find_free(0) < 0)
        return -EMFILE;

    if (inet) {
        struct inet_socket *accepted;
        struct sockaddr_in peer;
        int status = inet_accept(inet, file_is_nonblocking(fd), &accepted, &peer);

        if (status < 0)
            return status;

        int new_fd = inet_install(accepted, flags);

        if (new_fd < 0) {
            inet_close(accepted);
            return new_fd;
        }
        inet_address_to_user(&peer, address, length_ptr);
        return new_fd;
    }

    struct socket *connection;
    int status = socket_accept(socket, file_is_nonblocking(fd), &connection);

    if (status < 0)
        return status;

    bool installed = false;
    int new_fd = socket_install(connection, flags, &installed);

    if (!installed) {
        socket_close(connection);
        return new_fd;
    }
    copy_name_to_user("", address, length_ptr);
    return new_fd;
}

static int64_t sys_shutdown(uint64_t fd, uint64_t how) {
    struct inet_socket *inet = inet_of(fd);

    if (inet)
        return inet_shutdown(inet, how);

    int error;
    struct socket *socket = socket_of(fd, &error);

    return socket ? socket_shutdown(socket, how) : error;
}

static int64_t sys_socket_info(uint64_t fd, uint64_t option) {
    struct file *file = fd_lookup(fd);

    if (!file)
        return -EBADF;

    struct inet_socket *inet = file_inet(file);

    if (!inet && !file_socket(file))
        return -ENOTSOCK;
    if (option == SO_ERROR)
        return inet ? inet_take_error(inet) : 0;
    if (option == SO_TYPE)
        return inet ? inet_type(inet) : SOCK_STREAM;
    return -ENOPROTOOPT;
}

static int64_t sys_socket_name(uint64_t fd, uint64_t address, uint64_t length_ptr, uint64_t peer) {
    struct inet_socket *inet = inet_of(fd);

    if (inet) {
        struct sockaddr_in out;
        int status = inet_name(inet, peer, &out);

        return status < 0 ? status : inet_address_to_user(&out, address, length_ptr);
    }

    char name[UNIX_PATH_MAX + 1];
    int error;
    struct socket *socket = socket_of(fd, &error);

    if (!socket)
        return error;

    int status = socket_name(socket, peer, name, sizeof(name));

    return status < 0 ? status : copy_name_to_user(name, address, length_ptr);
}

static bool iovecs_ok(const struct msghdr *message) {
    if (message->msg_iovlen > MAX_IOVECS
        || !mm_user_range_ok((uint64_t)message->msg_iov, message->msg_iovlen * sizeof(struct iovec), false))
        return false;
    return true;
}

// Takes references to any descriptors being sent. The socket holds them until read.
static int send_control(struct socket *socket, const struct msghdr *message) {
    if (message->msg_controllen == 0)
        return 0;
    if (!mm_user_range_ok((uint64_t)message->msg_control, message->msg_controllen, false))
        return -EFAULT;

    for (struct cmsghdr *c = CMSG_FIRSTHDR(message); c; c = CMSG_NXTHDR(message, c)) {
        if (c->cmsg_len < CMSG_LEN(0) || c->cmsg_len > message->msg_controllen)
            return -EINVAL;
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            return -EINVAL;

        int count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        const int *fds = (const int *)CMSG_DATA(c);

        for (int i = 0; i < count; i++) {
            struct file *file = fds[i] >= 0 ? fd_lookup(fds[i]) : NULL;

            if (!file)
                return -EBADF;

            int status = socket_send_file(socket, file_ref(file));

            if (status < 0) {
                file_close(file);
                return status;
            }
        }
    }
    return 0;
}

static int64_t sendmsg_inet(struct inet_socket *inet, uint64_t fd, const struct msghdr *message, uint64_t flags) {
    struct sockaddr_in to;
    bool has_address = message->msg_name != NULL;

    if (has_address) {
        int status = inet_address_from_user((uint64_t)message->msg_name, message->msg_namelen, &to);

        if (status < 0)
            return status;
    }

    bool nonblocking = (flags & MSG_DONTWAIT) || file_is_nonblocking(fd);
    bool stream = inet_type(inet) == SOCK_STREAM;

    // Each datagram is one send, so its pieces would have to be gathered first.
    if (!stream && message->msg_iovlen > 1)
        return -EMSGSIZE;

    struct iovec none = {NULL, 0};
    int64_t total = 0;

    for (size_t i = 0; i < message->msg_iovlen || (i == 0 && !stream); i++) {
        struct iovec part = message->msg_iovlen ? message->msg_iov[i] : none;

        if (!mm_user_range_ok((uint64_t)part.iov_base, part.iov_len, false))
            return total ? total : -EFAULT;

        int64_t count = inet_send(inet, part.iov_base, part.iov_len, has_address ? &to : NULL, nonblocking, !(flags & MSG_NOSIGNAL));

        if (count < 0)
            return total ? total : count;
        total += count;
        if ((size_t)count < part.iov_len)
            break;
    }
    return total;
}

static int64_t sys_sendmsg(uint64_t fd, uint64_t message_ptr, uint64_t flags) {
    struct inet_socket *inet = inet_of(fd);

    if (inet) {
        if (!mm_user_range_ok(message_ptr, sizeof(struct msghdr), false))
            return -EFAULT;

        struct msghdr copied = *(const struct msghdr *)message_ptr;

        return iovecs_ok(&copied) ? sendmsg_inet(inet, fd, &copied, flags) : -EINVAL;
    }

    int error;
    struct socket *socket = socket_of(fd, &error);

    if (!socket)
        return error;
    if (!mm_user_range_ok(message_ptr, sizeof(struct msghdr), false))
        return -EFAULT;

    struct msghdr message = *(const struct msghdr *)message_ptr;

    if (!iovecs_ok(&message))
        return -EINVAL;

    int status = send_control(socket, &message);

    if (status < 0)
        return status;

    bool nonblocking = (flags & MSG_DONTWAIT) || file_is_nonblocking(fd);
    int64_t total = 0;

    for (size_t i = 0; i < message.msg_iovlen; i++) {
        struct iovec part = message.msg_iov[i];

        if (!mm_user_range_ok((uint64_t)part.iov_base, part.iov_len, false))
            return total ? total : -EFAULT;

        int64_t count = socket_write(socket, part.iov_base, part.iov_len, nonblocking, !(flags & MSG_NOSIGNAL));

        if (count < 0)
            return total ? total : count;
        total += count;
        if ((size_t)count < part.iov_len)
            break;
    }
    return total;
}

// Puts what arrived with the data into new descriptors, as far as there's room.
static void receive_control(struct socket *socket, struct msghdr *out) {
    size_t room = out->msg_controllen;
    int count = 0;
    int *fds = (int *)CMSG_DATA(out->msg_control);

    out->msg_controllen = 0;
    if (!out->msg_control || room < CMSG_LEN(sizeof(int)))
        return;

    while ((size_t)CMSG_LEN((count + 1) * sizeof(int)) <= room) {
        int fd = fd_find_free(0);

        if (fd < 0)
            break;

        struct file *file = socket_receive_file(socket);

        if (!file)
            break;
        process_files()[fd] = file;
        fds[count++] = fd;
    }
    if (count == 0)
        return;

    struct cmsghdr *header = out->msg_control;

    header->cmsg_len = CMSG_LEN(count * sizeof(int));
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    out->msg_controllen = CMSG_SPACE(count * sizeof(int));
}

// A datagram goes in the first buffer with room, and the rest of it is lost.
static int64_t recvmsg_inet(struct inet_socket *inet, uint64_t fd, struct msghdr *message, uint64_t flags) {
    bool nonblocking = (flags & MSG_DONTWAIT) || file_is_nonblocking(fd);
    bool stream = inet_type(inet) == SOCK_STREAM;
    bool wait_all = stream && (flags & MSG_WAITALL) && !(flags & MSG_PEEK);
    struct sockaddr_in from = {0};
    int64_t total = 0;

    for (size_t i = 0; i < message->msg_iovlen; i++) {
        struct iovec part = message->msg_iov[i];

        if (part.iov_len == 0)
            continue;
        if (!mm_user_range_ok((uint64_t)part.iov_base, part.iov_len, true))
            return total ? total : -EFAULT;

        for (size_t got = 0; got < part.iov_len;) {
            bool wait = !nonblocking && (total == 0 || wait_all);
            int64_t count = inet_receive(inet, (char *)part.iov_base + got, part.iov_len - got, &from, !wait, flags & MSG_PEEK);

            if (count < 0 && total == 0)
                return count;
            if (count <= 0)
                goto done;
            got += count;
            total += count;
            if (!stream || (!wait_all && got < part.iov_len))
                goto done;
        }
        if (!stream)
            break;
    }

done:
    if (message->msg_name && from.sin_family) {
        socklen_t room = message->msg_namelen;
        size_t copied = sizeof(from) < room ? sizeof(from) : room;

        if (!mm_user_range_ok((uint64_t)message->msg_name, copied, true))
            return -EFAULT;
        memcpy(message->msg_name, &from, copied);
        message->msg_namelen = sizeof(from);
    } else {
        message->msg_namelen = 0;
    }
    message->msg_controllen = 0;
    message->msg_flags = 0;
    return total;
}

static int64_t sys_recvmsg(uint64_t fd, uint64_t message_ptr, uint64_t flags) {
    struct inet_socket *inet = inet_of(fd);

    if (inet) {
        if (!mm_user_range_ok(message_ptr, sizeof(struct msghdr), true))
            return -EFAULT;
        if (!iovecs_ok((struct msghdr *)message_ptr))
            return -EINVAL;
        return recvmsg_inet(inet, fd, (struct msghdr *)message_ptr, flags);
    }

    int error;
    struct socket *socket = socket_of(fd, &error);

    if (!socket)
        return error;
    if (!mm_user_range_ok(message_ptr, sizeof(struct msghdr), true))
        return -EFAULT;

    struct msghdr *message = (struct msghdr *)message_ptr;

    if (!iovecs_ok(message))
        return -EINVAL;
    if (message->msg_controllen && !mm_user_range_ok((uint64_t)message->msg_control, message->msg_controllen, true))
        return -EFAULT;

    bool nonblocking = (flags & MSG_DONTWAIT) || file_is_nonblocking(fd);
    int64_t total = 0;

    bool wait_all = flags & MSG_WAITALL;
    bool done = false;

    for (size_t i = 0; i < message->msg_iovlen && !done; i++) {
        struct iovec part = message->msg_iov[i];

        if (!mm_user_range_ok((uint64_t)part.iov_base, part.iov_len, true))
            return total ? total : -EFAULT;

        // Only the start of the data is waited for, unless told to fill the buffer.
        for (size_t got = 0; got < part.iov_len;) {
            bool wait = !nonblocking && (total == 0 || wait_all);
            int64_t count = socket_read(socket, (char *)part.iov_base + got, part.iov_len - got, !wait, flags & MSG_PEEK);

            if (count < 0 && total == 0)
                return count;
            if (count <= 0 || (size_t)count < part.iov_len - got)
                done = !wait_all || count <= 0 || (flags & MSG_PEEK);
            if (count > 0) {
                got += count;
                total += count;
            }
            if (done)
                break;
        }
    }

    receive_control(socket, message);
    message->msg_flags = 0;
    message->msg_namelen = 0;
    return total;
}

// Flags, which match Linux, and the most one call returns.
#define GRND_NONBLOCK 1
#define GRND_RANDOM 2
#define GRND_INSECURE 4
#define GETRANDOM_MAX (1024 * 1024)

static int64_t sys_getrandom(uint64_t buf, uint64_t len, uint64_t flags) {
    if (flags & ~(uint64_t)(GRND_NONBLOCK | GRND_RANDOM | GRND_INSECURE))
        return -EINVAL;
    if ((flags & GRND_INSECURE) && (flags & GRND_RANDOM))
        return -EINVAL;
    if (len > GETRANDOM_MAX)
        len = GETRANDOM_MAX;
    if (!mm_user_range_ok(buf, len, true))
        return -EFAULT;
    if (!(flags & GRND_INSECURE)) {
        int status = random_wait_ready(flags & GRND_NONBLOCK);

        if (status < 0)
            return status;
    }
    random_bytes((void *)buf, len);
    return len;
}

static int64_t sys_net_info(uint64_t out) {
    if (!mm_user_range_ok(out, sizeof(struct net_info), true))
        return -EFAULT;
    net_get_info((struct net_info *)out);
    return 0;
}

static bool restartable(uint64_t number) {
    return number == SYS_GETRANDOM || number == SYS_READ || number == SYS_WRITE || number == SYS_WAIT || number == SYS_ACCEPT
        || number == SYS_SENDMSG || number == SYS_RECVMSG;
}

static int64_t sys_lseek(uint64_t fd, int64_t offset, uint64_t whence) {
    struct file *file = fd_lookup(fd);

    return file ? file_seek(file, offset, whence) : -EBADF;
}

static int64_t sys_mkdir(uint64_t path_ptr) {
    char path[PATH_MAX];
    struct vfs_node node;

    int64_t len = mm_copy_user_string(path, path_ptr, PATH_MAX);
    if (len < 0)
        return len;

    vfs_lock();

    // The path has to be free before a filesystem is asked to make it.
    int status = vfs_lookup(process_cwd(), path, &node) == VFS_OK
        ? -EEXIST : vfs_create(process_cwd(), path, true, &node);

    vfs_unlock();
    return status;
}

static int64_t sys_rename(uint64_t old_ptr, uint64_t new_ptr) {
    char old_path[PATH_MAX];
    char new_path[PATH_MAX];

    int64_t len = mm_copy_user_string(old_path, old_ptr, PATH_MAX);
    if (len < 0)
        return len;

    len = mm_copy_user_string(new_path, new_ptr, PATH_MAX);
    if (len < 0)
        return len;

    return file_rename(process_cwd(), old_path, new_path);
}

static int64_t sys_remove(uint64_t path_ptr, bool is_dir) {
    char path[PATH_MAX];

    int64_t len = mm_copy_user_string(path, path_ptr, PATH_MAX);
    if (len < 0)
        return len;
    return file_remove(process_cwd(), path, is_dir);
}

static int64_t sys_close(uint64_t fd) {
    struct file *file = fd_lookup(fd);

    if (!file)
        return -EBADF;

    file_close(file);
    process_files()[fd] = NULL;
    *process_close_on_exec() &= ~(1ull << fd);
    return 0;
}

static int64_t sys_exec(struct interrupt_frame *frame, uint64_t path_ptr, uint64_t argv, uint64_t envp) {
    char path[PATH_MAX];

    int64_t len = mm_copy_user_string(path, path_ptr, PATH_MAX);
    if (len < 0)
        return len;
    return process_exec(frame, path, argv, envp);
}

// Nothing wakes this channel: only the timeout or a signal end the sleep. Ticks
// don't line up exactly with the clock, so it goes round until the time has passed.
static int64_t sys_nanosleep(uint64_t request_ptr, uint64_t remaining_ptr) {
    static char channel;

    if (!mm_user_range_ok(request_ptr, sizeof(struct timespec), false) ||
        (remaining_ptr && !mm_user_range_ok(remaining_ptr, sizeof(struct timespec), true)))
        return -EFAULT;

    struct timespec request = *(const struct timespec *)request_ptr;

    if (request.tv_sec < 0 || request.tv_nsec < 0 || (uint64_t)request.tv_nsec >= NS_PER_SECOND)
        return -EINVAL;

    // Hundreds of years is as good as forever, and can't overflow.
    uint64_t seconds = request.tv_sec < MAX_SLEEP_SECONDS ? request.tv_sec : MAX_SLEEP_SECONDS;
    uint64_t deadline = clock_monotonic_ns() + seconds * NS_PER_SECOND + request.tv_nsec;

    for (;;) {
        uint64_t now = clock_monotonic_ns();

        if (now >= deadline)
            return 0;

        uint64_t left = deadline - now;

        if (!process_sleep_timeout(&channel, left / NS_PER_TICK + 1)) {
            if (remaining_ptr) {
                left = deadline - clock_monotonic_ns();
                *(struct timespec *)remaining_ptr =
                    (struct timespec){left / NS_PER_SECOND, left % NS_PER_SECOND};
            }
            return -EINTR;
        }
    }
}

static int64_t sys_clock_gettime(int64_t clock, uint64_t out_ptr) {
    uint64_t ns;
    int64_t base = 0;

    switch (clock) {
    case CLOCK_REALTIME:
        ns = clock_monotonic_ns();
        base = clock_boot_epoch();
        break;
    case CLOCK_MONOTONIC:
        ns = clock_monotonic_ns();
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
        ns = process_cpu_ticks() * NS_PER_TICK;
        break;
    default:
        return -EINVAL;
    }

    if (!mm_user_range_ok(out_ptr, sizeof(struct timespec), true))
        return -EFAULT;

    *(struct timespec *)out_ptr =
        (struct timespec){base + (int64_t)(ns / NS_PER_SECOND), ns % NS_PER_SECOND};
    return 0;
}

static int64_t sys_chdir(uint64_t path_ptr) {
    char path[PATH_MAX];
    char full[PATH_MAX];
    struct vfs_node node;

    int64_t len = mm_copy_user_string(path, path_ptr, PATH_MAX);
    if (len < 0)
        return len;

    vfs_lock();

    int status = vfs_lookup(process_cwd(), path, &node);

    vfs_unlock();
    if (status != VFS_OK)
        return status;
    if (!node.is_dir)
        return -ENOTDIR;
    if (!vfs_normalize(process_cwd(), path, full))
        return -ENAMETOOLONG;

    memcpy(process_cwd(), full, strlen(full) + 1);
    return 0;
}

static int64_t sys_getcwd(uint64_t buf, uint64_t len) {
    const char *cwd = process_cwd();
    size_t size = strlen(cwd) + 1;

    if (size > len)
        return -ERANGE;
    if (!mm_user_range_ok(buf, size, true))
        return -EFAULT;

    memcpy((void *)buf, cwd, size);
    return size - 1;
}

static int64_t sys_readdir(uint64_t fd, uint64_t entry) {
    struct file *file = fd_lookup(fd);

    if (!file)
        return -EBADF;
    if (!mm_user_range_ok(entry, sizeof(struct dirent), true))
        return -EFAULT;
    return file_readdir(file, (struct dirent *)entry);
}

static int64_t sys_stat(uint64_t path_ptr, uint64_t out) {
    char path[PATH_MAX];
    struct vfs_node node;

    int64_t len = mm_copy_user_string(path, path_ptr, PATH_MAX);
    if (len < 0)
        return len;
    if (!mm_user_range_ok(out, sizeof(struct stat), true))
        return -EFAULT;

    vfs_lock();

    int status = vfs_lookup(process_cwd(), path, &node);

    vfs_unlock();
    if (status != VFS_OK)
        return status;

    vfs_fill_stat(&node, (struct stat *)out);
    return 0;
}

static int64_t sys_statfs(uint64_t path_ptr, uint64_t out) {
    char path[PATH_MAX];
    struct vfs_node node;

    int64_t len = mm_copy_user_string(path, path_ptr, PATH_MAX);
    if (len < 0)
        return len;
    if (!mm_user_range_ok(out, sizeof(struct statfs), true))
        return -EFAULT;

    vfs_lock();

    int status = vfs_lookup(process_cwd(), path, &node);
    if (status == VFS_OK && !vfs_statfs(&node, (struct statfs *)out))
        status = -EINVAL;

    vfs_unlock();
    return status;
}

static int64_t sys_termsize(uint64_t rows, uint64_t cols) {
    if (!mm_user_range_ok(rows, sizeof(int), true) || !mm_user_range_ok(cols, sizeof(int), true))
        return -EFAULT;

    console_size((int *)rows, (int *)cols);
    return 0;
}

static int64_t sys_meminfo(uint64_t out) {
    if (!mm_user_range_ok(out, sizeof(struct meminfo), true))
        return -EFAULT;

    struct meminfo *info = (struct meminfo *)out;

    info->total = mm_total_page_count() * PAGE_SIZE;
    info->free = mm_free_page_count() * PAGE_SIZE;
    info->kernel = mm_kernel_page_count() * PAGE_SIZE;
    info->cached = block_cached_bytes();
    return 0;
}

static int64_t sys_shm_open(uint64_t name_ptr, uint64_t flags) {
    char name[PATH_MAX];
    uint64_t known = O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC;

    if ((flags & ~known) || (flags & O_ACCMODE) == O_ACCMODE || (flags & O_ACCMODE) == O_WRONLY)
        return -EINVAL;

    int64_t len = mm_copy_user_string(name, name_ptr, PATH_MAX);
    if (len < 0)
        return len;

    struct file *file;
    int status = file_shm_open(name, flags, &file);

    return fd_install(status, file, false);
}

static int64_t sys_shm_unlink(uint64_t name_ptr) {
    char name[PATH_MAX];

    int64_t len = mm_copy_user_string(name, name_ptr, PATH_MAX);
    return len < 0 ? len : shm_unlink(name);
}

static int64_t sys_ftruncate(uint64_t fd, uint64_t size) {
    struct file *file = fd_lookup(fd);

    return file ? file_truncate(file, size) : -EBADF;
}

static int64_t sys_mmap(uint64_t len, uint64_t prot, uint64_t flags, uint64_t fd, uint64_t offset) {
    if (flags & MAP_ANONYMOUS)
        return process_mmap(len, prot, flags, NULL, false, 0);

    struct file *file = fd_lookup(fd);
    if (!file)
        return -EBADF;

    bool writable;

    if (file_is_fb(file, &writable)) {
        if ((prot & PROT_WRITE) && !writable)
            return -EACCES;
        return process_mmap_framebuffer(len, prot, flags, fb_memory_address(), fb_memory_size(), offset);
    }

    struct shm *shm = file_shm(file, &writable);

    // Mapping files on disk isn't supported yet.
    if (!shm)
        return -EINVAL;
    return process_mmap(len, prot, flags, shm, writable, offset);
}

// Getting the size only takes having the device open. Changing what's on the
// screen takes having opened it for writing, which only one open file can.
static int64_t sys_fb_info(uint64_t fd, uint64_t out_ptr) {
    struct file *file = fd_lookup(fd);
    bool writable;

    if (!file || !file_is_fb(file, &writable))
        return -EBADF;
    if (!mm_user_range_ok(out_ptr, sizeof(struct fb_info), true))
        return -EFAULT;

    fb_get_info((struct fb_info *)out_ptr);
    return 0;
}

static int64_t sys_fb_set_mode(uint64_t fd, uint64_t width, uint64_t height) {
    struct file *file = fd_lookup(fd);
    bool writable;

    if (!file || !file_is_fb(file, &writable) || !writable)
        return -EBADF;
    return fb_set_mode(width, height);
}

static int64_t sys_fb_draw(uint64_t fd, uint64_t rect_ptr) {
    struct file *file = fd_lookup(fd);
    bool writable;

    if (!file || !file_is_fb(file, &writable) || !writable)
        return -EBADF;
    if (!mm_user_range_ok(rect_ptr, sizeof(struct fb_rect), false))
        return -EFAULT;
    return fb_draw((const struct fb_rect *)rect_ptr);
}

static int64_t sys_reboot(uint64_t command) {
    if (command == RB_AUTOBOOT)
        power_reboot();
    if (command == RB_POWER_OFF)
        power_off();
    return -EINVAL;
}

void syscall_dispatch(struct interrupt_frame *frame) {
    int64_t result;

    switch (frame->rax) {
    case SYS_EXIT:
        process_exit(frame->rdi);
    case SYS_WRITE:
        result = sys_write(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_BRK:
        result = process_brk(frame->rdi);
        break;
    case SYS_FORK:
        result = process_fork(frame);
        break;
    case SYS_EXEC:
        // A successful exec has replaced the frame, so there's no one to return to.
        result = sys_exec(frame, frame->rdi, frame->rsi, frame->rdx);
        if (result == 0)
            return;
        break;
    case SYS_WAIT:
        result = process_wait(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_GETPID:
        result = process_pid();
        break;
    case SYS_OPEN:
        result = sys_open(frame->rdi, frame->rsi);
        break;
    case SYS_CLOSE:
        result = sys_close(frame->rdi);
        break;
    case SYS_READ:
        result = sys_read(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_CHDIR:
        result = sys_chdir(frame->rdi);
        break;
    case SYS_GETCWD:
        result = sys_getcwd(frame->rdi, frame->rsi);
        break;
    case SYS_READDIR:
        result = sys_readdir(frame->rdi, frame->rsi);
        break;
    case SYS_STAT:
        result = sys_stat(frame->rdi, frame->rsi);
        break;
    case SYS_STATFS:
        result = sys_statfs(frame->rdi, frame->rsi);
        break;
    case SYS_TERMSIZE:
        result = sys_termsize(frame->rdi, frame->rsi);
        break;
    case SYS_REBOOT:
        result = sys_reboot(frame->rdi);
        break;
    case SYS_PIPE:
        result = sys_pipe(frame->rdi);
        break;
    case SYS_DUP:
        result = sys_dup(frame->rdi);
        break;
    case SYS_DUP2:
        result = sys_dup2(frame->rdi, frame->rsi);
        break;
    case SYS_SETPGID:
        result = process_set_group(frame->rdi, frame->rsi);
        break;
    case SYS_LSEEK:
        result = sys_lseek(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_UNLINK:
        result = sys_remove(frame->rdi, false);
        break;
    case SYS_MKDIR:
        result = sys_mkdir(frame->rdi);
        break;
    case SYS_RMDIR:
        result = sys_remove(frame->rdi, true);
        break;
    case SYS_TIME:
        result = clock_now();
        break;
    case SYS_RENAME:
        result = sys_rename(frame->rdi, frame->rsi);
        break;
    case SYS_MMAP:
        result = sys_mmap(frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
        break;
    case SYS_MUNMAP:
        result = process_munmap(frame->rdi, frame->rsi);
        break;
    case SYS_SHM_OPEN:
        result = sys_shm_open(frame->rdi, frame->rsi);
        break;
    case SYS_SHM_UNLINK:
        result = sys_shm_unlink(frame->rdi);
        break;
    case SYS_FTRUNCATE:
        result = sys_ftruncate(frame->rdi, frame->rsi);
        break;
    case SYS_FB_INFO:
        result = sys_fb_info(frame->rdi, frame->rsi);
        break;
    case SYS_FB_SET_MODE:
        result = sys_fb_set_mode(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_FB_DRAW:
        result = sys_fb_draw(frame->rdi, frame->rsi);
        break;
    case SYS_NANOSLEEP:
        result = sys_nanosleep(frame->rdi, frame->rsi);
        break;
    case SYS_CLOCK_GETTIME:
        result = sys_clock_gettime(frame->rdi, frame->rsi);
        break;
    case SYS_MEMINFO:
        result = sys_meminfo(frame->rdi);
        break;
    case SYS_KILL:
        result = process_kill(frame->rdi, frame->rsi);
        break;
    case SYS_SIGACTION:
        result = sys_sigaction(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_FSTAT:
        result = sys_fstat(frame->rdi, frame->rsi);
        break;
    case SYS_FCNTL:
        result = sys_fcntl(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_IOCTL:
        result = sys_ioctl(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_POLL:
        result = sys_poll(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_SIGPROCMASK:
        result = sys_sigprocmask(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_SIGPENDING:
        result = sys_sigpending(frame->rdi);
        break;
    case SYS_SIGSUSPEND:
        result = process_sigsuspend(frame->rdi);
        break;
    case SYS_ALARM:
        result = process_alarm(frame->rdi);
        break;
    case SYS_GETPPID:
        result = process_parent_pid();
        break;
    case SYS_GETPGID:
        result = process_group_of(frame->rdi);
        break;
    case SYS_THREAD_CREATE:
        result = process_thread_create(frame, frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_THREAD_EXIT:
        process_thread_exit(frame->rdi, frame->rsi);
    case SYS_FUTEX:
        result = sys_futex(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        break;
    case SYS_SET_THREAD_POINTER:
        if (frame->rdi >= LOWER_HALF_END) {
            result = -EINVAL;
            break;
        }
        process_set_thread_pointer(frame->rdi);
        result = 0;
        break;
    case SYS_GETTID:
        result = process_thread_id();
        break;
    case SYS_TGKILL:
        result = process_signal_thread(frame->rdi, frame->rsi);
        break;
    case SYS_YIELD:
        process_yield();
        result = 0;
        break;
    case SYS_EXE_PATH:
        result = sys_exe_path(frame->rdi, frame->rsi);
        break;
    case SYS_MPROTECT:
        result = process_mprotect(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_SOCKET:
        result = sys_socket(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_SOCKETPAIR:
        result = sys_socketpair(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        break;
    case SYS_BIND:
        result = sys_bind(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_LISTEN:
        result = sys_listen(frame->rdi, frame->rsi);
        break;
    case SYS_ACCEPT:
        result = sys_accept(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        break;
    case SYS_CONNECT:
        result = sys_connect(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_SHUTDOWN:
        result = sys_shutdown(frame->rdi, frame->rsi);
        break;
    case SYS_SOCKET_NAME:
        result = sys_socket_name(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        break;
    case SYS_SENDMSG:
        result = sys_sendmsg(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_RECVMSG:
        result = sys_recvmsg(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_SOCKET_INFO:
        result = sys_socket_info(frame->rdi, frame->rsi);
        break;
    case SYS_SETSID:
        result = process_new_session();
        break;
    case SYS_GETRANDOM:
        result = sys_getrandom(frame->rdi, frame->rsi, frame->rdx);
        break;
    case SYS_NET_INFO:
        result = sys_net_info(frame->rdi);
        break;
    case SYS_NET_SET:
        net_configure(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        result = 0;
        break;
    case SYS_SIGRETURN:
        // This restores a whole frame, including the result of whatever the
        // signal interrupted.
        process_signal_return(frame);
        return;
    default:
        result = -ENOSYS;
    }

    // Going back over the syscall instruction runs it again once the handler is done.
    if (result == -EINTR && restartable(frame->rax) && process_signal_restarts()) {
        frame->rip -= SYSCALL_INSTRUCTION_SIZE;
        return;
    }
    frame->rax = result;
}
