#include <aximos.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>

static uint64_t statistic(const char *card, const char *name) {
    char path[300];
    unsigned long long n = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/sys/class/net/%s/statistics/%s", card, name);
    if ((f = fopen(path, "r"))) {
        if (fscanf(f, "%llu", &n) != 1)
            n = 0;
        fclose(f);
    }
    return n;
}

int net_info(struct net_info *out) {
    struct ifaddrs *list;

    *out = (struct net_info){0};
    if (getifaddrs(&list) < 0)
        return -1;
    for (struct ifaddrs *i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || i->ifa_flags & IFF_LOOPBACK || !(i->ifa_flags & IFF_UP))
            continue;
        out->has_device = out->automatic = true;
        out->address = ntohl(((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr);
        out->netmask = i->ifa_netmask ? ntohl(((struct sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr) : 0;
        out->rx_bytes = statistic(i->ifa_name, "rx_bytes");
        out->tx_bytes = statistic(i->ifa_name, "tx_bytes");
        break;
    }
    freeifaddrs(list);
    return 0;
}

// Until the network's AximOS's own, in phase 5, there's nothing to switch off.
int net_airplane(bool on) {
    (void)on;
    return 0;
}
