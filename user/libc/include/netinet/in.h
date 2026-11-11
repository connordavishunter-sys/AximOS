#pragma once

#include <byteswap.h>
#include <sys/socket.h>

#define IPPROTO_IP 0
#define IPPROTO_ICMP 1
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define IPPROTO_IPV6 41

#define TCP_NODELAY 1

#define INADDR_ANY 0u
#define INADDR_BROADCAST 0xFFFFFFFFu
#define INADDR_NONE 0xFFFFFFFFu
#define INADDR_LOOPBACK 0x7F000001u

#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46

typedef uint16_t in_port_t;
typedef uint32_t in_addr_t;

struct in6_addr {
    unsigned char s6_addr[16];
};

struct sockaddr_in6 {
    sa_family_t sin6_family;
    in_port_t sin6_port;
    uint32_t sin6_flowinfo;
    struct in6_addr sin6_addr;
    uint32_t sin6_scope_id;
};

#define htons(x) bswap_16(x)
#define ntohs(x) bswap_16(x)
#define htonl(x) bswap_32(x)
#define ntohl(x) bswap_32(x)
