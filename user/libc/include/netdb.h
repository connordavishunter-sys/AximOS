#pragma once

#include <net_types.h>
#include <netinet/in.h>

// Lookups try numbers, then /system/etc/hosts, then the name server. Only IPv4 exists.

#define EAI_FAIL -4
#define EAI_NONAME -2
#define EAI_SERVICE -8
#define EAI_FAMILY -6
#define EAI_MEMORY -10
#define EAI_SYSTEM -11
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NAMEREQD 8
#define NI_DGRAM 16
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define HOST_NOT_FOUND 1

extern int h_errno;
const char *hstrerror(int code);

struct addrinfo {
    int ai_flags;
    int ai_family;
    int ai_socktype;
    int ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};

struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};
#define h_addr h_addr_list[0]

struct servent {
    char *s_name;
    char **s_aliases;
    int s_port;
    char *s_proto;
};

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **out);
void freeaddrinfo(struct addrinfo *list);
const char *gai_strerror(int code);
int getnameinfo(const struct sockaddr *address, socklen_t size, char *host, socklen_t host_size,
                char *service, socklen_t service_size, int flags);
struct hostent *gethostbyname(const char *name);
struct servent *getservbyname(const char *name, const char *protocol);

// Not POSIX: the network's configuration, and setting it by hand. An address of 0
// goes back to asking DHCP.
int net_info(struct net_info *out);
int net_set(uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns);
