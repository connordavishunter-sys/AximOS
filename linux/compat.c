#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

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
        break;
    }
    freeifaddrs(list);
    return 0;
}

int net_airplane(bool on) {
    (void)on;
    return 0;
}
