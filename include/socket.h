#pragma once

#include <socket_types.h>
#include <stddef.h>
#include <stdint.h>

// Local stream sockets, named by a path that only exists in a table here rather
// than on any disk. Everything that returns an int fails with a negative errno.
struct socket;

struct socket *socket_create(void);

// Makes two sockets connected to each other, for socketpair.
int socket_create_pair(struct socket **a, struct socket **b);

// Closes it, dropping its name. Whoever is connected to it sees the end of the data.
void socket_close(struct socket *socket);

// name is either a normalized path, or starts with @ for one in no directory.
int socket_bind(struct socket *socket, const char *name);
int socket_listen(struct socket *socket, int backlog);
int socket_connect(struct socket *socket, const char *name, bool nonblocking);

// Sets out to the socket for the next connection.
int socket_accept(struct socket *socket, bool nonblocking, struct socket **out);

int socket_shutdown(struct socket *socket, int how);

// Copies its name, or that of what it's connected to. Empty if it has none.
int socket_name(const struct socket *socket, bool peer, char *out, size_t size);

int64_t socket_read(struct socket *socket, void *buf, size_t len, bool nonblocking, bool peek);
int64_t socket_write(struct socket *socket, const void *buf, size_t len, bool nonblocking, bool raise_sigpipe);

// Which of the POLL events are true now.
int socket_poll(const struct socket *socket);

size_t socket_available(const struct socket *socket);

// Sends a file along with the data, or takes the next one to arrive, or NULL.
int socket_send_file(struct socket *socket, struct file *file);
struct file *socket_receive_file(struct socket *socket);
