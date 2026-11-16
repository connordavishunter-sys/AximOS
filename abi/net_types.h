#pragma once

#include <stdint.h>

// Shared by the kernel and libc. Addresses are in host byte order.
struct net_info {
    bool has_device;
    bool automatic;     // set by DHCP rather than by hand
    bool airplane;      // sending and taking nothing, in airplane mode
    uint8_t mac[6];
    uint32_t address;   // 0 until there is one
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
};
