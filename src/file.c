#include <console.h>
#include <errno_values.h>
#include <fb.h>
#include <file.h>
#include <keyboard.h>
#include <kmalloc.h>
#include <mm.h>
#include <mouse.h>
#include <pipe.h>
#include <process.h>
#include <serial.h>
#include <shm.h>
#include <signal_numbers.h>
#include <inet.h>
#include <random.h>
#include <socket.h>
#include <string.h>
#include <syscall_numbers.h>
#include <tty.h>
#include <vfs.h>

enum file_kind {
    FILE_CONSOLE,
    FILE_NODE,
    FILE_PIPE_READ,
    FILE_PIPE_WRITE,
    FILE_SHM,
    FILE_MOUSE,
    FILE_KEYBOARD,
    FILE_FB,
    FILE_NULL,
    FILE_ZERO,
    FILE_RANDOM,
    FILE_URANDOM,
    FILE_SOCKET,
    FILE_INET,
    FILE_TTY,        // a pseudo-terminal's slave
    FILE_PTY_MASTER,
};

// How much one read gets, so that a big one can't hold up the kernel for long.
#define RANDOM_READ_MAX (1024 * 1024)

struct file {
    int refs;
    enum file_kind kind;
    int flags;
    struct vfs_node node;
    uint64_t offset;
    struct pipe *pipe;
    struct shm *shm;
    struct socket *socket;
    struct inet_socket *inet;
    struct tty *tty;
    struct file *next_open; // the list of open nodes
    struct file *prev_open;
};

// Starts with a reference of its own, so closing descriptors never frees it.
static struct file console = {.refs = 1, .kind = FILE_CONSOLE, .flags = O_RDWR};

static struct file *open_nodes;

struct file *file_console(void) {
    return file_ref(&console);
}

static struct tty *tty_of(const struct file *file) {
    return file->kind == FILE_CONSOLE ? tty_console() : file->kind == FILE_TTY ? file->tty : NULL;
}

static struct file *new_tty_file(struct file *file, enum file_kind kind, struct tty *tty, int flags) {
    *file = (struct file){.refs = 1, .kind = kind, .flags = flags, .tty = tty};
    return file;
}

int file_open_peer(struct file *master, int flags, struct file **out) {
    struct file *file;

    if (master->kind != FILE_PTY_MASTER)
        return -ENOTTY;
    if (!(file = kmalloc(sizeof(*file))))
        return -ENOMEM;
    *out = new_tty_file(file, FILE_TTY, tty_open_slave(master->tty), (flags & O_NONBLOCK) | O_RDWR);
    return 0;
}

static int file_open_locked(const char *cwd, const char *path, int flags, struct file **out) {
    struct file *file = kmalloc(sizeof(*file));
    if (!file)
        return -ENOMEM;

    int status = vfs_lookup(cwd, path, &file->node);
    if (status == VFS_OK && (flags & O_CREAT) && (flags & O_EXCL)) {
        status = -EEXIST;
    } else if (status == VFS_NOT_FOUND && (flags & O_CREAT)) {
        size_t len = strlen(path);

        // A trailing slash means a directory, which O_CREAT can't make.
        status = len > 0 && path[len - 1] == '/' ? -EISDIR
                                                 : vfs_create(cwd, path, false, &file->node);
    }

    int access = flags & O_ACCMODE;
    if (status == VFS_OK && file->node.is_dir && access != O_RDONLY)
        status = -EISDIR;
    if (status == VFS_OK && vfs_device_of(&file->node) == VFS_DEVICE_FB) {
        // Opening it for writing is what takes the screen from the console.
        status = access == O_RDONLY ? 0 : fb_acquire();
        if (status < 0) {
            kfree(file);
            return status;
        }
        file->kind = FILE_FB;
        file->flags = flags;
        file->refs = 1;
        *out = file;
        return 0;
    }
    enum vfs_device device = status == VFS_OK ? vfs_device_of(&file->node) : VFS_DEVICE_NONE;

    if (device == VFS_DEVICE_MOUSE || device == VFS_DEVICE_KEYBOARD) {
        status = access != O_RDONLY ? -EACCES : device == VFS_DEVICE_KEYBOARD ? keyboard_open_events() : 0;
        if (status < 0) {
            kfree(file);
            return status;
        }
        file->kind = device == VFS_DEVICE_MOUSE ? FILE_MOUSE : FILE_KEYBOARD;
        file->flags = flags;
        file->refs = 1;
        *out = file;
        return 0;
    }
    if (device == VFS_DEVICE_PTMX) {
        struct tty *tty = tty_open_pty();

        if (!tty) {
            kfree(file);
            return -ENOMEM;
        }
        *out = new_tty_file(file, FILE_PTY_MASTER, tty, (flags & ~O_ACCMODE) | O_RDWR);
        return 0;
    }
    if (device == VFS_DEVICE_TTY) {
        struct tty *tty = process_tty();

        if (!tty || tty == tty_console()) {
            kfree(file);
            *out = tty ? file_console() : NULL;
            return tty ? 0 : -ENXIO;
        }
        *out = new_tty_file(file, FILE_TTY, tty_open_slave(tty), (flags & ~O_ACCMODE) | O_RDWR);
        return 0;
    }
    if (status == VFS_OK && (vfs_device_of(&file->node) == VFS_DEVICE_RANDOM
                             || vfs_device_of(&file->node) == VFS_DEVICE_URANDOM)) {
        file->kind = vfs_device_of(&file->node) == VFS_DEVICE_RANDOM ? FILE_RANDOM : FILE_URANDOM;
        file->refs = 1;
        file->flags = flags;
        file->offset = 0;
        *out = file;
        return 0;
    }
    if (status == VFS_OK && (vfs_device_of(&file->node) == VFS_DEVICE_NULL
                             || vfs_device_of(&file->node) == VFS_DEVICE_ZERO)) {
        file->kind = vfs_device_of(&file->node) == VFS_DEVICE_NULL ? FILE_NULL : FILE_ZERO;
        file->flags = flags;
        file->refs = 1;
        *out = file;
        return 0;
    }
    if (status == VFS_OK && (flags & O_DIRECTORY) && !file->node.is_dir)
        status = -ENOTDIR;
    if (status == VFS_OK && (flags & O_TRUNC) && access != O_RDONLY)
        status = vfs_truncate(&file->node, 0);

    if (status != VFS_OK) {
        kfree(file);
        return status;
    }

    file->kind = FILE_NODE;
    file->flags = flags;
    file->refs = 1;

    file->next_open = open_nodes;
    if (open_nodes)
        open_nodes->prev_open = file;
    open_nodes = file;

    *out = file;
    return 0;
}

int file_open(const char *cwd, const char *path, int flags, struct file **out) {
    vfs_lock();

    int status = file_open_locked(cwd, path, flags, out);

    vfs_unlock();
    return status;
}

int file_pipe(struct file **read_end, struct file **write_end) {
    struct pipe *pipe = pipe_create();
    struct file *reader = kmalloc(sizeof(*reader));
    struct file *writer = kmalloc(sizeof(*writer));

    if (!pipe || !reader || !writer) {
        if (pipe) {
            pipe_close_reader(pipe);
            pipe_close_writer(pipe);
        }
        kfree(reader);
        kfree(writer);
        return -ENOMEM;
    }

    *reader = (struct file){.refs = 1, .kind = FILE_PIPE_READ, .flags = O_RDONLY, .pipe = pipe};
    *writer = (struct file){.refs = 1, .kind = FILE_PIPE_WRITE, .flags = O_WRONLY, .pipe = pipe};

    *read_end = reader;
    *write_end = writer;
    return 0;
}

int file_from_socket(struct socket *socket, int flags, struct file **out) {
    struct file *file = kmalloc(sizeof(*file));

    if (!file)
        return -ENOMEM;
    *file = (struct file){.refs = 1, .kind = FILE_SOCKET, .flags = O_RDWR | flags, .socket = socket};
    *out = file;
    return 0;
}

int file_from_inet(struct inet_socket *socket, int flags, struct file **out) {
    struct file *file = kmalloc(sizeof(*file));

    if (!file)
        return -ENOMEM;
    *file = (struct file){.refs = 1, .kind = FILE_INET, .flags = O_RDWR | flags, .inet = socket};
    *out = file;
    return 0;
}

struct inet_socket *file_inet(const struct file *file) {
    return file->kind == FILE_INET ? file->inet : NULL;
}

struct socket *file_socket(const struct file *file) {
    return file->kind == FILE_SOCKET ? file->socket : NULL;
}

int file_shm_open(const char *name, int flags, struct file **out) {
    struct file *file = kmalloc(sizeof(*file));
    if (!file)
        return -ENOMEM;

    int status = shm_open(name, flags, &file->shm);
    if (status < 0) {
        kfree(file);
        return status;
    }

    file->kind = FILE_SHM;
    file->flags = flags;
    file->refs = 1;
    *out = file;
    return 0;
}

int file_truncate(struct file *file, uint64_t size) {
    if (file->kind != FILE_SHM && file->kind != FILE_NODE)
        return -EINVAL;
    if ((file->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;
    if (file->kind == FILE_SHM)
        return shm_set_size(file->shm, size);

    vfs_lock();

    int status = vfs_refresh(&file->node);

    if (status == VFS_OK)
        status = vfs_truncate(&file->node, size);

    vfs_unlock();
    return status;
}

int file_stat(struct file *file, struct stat *out) {
    int status = VFS_OK;

    *out = (struct stat){.st_nlink = 1, .st_blksize = 512};

    switch (file->kind) {
    case FILE_NODE:
        vfs_lock();
        status = vfs_refresh(&file->node);
        vfs_unlock();
        if (status == VFS_OK)
            vfs_fill_stat(&file->node, out);
        return status;
    case FILE_PIPE_READ:
    case FILE_PIPE_WRITE:
        out->st_mode = S_IFIFO | 0777;
        return 0;
    case FILE_INET:
    case FILE_SOCKET:
        out->st_mode = S_IFSOCK | 0777;
        return 0;
    case FILE_SHM:
        out->st_mode = S_IFREG | 0777;
        out->st_size = shm_size(file->shm);
        return 0;
    default:
        out->st_mode = S_IFCHR | 0777;
        return 0;
    }
}

int file_status_flags(const struct file *file) {
    return file->flags & (O_ACCMODE | O_APPEND | O_NONBLOCK);
}

void file_set_status_flags(struct file *file, int flags) {
    int changeable = O_APPEND | O_NONBLOCK;

    file->flags = (file->flags & ~changeable) | (flags & changeable);
}

int file_ioctl(struct file *file, uint64_t request, void *arg) {
    if (request == FIONREAD && file->kind == FILE_PIPE_READ) {
        *(int *)arg = pipe_available(file->pipe);
        return 0;
    }
    if (request == FIONREAD && file->kind == FILE_SOCKET) {
        *(int *)arg = socket_available(file->socket);
        return 0;
    }
    if (request == FIONREAD && file->kind == FILE_INET) {
        *(int *)arg = inet_available(file->inet);
        return 0;
    }
    // The master speaks for its terminal as a whole.
    if (file->kind == FILE_PTY_MASTER) {
        switch (request) {
        case TIOCSWINSZ:
            return tty_set_size(file->tty, arg);
        case TIOCGWINSZ:
            tty_get_size(file->tty, arg);
            return 0;
        }
        return -ENOTTY;
    }

    struct tty *tty = tty_of(file);

    if (!tty)
        return -ENOTTY;

    switch (request) {
    case FIONREAD:
        *(int *)arg = tty_pending(tty);
        return 0;
    case TCGETS:
        tty_get_settings(tty, arg);
        return 0;
    case TCSETS:
    case TCSETSW:
    case TCSETSF:
        tty_set_settings(tty, arg, request == TCSETSF);
        return 0;
    case TIOCGPGRP:
        *(int *)arg = tty_foreground(tty);
        return 0;
    case TIOCSPGRP:
        tty_set_foreground(tty, *(const int *)arg);
        return 0;
    case TIOCGWINSZ:
        tty_get_size(tty, arg);
        return 0;
    case TIOCSWINSZ:
        return tty_set_size(tty, arg);
    case TIOCSCTTY:
        return process_set_tty(tty);
    }
    return -ENOTTY;
}

int file_poll(struct file *file, int events) {
    int ready = 0;

    switch (file->kind) {
    case FILE_CONSOLE:
    case FILE_TTY:
        ready = (tty_writable(tty_of(file)) ? POLLOUT : 0) | (tty_readable(tty_of(file)) ? POLLIN : 0);
        break;
    case FILE_PTY_MASTER:
        ready = tty_master_poll(file->tty);
        break;
    case FILE_PIPE_READ:
        if (pipe_available(file->pipe) > 0)
            ready |= POLLIN;
        if (!pipe_has_writer(file->pipe))
            ready |= POLLHUP;
        break;
    case FILE_PIPE_WRITE:
        if (pipe_has_room(file->pipe))
            ready |= POLLOUT;
        if (!pipe_has_reader(file->pipe))
            ready |= POLLERR;
        break;
    case FILE_MOUSE:
        ready = mouse_readable() ? POLLIN : 0;
        break;
    case FILE_KEYBOARD:
        ready = keyboard_events_readable() ? POLLIN : 0;
        break;
    case FILE_SOCKET:
        ready = socket_poll(file->socket);
        break;
    case FILE_INET:
        ready = inet_poll(file->inet);
        break;
    default:
        ready = POLLIN | POLLOUT;
    }
    return ready & (events | POLLERR | POLLHUP);
}

bool file_is_fb(const struct file *file, bool *writable) {
    if (file->kind != FILE_FB)
        return false;

    *writable = (file->flags & O_ACCMODE) != O_RDONLY;
    return true;
}

struct shm *file_shm(const struct file *file, bool *writable) {
    if (file->kind != FILE_SHM)
        return NULL;

    *writable = (file->flags & O_ACCMODE) != O_RDONLY;
    return file->shm;
}

struct file *file_ref(struct file *file) {
    file->refs++;
    return file;
}

void file_close(struct file *file) {
    if (--file->refs > 0)
        return;

    if (file->kind == FILE_NODE) {
        if (file->prev_open)
            file->prev_open->next_open = file->next_open;
        else
            open_nodes = file->next_open;
        if (file->next_open)
            file->next_open->prev_open = file->prev_open;
    } else if (file->kind == FILE_PIPE_READ) {
        pipe_close_reader(file->pipe);
    } else if (file->kind == FILE_PIPE_WRITE) {
        pipe_close_writer(file->pipe);
    } else if (file->kind == FILE_SOCKET) {
        socket_close(file->socket);
    } else if (file->kind == FILE_INET) {
        inet_close(file->inet);
    } else if (file->kind == FILE_TTY) {
        tty_close_slave(file->tty);
    } else if (file->kind == FILE_PTY_MASTER) {
        tty_close_master(file->tty);
    } else if (file->kind == FILE_KEYBOARD) {
        keyboard_close_events();
    } else if (file->kind == FILE_SHM) {
        shm_unref(file->shm);
    } else if (file->kind == FILE_FB && (file->flags & O_ACCMODE) != O_RDONLY) {
        fb_release();
    }
    kfree(file);
}

int64_t file_read(struct file *file, void *buf, size_t len) {
    bool nonblocking = file->flags & O_NONBLOCK;

    switch (file->kind) {
    case FILE_CONSOLE:
    case FILE_TTY:
        return tty_read(tty_of(file), buf, len, nonblocking);
    case FILE_PTY_MASTER:
        return tty_master_read(file->tty, buf, len, nonblocking);
    case FILE_PIPE_READ:
        return pipe_read(file->pipe, buf, len, nonblocking);
    case FILE_PIPE_WRITE:
        return -EBADF;
    case FILE_MOUSE:
        return mouse_read(buf, len, nonblocking);
    case FILE_KEYBOARD:
        return keyboard_read_events(buf, len, nonblocking);
    case FILE_SOCKET:
        return socket_read(file->socket, buf, len, nonblocking, false);
    case FILE_INET:
        return inet_receive(file->inet, buf, len, NULL, nonblocking, false);
    case FILE_NULL:
        return 0;
    case FILE_ZERO:
        memset(buf, 0, len);
        return len;
    case FILE_RANDOM:
    case FILE_URANDOM: {
        // Only /dev/random waits for there to be enough to go on.
        int status = file->kind == FILE_RANDOM ? random_wait_ready(nonblocking) : 0;

        if (status < 0)
            return status;
        if (len > RANDOM_READ_MAX)
            len = RANDOM_READ_MAX;
        random_bytes(buf, len);
        return len;
    }
    case FILE_FB:
    case FILE_SHM:
        return -EINVAL;
    case FILE_NODE:
        break;
    }

    if (file->node.is_dir)
        return -EISDIR;
    if ((file->flags & O_ACCMODE) == O_WRONLY)
        return -EBADF;

    vfs_lock();

    int64_t count = vfs_refresh(&file->node);
    if (count == VFS_OK)
        count = vfs_read(&file->node, file->offset, buf, len);
    if (count > 0)
        file->offset += count;

    vfs_unlock();
    return count;
}

int64_t file_write(struct file *file, const void *buf, size_t len) {
    switch (file->kind) {
    case FILE_CONSOLE:
    case FILE_TTY:
        return tty_write(tty_of(file), buf, len, file->flags & O_NONBLOCK);
    case FILE_PTY_MASTER:
        return tty_master_write(file->tty, buf, len);
    case FILE_PIPE_WRITE:
        return pipe_write(file->pipe, buf, len, file->flags & O_NONBLOCK, true);
    case FILE_SOCKET:
        return socket_write(file->socket, buf, len, file->flags & O_NONBLOCK, true);
    case FILE_INET:
        return inet_send(file->inet, buf, len, NULL, file->flags & O_NONBLOCK, true);
    case FILE_PIPE_READ:
    case FILE_MOUSE:
    case FILE_KEYBOARD:
        return -EBADF;
    case FILE_RANDOM:
    case FILE_URANDOM:
        random_add_data(buf, len);
        return len;
    case FILE_NULL:
    case FILE_ZERO:
        return len;
    case FILE_FB:
    case FILE_SHM:
        return -EINVAL;
    case FILE_NODE:
        break;
    }

    if ((file->flags & O_ACCMODE) == O_RDONLY)
        return -EBADF;

    vfs_lock();

    int64_t count = vfs_refresh(&file->node);
    if (count == VFS_OK) {
        if (file->flags & O_APPEND)
            file->offset = file->node.size;
        count = vfs_write(&file->node, file->offset, buf, len);
    }
    if (count > 0)
        file->offset += count;

    vfs_unlock();
    return count;
}

int64_t file_seek(struct file *file, int64_t offset, int whence) {
    if (file->kind != FILE_NODE)
        return -ESPIPE;
    if (file->node.is_dir)
        return -EISDIR;

    vfs_lock();

    int status = vfs_refresh(&file->node);

    vfs_unlock();
    if (status != VFS_OK)
        return status;

    int64_t base;
    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = file->offset;
        break;
    case SEEK_END:
        base = file->node.size;
        break;
    default:
        return -EINVAL;
    }

    if (base + offset < 0)
        return -EINVAL;

    file->offset = base + offset;
    return file->offset;
}

int file_readdir(struct file *file, struct dirent *entry) {
    if (file->kind != FILE_NODE || !file->node.is_dir)
        return -ENOTDIR;

    vfs_lock();

    int status = vfs_readdir(&file->node, &file->offset, entry);

    vfs_unlock();
    if (status == VFS_END)
        return 0;
    return status == VFS_OK ? 1 : status;
}

static bool is_open(const struct vfs_node *node) {
    for (struct file *file = open_nodes; file; file = file->next_open) {
        if (vfs_same_file(&file->node, node))
            return true;
    }
    return false;
}

static int file_remove_locked(const char *cwd, const char *path, bool is_dir) {
    struct vfs_node node;

    int status = vfs_lookup(cwd, path, &node);
    if (status != VFS_OK)
        return status;
    if (node.is_dir != is_dir)
        return is_dir ? -ENOTDIR : -EISDIR;
    if (is_open(&node))
        return -EBUSY;
    return vfs_remove(&node);
}

int file_remove(const char *cwd, const char *path, bool is_dir) {
    vfs_lock();

    int status = file_remove_locked(cwd, path, is_dir);

    vfs_unlock();
    return status;
}

static int file_rename_locked(const char *cwd, const char *old_path, const char *new_path) {
    struct vfs_node node, target;

    int status = vfs_lookup(cwd, old_path, &node);
    if (status != VFS_OK)
        return status;
    if (is_open(&node))
        return -EBUSY;
    if (vfs_lookup(cwd, new_path, &target) == VFS_OK && is_open(&target))
        return -EBUSY;

    // A trailing slash only makes sense on a directory.
    size_t len = strlen(new_path);
    if (len == 0)
        return -ENOENT;
    if (new_path[len - 1] == '/' && !node.is_dir)
        return -ENOTDIR;

    return vfs_rename(&node, cwd, new_path);
}

int file_rename(const char *cwd, const char *old_path, const char *new_path) {
    vfs_lock();

    int status = file_rename_locked(cwd, old_path, new_path);

    vfs_unlock();
    return status;
}
