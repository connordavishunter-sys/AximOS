#include <errno.h>
#include <fcntl.h>
#include <gui.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define QUEUE_SIZE 64

static int connection = -1;

// Events that came while waiting for an answer, kept for gui_next_event.
static struct gui_message queue[QUEUE_SIZE];
static int queue_head, queue_count;

static bool transfer(int fd, void *data, bool sending) {
    for (size_t done = 0; done < sizeof(struct gui_message);) {
        ssize_t n = sending ? write(fd, (char *)data + done, sizeof(struct gui_message) - done)
                            : read(fd, (char *)data + done, sizeof(struct gui_message) - done);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += n;
    }
    return true;
}

bool gui_receive(int fd, struct gui_message *message) {
    return transfer(fd, message, false);
}

bool gui_send(int fd, const struct gui_message *message) {
    return transfer(fd, (void *)message, true);
}

bool gui_connect(void) {
    struct sockaddr_un address = {.sun_family = AF_UNIX, .sun_path = GUI_SOCKET};

    connection = socket(AF_UNIX, SOCK_STREAM, 0);
    if (connection >= 0 && connect(connection, (struct sockaddr *)&address, sizeof(address)) == 0)
        return true;
    close(connection);
    connection = -1;
    return false;
}

int gui_fd(void) {
    return connection;
}

// Shared memory for pixels, which wm maps and then removes the name of.
static uint32_t *new_buffer(int width, int height, char name[GUI_TITLE_MAX]) {
    static int created;
    size_t size = (size_t)width * height * sizeof(uint32_t);

    if (width <= 0 || height <= 0)
        return NULL;
    snprintf(name, GUI_TITLE_MAX, "/gui-%d-%d", getpid(), created++);

    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    void *pixels = fd < 0 || ftruncate(fd, size) < 0 ? MAP_FAILED
                                                     : mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (fd >= 0)
        close(fd);
    if (pixels == MAP_FAILED) {
        shm_unlink(name);
        return NULL;
    }
    return pixels;
}

static size_t canvas_size(const struct gui_canvas *c) {
    return (size_t)c->width * c->height * sizeof(uint32_t);
}

struct gui_window *gui_create_window(const char *title, int width, int height) {
    struct gui_message message = {.type = GUI_CREATE, .width = width, .height = height};
    struct gui_window *window = calloc(1, sizeof(*window));
    uint32_t *pixels = window ? new_buffer(width, height, message.buffer) : NULL;

    snprintf(message.text, sizeof(message.text), "%s", title);
    if (!pixels || !gui_send(connection, &message)) {
        if (pixels) {
            shm_unlink(message.buffer);
            munmap(pixels, (size_t)width * height * sizeof(uint32_t));
        }
        free(window);
        return NULL;
    }

    window->canvas = (struct gui_canvas){.pixels = pixels, .width = width, .height = height, .stride = width};
    while (gui_receive(connection, &message)) {
        if (message.type == GUI_CREATED && message.window) {
            window->id = message.window;
            return window;
        }
        if (message.type == GUI_CREATED)
            break;
        if (queue_count < QUEUE_SIZE)
            queue[(queue_head + queue_count++) % QUEUE_SIZE] = message;
    }
    munmap(pixels, canvas_size(&window->canvas));
    free(window);
    return NULL;
}

bool gui_resize(struct gui_window *window, int width, int height) {
    char name[GUI_TITLE_MAX];
    uint32_t *pixels = new_buffer(width, height, name);

    if (!pixels)
        return false;

    // One that wm never saw can go at once.
    if (window->pending[0]) {
        shm_unlink(window->pending);
        munmap(window->canvas.pixels, canvas_size(&window->canvas));
    } else {
        window->old_pixels = window->canvas.pixels;
        window->old_size = canvas_size(&window->canvas);
    }
    memcpy(window->pending, name, sizeof(name));
    window->canvas = (struct gui_canvas){.pixels = pixels, .width = width, .height = height, .stride = width};
    return true;
}

void gui_destroy_window(struct gui_window *window) {
    struct gui_message message = {.type = GUI_DESTROY, .window = window->id};

    gui_send(connection, &message);
    if (window->pending[0]) {
        shm_unlink(window->pending);
        munmap(window->old_pixels, window->old_size);
    }
    munmap(window->canvas.pixels, canvas_size(&window->canvas));
    free(window);
}

void gui_update(struct gui_window *window, int x, int y, int width, int height) {
    struct gui_message message = {
        .type = GUI_DAMAGE, .window = window->id, .x = x, .y = y, .width = width, .height = height,
    };

    if (window->pending[0]) {
        struct gui_message buffer = {
            .type = GUI_BUFFER, .window = window->id, .width = window->canvas.width, .height = window->canvas.height,
        };

        memcpy(buffer.buffer, window->pending, sizeof(buffer.buffer));
        gui_send(connection, &buffer);
        munmap(window->old_pixels, window->old_size);
        window->pending[0] = '\0';
    }
    gui_send(connection, &message);
}

void gui_show(struct gui_window *window) {
    gui_update(window, 0, 0, window->canvas.width, window->canvas.height);
}

bool gui_next_event(struct gui_message *event) {
    if (queue_count > 0) {
        *event = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_SIZE;
        queue_count--;
        return true;
    }
    return gui_receive(connection, event);
}
