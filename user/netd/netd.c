#include <aximos.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// The network: brings up each card as it appears, and for each with a cable or a
// network, gets an address by DHCP, or uses the one set by hand in CONF, and for
// IPv6, has Linux take addresses from the router while it asks DHCPv6 for one too.
// The DNS servers they give go in RESOLV. Wired cards' routes are preferred to Wi-Fi's.
//
// Programs ask it on SOCKET, a line a connection: "status" gives the cards and their
// addresses, a line each, then "end"; "dhcp CARD" and "set CARD ADDRESS/PREFIX
// [GATEWAY [DNS]]" change a card, for anyone with an account, as do wifi.c's "join",
// "forget" and "airplane", while "networks" and "scan" are anyone's. CONF's
// "CARD ignore" leaves a card to another program.

#include "netd.h"

// struct in6_ifreq, as Linux's headers have it, which glibc's can't be included with
struct in6_request {
    struct in6_addr address;
    uint32_t prefix;
    int index;
};

struct card cards[MAX_CARDS];
int card_count;
static bool unsettled; // a card udev hasn't finished with, such as naming it, which scan looks at again
static int inet = -1, inet6 = -1;

int64_t now_ms(void) {
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static void show4(uint32_t address, char out[INET_ADDRSTRLEN]) {
    struct in_addr in = {htonl(address)};

    inet_ntop(AF_INET, &in, out, INET_ADDRSTRLEN);
}

static int prefix_of(uint32_t netmask) {
    return __builtin_popcount(netmask);
}

static uint32_t netmask_of(int prefix) {
    return prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
}

static void random_bytes(void *out, size_t size) {
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);

    if (fd < 0 || read(fd, out, size) != (ssize_t)size)
        memset(out, (int)now_ms(), size);
    if (fd >= 0)
        close(fd);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

// The DNS servers of every card, wired first, as cards are listed.
static void write_resolv(void) {
    FILE *f = fopen(RESOLV ".new", "w");
    char text[INET6_ADDRSTRLEN];

    if (!f)
        return;
    fputs("# Written by netd, from what the network gives\n", f);
    for (int wifi = 0; wifi < 2; wifi++) {
        for (int i = 0; i < card_count; i++) {
            struct card *c = &cards[i];

            if (c->wifi != wifi || !c->carrier)
                continue;
            if (c->hand && c->hand_dns) {
                show4(c->hand_dns, text);
                fprintf(f, "nameserver %s\n", text);
            }
            for (int d = 0; !c->hand && d < MAX_DNS && c->dns[d]; d++) {
                show4(c->dns[d], text);
                fprintf(f, "nameserver %s\n", text);
            }
            for (int d = 0; d < MAX_DNS && !IN6_IS_ADDR_UNSPECIFIED(&c->v6_dns[d]); d++)
                fprintf(f, "nameserver %s\n", inet_ntop(AF_INET6, &c->v6_dns[d], text, sizeof(text)));
        }
    }
    fclose(f);
    rename(RESOLV ".new", RESOLV);
}

static struct ifreq request_for(const struct card *c) {
    struct ifreq r = {0};

    snprintf(r.ifr_name, sizeof(r.ifr_name), "%s", c->name);
    return r;
}

static void set_ipv4(int code, const struct card *c, uint32_t address) {
    struct ifreq r = request_for(c);
    struct sockaddr_in *in = (struct sockaddr_in *)&r.ifr_addr;

    in->sin_family = AF_INET;
    in->sin_addr.s_addr = htonl(address);
    if (ioctl(inet, code, &r) < 0 && address)
        fprintf(stderr, "netd: can't set %s's address: %s\n", c->name, strerror(errno));
}

// An address, its subnet, and the default route through the gateway, or with 0,
// none of them.
static void apply4(struct card *c, uint32_t address, uint32_t netmask, uint32_t gateway) {
    char text[INET_ADDRSTRLEN];

    set_ipv4(SIOCSIFADDR, c, 0); // which takes its routes too
    if (!address)
        return;
    set_ipv4(SIOCSIFADDR, c, address);
    set_ipv4(SIOCSIFNETMASK, c, netmask);
    set_ipv4(SIOCSIFBRDADDR, c, address | ~netmask);
    if (gateway) {
        struct rtentry route = {.rt_flags = RTF_UP | RTF_GATEWAY, .rt_metric = (c->wifi ? WIFI_METRIC : WIRED_METRIC) + 1,
                                .rt_dev = c->name};

        ((struct sockaddr_in *)&route.rt_dst)->sin_family = AF_INET;
        ((struct sockaddr_in *)&route.rt_genmask)->sin_family = AF_INET;
        ((struct sockaddr_in *)&route.rt_gateway)->sin_family = AF_INET;
        ((struct sockaddr_in *)&route.rt_gateway)->sin_addr.s_addr = htonl(gateway);
        if (ioctl(inet, SIOCADDRT, &route) < 0)
            fprintf(stderr, "netd: can't route through %s: %s\n", c->name, strerror(errno));
    }
    show4(address, text);
    printf("netd: %s is %s/%d\n", c->name, text, prefix_of(netmask));
    fflush(stdout);
}

static void apply6(struct card *c, bool add) {
    struct in6_request r = {.address = c->v6_address, .prefix = 128, .index = c->index};
    char text[INET6_ADDRSTRLEN];

    if (ioctl(inet6, add ? SIOCSIFADDR : SIOCDIFADDR, &r) < 0 && add)
        fprintf(stderr, "netd: can't set %s's IPv6 address: %s\n", c->name, strerror(errno));
    else if (add)
        printf("netd: %s is %s\n", c->name, inet_ntop(AF_INET6, &c->v6_address, text, sizeof(text)));
    fflush(stdout);
}

// --- DHCPv4 ---

static uint8_t *option(uint8_t *at, int code, const void *data, int size) {
    *at++ = code;
    *at++ = size;
    memcpy(at, data, size);
    return at + size;
}

static void dhcp_send(struct card *c, int type) {
    uint8_t packet[548] = {1, 1, 6}, *at = packet + 240;
    uint32_t magic = htonl(DHCP_MAGIC), xid = htonl(c->xid), requested = htonl(c->offered), server = htonl(c->server);
    uint32_t ciaddr = htonl(c->state == RENEWING || c->state == REBINDING ? c->address : 0);
    static const uint8_t wanted[] = {1, 3, 6, 51, 54, 58, 59};
    char hostname[64] = "";
    struct sockaddr_in to = {.sin_family = AF_INET, .sin_port = htons(67),
                             .sin_addr.s_addr = c->state == RENEWING ? htonl(c->server) : INADDR_BROADCAST};

    memcpy(packet + 4, &xid, 4);
    packet[10] = ciaddr ? 0 : 0x80; // replies broadcast, as there's no address to send them to yet
    memcpy(packet + 12, &ciaddr, 4);
    memcpy(packet + 28, c->mac, 6);
    memcpy(packet + 236, &magic, 4);
    at = option(at, 53, &(uint8_t){type}, 1);
    if (c->state == REQUESTING) {
        at = option(at, 50, &requested, 4);
        at = option(at, 54, &server, 4);
    }
    at = option(at, 55, wanted, sizeof(wanted));
    gethostname(hostname, sizeof(hostname) - 1);
    if (hostname[0])
        at = option(at, 12, hostname, strlen(hostname));
    *at++ = 255;
    if (sendto(c->fd, packet, sizeof(packet), 0, (struct sockaddr *)&to, sizeof(to)) < 0)
        fprintf(stderr, "netd: DHCP on %s: %s\n", c->name, strerror(errno));
}

// Asks again after a while, longer each time.
static void dhcp_retry(struct card *c) {
    int wait = RETRY_MS << (c->tries < 4 ? c->tries : 4);

    c->tries++;
    c->next = now_ms() + (wait < RETRY_MAX_MS ? wait : RETRY_MAX_MS);
}

static void dhcp_start(struct card *c) {
    random_bytes(&c->xid, sizeof(c->xid));
    c->state = SELECTING;
    c->tries = 0;
    c->offered = c->server = 0;
    dhcp_send(c, 1); // DISCOVER
    dhcp_retry(c);
}

static void dhcp_lost(struct card *c) {
    if (c->address) {
        printf("netd: %s lost its address\n", c->name);
        apply4(c, 0, 0, 0);
    }
    c->address = c->gateway = 0;
    memset(c->dns, 0, sizeof(c->dns));
    write_resolv();
}

static void dhcp_read(struct card *c) {
    uint8_t packet[1500];
    ssize_t n = recv(c->fd, packet, sizeof(packet), 0);
    uint32_t xid, magic, yiaddr, mask = 0, router = 0, server = 0, lease = 3600, t1 = 0, t2 = 0, dns[MAX_DNS] = {0};
    int type = 0;

    if (n < 240 || packet[0] != 2)
        return;
    memcpy(&xid, packet + 4, 4);
    memcpy(&magic, packet + 236, 4);
    if (ntohl(xid) != c->xid || ntohl(magic) != DHCP_MAGIC || memcmp(packet + 28, c->mac, 6))
        return;
    memcpy(&yiaddr, packet + 16, 4);
    for (uint8_t *at = packet + 240; at < packet + n && *at != 255;) {
        uint8_t code = at[0], size = at + 1 < packet + n ? at[1] : 0;
        uint32_t v = 0;

        if (code == 0) {
            at++;
            continue;
        }
        if (at + 2 + size > packet + n)
            break;
        if (size >= 4)
            memcpy(&v, at + 2, 4);
        v = ntohl(v);
        if (code == 53 && size)
            type = at[2];
        else if (code == 1)
            mask = v;
        else if (code == 3)
            router = v;
        else if (code == 54)
            server = v;
        else if (code == 51)
            lease = v;
        else if (code == 58)
            t1 = v;
        else if (code == 59)
            t2 = v;
        else if (code == 6)
            for (int i = 0; i < size / 4 && i < MAX_DNS; i++) {
                memcpy(&dns[i], at + 2 + 4 * i, 4);
                dns[i] = ntohl(dns[i]);
            }
        at += 2 + size;
    }

    if (type == 2 && c->state == SELECTING) { // OFFER
        c->offered = ntohl(yiaddr);
        c->server = server;
        c->state = REQUESTING;
        c->tries = 0;
        dhcp_send(c, 3); // REQUEST
        dhcp_retry(c);
    } else if (type == 5 && c->state >= REQUESTING) { // ACK
        int64_t now = now_ms();
        bool changed = c->address != ntohl(yiaddr) || c->netmask != mask || c->gateway != router;

        c->address = ntohl(yiaddr);
        c->netmask = mask ? mask : 0xFFFFFF00;
        c->gateway = router;
        c->server = server ? server : c->server;
        memcpy(c->dns, dns, sizeof(dns));
        c->state = BOUND;
        c->expires = now + lease * 1000LL;
        c->renew = now + (t1 ? t1 : lease / 2) * 1000LL;
        c->rebind = now + (t2 ? t2 : lease * 7 / 8) * 1000LL;
        c->next = c->renew;
        if (changed)
            apply4(c, c->address, c->netmask, c->gateway);
        write_resolv();
    } else if (type == 6 && c->state >= REQUESTING) { // NAK
        dhcp_lost(c);
        dhcp_start(c);
    }
}

// What's due: asking again, renewing, rebinding, or giving up the lease.
static void dhcp_tick(struct card *c, int64_t now) {
    if (c->hand || c->state == IDLE || now < c->next)
        return;
    if (c->state == SELECTING || (c->state == REQUESTING && c->tries > 3)) {
        dhcp_start(c);
    } else if (c->state == REQUESTING) {
        dhcp_send(c, 3);
        dhcp_retry(c);
    } else if (now >= c->expires) {
        dhcp_lost(c);
        dhcp_start(c);
    } else {
        c->state = now >= c->rebind ? REBINDING : RENEWING;
        dhcp_send(c, 3);
        c->next = now + (c->expires - now) / 2 > 60000 ? now + (c->expires - now) / 2 : now + 60000;
    }
}

// --- DHCPv6 ---

static uint8_t *option6(uint8_t *at, int code, const void *data, int size) {
    at[0] = code >> 8, at[1] = code, at[2] = size >> 8, at[3] = size;
    memcpy(at + 4, data, size);
    return at + 4 + size;
}

// Its client ID, from the card's MAC: a DUID-LL, type 3, on Ethernet.
static int duid(const struct card *c, uint8_t out[10]) {
    memcpy(out, (uint8_t[]){0, 3, 0, 1}, 4);
    memcpy(out + 4, c->mac, 6);
    return 10;
}

static void dhcp6_send(struct card *c, int type) {
    uint8_t packet[512], id[10], *at = packet + 4;
    static const uint8_t wanted[] = {0, 23}; // DNS servers
    struct sockaddr_in6 to = {.sin6_family = AF_INET6, .sin6_port = htons(547), .sin6_scope_id = c->index};
    uint8_t ia[12] = {0};

    inet_pton(AF_INET6, "ff02::1:2", &to.sin6_addr);
    packet[0] = type;
    memcpy(packet + 1, c->v6_xid, 3);
    at = option6(at, 1, id, duid(c, id));
    if (c->v6_server_len && type == 3)
        at = option6(at, 2, c->v6_server, c->v6_server_len);
    at = option6(at, 6, wanted, sizeof(wanted));
    at = option6(at, 8, (uint8_t[]){0, 0}, 2); // elapsed time
    if (type != 11) {
        memcpy(ia, &c->index, 4); // an IA_NA, named by the card
        at = option6(at, 3, ia, sizeof(ia));
    }
    if (sendto(c->v6_fd, packet, at - packet, 0, (struct sockaddr *)&to, sizeof(to)) < 0 && errno != EADDRNOTAVAIL)
        fprintf(stderr, "netd: DHCPv6 on %s: %s\n", c->name, strerror(errno));
}

static void dhcp6_start(struct card *c, int state) {
    random_bytes(c->v6_xid, 3);
    c->v6_state = state;
    c->v6_tries = 0;
    c->v6_next = now_ms(); // sent from the tick, once Linux has the card's link-local address
}

static void dhcp6_read(struct card *c) {
    uint8_t packet[1500];
    ssize_t n = recv(c->v6_fd, packet, sizeof(packet), 0);
    struct in6_addr address = {0};
    bool has_address = false;
    uint32_t valid = 0, t1 = 0;
    int dns_count = 0;

    if (n < 4 || memcmp(packet + 1, c->v6_xid, 3) || (packet[0] != 2 && packet[0] != 7))
        return;
    memset(c->v6_dns, 0, sizeof(c->v6_dns));
    for (uint8_t *at = packet + 4; at + 4 <= packet + n;) {
        int code = at[0] << 8 | at[1], size = at[2] << 8 | at[3];
        uint8_t *data = at + 4;

        if (data + size > packet + n)
            break;
        if (code == 2 && size <= (int)sizeof(c->v6_server)) { // the server's ID
            memcpy(c->v6_server, data, size);
            c->v6_server_len = size;
        } else if (code == 3 && size >= 12) { // an IA_NA, with the address in it
            memcpy(&t1, data + 4, 4);
            for (uint8_t *o = data + 12; o + 4 <= data + size;) {
                int ocode = o[0] << 8 | o[1], osize = o[2] << 8 | o[3];

                if (ocode == 5 && osize >= 24 && o + 4 + osize <= data + size) {
                    memcpy(&address, o + 4, 16);
                    memcpy(&valid, o + 24, 4);
                    has_address = ntohl(valid) > 0;
                }
                o += 4 + osize;
            }
        } else if (code == 23) {
            for (int i = 0; i + 16 <= size && dns_count < MAX_DNS; i += 16)
                memcpy(&c->v6_dns[dns_count++], data + i, 16);
        }
        at += 4 + size;
    }

    if (packet[0] == 2 && c->v6_state == V6_SOLICIT && has_address) { // ADVERTISE
        dhcp6_start(c, V6_REQUEST);
    } else if (packet[0] == 7) { // REPLY
        if (has_address && (!c->has_v6_address || memcmp(&address, &c->v6_address, 16))) {
            if (c->has_v6_address)
                apply6(c, false);
            c->v6_address = address;
            c->has_v6_address = true;
            apply6(c, true);
        }
        c->v6_state = V6_DONE;
        c->v6_renew = now_ms() + (ntohl(t1) ? ntohl(t1) : has_address ? ntohl(valid) / 2 : 3600) * 1000LL;
        write_resolv();
    }
}

static void dhcp6_tick(struct card *c, int64_t now) {
    if (c->v6_state == V6_DONE && now >= c->v6_renew)
        dhcp6_start(c, V6_SOLICIT);
    if (c->v6_state == V6_IDLE || c->v6_state == V6_DONE || now < c->v6_next)
        return;
    if (c->v6_state == V6_SOLICIT && c->v6_tries >= SOLICITS) // no addresses here: only its DNS
        dhcp6_start(c, V6_INFO);
    if (c->v6_state == V6_REQUEST && c->v6_tries >= SOLICITS)
        dhcp6_start(c, V6_SOLICIT);
    dhcp6_send(c, c->v6_state == V6_SOLICIT ? 1 : c->v6_state == V6_REQUEST ? 3 : 11);
    c->v6_next = now + (1000 << (c->v6_tries < 6 ? c->v6_tries : 6));
    c->v6_tries++;
}

// --- Cards ---

static int udp_socket(int family, int port, const char *card) {
    int fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0), on = 1;
    struct sockaddr_in in = {.sin_family = AF_INET, .sin_port = htons(port)};
    struct sockaddr_in6 in6 = {.sin6_family = AF_INET6, .sin6_port = htons(port)};

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, card, strlen(card));
    if (family == AF_INET6)
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
    if (family == AF_INET ? bind(fd, (struct sockaddr *)&in, sizeof(in)) : bind(fd, (struct sockaddr *)&in6, sizeof(in6))) {
        close(fd);
        return -1;
    }
    return fd;
}

static void sys_value(const char *card, const char *name, char *out, size_t size) {
    char path[128];
    FILE *f;

    snprintf(path, sizeof(path), "/sys/class/net/%s/%s", card, name);
    out[0] = '\0';
    if ((f = fopen(path, "r"))) {
        if (fgets(out, size, f))
            out[strcspn(out, "\n")] = '\0';
        fclose(f);
    }
}

// A cable plugged in, or a network joined: an address, from DHCP or set by hand.
static void connected(struct card *c) {
    char path[128], metric[16];

    printf("netd: %s connected\n", c->name);
    fflush(stdout);
    snprintf(metric, sizeof(metric), "%d\n", c->wifi ? WIFI_METRIC : WIRED_METRIC);
    snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/%s/ra_defrtr_metric", c->name);
    write_file(path, metric);
    c->fd = udp_socket(AF_INET, 68, c->name);
    c->v6_fd = udp_socket(AF_INET6, 546, c->name);
    if (c->hand)
        apply4(c, c->hand_address, netmask_of(c->hand_prefix), c->hand_gateway);
    else
        dhcp_start(c);
    dhcp6_start(c, V6_SOLICIT);
    write_resolv();
}

static void disconnected(struct card *c) {
    printf("netd: %s disconnected\n", c->name);
    fflush(stdout);
    if (c->fd >= 0)
        close(c->fd);
    if (c->v6_fd >= 0)
        close(c->v6_fd);
    c->fd = c->v6_fd = -1;
    dhcp_lost(c);
    apply4(c, 0, 0, 0);
    if (c->has_v6_address)
        apply6(c, false);
    c->has_v6_address = false;
    c->state = IDLE;
    c->v6_state = V6_IDLE;
    memset(c->v6_dns, 0, sizeof(c->v6_dns));
    write_resolv();
}

// What CONF says of a card: "CARD ADDRESS/PREFIX [GATEWAY [DNS]]", or nothing for DHCP.
static void read_conf(struct card *c) {
    FILE *f = fopen(CONF, "r");
    char line[256];

    c->hand = false;
    while (f && fgets(line, sizeof(line), f)) {
        char name[IF_NAMESIZE], address[64], gateway[64] = "", dns[64] = "";
        struct in_addr a, g = {0}, d = {0};
        int prefix = 24;

        if (sscanf(line, "%15s %63s %63s %63s", name, address, gateway, dns) < 2 || strcmp(name, c->name))
            continue;
        if (strchr(address, '/'))
            prefix = atoi(strchr(address, '/') + 1), *strchr(address, '/') = '\0';
        if (inet_aton(address, &a) && (!gateway[0] || inet_aton(gateway, &g)) && (!dns[0] || inet_aton(dns, &d))) {
            c->hand = true;
            c->hand_address = ntohl(a.s_addr);
            c->hand_prefix = prefix;
            c->hand_gateway = ntohl(g.s_addr);
            c->hand_dns = ntohl(d.s_addr);
        }
    }
    if (f)
        fclose(f);
}

// Whether CONF says to leave a card alone, as "CARD ignore", for another program.
static bool ignored(const char *name) {
    FILE *f = fopen(CONF, "r");
    char line[256], card[IF_NAMESIZE], word[16];
    bool found = false;

    while (f && fgets(line, sizeof(line), f))
        found |= sscanf(line, "%15s %15s", card, word) == 2 && !strcmp(card, name) && !strcmp(word, "ignore");
    if (f)
        fclose(f);
    return found;
}

// The cards Linux has, apart from loopback and those of no device, such as tunnels,
// once udev's named them: each is brought up, and those with a carrier connected.
static void scan(void) {
    DIR *dir = opendir("/sys/class/net");
    struct dirent *e;

    unsettled = false;
    for (int i = 0; i < card_count; i++)
        cards[i].seen = false;
    while (dir && (e = readdir(dir))) {
        char path[300], value[32];
        struct card *c = NULL;

        snprintf(path, sizeof(path), "/sys/class/net/%s/device", e->d_name);
        if (e->d_name[0] == '.' || access(path, F_OK) || strlen(e->d_name) >= IF_NAMESIZE || ignored(e->d_name))
            continue;
        snprintf(path, sizeof(path), "/run/udev/data/n%u", if_nametoindex(e->d_name));
        if (access(path, F_OK)) { // which it writes once it's done, and named the card
            unsettled = true;
            continue;
        }
        for (int i = 0; i < card_count; i++)
            c = !strcmp(cards[i].name, e->d_name) ? &cards[i] : c;
        if (!c && card_count < MAX_CARDS) {
            struct ifreq r = {0};
            unsigned mac[6] = {0};

            c = &cards[card_count++];
            *c = (struct card){.fd = -1, .v6_fd = -1, .index = if_nametoindex(e->d_name)};
            snprintf(c->name, sizeof(c->name), "%s", e->d_name);
            snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", e->d_name);
            c->wifi = access(path, F_OK) == 0;
            sys_value(c->name, "address", value, sizeof(value));
            sscanf(value, "%x:%x:%x:%x:%x:%x", &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]);
            for (int i = 0; i < 6; i++)
                c->mac[i] = mac[i];
            read_conf(c);
            r = request_for(c);
            if (ioctl(inet, SIOCGIFFLAGS, &r) == 0 && !(r.ifr_flags & IFF_UP)) {
                r.ifr_flags |= IFF_UP;
                ioctl(inet, SIOCSIFFLAGS, &r);
            }
            if (c->wifi)
                wifi_found(c);
            printf("netd: found %s, %s\n", c->name, c->wifi ? "Wi-Fi" : "wired");
            fflush(stdout);
        }
        if (!c)
            continue;
        c->seen = true;
        sys_value(c->name, "carrier", value, sizeof(value));

        bool carrier = value[0] == '1';

        if (carrier && !c->carrier)
            c->carrier = true, connected(c);
        else if (!carrier && c->carrier)
            c->carrier = false, disconnected(c);
    }
    if (dir)
        closedir(dir);
    for (int i = 0; i < card_count; i++) { // those unplugged
        if (!cards[i].seen) {
            if (cards[i].carrier)
                disconnected(&cards[i]);
            if (cards[i].wifi)
                wifi_gone(&cards[i]);
            cards[i--] = cards[--card_count];
        }
    }
}

// --- Asking it ---

static struct card *card_named(const char *name) {
    for (int i = 0; i < card_count; i++) {
        if (!strcmp(cards[i].name, name))
            return &cards[i];
    }
    return NULL;
}

static void status(FILE *out) {
    char text[INET6_ADDRSTRLEN];

    fprintf(out, "airplane %s\n", airplane_on() ? "on" : "off");
    for (int wifi = 0; wifi < 2; wifi++) {
        for (int i = 0; i < card_count; i++) {
            struct card *c = &cards[i];
            uint32_t address = c->hand ? c->hand_address : c->address;
            uint32_t gateway = c->hand ? c->hand_gateway : c->gateway;

            if (c->wifi != wifi)
                continue;
            fprintf(out, "card %s %s %s %s\n", c->name, c->wifi ? "wifi" : "wired", c->carrier ? "up" : "down",
                    c->hand ? "hand" : "dhcp");
            if (c->carrier && address) {
                show4(address, text);
                fprintf(out, "address %s %s/%d\n", c->name, text, c->hand ? c->hand_prefix : prefix_of(c->netmask));
            }
            if (c->carrier && gateway) {
                show4(gateway, text);
                fprintf(out, "gateway %s %s\n", c->name, text);
            }
            if (c->has_v6_address)
                fprintf(out, "address %s %s/128\n", c->name, inet_ntop(AF_INET6, &c->v6_address, text, sizeof(text)));
            if (c->wifi)
                wifi_status(out, c);
        }
    }

    FILE *resolv = fopen(RESOLV, "r");
    char line[128];

    while (resolv && fgets(line, sizeof(line), resolv)) {
        if (!strncmp(line, "nameserver ", 11))
            fprintf(out, "dns %s", line + 11);
    }
    if (resolv)
        fclose(resolv);
    fputs("end\n", out);
}

// CONF, without the card's line, and with its new one, if any.
static bool save_conf(const char *name, const char *line) {
    FILE *in = fopen(CONF, "r"), *out = fopen(CONF ".new", "w");
    char old[256], first[IF_NAMESIZE + 1];

    if (!out) {
        if (in)
            fclose(in);
        return false;
    }
    fchmod(fileno(out), 0600);
    while (in && fgets(old, sizeof(old), in)) {
        if (sscanf(old, "%16s", first) != 1 || strcmp(first, name))
            fputs(old, out);
    }
    if (line)
        fprintf(out, "%s\n", line);
    if (in)
        fclose(in);
    return fclose(out) == 0 && rename(CONF ".new", CONF) == 0;
}

static void command(char *line, FILE *out, uid_t uid) {
    char *word = strtok(line, " \t\n"), *rest = strtok(NULL, "\n"), *name;
    struct card *c;

    if (!word || !strcmp(word, "status")) {
        status(out);
        return;
    }
    if (strcmp(word, "networks") && strcmp(word, "scan") && uid != 0 && uid < FIRST_UID) {
        fputs("error only accounts can change the network\n", out);
        return;
    }
    if (wifi_command(word, rest ? rest : (char[]){""}, out))
        return;
    name = rest ? strtok(rest, " \t\n") : NULL;
    c = name ? card_named(name) : NULL;
    if (!c) {
        fprintf(out, "error there's no card called %s\n", name ? name : "that");
        return;
    }
    if (!strcmp(word, "dhcp")) {
        save_conf(c->name, NULL);
    } else if (!strcmp(word, "set")) {
        char saved[256], plain[64], *address = strtok(NULL, " \t\n"), *gateway = strtok(NULL, " \t\n");
        char *dns = strtok(NULL, " \t\n"), *slash;
        struct in_addr a;
        int prefix = 24;

        snprintf(plain, sizeof(plain), "%s", address ? address : "");
        if ((slash = strchr(plain, '/')))
            prefix = atoi(slash + 1), *slash = '\0';
        if (!inet_aton(plain, &a) || prefix < 1 || prefix > 32 || (gateway && !inet_aton(gateway, &a))
            || (dns && !inet_aton(dns, &a))) {
            fputs("error that isn't an address\n", out);
            return;
        }
        snprintf(saved, sizeof(saved), "%s %s %s %s", c->name, address, gateway ? gateway : "", dns ? dns : "");
        if (!save_conf(c->name, saved)) {
            fputs("error can't save that\n", out);
            return;
        }
    } else {
        fputs("error try status, dhcp, set, networks, scan, join, forget or airplane\n", out);
        return;
    }
    read_conf(c);
    if (c->carrier) { // as if plugged in again
        disconnected(c);
        connected(c);
    }
    fputs("ok\n", out);
}

static void answer(int listener) {
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    struct ucred who;
    socklen_t size = sizeof(who);
    char line[256] = "";
    FILE *f;

    if (fd < 0)
        return;
    struct timeval wait = {.tv_sec = 1};

    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &who, &size) < 0 || !(f = fdopen(fd, "r+"))) {
        close(fd);
        return;
    }
    if (fgets(line, sizeof(line), f))
        command(line, f, who.uid);
    fclose(f);
}

// Its wpa_supplicants go with it, so a netd started again starts its own.
static void stopped(int signal) {
    for (int i = 0; i < card_count; i++) {
        if (cards[i].wpa > 0)
            kill(cards[i].wpa, SIGTERM);
    }
    _exit(128 + signal);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: netd\n"
                            "Keeps the network: brings up each card, gets addresses by DHCP and DHCPv6, or\n"
                            "uses those set by hand, and sets DNS, and joins Wi-Fi networks through\n"
                            "wpa_supplicant. Started by init, as root. net and wifi ask it.\n");

    int links = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_nl nl = {.nl_family = AF_NETLINK, .nl_groups = RTMGRP_LINK};
    struct sockaddr_un un = {.sun_family = AF_UNIX, .sun_path = SOCKET};

    inet = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    inet6 = socket(AF_INET6, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stopped);
    signal(SIGINT, stopped);
    unlink(SOCKET);
    if (links < 0 || bind(links, (struct sockaddr *)&nl, sizeof(nl)) < 0 || listener < 0
        || bind(listener, (struct sockaddr *)&un, sizeof(un)) < 0 || listen(listener, 8) < 0 || inet < 0) {
        perror("netd");
        return 1;
    }
    chmod(SOCKET, 0666); // anyone may ask, and it checks who changes things
    write_resolv();
    airplane_load();
    scan();

    for (;;) {
        struct pollfd fds[2 + 2 * MAX_CARDS] = {{.fd = links, .events = POLLIN}, {.fd = listener, .events = POLLIN}};
        int64_t now = now_ms(), wait = 60000;

        for (int i = 0; i < card_count; i++) {
            fds[2 + 2 * i] = (struct pollfd){.fd = cards[i].fd, .events = POLLIN};
            fds[3 + 2 * i] = (struct pollfd){.fd = cards[i].v6_fd, .events = POLLIN};
            if (cards[i].state != IDLE && !cards[i].hand && cards[i].next - now < wait)
                wait = cards[i].next - now;
            if (cards[i].v6_state != V6_IDLE) {
                int64_t due = cards[i].v6_state == V6_DONE ? cards[i].v6_renew : cards[i].v6_next;

                wait = due - now < wait ? due - now : wait;
            }
            if (cards[i].wifi) {
                int due = wifi_tick(&cards[i], now);

                wait = due < wait ? due : wait;
            }
        }
        if (unsettled && wait > 500)
            wait = 500;
        poll(fds, 2 + 2 * card_count, wait > 0 ? (int)wait : 0);
        if (unsettled)
            scan();
        if (fds[0].revents & POLLIN) { // a card came, went, or its cable did
            char buffer[8192];

            while (recv(links, buffer, sizeof(buffer), MSG_DONTWAIT) > 0) {
            }
            scan();
        }
        if (fds[1].revents & POLLIN)
            answer(listener);
        for (int i = 0; i < card_count; i++) {
            if (fds[2 + 2 * i].revents & POLLIN)
                dhcp_read(&cards[i]);
            if (fds[3 + 2 * i].revents & POLLIN)
                dhcp6_read(&cards[i]);
            dhcp_tick(&cards[i], now_ms());
            dhcp6_tick(&cards[i], now_ms());
        }
    }
}
