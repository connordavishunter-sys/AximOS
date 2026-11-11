#pragma once

#include <socket_types.h>
#include <stddef.h>
#include <stdint.h>

// Sockets of the Internet family, over UDP, TCP and ICMP echo. What returns an int
// fails with a negative errno.
struct inet_socket;

int inet_create(int type, int protocol, struct inet_socket **out);
void inet_close(struct inet_socket *socket);

int inet_bind(struct inet_socket *socket, const struct sockaddr_in *address);
int inet_listen(struct inet_socket *socket, int backlog);
int inet_connect(struct inet_socket *socket, const struct sockaddr_in *address, bool nonblocking);
int inet_accept(struct inet_socket *socket, bool nonblocking, struct inet_socket **out, struct sockaddr_in *peer);
int inet_shutdown(struct inet_socket *socket, int how);
int inet_name(const struct inet_socket *socket, bool peer, struct sockaddr_in *out);

// to and from may be NULL.
int64_t inet_send(struct inet_socket *socket, const void *buf, size_t len, const struct sockaddr_in *to, bool nonblocking, bool raise_sigpipe);
int64_t inet_receive(struct inet_socket *socket, void *buf, size_t len, struct sockaddr_in *from, bool nonblocking, bool peek);

int inet_poll(const struct inet_socket *socket);
size_t inet_available(const struct inet_socket *socket);
int inet_type(const struct inet_socket *socket);
int inet_take_error(struct inet_socket *socket);

void inet_deliver_udp(uint32_t source, uint16_t source_port, uint32_t destination, uint16_t port, const void *data, size_t len);
void inet_deliver_echo_reply(uint32_t source, uint16_t id, const void *message, size_t len);
void inet_port_unreachable(uint32_t destination, uint16_t our_port, uint16_t their_port);
