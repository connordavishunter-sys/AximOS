#include <aximos.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: sync\n"
                            "Writes everything waiting in memory out to the disks.\n");
    sync();
    return 0;
}
