#pragma once

#include <stddef.h>
#include <stdint.h>

struct file;

// A one-way byte queue, used by pipes and as each direction of a socket.
struct pipe;

struct pipe *pipe_create(void);

// Pipes go away once both ends have been closed.
void pipe_close_reader(struct pipe *pipe);
void pipe_close_writer(struct pipe *pipe);

// These block unless told not to, and fail with a negative errno. Writing with no
// reader left fails with EPIPE, raising SIGPIPE first if asked to.
int64_t pipe_read(struct pipe *pipe, uint8_t *buf, size_t len, bool nonblocking);
int64_t pipe_write(struct pipe *pipe, const uint8_t *buf, size_t len, bool nonblocking, bool raise_sigpipe);

// Copies out what's next without removing it, and never blocks.
size_t pipe_peek(const struct pipe *pipe, uint8_t *buf, size_t len);

size_t pipe_available(const struct pipe *pipe);
bool pipe_has_room(const struct pipe *pipe);
bool pipe_has_reader(const struct pipe *pipe);
bool pipe_has_writer(const struct pipe *pipe);

// Descriptors on their way through a socket, which hold a reference to their file.
// Adding fails if too many are waiting. Taking returns NULL when there are none.
bool pipe_add_file(struct pipe *pipe, struct file *file);
struct file *pipe_take_file(struct pipe *pipe);
