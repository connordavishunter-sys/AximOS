#include <errno_values.h>
#include <fs_types.h>
#include <kmalloc.h>
#include <pipe.h>
#include <process.h>
#include <signal_numbers.h>
#include <socket.h>
#include <string.h>

#define NAME_SIZE (UNIX_PATH_MAX + 1)

enum socket_state {
    SOCKET_NEW,
    SOCKET_LISTENING,
    SOCKET_CONNECTED,
};

struct socket {
    enum socket_state state;
    char name[NAME_SIZE]; // what it's bound to, or empty
    char peer[NAME_SIZE]; // what it connected to, or empty
    struct pipe *rx;      // NULL once shut down, or before connecting
    struct pipe *tx;
    bool connected_once;
    int backlog;
    int queued;
    struct socket *queue_head; // connections waiting for accept
    struct socket *queue_tail;
    struct socket *queue_next;
    struct socket *bound_next; // the list of everything with a name
};

static struct socket *bound;

struct socket *socket_create(void) {
    struct socket *socket = kmalloc(sizeof(*socket));

    if (socket)
        *socket = (struct socket){.state = SOCKET_NEW};
    return socket;
}

static struct socket *find_bound(const char *name) {
    for (struct socket *s = bound; s; s = s->bound_next) {
        if (strcmp(s->name, name) == 0)
            return s;
    }
    return NULL;
}

// Joins two new sockets with a pipe each way.
static int join(struct socket *a, struct socket *b) {
    struct pipe *to_a = pipe_create();
    struct pipe *to_b = pipe_create();

    if (!to_a || !to_b) {
        if (to_a) {
            pipe_close_reader(to_a);
            pipe_close_writer(to_a);
        }
        if (to_b) {
            pipe_close_reader(to_b);
            pipe_close_writer(to_b);
        }
        return -ENOMEM;
    }

    a->rx = to_a;
    a->tx = to_b;
    b->rx = to_b;
    b->tx = to_a;
    a->state = b->state = SOCKET_CONNECTED;
    a->connected_once = b->connected_once = true;
    return 0;
}

int socket_create_pair(struct socket **a, struct socket **b) {
    *a = socket_create();
    *b = socket_create();

    int status = *a && *b ? join(*a, *b) : -ENOMEM;

    if (status < 0) {
        kfree(*a);
        kfree(*b);
    }
    return status;
}

void socket_close(struct socket *socket) {
    if (socket->rx)
        pipe_close_reader(socket->rx);
    if (socket->tx)
        pipe_close_writer(socket->tx);

    while (socket->queue_head) {
        struct socket *waiting = socket->queue_head;

        socket->queue_head = waiting->queue_next;
        socket_close(waiting);
    }

    for (struct socket **link = &bound; *link; link = &(*link)->bound_next) {
        if (*link == socket) {
            *link = socket->bound_next;
            break;
        }
    }
    process_wakeup(socket);
    kfree(socket);
}

int socket_bind(struct socket *socket, const char *name) {
    if (strlen(name) >= NAME_SIZE)
        return -ENAMETOOLONG;
    if (find_bound(name))
        return -EADDRINUSE;
    if (socket->name[0] || socket->state != SOCKET_NEW)
        return -EINVAL;

    memcpy(socket->name, name, strlen(name) + 1);
    socket->bound_next = bound;
    bound = socket;
    return 0;
}

int socket_listen(struct socket *socket, int backlog) {
    if (socket->state == SOCKET_CONNECTED)
        return -EISCONN;
    if (!socket->name[0])
        return -EDESTADDRREQ;

    socket->state = SOCKET_LISTENING;
    socket->backlog = backlog < 1 ? 1 : backlog > SOMAXCONN ? SOMAXCONN : backlog;
    return 0;
}

int socket_connect(struct socket *socket, const char *name, bool nonblocking) {
    if (socket->state == SOCKET_CONNECTED)
        return -EISCONN;
    if (socket->state == SOCKET_LISTENING)
        return -EINVAL;
    if (strlen(name) >= NAME_SIZE)
        return -ENAMETOOLONG;

    for (;;) {
        // Looked up afresh every time, as it may have closed during the wait.
        struct socket *listener = find_bound(name);

        if (!listener)
            return -ENOENT;
        if (listener->state != SOCKET_LISTENING)
            return -ECONNREFUSED;

        if (listener->queued < listener->backlog) {
            struct socket *server = socket_create();
            int status = server ? join(socket, server) : -ENOMEM;

            if (status < 0) {
                kfree(server);
                return status;
            }

            memcpy(server->name, name, strlen(name) + 1);
            memcpy(socket->peer, name, strlen(name) + 1);
            if (listener->queue_tail && listener->queue_head)
                listener->queue_tail->queue_next = server;
            else
                listener->queue_head = server;
            listener->queue_tail = server;
            listener->queued++;
            process_wakeup(listener);
            return 0;
        }

        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep(listener))
            return -EINTR;
    }
}

int socket_accept(struct socket *socket, bool nonblocking, struct socket **out) {
    if (socket->state != SOCKET_LISTENING)
        return -EINVAL;

    while (!socket->queue_head) {
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep(socket))
            return -EINTR;
    }

    *out = socket->queue_head;
    socket->queue_head = (*out)->queue_next;
    (*out)->queue_next = NULL;
    socket->queued--;
    process_wakeup(socket);
    return 0;
}

int socket_shutdown(struct socket *socket, int how) {
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return -EINVAL;
    if (socket->state != SOCKET_CONNECTED)
        return -ENOTCONN;

    if (how != SHUT_WR && socket->rx) {
        pipe_close_reader(socket->rx);
        socket->rx = NULL;
    }
    if (how != SHUT_RD && socket->tx) {
        pipe_close_writer(socket->tx);
        socket->tx = NULL;
    }
    return 0;
}

int socket_name(const struct socket *socket, bool peer, char *out, size_t size) {
    const char *name = peer ? socket->peer : socket->name;

    if (peer && socket->state != SOCKET_CONNECTED)
        return -ENOTCONN;
    if (strlen(name) >= size)
        return -ENAMETOOLONG;
    memcpy(out, name, strlen(name) + 1);
    return 0;
}

int64_t socket_read(struct socket *socket, void *buf, size_t len, bool nonblocking, bool peek) {
    if (socket->state != SOCKET_CONNECTED)
        return -ENOTCONN;
    if (!socket->rx)
        return 0;
    if (peek) {
        while (pipe_available(socket->rx) == 0 && pipe_has_writer(socket->rx)) {
            if (nonblocking)
                return -EAGAIN;
            if (!process_sleep(socket->rx))
                return -EINTR;
        }
        return pipe_peek(socket->rx, buf, len);
    }
    return pipe_read(socket->rx, buf, len, nonblocking);
}

int64_t socket_write(struct socket *socket, const void *buf, size_t len, bool nonblocking, bool raise_sigpipe) {
    if (socket->state != SOCKET_CONNECTED)
        return -ENOTCONN;
    if (!socket->tx) {
        if (raise_sigpipe)
            process_signal_self(SIGPIPE);
        return -EPIPE;
    }
    return pipe_write(socket->tx, buf, len, nonblocking, raise_sigpipe);
}

int socket_poll(const struct socket *socket) {
    if (socket->state == SOCKET_LISTENING)
        return socket->queue_head ? POLLIN : 0;
    if (socket->state != SOCKET_CONNECTED)
        return POLLOUT | POLLHUP;

    int ready = 0;

    if (!socket->rx || pipe_available(socket->rx) > 0)
        ready |= POLLIN;
    if (!socket->rx || !pipe_has_writer(socket->rx))
        ready |= POLLIN | POLLHUP;
    if (socket->tx && pipe_has_room(socket->tx))
        ready |= POLLOUT;
    if (!socket->tx || !pipe_has_reader(socket->tx))
        ready |= POLLERR | POLLOUT;
    return ready;
}

size_t socket_available(const struct socket *socket) {
    return socket->rx ? pipe_available(socket->rx) : 0;
}

int socket_send_file(struct socket *socket, struct file *file) {
    if (socket->state != SOCKET_CONNECTED)
        return -ENOTCONN;
    if (!socket->tx || !pipe_has_reader(socket->tx))
        return -EPIPE;
    return pipe_add_file(socket->tx, file) ? 0 : -ENOBUFS;
}

struct file *socket_receive_file(struct socket *socket) {
    return socket->rx ? pipe_take_file(socket->rx) : NULL;
}
