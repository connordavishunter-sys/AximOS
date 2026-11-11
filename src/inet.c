#include <errno_values.h>
#include <fs_types.h>
#include <inet.h>
#include <kmalloc.h>
#include <net.h>
#include <process.h>
#include <string.h>
#include <tcp.h>

#define UDP_HEADER 8
#define ICMP_HEADER 8
#define ICMP_ECHO_REQUEST 8
#define MAX_DATAGRAM (NET_MAX_PAYLOAD - UDP_HEADER)
#define MAX_QUEUED_BYTES (256 * 1024)
#define FIRST_EPHEMERAL_PORT 49152
#define LAST_EPHEMERAL_PORT 65535

struct datagram {
    struct datagram *next;
    uint32_t source;
    uint16_t source_port;
    uint32_t len;
    uint8_t data[];
};

struct inet_socket {
    struct inet_socket *next; // in the list of those with a port
    int type;
    int protocol;
    uint32_t local;
    uint16_t local_port;
    uint32_t remote;
    uint16_t remote_port;
    bool connected;
    bool read_closed;
    int error;
    size_t queued_bytes;
    struct datagram *head;
    struct datagram *tail;
    struct tcp *tcp;
};

static struct inet_socket *with_port;
static uint16_t next_port = FIRST_EPHEMERAL_PORT;

int inet_create(int type, int protocol, struct inet_socket **out) {
    if (type != SOCK_STREAM && type != SOCK_DGRAM)
        return -ESOCKTNOSUPPORT;

    int expected = type == SOCK_STREAM ? IP_PROTO_TCP : IP_PROTO_UDP;

    if (protocol != 0 && protocol != expected && !(type == SOCK_DGRAM && protocol == IP_PROTO_ICMP))
        return -EPROTONOSUPPORT;

    struct inet_socket *socket = kmalloc(sizeof(*socket));

    if (!socket)
        return -ENOMEM;

    *socket = (struct inet_socket){.type = type, .protocol = protocol ? protocol : expected};
    if (type == SOCK_STREAM) {
        socket->tcp = tcp_create();
        if (!socket->tcp) {
            kfree(socket);
            return -ENOMEM;
        }
    }
    *out = socket;
    return 0;
}

static void add_to_port_list(struct inet_socket *socket) {
    socket->next = with_port;
    with_port = socket;
}

static void remove_from_port_list(struct inet_socket *socket) {
    for (struct inet_socket **link = &with_port; *link; link = &(*link)->next) {
        if (*link == socket) {
            *link = socket->next;
            return;
        }
    }
}

void inet_close(struct inet_socket *socket) {
    remove_from_port_list(socket);
    while (socket->head) {
        struct datagram *d = socket->head;

        socket->head = d->next;
        kfree(d);
    }
    if (socket->tcp)
        tcp_close(socket->tcp);
    process_wakeup(socket);
    kfree(socket);
}

// Whether anything of the same protocol is using the port.
static bool port_taken(int protocol, uint16_t port) {
    for (struct inet_socket *s = with_port; s; s = s->next) {
        if (s->protocol == protocol && s->local_port == port)
            return true;
    }
    return protocol == IP_PROTO_TCP && tcp_port_in_use(port);
}

static uint16_t pick_port(int protocol) {
    for (int tries = LAST_EPHEMERAL_PORT - FIRST_EPHEMERAL_PORT + 1; tries > 0; tries--) {
        uint16_t port = next_port;

        next_port = next_port == LAST_EPHEMERAL_PORT ? FIRST_EPHEMERAL_PORT : next_port + 1;
        if (!port_taken(protocol, port))
            return port;
    }
    return 0;
}

static bool valid_family(const struct sockaddr_in *address) {
    return address->sin_family == AF_INET;
}

int inet_bind(struct inet_socket *socket, const struct sockaddr_in *address) {
    if (!valid_family(address))
        return -EAFNOSUPPORT;
    if (socket->local_port)
        return -EINVAL;

    uint32_t local = __builtin_bswap32(address->sin_addr.s_addr);
    uint16_t port = __builtin_bswap16(address->sin_port);

    if (local != 0 && !net_is_local(local) && local != NET_BROADCAST)
        return -EADDRNOTAVAIL;

    // Connections left over on the port don't stop a new listener, which is the case
    // SO_REUSEADDR exists for. They're told apart by who they're connected to.
    for (struct inet_socket *s = with_port; port && s; s = s->next) {
        if (s->protocol == socket->protocol && s->local_port == port)
            return -EADDRINUSE;
    }

    if (port == 0)
        port = pick_port(socket->protocol);
    if (port == 0)
        return -EADDRINUSE;

    socket->local = local;
    socket->local_port = port;
    add_to_port_list(socket);
    return 0;
}

int inet_listen(struct inet_socket *socket, int backlog) {
    if (socket->type != SOCK_STREAM)
        return -EOPNOTSUPP;
    if (tcp_is_connected(socket->tcp))
        return -EISCONN;
    if (!socket->local_port) {
        socket->local_port = pick_port(IP_PROTO_TCP);
        if (!socket->local_port)
            return -EADDRINUSE;
        add_to_port_list(socket);
    }
    return tcp_listen(socket->tcp, socket->local, socket->local_port, backlog);
}

int inet_connect(struct inet_socket *socket, const struct sockaddr_in *address, bool nonblocking) {
    if (!valid_family(address))
        return address->sin_family == AF_UNSPEC && socket->type == SOCK_DGRAM ? (socket->connected = false, 0) : -EAFNOSUPPORT;

    uint32_t remote = __builtin_bswap32(address->sin_addr.s_addr);
    uint16_t remote_port = __builtin_bswap16(address->sin_port);

    // As on other systems, nowhere in particular means here.
    if (remote == 0)
        remote = 0x7F000001u;

    uint32_t local = socket->local ? socket->local : net_source_for(remote);

    if (!local)
        return -ENETUNREACH;

    if (!socket->local_port) {
        socket->local_port = pick_port(socket->protocol);
        if (!socket->local_port)
            return -EADDRNOTAVAIL;
        add_to_port_list(socket);
    }

    if (socket->type == SOCK_DGRAM) {
        socket->remote = remote;
        socket->remote_port = remote_port;
        socket->connected = true;
        return 0;
    }

    // The listening list only matters to bind, so a connected socket leaves it.
    int status = tcp_connect(socket->tcp, local, socket->local_port, remote, remote_port, nonblocking);

    if (status == 0 || status == -EINPROGRESS) {
        remove_from_port_list(socket);
        socket->next = NULL;
    }
    return status;
}

static struct sockaddr_in make_address(uint32_t address, uint16_t port) {
    return (struct sockaddr_in){
        .sin_family = AF_INET,
        .sin_port = __builtin_bswap16(port),
        .sin_addr = {__builtin_bswap32(address)},
    };
}

int inet_accept(struct inet_socket *socket, bool nonblocking, struct inet_socket **out, struct sockaddr_in *peer) {
    if (socket->type != SOCK_STREAM)
        return -EOPNOTSUPP;

    struct inet_socket *connection = kmalloc(sizeof(*connection));

    if (!connection)
        return -ENOMEM;

    struct tcp *tcp;
    int status = tcp_accept(socket->tcp, nonblocking, &tcp);

    if (status < 0) {
        kfree(connection);
        return status;
    }

    uint32_t local, remote;
    uint16_t local_port, remote_port;

    tcp_addresses(tcp, &local, &local_port, &remote, &remote_port);
    *connection = (struct inet_socket){.type = SOCK_STREAM, .protocol = IP_PROTO_TCP, .tcp = tcp};
    *peer = make_address(remote, remote_port);
    *out = connection;
    return 0;
}

int inet_shutdown(struct inet_socket *socket, int how) {
    if (socket->tcp)
        return tcp_shutdown(socket->tcp, how);
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return -EINVAL;
    if (!socket->connected)
        return -ENOTCONN;
    if (how != SHUT_WR) {
        socket->read_closed = true;
        process_wakeup(socket);
    }
    return 0;
}

int inet_name(const struct inet_socket *socket, bool peer, struct sockaddr_in *out) {
    uint32_t local = socket->local, remote = socket->remote;
    uint16_t local_port = socket->local_port, remote_port = socket->remote_port;

    if (socket->tcp && (tcp_is_connected(socket->tcp) || !local_port))
        tcp_addresses(socket->tcp, &local, &local_port, &remote, &remote_port);
    if (peer && !(socket->tcp ? tcp_is_connected(socket->tcp) : socket->connected))
        return -ENOTCONN;

    *out = peer ? make_address(remote, remote_port) : make_address(local, local_port);
    return 0;
}

static int64_t send_datagram(struct inet_socket *socket, const void *buf, size_t len, uint32_t remote, uint16_t remote_port) {
    uint8_t frame[NET_FRAME_SIZE];
    uint8_t *out = frame + NET_HEADROOM;
    uint32_t local = socket->local && socket->local != NET_BROADCAST ? socket->local : net_source_for(remote);

    if (!local)
        return -ENETUNREACH;

    int status;

    if (socket->protocol == IP_PROTO_ICMP) {
        if (len < ICMP_HEADER || len > NET_MAX_PAYLOAD)
            return len < ICMP_HEADER ? -EINVAL : -EMSGSIZE;
        if (((const uint8_t *)buf)[0] != ICMP_ECHO_REQUEST)
            return -EINVAL;

        // The port takes the place of the identifier, as nothing else tells pings apart.
        memcpy(out, buf, len);
        net_write16(out + 4, socket->local_port);
        net_write16(out + 2, 0);
        net_write16(out + 2, net_checksum(out, len, 0));
        status = net_send_ip(frame, len, IP_PROTO_ICMP, local, remote);
    } else {
        if (len > MAX_DATAGRAM)
            return -EMSGSIZE;

        net_write16(out, socket->local_port);
        net_write16(out + 2, remote_port);
        net_write16(out + 4, UDP_HEADER + len);
        net_write16(out + 6, 0);
        memcpy(out + UDP_HEADER, buf, len);

        uint16_t checksum = net_payload_checksum(local, remote, IP_PROTO_UDP, out, UDP_HEADER + len);

        net_write16(out + 6, checksum ? checksum : 0xFFFF);
        status = net_send_ip(frame, UDP_HEADER + len, IP_PROTO_UDP, local, remote);
    }
    return status < 0 ? status : (int64_t)len;
}

int64_t inet_send(struct inet_socket *socket, const void *buf, size_t len, const struct sockaddr_in *to, bool nonblocking, bool raise_sigpipe) {
    if (socket->tcp) {
        if (to && tcp_is_connected(socket->tcp))
            return -EISCONN;
        return tcp_write(socket->tcp, buf, len, nonblocking, raise_sigpipe);
    }

    if (socket->error) {
        int error = socket->error;

        socket->error = 0;
        return -error;
    }

    uint32_t remote = socket->remote;
    uint16_t remote_port = socket->remote_port;

    if (to) {
        if (!valid_family(to))
            return -EAFNOSUPPORT;
        remote = __builtin_bswap32(to->sin_addr.s_addr);
        remote_port = __builtin_bswap16(to->sin_port);
        if (remote == 0)
            remote = 0x7F000001u;
    } else if (!socket->connected) {
        return -EDESTADDRREQ;
    }

    if (!socket->local_port) {
        socket->local_port = pick_port(socket->protocol);
        if (!socket->local_port)
            return -EAGAIN;
        add_to_port_list(socket);
    }
    return send_datagram(socket, buf, len, remote, remote_port);
}

int64_t inet_receive(struct inet_socket *socket, void *buf, size_t len, struct sockaddr_in *from, bool nonblocking, bool peek) {
    if (socket->tcp) {
        int64_t got = tcp_read(socket->tcp, buf, len, nonblocking, peek);

        if (got >= 0 && from)
            inet_name(socket, true, from);
        return got;
    }

    while (!socket->head) {
        if (socket->error) {
            int error = socket->error;

            socket->error = 0;
            return -error;
        }
        if (socket->read_closed)
            return 0;
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep(socket))
            return -EINTR;
    }

    struct datagram *d = socket->head;
    size_t copied = len < d->len ? len : d->len;

    memcpy(buf, d->data, copied);
    if (from)
        *from = make_address(d->source, d->source_port);
    if (!peek) {
        socket->head = d->next;
        if (!socket->head)
            socket->tail = NULL;
        socket->queued_bytes -= d->len;
        kfree(d);
    }
    return copied;
}

int inet_poll(const struct inet_socket *socket) {
    if (socket->tcp)
        return tcp_poll(socket->tcp);

    int ready = POLLOUT;

    if (socket->head || socket->read_closed)
        ready |= POLLIN;
    if (socket->error)
        ready |= POLLIN | POLLERR;
    return ready;
}

size_t inet_available(const struct inet_socket *socket) {
    if (socket->tcp)
        return tcp_available(socket->tcp);
    return socket->head ? socket->head->len : 0;
}

int inet_type(const struct inet_socket *socket) {
    return socket->type;
}

int inet_take_error(struct inet_socket *socket) {
    if (socket->tcp)
        return tcp_take_error(socket->tcp);

    int error = socket->error;

    socket->error = 0;
    return error;
}

static void enqueue(struct inet_socket *socket, uint32_t source, uint16_t source_port, const void *data, size_t len) {
    if (socket->read_closed || socket->queued_bytes + len > MAX_QUEUED_BYTES)
        return;

    struct datagram *d = kmalloc(sizeof(*d) + len);

    if (!d)
        return;

    d->next = NULL;
    d->source = source;
    d->source_port = source_port;
    d->len = len;
    memcpy(d->data, data, len);
    if (socket->tail)
        socket->tail->next = d;
    else
        socket->head = d;
    socket->tail = d;
    socket->queued_bytes += len;
    process_wakeup(socket);
}

void inet_deliver_udp(uint32_t source, uint16_t source_port, uint32_t destination, uint16_t port, const void *data, size_t len) {
    for (struct inet_socket *s = with_port; s; s = s->next) {
        if (s->protocol != IP_PROTO_UDP || s->local_port != port)
            continue;
        if (s->local && s->local != destination && destination != NET_BROADCAST)
            continue;
        if (s->connected && (s->remote != source || s->remote_port != source_port))
            continue;

        enqueue(s, source, source_port, data, len);
        return;
    }

    // Nobody's there, and the sender should know unless the whole network was asked.
    if (destination != NET_BROADCAST && net_is_local(destination)) {
        uint8_t frame[NET_FRAME_SIZE];
        uint8_t *out = frame + NET_HEADROOM;
        size_t quoted = 20 + UDP_HEADER;

        memset(out, 0, ICMP_HEADER);
        out[0] = 3;
        out[1] = 3;

        uint8_t *ip = out + ICMP_HEADER;

        memset(ip, 0, 20);
        ip[0] = 0x45;
        ip[9] = IP_PROTO_UDP;
        net_write16(ip + 2, 20 + UDP_HEADER + len);
        net_write32(ip + 12, source);
        net_write32(ip + 16, destination);
        net_write16(ip + 20, source_port);
        net_write16(ip + 22, port);
        net_write16(ip + 24, UDP_HEADER + len);
        net_write16(ip + 10, net_checksum(ip, 20, 0));
        net_write16(out + 2, net_checksum(out, ICMP_HEADER + quoted, 0));
        net_send_ip(frame, ICMP_HEADER + quoted, IP_PROTO_ICMP, destination, source);
    }
}

void inet_deliver_echo_reply(uint32_t source, uint16_t id, const void *message, size_t len) {
    for (struct inet_socket *s = with_port; s; s = s->next) {
        if (s->protocol == IP_PROTO_ICMP && s->local_port == id) {
            enqueue(s, source, 0, message, len);
            return;
        }
    }
}

void inet_port_unreachable(uint32_t destination, uint16_t our_port, uint16_t their_port) {
    for (struct inet_socket *s = with_port; s; s = s->next) {
        if (s->protocol == IP_PROTO_UDP && s->connected && s->local_port == our_port
            && s->remote == destination && s->remote_port == their_port) {
            s->error = ECONNREFUSED;
            process_wakeup(s);
            return;
        }
    }
}
