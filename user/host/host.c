#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: host name...\n"
                            "Looks up the addresses of names.\n");

    if (argc < 2) {
        dprintf(STDERR_FILENO, "usage: host name...\n");
        return 1;
    }

    int status = 0;

    for (int i = 1; i < argc; i++) {
        struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
        struct addrinfo *found;
        int error = getaddrinfo(argv[i], NULL, &hints, &found);

        if (error) {
            dprintf(STDERR_FILENO, "host: %s: %s\n", argv[i], gai_strerror(error));
            status = 1;
            continue;
        }
        for (struct addrinfo *a = found; a; a = a->ai_next)
            printf("%s has address %s\n", argv[i], inet_ntoa(((struct sockaddr_in *)a->ai_addr)->sin_addr));
        freeaddrinfo(found);
    }
    return status;
}
