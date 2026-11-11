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

struct gui_window *gui_create_window(const char *title, int width, int height) {
    static int created;
    struct gui_message message = {.type = GUI_CREATE, .width = width, .height = height};
    size_t size = (size_t)width * height * sizeof(uint32_t);
    struct gui_window *window = malloc(sizeof(*window));

    if (!window || width <= 0 || height <= 0) {
        free(window);
        return NULL;
    }
    snprintf(message.text, sizeof(message.text), "%s", title);
    snprintf(message.buffer, sizeof(message.buffer), "/gui-%d-%d", getpid(), created++);

    // wm maps the memory and removes its name.
    int fd = shm_open(message.buffer, O_RDWR | O_CREAT | O_EXCL, 0600);
    void *pixels = fd < 0 || ftruncate(fd, size) < 0 ? MAP_FAILED
                                                     : mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

    if (fd >= 0)
        close(fd);
    if (pixels == MAP_FAILED || !gui_send(connection, &message)) {
        shm_unlink(message.buffer);
        free(window);
        return NULL;
    }

    window->canvas = (struct gui_canvas){.pixels = pixels, .width = width, .height = height, .stride = width};
    while (gui_receive(connection, &message)) {
        if (message.type == GUI_CREATED) {
            window->id = message.window;
            return window;
        }
        if (queue_count < QUEUE_SIZE)
            queue[(queue_head + queue_count++) % QUEUE_SIZE] = message;
    }
    munmap(pixels, size);
    free(window);
    return NULL;
}

void gui_destroy_window(struct gui_window *window) {
    struct gui_message message = {.type = GUI_DESTROY, .window = window->id};

    gui_send(connection, &message);
    munmap(window->canvas.pixels, (size_t)window->canvas.width * window->canvas.height * sizeof(uint32_t));
    free(window);
}

void gui_update(struct gui_window *window, int x, int y, int width, int height) {
    struct gui_message message = {
        .type = GUI_DAMAGE, .window = window->id, .x = x, .y = y, .width = width, .height = height,
    };

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
