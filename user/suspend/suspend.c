#include <aximos.h>
#include <stdio.h>
#include <unistd.h>

// Suspends to idle, s2idle, which laptops wake from by their lid, power button or
// keyboard. It's installed setuid root, as reboot is, so anyone at the computer can.

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: suspend\n"
                            "Suspends the computer, until its lid opens or a key's pressed.\n");

    FILE *f;

    sync();
    if (!(f = fopen("/sys/power/state", "w")) || fputs("freeze", f) < 0 || fclose(f) != 0) {
        perror("suspend");
        return 1;
    }
    return 0;
}
