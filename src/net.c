#include <e1000.h>
#include <errno_values.h>
#include <inet.h>
#include <irq.h>
#include <kmalloc.h>
#include <net.h>
#include <serial.h>
#include <string.h>
#include <tcp.h>

#define ETH_HEADER 14
#define ETH_ARP 0x0806
#define ETH_IP 0x0800

#define ARP_SIZE 28
#define ARP_REQUEST 1
#define ARP_REPLY 2
#define ARP_ENTRIES 32
#define ARP_LIFETIME (300 * TIMER_HZ)
#define ARP_RETRY (1 * TIMER_HZ)
#define ARP_TRIES 3

#define IP_HEADER 20
#define IP_TTL 64
#define IP_DONT_FRAGMENT 0x4000
#define IP_FRAGMENT_MASK 0x3FFF

#define ICMP_ECHO_REPLY 0
#define ICMP_UNREACHABLE 3
#define ICMP_ECHO_REQUEST 8
#define ICMP_PORT_UNREACHABLE 3
#define ICMP_HEADER 8

#define LOOPBACK_ADDRESS 0x7F000001u
#define LOOPBACK_NETMASK 0xFF000000u
#define LOOPBACK_QUEUE_MAX 256

#define UDP_HEADER 8
#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_FIXED_SIZE 240
#define DHCP_MAGIC 0x63825363u
#define DHCP_BOOT_REQUEST 1
#define DHCP_BOOT_REPLY 2
#define DHCP_DISCOVER 1
#define DHCP_OFFER 2
#define DHCP_REQUEST 3
#define DHCP_ACK 5
#define DHCP_NAK 6
#define OPTION_NETMASK 1
#define OPTION_ROUTER 3
#define OPTION_DNS 6
#define OPTION_REQUESTED_IP 50
#define OPTION_LEASE 51
#define OPTION_MESSAGE_TYPE 53
#define OPTION_SERVER_ID 54
#define OPTION_PARAMETERS 55
#define OPTION_END 255
#define DHCP_FIRST_RETRY (2 * TIMER_HZ)
#define DHCP_LAST_RETRY (32 * TIMER_HZ)

struct arp_entry {
    uint32_t address;
    uint8_t mac[6];
    bool resolved;
    uint8_t tries;
    uint64_t expires;       // when to forget it or, while unresolved, to ask again
    uint8_t *waiting;       // the last frame to want this address
    size_t waiting_len;
};

struct loopback_packet {
    struct loopback_packet *next;
    size_t len;
    uint8_t data[];
};

enum dhcp_state {
    DHCP_OFF,
    DHCP_DISCOVERING,
    DHCP_REQUESTING,
    DHCP_BOUND,
};

static bool has_device;
static uint8_t our_mac[6];
static uint32_t our_address;
static uint32_t our_netmask;
static uint32_t our_gateway;
static uint32_t our_dns;
static uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
static struct arp_entry arp[ARP_ENTRIES];

static struct loopback_packet *loopback_head;
static struct loopback_packet *loopback_tail;
static int loopback_count;
static bool loopback_draining;

static enum dhcp_state dhcp_state;
static uint32_t dhcp_id;
static uint32_t dhcp_offered;
static uint32_t dhcp_server;
static uint64_t dhcp_next;   // when to send again or, once bound, to renew
static uint64_t dhcp_wait;

static const uint8_t broadcast_mac[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

uint16_t net_checksum(const void *data, size_t len, uint32_t initial) {
    const uint8_t *bytes = data;
    uint64_t sum = initial;

    for (; len >= 2; len -= 2, bytes += 2)
        sum += bytes[0] << 8 | bytes[1];
    if (len)
        sum += bytes[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);
    return ~sum;
}

uint16_t net_payload_checksum(uint32_t source, uint32_t destination, uint8_t protocol, const void *data, size_t len) {
    uint32_t pseudo = (source >> 16) + (source & 0xFFFF) + (destination >> 16) + (destination & 0xFFFF) + protocol + len;

    return net_checksum(data, len, pseudo);
}

static void send_frame(uint8_t *frame, const uint8_t *to, uint16_t type, size_t payload_len) {
    memcpy(frame, to, 6);
    memcpy(frame + 6, our_mac, 6);
    net_write16(frame + 12, type);

    size_t len = ETH_HEADER + payload_len;

    tx_packets++;
    tx_bytes += len;
    e1000_send(frame, len);
}

static void send_arp(uint16_t operation, const uint8_t *to_mac, uint32_t to_address) {
    uint8_t frame[ETH_HEADER + ARP_SIZE];
    uint8_t *p = frame + ETH_HEADER;

    net_write16(p, 1);
    net_write16(p + 2, ETH_IP);
    p[4] = 6;
    p[5] = 4;
    net_write16(p + 6, operation);
    memcpy(p + 8, our_mac, 6);
    net_write32(p + 14, our_address);
    memset(p + 18, 0, 6);
    if (operation == ARP_REPLY)
        memcpy(p + 18, to_mac, 6);
    net_write32(p + 24, to_address);
    send_frame(frame, to_mac, ETH_ARP, ARP_SIZE);
}

static struct arp_entry *arp_find(uint32_t address) {
    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp[i].address == address)
            return &arp[i];
    }
    return NULL;
}

static void arp_clear(struct arp_entry *entry) {
    kfree(entry->waiting);
    *entry = (struct arp_entry){0};
}

// A free entry or else the one that's going to expire first.
static struct arp_entry *arp_new(uint32_t address) {
    struct arp_entry *best = &arp[0];

    for (int i = 0; i < ARP_ENTRIES; i++) {
        if (arp[i].address == 0) {
            best = &arp[i];
            break;
        }
        if (arp[i].expires < best->expires)
            best = &arp[i];
    }
    arp_clear(best);
    best->address = address;
    return best;
}

static void arp_learn(uint32_t address, const uint8_t *mac) {
    struct arp_entry *entry = arp_find(address);

    if (!entry)
        return;

    memcpy(entry->mac, mac, 6);
    entry->resolved = true;
    entry->expires = irq_ticks() + ARP_LIFETIME;

    if (entry->waiting) {
        send_frame(entry->waiting, mac, ETH_IP, entry->waiting_len);
        kfree(entry->waiting);
        entry->waiting = NULL;
    }
}

static void handle_arp(const uint8_t *p, size_t len) {
    if (len < ARP_SIZE || net_read16(p) != 1 || net_read16(p + 2) != ETH_IP || p[4] != 6 || p[5] != 4)
        return;

    uint32_t sender = net_read32(p + 14);
    uint32_t target = net_read32(p + 24);

    arp_learn(sender, p + 8);
    if (our_address == 0 || target != our_address)
        return;

    // Asked for us, so it's probably about to be needed the other way as well.
    if (!arp_find(sender))
        arp_new(sender);
    arp_learn(sender, p + 8);
    if (net_read16(p + 6) == ARP_REQUEST)
        send_arp(ARP_REPLY, p + 8, sender);
}

// Holds on to the frame until the answer comes. Only the last one per address waits.
static void send_via_arp(uint8_t *frame, size_t payload_len, uint32_t next_hop) {
    struct arp_entry *entry = arp_find(next_hop);

    if (entry && entry->resolved) {
        send_frame(frame, entry->mac, ETH_IP, payload_len);
        return;
    }

    bool asking = entry != NULL;

    if (!entry)
        entry = arp_new(next_hop);

    uint8_t *copy = kmalloc(ETH_HEADER + payload_len);

    if (copy) {
        memcpy(copy, frame, ETH_HEADER + payload_len);
        kfree(entry->waiting);
        entry->waiting = copy;
        entry->waiting_len = payload_len;
    }
    if (!asking) {
        entry->tries = 1;
        entry->expires = irq_ticks() + ARP_RETRY;
        send_arp(ARP_REQUEST, broadcast_mac, next_hop);
    }
}

bool net_is_local(uint32_t address) {
    return (address & LOOPBACK_NETMASK) == (LOOPBACK_ADDRESS & LOOPBACK_NETMASK)
        || (our_address && address == our_address);
}

uint32_t net_source_for(uint32_t destination) {
    if ((destination & LOOPBACK_NETMASK) == (LOOPBACK_ADDRESS & LOOPBACK_NETMASK))
        return LOOPBACK_ADDRESS;
    if (!our_address)
        return 0;
    if (destination == our_address || destination == NET_BROADCAST)
        return our_address;
    if ((destination & our_netmask) == (our_address & our_netmask) || our_gateway)
        return our_address;
    return 0;
}

static void handle_ip(const uint8_t *packet, size_t len);

// Packets to this machine wait in a queue until net_flush, so that whatever sent one
// has finished with its own state before the answer comes in.
static void loop_back(const uint8_t *packet, size_t len) {
    struct loopback_packet *copy = loopback_count < LOOPBACK_QUEUE_MAX ? kmalloc(sizeof(*copy) + len) : NULL;

    if (!copy)
        return;

    copy->next = NULL;
    copy->len = len;
    memcpy(copy->data, packet, len);
    if (loopback_tail)
        loopback_tail->next = copy;
    else
        loopback_head = copy;
    loopback_tail = copy;
    loopback_count++;
}

void net_flush(void) {
    if (loopback_draining)
        return;

    loopback_draining = true;
    while (loopback_head) {
        struct loopback_packet *p = loopback_head;

        loopback_head = p->next;
        if (!loopback_head)
            loopback_tail = NULL;
        loopback_count--;
        handle_ip(p->data, p->len);
        kfree(p);
    }
    loopback_draining = false;
}

int net_send_ip(uint8_t *frame, size_t payload_len, uint8_t protocol, uint32_t source, uint32_t destination) {
    static uint16_t next_id;
    uint8_t *ip = frame + ETH_HEADER;
    size_t total = IP_HEADER + payload_len;

    ip[0] = 0x45;
    ip[1] = 0;
    net_write16(ip + 2, total);
    net_write16(ip + 4, next_id++);
    net_write16(ip + 6, IP_DONT_FRAGMENT);
    ip[8] = IP_TTL;
    ip[9] = protocol;
    net_write16(ip + 10, 0);
    net_write32(ip + 12, source);
    net_write32(ip + 16, destination);
    net_write16(ip + 10, net_checksum(ip, IP_HEADER, 0));

    if (net_is_local(destination)) {
        loop_back(ip, total);
        return 0;
    }
    if (!has_device)
        return -ENETUNREACH;

    uint32_t host_bits = ~our_netmask;

    if (destination == NET_BROADCAST || (our_address && host_bits && (destination & host_bits) == host_bits
                                         && (destination & our_netmask) == (our_address & our_netmask))) {
        send_frame(frame, broadcast_mac, ETH_IP, total);
        return 0;
    }
    if (!our_address)
        return -ENETUNREACH;
    if ((destination & our_netmask) == (our_address & our_netmask)) {
        send_via_arp(frame, total, destination);
        return 0;
    }
    if (!our_gateway)
        return -ENETUNREACH;
    send_via_arp(frame, total, our_gateway);
    return 0;
}

static void handle_icmp(uint32_t source, uint32_t destination, const uint8_t *icmp, size_t len) {
    if (len < ICMP_HEADER || net_checksum(icmp, len, 0) != 0)
        return;

    if (icmp[0] == ICMP_ECHO_REQUEST) {
        uint8_t frame[NET_FRAME_SIZE];
        uint8_t *out = frame + NET_HEADROOM;

        memcpy(out, icmp, len);
        out[0] = ICMP_ECHO_REPLY;
        net_write16(out + 2, 0);
        net_write16(out + 2, net_checksum(out, len, 0));
        net_send_ip(frame, len, IP_PROTO_ICMP, destination, source);
    } else if (icmp[0] == ICMP_ECHO_REPLY) {
        inet_deliver_echo_reply(source, net_read16(icmp + 4), icmp, len);
    } else if (icmp[0] == ICMP_UNREACHABLE && icmp[1] == ICMP_PORT_UNREACHABLE && len >= ICMP_HEADER + IP_HEADER + 4) {
        const uint8_t *inner = icmp + ICMP_HEADER;
        size_t header = (inner[0] & 0xF) * 4;

        if (inner[9] == IP_PROTO_UDP && len >= ICMP_HEADER + header + 4)
            inet_port_unreachable(net_read32(inner + 16), net_read16(inner + header), net_read16(inner + header + 2));
    }
}

static void dhcp_send(uint8_t type);

static void dhcp_start(void) {
    dhcp_state = DHCP_DISCOVERING;
    dhcp_id = (uint32_t)__builtin_ia32_rdtsc();
    dhcp_wait = DHCP_FIRST_RETRY;
    dhcp_next = irq_ticks() + dhcp_wait;
    dhcp_send(DHCP_DISCOVER);
}

static void dhcp_send(uint8_t type) {
    uint8_t frame[NET_FRAME_SIZE];
    uint8_t *udp = frame + NET_HEADROOM;
    uint8_t *d = udp + UDP_HEADER;

    memset(d, 0, DHCP_FIXED_SIZE);
    d[0] = DHCP_BOOT_REQUEST;
    d[1] = 1;
    d[2] = 6;
    net_write32(d + 4, dhcp_id);
    net_write16(d + 10, 0x8000); // answer by broadcast, as there's no address to answer to
    memcpy(d + 28, our_mac, 6);
    net_write32(d + 236, DHCP_MAGIC);

    uint8_t *o = d + DHCP_FIXED_SIZE;

    *o++ = OPTION_MESSAGE_TYPE;
    *o++ = 1;
    *o++ = type;
    if (type == DHCP_REQUEST) {
        *o++ = OPTION_REQUESTED_IP;
        *o++ = 4;
        net_write32(o, dhcp_offered);
        o += 4;
        *o++ = OPTION_SERVER_ID;
        *o++ = 4;
        net_write32(o, dhcp_server);
        o += 4;
    }
    *o++ = OPTION_PARAMETERS;
    *o++ = 3;
    *o++ = OPTION_NETMASK;
    *o++ = OPTION_ROUTER;
    *o++ = OPTION_DNS;
    *o++ = OPTION_END;

    size_t len = o - udp;

    net_write16(udp, DHCP_CLIENT_PORT);
    net_write16(udp + 2, DHCP_SERVER_PORT);
    net_write16(udp + 4, len);
    net_write16(udp + 6, 0);
    net_write16(udp + 6, net_payload_checksum(0, NET_BROADCAST, IP_PROTO_UDP, udp, len));
    net_send_ip(frame, len, IP_PROTO_UDP, 0, NET_BROADCAST);
}

static void handle_dhcp(const uint8_t *d, size_t len) {
    if (len < DHCP_FIXED_SIZE || d[0] != DHCP_BOOT_REPLY || net_read32(d + 4) != dhcp_id
        || net_read32(d + 236) != DHCP_MAGIC || memcmp(d + 28, our_mac, 6) != 0)
        return;

    uint8_t type = 0;
    uint32_t netmask = 0, router = 0, dns = 0, server = 0, lease = 0;

    for (size_t at = DHCP_FIXED_SIZE; at + 2 <= len && d[at] != OPTION_END;) {
        if (d[at] == 0) {
            at++;
            continue;
        }

        size_t size = d[at + 1];
        const uint8_t *value = d + at + 2;

        if (at + 2 + size > len)
            break;
        if (d[at] == OPTION_MESSAGE_TYPE && size >= 1)
            type = value[0];
        else if (size >= 4 && d[at] == OPTION_NETMASK)
            netmask = net_read32(value);
        else if (size >= 4 && d[at] == OPTION_ROUTER)
            router = net_read32(value);
        else if (size >= 4 && d[at] == OPTION_DNS)
            dns = net_read32(value);
        else if (size >= 4 && d[at] == OPTION_SERVER_ID)
            server = net_read32(value);
        else if (size >= 4 && d[at] == OPTION_LEASE)
            lease = net_read32(value);
        at += 2 + size;
    }

    if (type == DHCP_OFFER && dhcp_state == DHCP_DISCOVERING) {
        dhcp_offered = net_read32(d + 16);
        dhcp_server = server;
        dhcp_state = DHCP_REQUESTING;
        dhcp_wait = DHCP_FIRST_RETRY;
        dhcp_next = irq_ticks() + dhcp_wait;
        dhcp_send(DHCP_REQUEST);
    } else if (type == DHCP_ACK && dhcp_state == DHCP_REQUESTING) {
        bool changed = our_address != net_read32(d + 16);

        our_address = net_read32(d + 16);
        our_netmask = netmask ? netmask : 0xFFFFFF00u;
        our_gateway = router;
        our_dns = dns;
        dhcp_state = DHCP_BOUND;

        // Half way through the lease is when to ask for more. Forever is 0xFFFFFFFF.
        uint64_t renew = lease && lease != 0xFFFFFFFFu ? lease / 2 : 3600;

        dhcp_next = irq_ticks() + renew * TIMER_HZ;
        if (changed)
            kprintf("net: address %u.%u.%u.%u\n", our_address >> 24, our_address >> 16 & 0xFF,
                    our_address >> 8 & 0xFF, our_address & 0xFF);
    } else if (type == DHCP_NAK && dhcp_state == DHCP_REQUESTING) {
        our_address = 0;
        dhcp_start();
    }
}

static void dhcp_tick(void) {
    if (dhcp_state == DHCP_OFF || irq_ticks() < dhcp_next)
        return;

    if (dhcp_state == DHCP_BOUND) {
        dhcp_id++;
        dhcp_state = DHCP_REQUESTING;
        dhcp_offered = our_address;
        dhcp_wait = DHCP_FIRST_RETRY;
        dhcp_send(DHCP_REQUEST);
    } else if (dhcp_state == DHCP_REQUESTING && dhcp_wait >= DHCP_LAST_RETRY) {
        // The server's gone quiet, so start over. The address stays until told otherwise.
        dhcp_start();
        return;
    } else {
        dhcp_wait = dhcp_wait < DHCP_LAST_RETRY ? dhcp_wait * 2 : DHCP_LAST_RETRY;
        dhcp_send(dhcp_state == DHCP_DISCOVERING ? DHCP_DISCOVER : DHCP_REQUEST);
    }
    dhcp_next = irq_ticks() + dhcp_wait;
}

static void handle_udp(uint32_t source, uint32_t destination, const uint8_t *udp, size_t len) {
    if (len < UDP_HEADER)
        return;

    size_t udp_len = net_read16(udp + 4);

    if (udp_len < UDP_HEADER || udp_len > len)
        return;
    if (net_read16(udp + 6) != 0 && net_payload_checksum(source, destination, IP_PROTO_UDP, udp, udp_len) != 0)
        return;

    uint16_t source_port = net_read16(udp);
    uint16_t port = net_read16(udp + 2);
    const uint8_t *data = udp + UDP_HEADER;

    if (port == DHCP_CLIENT_PORT && source_port == DHCP_SERVER_PORT && dhcp_state != DHCP_OFF) {
        handle_dhcp(data, udp_len - UDP_HEADER);
        return;
    }
    inet_deliver_udp(source, source_port, destination, port, data, udp_len - UDP_HEADER);
}

static void handle_ip(const uint8_t *ip, size_t len) {
    if (len < IP_HEADER || ip[0] >> 4 != 4)
        return;

    size_t header = (ip[0] & 0xF) * 4;
    size_t total = net_read16(ip + 2);

    if (header < IP_HEADER || total < header || total > len || net_checksum(ip, header, 0) != 0)
        return;

    // Nothing here sends fragments, and hardly anything else does any more.
    if (net_read16(ip + 6) & IP_FRAGMENT_MASK)
        return;

    uint32_t source = net_read32(ip + 12);
    uint32_t destination = net_read32(ip + 16);
    bool directed_broadcast = our_address && (destination | our_netmask) == NET_BROADCAST
        && (destination & our_netmask) == (our_address & our_netmask);

    if (!net_is_local(destination) && destination != NET_BROADCAST && !directed_broadcast) {
        // DHCP servers may answer the address they're offering, before it's ours.
        if (our_address || ip[9] != IP_PROTO_UDP)
            return;
    }

    const uint8_t *payload = ip + header;
    size_t payload_len = total - header;

    switch (ip[9]) {
    case IP_PROTO_ICMP:
        // Pings of a broadcast address get no answer, as they could be used to flood.
        if (net_is_local(destination))
            handle_icmp(source, destination, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        handle_udp(source, destination, payload, payload_len);
        break;
    case IP_PROTO_TCP:
        if (net_is_local(destination))
            tcp_input(source, destination, payload, payload_len);
        break;
    }
}

void net_receive(const void *frame, size_t len) {
    const uint8_t *bytes = frame;

    if (len < ETH_HEADER)
        return;

    rx_packets++;
    rx_bytes += len;

    uint16_t type = net_read16(bytes + 12);

    if (type == ETH_ARP)
        handle_arp(bytes + ETH_HEADER, len - ETH_HEADER);
    else if (type == ETH_IP)
        handle_ip(bytes + ETH_HEADER, len - ETH_HEADER);
}

static void arp_tick(void) {
    uint64_t now = irq_ticks();

    for (int i = 0; i < ARP_ENTRIES; i++) {
        struct arp_entry *entry = &arp[i];

        if (entry->address == 0 || now < entry->expires)
            continue;
        if (entry->resolved || entry->tries == ARP_TRIES) {
            arp_clear(entry);
        } else {
            entry->tries++;
            entry->expires = now + ARP_RETRY;
            send_arp(ARP_REQUEST, broadcast_mac, entry->address);
        }
    }
}

void net_init(void) {
    has_device = e1000_init();
    if (!has_device)
        return;

    e1000_mac(our_mac);
    dhcp_start();
}

void net_tick(void) {
    if (has_device) {
        e1000_poll();
        arp_tick();
        dhcp_tick();
    }
    tcp_tick();
}

void net_get_info(struct net_info *out) {
    *out = (struct net_info){
        .has_device = has_device,
        .automatic = dhcp_state != DHCP_OFF,
        .address = our_address,
        .netmask = our_netmask,
        .gateway = our_gateway,
        .dns = our_dns,
        .rx_packets = rx_packets,
        .tx_packets = tx_packets,
        .rx_bytes = rx_bytes,
        .tx_bytes = tx_bytes,
    };
    memcpy(out->mac, our_mac, 6);
}

void net_configure(uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns) {
    for (int i = 0; i < ARP_ENTRIES; i++)
        arp_clear(&arp[i]);

    our_address = address;
    our_netmask = netmask;
    our_gateway = gateway;
    our_dns = dns;
    dhcp_state = DHCP_OFF;
    if (address == 0 && has_device)
        dhcp_start();
}
