#include <aximos.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_address(const char *label, uint32_t address) {
    struct in_addr in = {htonl(address)};

    printf("%-10s %s\n", label, address ? inet_ntoa(in) : "none");
}

static bool parse_address(const char *text, uint32_t *out) {
    struct in_addr in;

    if (!inet_aton(text, &in))
        return false;
    *out = ntohl(in.s_addr);
    return true;
}

static int show(void) {
    struct net_info info;

    if (net_info(&info) < 0) {
        perror("net");
        return 1;
    }
    if (info.airplane)
        printf("airplane mode, so nothing's sent or taken\n");
    if (!info.has_device)
        printf("no network card, only this machine can be reached\n");
    else
        printf("%-10s %02x:%02x:%02x:%02x:%02x:%02x\n", "card", info.mac[0], info.mac[1], info.mac[2],
               info.mac[3], info.mac[4], info.mac[5]);

    print_address("address", info.address);
    print_address("netmask", info.netmask);
    print_address("gateway", info.gateway);
    print_address("dns", info.dns);
    printf("%-10s %s\n", "set by", info.automatic ? "DHCP" : "hand");
    printf("%-10s %lu packets, ", "received", info.rx_packets);
    print_size(info.rx_bytes, 0);
    printf("\n%-10s %lu packets, ", "sent", info.tx_packets);
    print_size(info.tx_bytes, 0);
    printf("\n");
    return 0;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: net [dhcp | set address netmask [gateway [dns]]]\n"
                            "Shows the network's configuration, or changes it.\n"
                            "  dhcp  ask the network for one\n"
                            "  set   use the one given\n");

    if (argc == 1)
        return show();
    if (argc == 2 && strcmp(argv[1], "dhcp") == 0)
        return net_set(0, 0, 0, 0) < 0;

    uint32_t address, netmask, gateway = 0, dns = 0;

    if (argc < 4 || argc > 6 || strcmp(argv[1], "set") != 0 || !parse_address(argv[2], &address)
        || !parse_address(argv[3], &netmask) || (argc > 4 && !parse_address(argv[4], &gateway))
        || (argc > 5 && !parse_address(argv[5], &dns))) {
        dprintf(STDERR_FILENO, "usage: net [dhcp | set address netmask [gateway [dns]]]\n");
        return 1;
    }
    return net_set(address, netmask, gateway, dns) < 0;
}
