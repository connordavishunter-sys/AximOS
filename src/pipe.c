#include <errno_values.h>
#include <file.h>
#include <kmalloc.h>
#include <mm.h>
#include <pipe.h>
#include <process.h>
#include <signal_numbers.h>

#define PIPE_SIZE PAGE_SIZE
#define MAX_PASSED_FILES 16

struct pipe {
    uint64_t buffer; // a physical page
    size_t head;
    size_t count;
    bool reader_open;
    bool writer_open;
    struct file *files[MAX_PASSED_FILES];
    int file_count;
};

struct pipe *pipe_create(void) {
    struct pipe *pipe = kmalloc(sizeof(*pipe));
    uint64_t buffer = mm_alloc_kernel_page();

    if (!pipe || !buffer) {
        kfree(pipe);
        if (buffer)
            mm_free_kernel_page(buffer);
        return NULL;
    }
    *pipe = (struct pipe){.buffer = buffer, .reader_open = true, .writer_open = true};
    return pipe;
}

static void closed_an_end(struct pipe *pipe) {
    process_wakeup(pipe);
    if (pipe->reader_open || pipe->writer_open)
        return;

    while (pipe->file_count > 0)
        file_close(pipe_take_file(pipe));
    mm_free_kernel_page(pipe->buffer);
    kfree(pipe);
}

void pipe_close_reader(struct pipe *pipe) {
    pipe->reader_open = false;
    closed_an_end(pipe);
}

void pipe_close_writer(struct pipe *pipe) {
    pipe->writer_open = false;
    closed_an_end(pipe);
}

size_t pipe_peek(const struct pipe *pipe, uint8_t *buf, size_t len) {
    const uint8_t *data = mm_phys_to_virt(pipe->buffer);
    size_t count = len < pipe->count ? len : pipe->count;

    for (size_t i = 0; i < count; i++)
        buf[i] = data[(pipe->head + i) % PIPE_SIZE];
    return count;
}

int64_t pipe_read(struct pipe *pipe, uint8_t *buf, size_t len, bool nonblocking) {
    if (len == 0)
        return 0;

    while (pipe->count == 0) {
        if (!pipe->writer_open)
            return 0;
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep(pipe))
            return -EINTR;
    }

    size_t count = pipe_peek(pipe, buf, len);

    pipe->head = (pipe->head + count) % PIPE_SIZE;
    pipe->count -= count;
    process_wakeup(pipe);
    return count;
}

int64_t pipe_write(struct pipe *pipe, const uint8_t *buf, size_t len, bool nonblocking, bool raise_sigpipe) {
    uint8_t *data = mm_phys_to_virt(pipe->buffer);
    size_t written = 0;

    while (written < len) {
        if (!pipe->reader_open) {
            if (raise_sigpipe)
                process_signal_self(SIGPIPE);
            return written ? (int64_t)written : -EPIPE;
        }
        if (pipe->count == PIPE_SIZE) {
            if (nonblocking)
                return written ? (int64_t)written : -EAGAIN;
            if (!process_sleep(pipe))
                return written ? (int64_t)written : -EINTR;
            continue;
        }

        data[(pipe->head + pipe->count) % PIPE_SIZE] = buf[written++];
        pipe->count++;

        if (pipe->count == PIPE_SIZE || written == len)
            process_wakeup(pipe);
    }
    return written;
}

size_t pipe_available(const struct pipe *pipe) {
    return pipe->count;
}

bool pipe_has_room(const struct pipe *pipe) {
    return pipe->count < PIPE_SIZE;
}

bool pipe_has_reader(const struct pipe *pipe) {
    return pipe->reader_open;
}

bool pipe_has_writer(const struct pipe *pipe) {
    return pipe->writer_open;
}

bool pipe_add_file(struct pipe *pipe, struct file *file) {
    if (pipe->file_count == MAX_PASSED_FILES)
        return false;
    pipe->files[pipe->file_count++] = file;
    return true;
}

struct file *pipe_take_file(struct pipe *pipe) {
    if (pipe->file_count == 0)
        return NULL;

    struct file *file = pipe->files[0];

    pipe->file_count--;
    for (int i = 0; i < pipe->file_count; i++)
        pipe->files[i] = pipe->files[i + 1];
    return file;
}
