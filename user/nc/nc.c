#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 4096

static const char usage[] = "usage: nc [-u] host port, or nc -l [-u] port\n";

static int fail(const char *what) {
    dprintf(STDERR_FILENO, "nc: %s: %s\n", what, strerror(errno));
    return 1;
}

static bool write_all(int fd, const char *data, ssize_t len) {
    while (len > 0) {
        ssize_t wrote = write(fd, data, len);

        if (wrote <= 0)
            return false;
        data += wrote;
        len -= wrote;
    }
    return true;
}

// Passes data both ways until the other side finishes.
static int relay(int fd, bool datagrams) {
    static char buffer[BUFFER_SIZE];
    struct pollfd watch[2] = {{.fd = fd, .events = POLLIN}, {.fd = STDIN_FILENO, .events = POLLIN}};
    bool input_open = true;

    for (;;) {
        watch[1].fd = input_open ? STDIN_FILENO : -1;
        if (poll(watch, 2, -1) < 0)
            return errno == EINTR ? 0 : fail("poll");

        if (watch[0].revents) {
            ssize_t got = read(fd, buffer, sizeof(buffer));

            if (got < 0)
                return fail("read");
            if (got == 0)
                return 0;
            if (!write_all(STDOUT_FILENO, buffer, got))
                return fail("write");
        }
        if (watch[1].revents) {
            ssize_t got = read(STDIN_FILENO, buffer, sizeof(buffer));

            if (got <= 0) {
                input_open = false;
                if (!datagrams)
                    shutdown(fd, SHUT_WR);
            } else if (!write_all(fd, buffer, got)) {
                return fail("send");
            }
        }
    }
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: nc [-u] host port, or nc -l [-u] port\n"
                            "Joins standard input and output to a network connection.\n"
                            "  -l  wait for one to arrive instead of making one\n"
                            "  -u  use UDP rather than TCP\n");

    bool listening = false;
    int type = SOCK_STREAM;
    int option;

    while ((option = getopt(argc, argv, "lu")) != -1) {
        if (option == 'l') {
            listening = true;
        } else if (option == 'u') {
            type = SOCK_DGRAM;
        } else {
            dprintf(STDERR_FILENO, "%s", usage);
            return 2;
        }
    }
    if (optind != argc - (listening ? 1 : 2)) {
        dprintf(STDERR_FILENO, "%s", usage);
        return 2;
    }

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = type, .ai_flags = listening ? AI_PASSIVE : 0};
    struct addrinfo *found;
    int error = getaddrinfo(listening ? NULL : argv[optind], argv[argc - 1], &hints, &found);

    if (error) {
        dprintf(STDERR_FILENO, "nc: %s\n", gai_strerror(error));
        return 1;
    }

    int fd = socket(AF_INET, type, 0);

    if (fd < 0)
        return fail("socket");

    if (!listening) {
        if (connect(fd, found->ai_addr, found->ai_addrlen) < 0)
            return fail("connect");
        return relay(fd, type == SOCK_DGRAM);
    }

    if (bind(fd, found->ai_addr, found->ai_addrlen) < 0)
        return fail("bind");
    if (type == SOCK_DGRAM) {
        // The first to send is who to answer.
        static char first[BUFFER_SIZE];
        struct sockaddr_in from;
        socklen_t from_size = sizeof(from);
        ssize_t got = recvfrom(fd, first, sizeof(first), 0, (struct sockaddr *)&from, &from_size);

        if (got < 0 || connect(fd, (struct sockaddr *)&from, from_size) < 0)
            return fail("receive");
        write_all(STDOUT_FILENO, first, got);
        return relay(fd, true);
    }

    if (listen(fd, 1) < 0)
        return fail("listen");

    int connection = accept(fd, NULL, NULL);

    if (connection < 0)
        return fail("accept");
    close(fd);
    return relay(connection, false);
}
