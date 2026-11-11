#pragma once

#include <stddef.h>
#include <stdint.h>

// A connection outlives the socket that closed it, for as long as it takes to deliver
// what's left and say goodbye. What returns an int fails with a negative errno.
struct tcp;

struct tcp *tcp_create(void);

// Whether a port has a listener or connection on it.
bool tcp_port_in_use(uint16_t port);

int tcp_connect(struct tcp *c, uint32_t local, uint16_t local_port, uint32_t remote, uint16_t remote_port, bool nonblocking);
int tcp_listen(struct tcp *c, uint32_t local, uint16_t local_port, int backlog);

// Sets out to the next connection, which is the caller's to tcp_close.
int tcp_accept(struct tcp *c, bool nonblocking, struct tcp **out);

int64_t tcp_read(struct tcp *c, void *buf, size_t len, bool nonblocking, bool peek);
int64_t tcp_write(struct tcp *c, const void *buf, size_t len, bool nonblocking, bool raise_sigpipe);
int tcp_shutdown(struct tcp *c, int how);
void tcp_close(struct tcp *c);

// Which of the POLL events are true now.
int tcp_poll(const struct tcp *c);

size_t tcp_available(const struct tcp *c);

// Returns and clears the error waiting to be reported, for SO_ERROR.
int tcp_take_error(struct tcp *c);

bool tcp_is_connected(const struct tcp *c);
bool tcp_is_listening(const struct tcp *c);
void tcp_addresses(const struct tcp *c, uint32_t *local, uint16_t *local_port, uint32_t *remote, uint16_t *remote_port);

void tcp_input(uint32_t source, uint32_t destination, const uint8_t *segment, size_t len);
void tcp_tick(void);
