#pragma once

#include <net_types.h>
#include <stddef.h>
#include <stdint.h>

// Ethernet, ARP, IP, ICMP and DHCP. Everything here runs with the kernel lock held, so
// none of it needs a lock of its own. Addresses are in host byte order.

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP 6
#define IP_PROTO_UDP 17

#define NET_HEADROOM 34 // Ethernet and IP headers
#define NET_MTU 1500
#define NET_MAX_PAYLOAD (NET_MTU - 20)
#define NET_FRAME_SIZE (NET_HEADROOM + NET_MAX_PAYLOAD)
#define NET_BROADCAST 0xFFFFFFFFu

void net_init(void);

// Delivers packets this machine sent to itself. Called before anything else can run.
void net_flush(void);

// Called by the timer, on one CPU, each tick.
void net_tick(void);

// Takes a frame from the network card.
void net_receive(const void *frame, size_t len);

// Sends what's in frame after NET_HEADROOM.
int net_send_ip(uint8_t *frame, size_t payload_len, uint8_t protocol, uint32_t source, uint32_t destination);

// The address to send to destination from, or 0 if there's no way to.
uint32_t net_source_for(uint32_t destination);
bool net_is_local(uint32_t address);

uint16_t net_checksum(const void *data, size_t len, uint32_t initial);

// The checksum of UDP and TCP, which covers part of the IP header too.
uint16_t net_payload_checksum(uint32_t source, uint32_t destination, uint8_t protocol, const void *data, size_t len);

void net_get_info(struct net_info *out);

// Sets the configuration by hand. With an address of 0, goes back to asking DHCP.
void net_configure(uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns);
void net_set_airplane(bool on);

static inline uint16_t net_read16(const void *p) {
    const uint8_t *b = p;
    return (uint16_t)(b[0] << 8 | b[1]);
}

static inline uint32_t net_read32(const void *p) {
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
}

static inline void net_write16(void *p, uint16_t value) {
    uint8_t *b = p;
    b[0] = value >> 8;
    b[1] = value;
}

static inline void net_write32(void *p, uint32_t value) {
    uint8_t *b = p;
    b[0] = value >> 24;
    b[1] = value >> 16;
    b[2] = value >> 8;
    b[3] = value;
}
