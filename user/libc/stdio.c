#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "lock.h"

// Holds unread data in buffer[pos..len] while reading, or unwritten data in
// buffer[0..pos] while writing. Streams are in a list for fflush(NULL).
struct FILE {
    int fd;
    int mode;
    char *buffer;
    size_t size;
    size_t pos, len;
    bool readable, writable, writing, eof, error;
    bool own_buffer, on_heap;
    char *temporary_path; // removed on closing
    pid_t pid;            // the command behind popen
    recursive_lock lock;
    FILE *next;
};

static FILE stdin_file = {.fd = STDIN_FILENO, .mode = _IOFBF, .readable = true};
static FILE stderr_file = {.fd = STDERR_FILENO, .mode = _IONBF, .writable = true, .next = &stdin_file};
static FILE stdout_file = {.fd = STDOUT_FILENO, .mode = _IONBF, .writable = true, .next = &stderr_file};
static FILE *streams = &stdout_file;
static recursive_lock streams_lock;

FILE *stdin = &stdin_file, *stdout = &stdout_file, *stderr = &stderr_file;

static bool write_all(FILE *f, const char *data, size_t len) {
    while (len > 0) {
        ssize_t wrote = write(f->fd, data, len);

        if (wrote <= 0) {
            f->error = true;
            return false;
        }
        data += wrote;
        len -= wrote;
    }
    return true;
}

static int flush_stream(FILE *f) {
    if (f->writing) {
        size_t pending = f->pos;

        f->pos = 0;
        return write_all(f, f->buffer, pending) ? 0 : EOF;
    }

    // Moves the file back over what was read ahead, where the file can do that.
    if (f->len > f->pos && lseek(f->fd, -(long)(f->len - f->pos), SEEK_CUR) >= 0)
        f->pos = f->len = 0;
    return 0;
}

int fflush(FILE *f) {
    if (f)
        return flush_stream(f);

    int result = 0;

    for (f = streams; f; f = f->next) {
        if (f->writing && flush_stream(f) < 0)
            result = EOF;
    }
    return result;
}

static bool ensure_buffer(FILE *f) {
    if (f->buffer)
        return true;

    f->buffer = malloc(BUFSIZ);
    if (!f->buffer) {
        f->error = true;
        errno = ENOMEM;
        return false;
    }
    f->size = BUFSIZ;
    f->own_buffer = true;
    return true;
}

static bool parse_mode(const char *mode, int *flags) {
    bool plus = strchr(mode, '+');

    switch (mode[0]) {
    case 'r':
        *flags = plus ? O_RDWR : O_RDONLY;
        break;
    case 'w':
        *flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
        break;
    case 'a':
        *flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
        break;
    default:
        errno = EINVAL;
        return false;
    }
    if (strchr(mode, 'x'))
        *flags |= O_EXCL;
    return true;
}

static void init_stream(FILE *f, int fd, int flags) {
    f->fd = fd;
    f->mode = _IOFBF;
    f->pos = f->len = 0;
    f->writing = f->eof = f->error = false;
    f->readable = (flags & O_ACCMODE) != O_WRONLY;
    f->writable = (flags & O_ACCMODE) != O_RDONLY;
}

FILE *fdopen(int fd, const char *mode) {
    int flags;

    if (!parse_mode(mode, &flags))
        return NULL;

    FILE *f = calloc(1, sizeof(*f));

    if (!f) {
        errno = ENOMEM;
        return NULL;
    }
    init_stream(f, fd, flags);
    f->on_heap = true;
    f->next = streams;
    streams = f;
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int flags;

    if (!parse_mode(mode, &flags))
        return NULL;

    int fd = open(path, flags);

    if (fd < 0)
        return NULL;

    FILE *f = fdopen(fd, mode);

    if (!f)
        close(fd);
    return f;
}

FILE *freopen(const char *path, const char *mode, FILE *f) {
    int flags;

    flush_stream(f);
    close(f->fd);
    if (!parse_mode(mode, &flags) || (f->fd = open(path, flags)) < 0) {
        fclose(f);
        return NULL;
    }
    init_stream(f, f->fd, flags);
    return f;
}

int fclose(FILE *f) {
    __lock_take(&streams_lock);
    __lock_take(&f->lock);

    int result = flush_stream(f);

    if (close(f->fd) < 0)
        result = EOF;
    if (f->own_buffer)
        free(f->buffer);
    if (f->temporary_path) {
        unlink(f->temporary_path);
        free(f->temporary_path);
    }

    for (FILE **link = &streams; *link; link = &(*link)->next) {
        if (*link == f) {
            *link = f->next;
            break;
        }
    }
    __lock_give(&f->lock);
    __lock_give(&streams_lock);
    if (f->on_heap)
        free(f);
    return result;
}

int setvbuf(FILE *f, char *buffer, int mode, size_t size) {
    LOCKED(&f->lock);
    if (mode != _IOFBF && mode != _IOLBF && mode != _IONBF) {
        errno = EINVAL;
        return -1;
    }

    flush_stream(f);
    f->mode = mode;
    if (buffer && size > 0 && f->pos == f->len) {
        if (f->own_buffer)
            free(f->buffer);
        f->buffer = buffer;
        f->size = size;
        f->own_buffer = false;
        f->pos = f->len = 0;
    }
    return 0;
}

void setbuf(FILE *f, char *buffer) {
    setvbuf(f, buffer, buffer ? _IOFBF : _IONBF, BUFSIZ);
}

int fileno(FILE *f) { return f->fd; }
int feof(FILE *f) { return f->eof; }
int ferror(FILE *f) { return f->error; }

void clearerr(FILE *f) {
    LOCKED(&f->lock);
    f->eof = f->error = false;
}

static bool start_writing(FILE *f) {
    if (!f->writable) {
        f->error = true;
        errno = EBADF;
        return false;
    }
    if (!f->writing) {
        flush_stream(f);
        f->pos = f->len = 0;
        f->writing = true;
    }
    return true;
}

static bool start_reading(FILE *f) {
    if (!f->readable) {
        f->error = true;
        errno = EBADF;
        return false;
    }
    if (f->writing) {
        if (flush_stream(f) < 0)
            return false;
        f->writing = false;
        f->pos = f->len = 0;
    }
    return true;
}

// Returns false at the end of the file or on an error.
static bool fill(FILE *f) {
    if (f->eof || !ensure_buffer(f))
        return false;

    // A prompt without a newline should be on screen before waiting for an answer.
    if (f == stdin && stdout->mode == _IOLBF)
        fflush(stdout);

    ssize_t got = read(f->fd, f->buffer, f->mode == _IONBF ? 1 : f->size);

    f->pos = 0;
    f->len = got > 0 ? got : 0;
    if (got == 0)
        f->eof = true;
    else if (got < 0)
        f->error = true;
    return got > 0;
}

size_t fread(void *dest, size_t size, size_t count, FILE *f) {
    LOCKED(&f->lock);
    size_t total;

    if (__builtin_mul_overflow(size, count, &total) || total == 0 || !start_reading(f))
        return 0;

    char *out = dest;
    size_t left = total;

    while (left > 0) {
        size_t ready = f->len - f->pos;

        if (ready > 0) {
            size_t take = ready < left ? ready : left;

            memcpy(out, f->buffer + f->pos, take);
            f->pos += take;
            out += take;
            left -= take;
        } else if (!fill(f)) {
            break;
        }
    }
    return (total - left) / size;
}

size_t fwrite(const void *src, size_t size, size_t count, FILE *f) {
    LOCKED(&f->lock);
    size_t total;

    if (__builtin_mul_overflow(size, count, &total) || total == 0 || !start_writing(f))
        return 0;
    if (f->mode == _IONBF)
        return write_all(f, src, total) ? count : 0;
    if (!ensure_buffer(f))
        return 0;

    // Whatever doesn't fit in the buffer goes straight out.
    if (total > f->size - f->pos) {
        if (flush_stream(f) < 0)
            return 0;
        if (total >= f->size)
            return write_all(f, src, total) ? count : 0;
    }

    memcpy(f->buffer + f->pos, src, total);
    f->pos += total;
    if (f->mode == _IOLBF && memchr(src, '\n', total) && flush_stream(f) < 0)
        return 0;
    return count;
}

int fgetc(FILE *f) {
    LOCKED(&f->lock);
    if (!start_reading(f) || (f->pos == f->len && !fill(f)))
        return EOF;
    return (unsigned char)f->buffer[f->pos++];
}

int getc(FILE *f) { return fgetc(f); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int c, FILE *f) {
    LOCKED(&f->lock);
    if (c == EOF || !start_reading(f) || !ensure_buffer(f))
        return EOF;

    if (f->pos == 0) {
        if (f->len == f->size)
            return EOF;
        memmove(f->buffer + 1, f->buffer, f->len);
        f->len++;
        f->pos = 1;
    }
    f->buffer[--f->pos] = c;
    f->eof = false;
    return (unsigned char)c;
}

char *fgets(char *dest, int size, FILE *f) {
    LOCKED(&f->lock);
    int used = 0;

    while (used < size - 1) {
        int c = fgetc(f);

        if (c == EOF)
            break;
        dest[used++] = c;
        if (c == '\n')
            break;
    }
    if (used == 0 || size <= 0)
        return NULL;
    dest[used] = '\0';
    return dest;
}

ssize_t getdelim(char **line, size_t *size, int delimiter, FILE *f) {
    LOCKED(&f->lock);
    size_t used = 0;

    for (;;) {
        int c = fgetc(f);

        if (c == EOF && used == 0)
            return -1;

        if (used + 2 > *size || !*line) {
            size_t bigger = *size && *line ? *size * 2 : 128;
            char *grown = realloc(*line, bigger);

            if (!grown) {
                errno = ENOMEM;
                return -1;
            }
            *line = grown;
            *size = bigger;
        }

        if (c == EOF)
            break;
        (*line)[used++] = c;
        if (c == delimiter)
            break;
    }
    (*line)[used] = '\0';
    return used;
}

ssize_t getline(char **line, size_t *size, FILE *f) {
    return getdelim(line, size, '\n', f);
}

int fputc(int c, FILE *f) {
    LOCKED(&f->lock);
    unsigned char byte = c;

    return fwrite(&byte, 1, 1, f) == 1 ? byte : EOF;
}

int putc(int c, FILE *f) { return fputc(c, f); }
int putchar(int c) { return fputc(c, stdout); }

int fputs(const char *s, FILE *f) {
    LOCKED(&f->lock);
    size_t len = strlen(s);

    return len == 0 || fwrite(s, 1, len, f) == len ? 0 : EOF;
}

int puts(const char *s) {
    return printf("%s\n", s) < 0 ? EOF : 0;
}

int fseek(FILE *f, long offset, int whence) {
    LOCKED(&f->lock);
    if (flush_stream(f) < 0)
        return -1;

    // Unread data that flushing couldn't undo is what a relative seek starts from.
    if (whence == SEEK_CUR && !f->writing)
        offset -= f->len - f->pos;
    if (lseek(f->fd, offset, whence) < 0)
        return -1;
    f->pos = f->len = 0;
    f->eof = false;
    return 0;
}

long ftell(FILE *f) {
    LOCKED(&f->lock);
    if (f->writing && flush_stream(f) < 0)
        return -1;

    long at = lseek(f->fd, 0, SEEK_CUR);

    return at < 0 ? -1 : at - (long)(f->writing ? 0 : f->len - f->pos);
}

void rewind(FILE *f) {
    fseek(f, 0, SEEK_SET);
    f->error = false;
}

int remove(const char *path) {
    if (unlink(path) == 0)
        return 0;
    return errno == EISDIR ? rmdir(path) : -1;
}

void perror(const char *s) {
    if (s && *s)
        dprintf(STDERR_FILENO, "%s: %s\n", s, strerror(errno));
    else
        dprintf(STDERR_FILENO, "%s\n", strerror(errno));
}

void setlinebuf(FILE *f) {
    setvbuf(f, NULL, _IOLBF, 0);
}

int fseeko(FILE *f, off_t offset, int whence) {
    return fseek(f, offset, whence);
}

off_t ftello(FILE *f) {
    return ftell(f);
}

int fgetpos(FILE *f, fpos_t *out) {
    long position = ftell(f);

    if (position < 0)
        return -1;
    *out = position;
    return 0;
}

int fsetpos(FILE *f, const fpos_t *position) {
    return fseek(f, *position, SEEK_SET);
}

int getc_unlocked(FILE *f) {
    return fgetc(f);
}

int getchar_unlocked(void) {
    return fgetc(stdin);
}

int putc_unlocked(int c, FILE *f) {
    return fputc(c, f);
}

int putchar_unlocked(int c) {
    return fputc(c, stdout);
}

char *tmpnam(char *out) {
    static char shared[L_tmpnam];
    char *name = out ? out : shared;

    for (int attempt = 0; attempt < TMP_MAX; attempt++) {
        struct stat st;
        int fd;

        strcpy(name, P_tmpdir "/tmpXXXXXX");
        fd = mkstemp(name);
        if (fd >= 0) {
            close(fd);
            unlink(name);
            return stat(name, &st) < 0 ? name : NULL;
        }
        if (errno != EEXIST)
            return NULL;
    }
    return NULL;
}

// An open file can't be deleted here, so the name is kept until it's closed.
FILE *tmpfile(void) {
    char path[] = P_tmpdir "/tmpXXXXXX";
    int fd = mkstemp(path);

    if (fd < 0)
        return NULL;

    FILE *f = fdopen(fd, "w+");

    if (f)
        f->temporary_path = strdup(path);
    if (!f || !f->temporary_path) {
        if (f)
            fclose(f);
        else
            close(fd);
        unlink(path);
        return NULL;
    }
    return f;
}

void __remove_temporaries(void) {
    for (FILE *f = streams; f; f = f->next) {
        if (f->temporary_path) {
            close(f->fd);
            unlink(f->temporary_path);
        }
    }
}

FILE *popen(const char *command, const char *mode) {
    bool reading = mode[0] == 'r';
    int fds[2];

    if ((mode[0] != 'r' && mode[0] != 'w') || pipe2(fds, O_CLOEXEC) < 0) {
        if (mode[0] != 'r' && mode[0] != 'w')
            errno = EINVAL;
        return NULL;
    }

    pid_t pid = fork();

    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        dup2(fds[reading ? 1 : 0], reading ? STDOUT_FILENO : STDIN_FILENO);
        execl("/system/bin/dash", "dash", "-c", command, (char *)NULL);
        _Exit(127);
    }

    close(fds[reading ? 1 : 0]);

    FILE *f = fdopen(fds[reading ? 0 : 1], reading ? "r" : "w");

    if (f)
        f->pid = pid;
    return f;
}

int pclose(FILE *f) {
    pid_t pid = f->pid;
    int status;

    fclose(f);
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

void flockfile(FILE *f) {
    __lock_take(&f->lock);
}

void funlockfile(FILE *f) {
    __lock_give(&f->lock);
}

int ftrylockfile(FILE *f) {
    return __lock_try(&f->lock) ? 0 : -1;
}

// Nothing can be mid-operation in the child on a list only fork's caller could hold.
void __stdio_fork_prepare(void) {
    __lock_take(&streams_lock);
    for (FILE *f = streams; f; f = f->next)
        __lock_take(&f->lock);
}

void __stdio_fork_parent(void) {
    for (FILE *f = streams; f; f = f->next)
        __lock_give(&f->lock);
    __lock_give(&streams_lock);
}

// The caller's id has changed, and the locks now belong to it.
void __stdio_fork_child(void) {
    int self = __current_thread()->id;

    streams_lock.owner = self;
    for (FILE *f = streams; f; f = f->next)
        f->lock.owner = self;
    __stdio_fork_parent();
}
