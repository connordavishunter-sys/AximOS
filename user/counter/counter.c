#include <aximos.h>
#include <stdio.h>
#include <unistd.h>

// Never yields, so taking turns with other copies of it requires preemption.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: counter [name]\n"
                            "Counts to 5 slowly, printing each number with the name. Tests that programs take turns.\n");

    const char *name = argc > 1 ? argv[1] : "?";

    for (int i = 1; i <= 5; i++) {
        for (volatile long spin = 0; spin < 20000000; spin++)
            ;
        printf("counter %s: %d\n", name, i);
    }
    return 0;
}
