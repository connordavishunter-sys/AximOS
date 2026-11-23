#include <aximos.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

#define MOTD_PATH "/system/etc/motd"

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: hello [word...]\n"
                            "Prints its process ID and arguments, then the message of the day.\n");

    printf("hello: pid %d with %d arguments:", getpid(), argc);
    for (int i = 0; i < argc; i++)
        printf(" %s", argv[i]);
    printf("\n");

    int fd = open(MOTD_PATH, O_RDONLY);
    if (fd < 0) {
        printf("hello: can't open %s\n", MOTD_PATH);
        return 1;
    }

    char buffer[128];
    ssize_t count;
    while ((count = read(fd, buffer, sizeof(buffer))) > 0)
        write(STDOUT_FILENO, buffer, count);

    close(fd);
    return count < 0;
}
