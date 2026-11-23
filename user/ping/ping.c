#include <aximos.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define ICMP_ECHO_REQUEST 8
#define ICMP_ECHO_REPLY 0
#define PAYLOAD_SIZE 56
#define MESSAGE_SIZE (8 + PAYLOAD_SIZE)
#define WAIT_MS 1000

static int sent, received;
static volatile sig_atomic_t stop;

static void on_interrupt(int signal) {
    (void)signal;
    stop = 1;
}

static long now_us(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000 + t.tv_nsec / 1000;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: ping [-c count] host\n"
                            "Asks a machine to answer, and shows how long it took.\n"
                            "  -c  stop after this many, instead of at Ctrl+C\n");

    int count = 0;
    int option;

    while ((option = getopt(argc, argv, "c:")) != -1) {
        if (option != 'c') {
            dprintf(STDERR_FILENO, "usage: ping [-c count] host\n");
            return 2;
        }
        count = atoi(optarg);
    }
    if (optind != argc - 1) {
        dprintf(STDERR_FILENO, "usage: ping [-c count] host\n");
        return 2;
    }

    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *found;
    int error = getaddrinfo(argv[optind], NULL, &hints, &found);

    if (error) {
        dprintf(STDERR_FILENO, "ping: %s: %s\n", argv[optind], gai_strerror(error));
        return 2;
    }

    struct sockaddr_in target = *(struct sockaddr_in *)found->ai_addr;

    freeaddrinfo(found);

    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_ICMP);

    if (fd < 0) {
        perror("ping");
        return 2;
    }

    signal(SIGINT, on_interrupt);
    printf("PING %s (%s)\n", argv[optind], inet_ntoa(target.sin_addr));

    for (int sequence = 1; !stop && (count == 0 || sequence <= count); sequence++) {
        uint8_t message[MESSAGE_SIZE] = {ICMP_ECHO_REQUEST};
        long started = now_us();

        message[6] = sequence >> 8;
        message[7] = sequence;
        if (sendto(fd, message, sizeof(message), 0, (struct sockaddr *)&target, sizeof(target)) < 0) {
            perror("ping");
            return 2;
        }
        sent++;

        long deadline = started + WAIT_MS * 1000;

        // The rest of the second is waited out, so answers come one a second.
        while (!stop) {
            long left = deadline - now_us();

            if (left <= 0)
                break;

            struct pollfd wait = {.fd = fd, .events = POLLIN};

            if (poll(&wait, 1, left / 1000 + 1) <= 0)
                continue;

            struct sockaddr_in from;
            socklen_t from_size = sizeof(from);
            uint8_t reply[MESSAGE_SIZE];
            ssize_t got = recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from, &from_size);

            if (got < 8 || reply[0] != ICMP_ECHO_REPLY || (reply[6] << 8 | reply[7]) != sequence)
                continue;

            long took = now_us() - started;

            received++;
            printf("%zd bytes from %s: sequence %d time %ld.%02ld ms\n", got, inet_ntoa(from.sin_addr), sequence, took / 1000, took / 10 % 100);
            if (count && sequence == count)
                break;
        }
    }

    printf("%d sent, %d received\n", sent, received);
    return received ? 0 : 1;
}
