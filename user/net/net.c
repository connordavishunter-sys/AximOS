#include <aximos.h>
#include <stdio.h>
#include <string.h>

// Shows the network, as netd has it, or has netd change a card's.

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: net\n"
                            "       net dhcp CARD\n"
                            "       net set CARD ADDRESS/PREFIX [GATEWAY [DNS]]\n"
                            "Shows each network card, its addresses and the DNS servers, or changes a\n"
                            "card's: to get its address by DHCP, or to one set by hand, kept across restarts.\n");

    char line[512] = "", reply[4096];

    for (int i = 1; i < argc; i++)
        snprintf(line + strlen(line), sizeof(line) - strlen(line), "%s%s", i > 1 ? " " : "", argv[i]);
    if ((argc != 1 && !(argc == 3 && !strcmp(argv[1], "dhcp")) && !(argc >= 4 && argc <= 6 && !strcmp(argv[1], "set")))) {
        fprintf(stderr, "net: try net --help\n");
        return 1;
    }

    int failed = net_ask(argc == 1 ? "status" : line, reply, sizeof(reply));

    if (failed) {
        fprintf(stderr, "net: %s", reply + (strncmp(reply, "error ", 6) ? 0 : 6));
        return 1;
    }
    if (argc > 1)
        return 0;

    // "card enp0s2 wired up dhcp" and so on, shown as a card's heading with its addresses under it
    for (char *at = strtok(reply, "\n"); at; at = strtok(NULL, "\n")) {
        char a[32], b[64], c[16], d[16];

        if (sscanf(at, "card %31s %15s %15s %15s", a, b, c, d) == 4)
            printf("%s, %s, %s%s\n", a, b, !strcmp(c, "up") ? "connected" : "not connected",
                   !strcmp(c, "up") ? (!strcmp(d, "hand") ? ", set by hand" : ", by DHCP") : "");
        else if (sscanf(at, "address %31s %63s", a, b) == 2)
            printf("  address  %s\n", b);
        else if (sscanf(at, "gateway %31s %63s", a, b) == 2)
            printf("  gateway  %s\n", b);
        else if (sscanf(at, "dns %63s", b) == 1)
            printf("dns        %s\n", b);
    }
    return 0;
}
