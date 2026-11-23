#include <aximos.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: hostname [name]\n"
                            "Shows the computer's name, or changes it until it restarts.\n");

    char name[256];

    if (argc > 1) {
        if (sethostname(argv[1], strlen(argv[1])) < 0) {
            perror("hostname");
            return 1;
        }
        return 0;
    }
    gethostname(name, sizeof(name));
    printf("%s\n", name);
    return 0;
}
