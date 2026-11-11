#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>
#include "syscall.h"

#define DNS_PORT 53
#define DNS_HEADER_SIZE 12
#define DNS_MAX_MESSAGE 1024
#define DNS_TYPE_A 1
#define DNS_CLASS_IN 1
#define DNS_FLAG_RESPONSE 0x8000
#define DNS_FLAG_RECURSION 0x0100
#define DNS_RCODE_MASK 0xF
#define DNS_POINTER 0xC0
#define DNS_MAX_ADDRESSES 8
#define DNS_TRIES 3
#define DNS_WAIT_MS 2000
#define DNS_NAME_MAX 255

int h_errno;

int net_info(struct net_info *out) {
    long result = syscall3(SYS_NET_INFO, (long)out, 0, 0);

    if (result < 0) {
        errno = -result;
        return -1;
    }
    return 0;
}

int net_set(uint32_t address, uint32_t netmask, uint32_t gateway, uint32_t dns) {
    long result = syscall6(SYS_NET_SET, address, netmask, gateway, dns, 0, 0);

    if (result < 0) {
        errno = -result;
        return -1;
    }
    return 0;
}

static uint16_t read16(const uint8_t *p) {
    return (uint16_t)(p[0] << 8 | p[1]);
}

// Writes a name as its labels, returning the length or 0 if it can't be one.
static size_t encode_name(const char *name, uint8_t *out) {
    size_t len = strlen(name);
    size_t used = 0;

    if (len > 0 && name[len - 1] == '.')
        len--;
    if (len == 0 || len > DNS_NAME_MAX - 2)
        return 0;

    for (size_t start = 0; start < len;) {
        size_t end = start;

        while (end < len && name[end] != '.')
            end++;
        if (end == start || end - start > 63)
            return 0;

        out[used++] = end - start;
        memcpy(&out[used], &name[start], end - start);
        used += end - start;
        start = end + 1;
    }
    out[used++] = 0;
    return used;
}

// Moves past a name in a message, returning where it ends or 0 if it's malformed.
static size_t skip_name(const uint8_t *message, size_t size, size_t at) {
    while (at < size) {
        if ((message[at] & DNS_POINTER) == DNS_POINTER)
            return at + 2 <= size ? at + 2 : 0;
        if (message[at] & DNS_POINTER)
            return 0;
        if (message[at] == 0)
            return at + 1;
        at += message[at] + 1;
    }
    return 0;
}

// Returns how many addresses it found, or -1 for a malformed or failed answer. An
// alias comes with the addresses it leads to, so only those need looking at.
static int parse_answer(const uint8_t *message, size_t size, uint16_t id, in_addr_t *out) {
    if (size < DNS_HEADER_SIZE || read16(message) != id || !(read16(message + 2) & DNS_FLAG_RESPONSE)
        || (read16(message + 2) & DNS_RCODE_MASK))
        return -1;

    int questions = read16(message + 4);
    int answers = read16(message + 6);
    size_t at = DNS_HEADER_SIZE;
    int found = 0;

    for (int i = 0; i < questions; i++) {
        at = skip_name(message, size, at);
        if (at == 0 || at + 4 > size)
            return -1;
        at += 4;
    }
    for (int i = 0; i < answers; i++) {
        at = skip_name(message, size, at);
        if (at == 0 || at + 10 > size)
            return -1;

        uint16_t type = read16(message + at);
        uint16_t class = read16(message + at + 2);
        uint16_t length = read16(message + at + 8);

        at += 10;
        if (at + length > size)
            return -1;
        if (type == DNS_TYPE_A && class == DNS_CLASS_IN && length == 4 && found < DNS_MAX_ADDRESSES)
            memcpy(&out[found++], message + at, 4);
        at += length;
    }
    return found;
}

// Asks the name server, trying a few times since nothing else will notice a lost packet.
static int dns_lookup(const char *name, in_addr_t *out) {
    struct net_info info;

    if (net_info(&info) < 0 || info.dns == 0)
        return -1;

    uint8_t query[DNS_HEADER_SIZE + DNS_NAME_MAX + 4] = {0};
    size_t name_len = encode_name(name, query + DNS_HEADER_SIZE);

    if (name_len == 0)
        return -1;

    uint16_t id;

    arc4random_buf(&id, sizeof(id));
    uint8_t *q = query + DNS_HEADER_SIZE + name_len;

    query[0] = id >> 8;
    query[1] = id;
    query[2] = DNS_FLAG_RECURSION >> 8;
    query[5] = 1;
    q[1] = DNS_TYPE_A;
    q[3] = DNS_CLASS_IN;

    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return -1;

    struct sockaddr_in server = {.sin_family = AF_INET, .sin_port = htons(DNS_PORT), .sin_addr = {htonl(info.dns)}};
    int found = -1;

    if (connect(fd, (struct sockaddr *)&server, sizeof(server)) == 0) {
        for (int attempt = 0; attempt < DNS_TRIES && found < 0; attempt++) {
            struct pollfd wait = {.fd = fd, .events = POLLIN};
            static uint8_t answer[DNS_MAX_MESSAGE];

            if (send(fd, query, DNS_HEADER_SIZE + name_len + 4, 0) < 0)
                break;
            while (found < 0 && poll(&wait, 1, DNS_WAIT_MS) > 0) {
                ssize_t got = recv(fd, answer, sizeof(answer), 0);

                if (got < 0)
                    break;

                // Anything that isn't the answer gets ignored. A failed one is final.
                if ((size_t)got >= DNS_HEADER_SIZE && read16(answer) == id) {
                    found = parse_answer(answer, got, id, out);
                    if (found < 0)
                        attempt = DNS_TRIES;
                    break;
                }
            }
        }
    }
    close(fd);
    return found;
}

// /system/etc/hosts has lines of an address and the names it goes by.
static bool hosts_lookup(const char *name, in_addr_t *out) {
    FILE *file = fopen("/system/etc/hosts", "r");
    char line[256];
    bool found = false;

    if (!file)
        return false;

    while (!found && fgets(line, sizeof(line), file)) {
        char *comment = strchr(line, '#');

        if (comment)
            *comment = '\0';

        char *word = strtok(line, " \t\n");
        struct in_addr address;

        if (!word || !inet_aton(word, &address))
            continue;
        while (!found && (word = strtok(NULL, " \t\n"))) {
            if (strcasecmp(word, name) == 0) {
                *out = address.s_addr;
                found = true;
            }
        }
    }
    fclose(file);
    return found;
}

// Returns how many addresses a name has, or 0 if none can be found.
static int resolve(const char *name, in_addr_t *out) {
    struct in_addr numeric;

    if (inet_aton(name, &numeric)) {
        *out = numeric.s_addr;
        return 1;
    }
    if (strcasecmp(name, "localhost") == 0) {
        *out = htonl(INADDR_LOOPBACK);
        return 1;
    }
    if (hosts_lookup(name, out))
        return 1;

    int found = dns_lookup(name, out);

    return found > 0 ? found : 0;
}

static const struct servent_entry {
    const char *name;
    int port;
} known_services[] = {
    {"ftp", 21}, {"ssh", 22}, {"telnet", 23}, {"smtp", 25}, {"domain", 53},
    {"http", 80}, {"pop3", 110}, {"ntp", 123}, {"imap", 143}, {"https", 443},
};

static int service_port(const char *service) {
    for (size_t i = 0; i < sizeof(known_services) / sizeof(known_services[0]); i++) {
        if (strcmp(known_services[i].name, service) == 0)
            return known_services[i].port;
    }
    return -1;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **out) {
    int flags = hints ? hints->ai_flags : 0;
    int family = hints ? hints->ai_family : AF_UNSPEC;
    int type = hints ? hints->ai_socktype : 0;
    int protocol = hints ? hints->ai_protocol : 0;
    int port = 0;

    *out = NULL;
    if (!node && !service)
        return EAI_NONAME;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;

    if (service) {
        char *end;
        long number = strtol(service, &end, 10);

        port = *service && !*end ? number : service_port(service);
        if (port < 0 || port > 65535)
            return EAI_SERVICE;
    }

    in_addr_t addresses[DNS_MAX_ADDRESSES];
    int count;

    if (!node) {
        addresses[0] = htonl(flags & AI_PASSIVE ? INADDR_ANY : INADDR_LOOPBACK);
        count = 1;
    } else if (flags & AI_NUMERICHOST) {
        struct in_addr numeric;

        if (!inet_aton(node, &numeric))
            return EAI_NONAME;
        addresses[0] = numeric.s_addr;
        count = 1;
    } else {
        count = resolve(node, addresses);
        if (count == 0)
            return EAI_NONAME;
    }

    static const int types[][2] = {{SOCK_STREAM, IPPROTO_TCP}, {SOCK_DGRAM, IPPROTO_UDP}};
    struct addrinfo **link = out;

    for (int i = 0; i < count; i++) {
        for (size_t t = 0; t < sizeof(types) / sizeof(types[0]); t++) {
            if ((type && type != types[t][0]) || (protocol && protocol != types[t][1]))
                continue;

            struct addrinfo *entry = calloc(1, sizeof(*entry) + sizeof(struct sockaddr_in));

            if (!entry) {
                freeaddrinfo(*out);
                *out = NULL;
                return EAI_MEMORY;
            }

            struct sockaddr_in *address = (struct sockaddr_in *)(entry + 1);

            address->sin_family = AF_INET;
            address->sin_port = htons(port);
            address->sin_addr.s_addr = addresses[i];
            entry->ai_family = AF_INET;
            entry->ai_socktype = types[t][0];
            entry->ai_protocol = types[t][1];
            entry->ai_addrlen = sizeof(*address);
            entry->ai_addr = (struct sockaddr *)address;
            *link = entry;
            link = &entry->ai_next;
        }
    }
    return *out ? 0 : EAI_SERVICE;
}

void freeaddrinfo(struct addrinfo *list) {
    while (list) {
        struct addrinfo *next = list->ai_next;

        free(list);
        list = next;
    }
}

const char *gai_strerror(int code) {
    switch (code) {
    case EAI_NONAME: return "Name not found";
    case EAI_SERVICE: return "Unknown service";
    case EAI_FAMILY: return "Address family not supported";
    case EAI_MEMORY: return "Out of memory";
    case EAI_SYSTEM: return strerror(errno);
    }
    return "Lookup failed";
}

// Addresses are only ever written as numbers, as asking for a name is rarely answered.
int getnameinfo(const struct sockaddr *address, socklen_t size, char *host, socklen_t host_size,
                char *service, socklen_t service_size, int flags) {
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;

    if (size < sizeof(*in) || in->sin_family != AF_INET)
        return EAI_FAMILY;
    if (flags & NI_NAMEREQD)
        return EAI_NONAME;
    if (host && !inet_ntop(AF_INET, &in->sin_addr, host, host_size))
        return EAI_FAIL;
    if (service)
        snprintf(service, service_size, "%d", ntohs(in->sin_port));
    return 0;
}

struct hostent *gethostbyname(const char *name) {
    static in_addr_t addresses[DNS_MAX_ADDRESSES];
    static char *pointers[DNS_MAX_ADDRESSES + 1];
    static char saved_name[DNS_NAME_MAX + 1];
    static struct hostent entry;
    int count = resolve(name, addresses);

    if (count == 0) {
        h_errno = HOST_NOT_FOUND;
        return NULL;
    }

    static char *no_aliases[] = {NULL};

    for (int i = 0; i < count; i++)
        pointers[i] = (char *)&addresses[i];
    pointers[count] = NULL;
    snprintf(saved_name, sizeof(saved_name), "%s", name);
    entry = (struct hostent){saved_name, no_aliases, AF_INET, sizeof(in_addr_t), pointers};
    return &entry;
}

struct servent *getservbyname(const char *name, const char *protocol) {
    static struct servent entry;
    static char *no_aliases[] = {NULL};
    int port = service_port(name);

    if (port < 0)
        return NULL;

    entry = (struct servent){(char *)name, no_aliases, htons(port), (char *)(protocol ? protocol : "tcp")};
    return &entry;
}

const char *hstrerror(int code) {
    return code == HOST_NOT_FOUND ? "Host not found" : "Lookup failed";
}

int inet_aton(const char *text, struct in_addr *out) {
    unsigned parts[4];
    char end;

    if (sscanf(text, "%u.%u.%u.%u%c", &parts[0], &parts[1], &parts[2], &parts[3], &end) != 4)
        return 0;

    in_addr_t value = 0;

    for (int i = 0; i < 4; i++) {
        if (parts[i] > 255)
            return 0;
        value = value << 8 | parts[i];
    }
    out->s_addr = htonl(value);
    return 1;
}

in_addr_t inet_addr(const char *text) {
    struct in_addr address;

    return inet_aton(text, &address) ? address.s_addr : INADDR_NONE;
}

char *inet_ntoa(struct in_addr address) {
    static char text[INET_ADDRSTRLEN];

    inet_ntop(AF_INET, &address, text, sizeof(text));
    return text;
}

const char *inet_ntop(int family, const void *address, char *out, socklen_t size) {
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return NULL;
    }

    const unsigned char *b = address;

    if ((socklen_t)snprintf(out, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]) >= size) {
        errno = ENOSPC;
        return NULL;
    }
    return out;
}

int inet_pton(int family, const char *text, void *out) {
    if (family != AF_INET) {
        errno = EAFNOSUPPORT;
        return -1;
    }
    return inet_aton(text, out);
}
