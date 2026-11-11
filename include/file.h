#pragma once

#include <fs_types.h>
#include <stddef.h>
#include <stdint.h>

// An open file, directory, pipe end or the console. Descriptors sharing one,
// through fork or dup, also share its offset.
struct file;
struct socket;
struct inet_socket;

// Everything here that returns an int or int64_t fails with a negative errno.

struct file *file_console(void);

// Opens the slave of a pseudo-terminal's master, for TIOCGPTPEER.
int file_open_peer(struct file *master, int flags, struct file **out);

// flags are the O_ values.
int file_open(const char *cwd, const char *path, int flags, struct file **out);

int file_pipe(struct file **read_end, struct file **write_end);

// Wraps a socket, taking it over. flags can have O_NONBLOCK.
int file_from_socket(struct socket *socket, int flags, struct file **out);

// The same for an Internet socket.
int file_from_inet(struct inet_socket *socket, int flags, struct file **out);
struct inet_socket *file_inet(const struct file *file);

// The socket behind a descriptor, or NULL if it isn't one.
struct socket *file_socket(const struct file *file);

int file_shm_open(const char *name, int flags, struct file **out);

// Sets the size of a file or shared memory object. A file grows with zeros.
int file_truncate(struct file *file, uint64_t size);

// The shared memory behind a descriptor, or NULL if it isn't that kind. write
// says whether it was opened for writing.
struct shm *file_shm(const struct file *file, bool *writable);

// Whether this is /dev/fb0, and if so whether it holds the screen.
bool file_is_fb(const struct file *file, bool *writable);

int file_stat(struct file *file, struct stat *out);

// The O_ flags in effect, and of those only O_APPEND and O_NONBLOCK can change.
int file_status_flags(const struct file *file);
void file_set_status_flags(struct file *file, int flags);

// arg must be a validated pointer to as much as the request needs.
int file_ioctl(struct file *file, uint64_t request, void *arg);

// Which of the POLL events are true now, plus POLLERR and POLLHUP if they are.
int file_poll(struct file *file, int events);

struct file *file_ref(struct file *file);
void file_close(struct file *file);

// These return the bytes transferred. buf must be a validated pointer. The
// console and pipes can block, and writing to a pipe nobody can read from raises
// SIGPIPE.
int64_t file_read(struct file *file, void *buf, size_t len);
int64_t file_write(struct file *file, const void *buf, size_t len);

int64_t file_seek(struct file *file, int64_t offset, int whence);

// Returns 1 with the next entry, or 0 after the last one.
int file_readdir(struct file *file, struct dirent *entry);

// Removes a file or an empty directory, unless it is open somewhere. That's
// stricter than Unix, but a stale descriptor could otherwise write into clusters
// that now belong to something else.
int file_remove(const char *cwd, const char *path, bool is_dir);

// Moves a file or directory on the same filesystem, unless it or a file it would
// replace is open. Open descriptors remember where a directory entry is stored,
// which a move changes.
int file_rename(const char *cwd, const char *old_path, const char *new_path);
