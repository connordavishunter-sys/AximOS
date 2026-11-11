#include <errno_values.h>
#include <fs_types.h>
#include <irq.h>
#include <kmalloc.h>
#include <mm.h>
#include <net.h>
#include <process.h>
#include <random.h>
#include <signal_numbers.h>
#include <socket_types.h>
#include <string.h>
#include <tcp.h>

#define FLAG_FIN 0x01
#define FLAG_SYN 0x02
#define FLAG_RST 0x04
#define FLAG_PSH 0x08
#define FLAG_ACK 0x10

#define HEADER_SIZE 20
#define OPTION_END 0
#define OPTION_NOP 1
#define OPTION_MSS 2
#define DEFAULT_MSS 536
#define OUR_MSS (NET_MAX_PAYLOAD - HEADER_SIZE)

#define RING_PAGES 16
#define RING_SIZE (RING_PAGES * PAGE_SIZE)
#define MAX_WINDOW 65535

#define RTO_MIN (TIMER_HZ / 5)
#define RTO_INITIAL (1 * TIMER_HZ)
#define RTO_MAX (60 * TIMER_HZ)
#define MAX_RETRIES 12
#define MAX_SYN_RETRIES 6
#define DUPLICATE_ACKS_TO_RETRANSMIT 3
#define TIME_WAIT_TICKS (30 * TIMER_HZ)
#define ORPHAN_FIN_WAIT_TICKS (60 * TIMER_HZ)
#define DELAYED_ACK_SEGMENTS 2
#define INITIAL_CWND_SEGMENTS 10
#define MAX_OUT_OF_ORDER 64

enum tcp_state {
    TCP_CLOSED,
    TCP_LISTEN,
    TCP_SYN_SENT,
    TCP_SYN_RECEIVED,
    TCP_ESTABLISHED,
    TCP_FIN_WAIT_1,
    TCP_FIN_WAIT_2,
    TCP_CLOSE_WAIT,
    TCP_CLOSING,
    TCP_LAST_ACK,
    TCP_TIME_WAIT,
};

// Bytes in pages of their own, as there's no way to allocate this much in one piece.
struct ring {
    uint8_t *pages[RING_PAGES];
    uint32_t head;
    uint32_t count;
};

// A segment that arrived ahead of one that's missing.
struct out_of_order {
    struct out_of_order *next;
    uint32_t seq;
    uint32_t len;
    bool fin;
    uint8_t data[];
};

struct tcp {
    struct tcp *next; // in the list of all connections
    enum tcp_state state;
    uint32_t local;
    uint32_t remote;
    uint16_t local_port;
    uint16_t remote_port;
    int error;           // what to report, and reset once reported
    bool orphan;         // the socket has closed
    bool fin_queued;     // nothing more will be written
    bool read_closed;    // no more will be read
    bool peer_finished;  // the other side won't send any more

    uint32_t snd_una;    // the oldest byte not acknowledged, which send.head holds
    uint32_t snd_nxt;    // the next one to send, which goes back when resending
    uint32_t snd_max;    // the highest that's been sent
    uint32_t snd_wl1;    // the segment that last set the window
    uint32_t snd_wl2;
    uint32_t snd_wnd;
    uint32_t rcv_nxt;
    uint32_t iss;
    uint16_t mss;
    uint32_t last_window; // what the other side was last told
    uint8_t unacked_segments;

    uint32_t cwnd;
    uint32_t ssthresh;
    uint32_t recover;     // where the data sent before the current loss ends
    bool recovering;
    uint8_t duplicate_acks;

    uint32_t srtt;        // in ticks times 8, or 0 before there's a measurement
    uint32_t rttvar;      // in ticks times 4
    uint32_t rto;
    uint32_t timed_seq;   // the segment being timed, if timing
    uint64_t timed_at;
    bool timing;

    uint64_t retransmit_at; // or 0
    uint64_t linger_until;  // when TIME_WAIT or an abandoned FIN_WAIT_2 ends
    int retries;

    struct ring send;
    struct ring receive;
    struct out_of_order *ahead; // in order of sequence
    int ahead_count;

    struct tcp *parent;        // the listener, until accepted
    struct tcp *accept_next;
    struct tcp *accept_head;   // for a listener, established and waiting to be accepted
    struct tcp *accept_tail;
    int backlog;
    int pending;               // what's queued or still shaking hands
};

static struct tcp *connections;

static bool seq_lt(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) < 0;
}

static bool seq_le(uint32_t a, uint32_t b) {
    return (int32_t)(a - b) <= 0;
}

static uint32_t min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

static bool ring_alloc(struct ring *r) {
    for (int i = 0; i < RING_PAGES; i++) {
        uint64_t phys = mm_alloc_kernel_page();

        if (!phys)
            return false;
        r->pages[i] = mm_phys_to_virt(phys);
    }
    return true;
}

static void ring_free(struct ring *r) {
    for (int i = 0; i < RING_PAGES; i++) {
        if (r->pages[i])
            mm_free_kernel_page(mm_virt_to_phys(r->pages[i]));
        r->pages[i] = NULL;
    }
    r->head = r->count = 0;
}

static void ring_copy_out(const struct ring *r, uint32_t offset, void *out, uint32_t len) {
    uint8_t *to = out;

    while (len > 0) {
        uint32_t at = (r->head + offset) % RING_SIZE;
        uint32_t part = min_u32(len, PAGE_SIZE - at % PAGE_SIZE);

        memcpy(to, r->pages[at / PAGE_SIZE] + at % PAGE_SIZE, part);
        to += part;
        offset += part;
        len -= part;
    }
}

static void ring_append(struct ring *r, const void *in, uint32_t len) {
    const uint8_t *from = in;

    while (len > 0) {
        uint32_t at = (r->head + r->count) % RING_SIZE;
        uint32_t part = min_u32(len, PAGE_SIZE - at % PAGE_SIZE);

        memcpy(r->pages[at / PAGE_SIZE] + at % PAGE_SIZE, from, part);
        from += part;
        r->count += part;
        len -= part;
    }
}

static void ring_drop(struct ring *r, uint32_t len) {
    r->head = (r->head + len) % RING_SIZE;
    r->count -= len;
}

static struct tcp *find(uint32_t local, uint16_t local_port, uint32_t remote, uint16_t remote_port) {
    struct tcp *listener = NULL;

    for (struct tcp *c = connections; c; c = c->next) {
        if (c->state == TCP_LISTEN) {
            if (c->local_port == local_port && (c->local == 0 || c->local == local))
                listener = c;
        } else if (c->state != TCP_CLOSED && c->local_port == local_port && c->remote_port == remote_port
                   && c->local == local && c->remote == remote) {
            return c;
        }
    }
    return listener;
}

bool tcp_port_in_use(uint16_t port) {
    for (struct tcp *c = connections; c; c = c->next) {
        if (c->state != TCP_CLOSED && c->local_port == port)
            return true;
    }
    return false;
}

static uint32_t initial_sequence(void) {
    uint32_t value;

    random_bytes(&value, sizeof(value));
    return value;
}

struct tcp *tcp_create(void) {
    struct tcp *c = kmalloc(sizeof(*c));

    if (!c)
        return NULL;

    *c = (struct tcp){.next = connections, .mss = DEFAULT_MSS, .rto = RTO_INITIAL};
    connections = c;
    return c;
}

static void free_ahead(struct tcp *c) {
    while (c->ahead) {
        struct out_of_order *next = c->ahead->next;

        kfree(c->ahead);
        c->ahead = next;
    }
    c->ahead_count = 0;
}

// Gone from the list, and everything it held given back. Nothing can refer to it.
static void destroy(struct tcp *c) {
    for (struct tcp **link = &connections; *link; link = &(*link)->next) {
        if (*link == c) {
            *link = c->next;
            break;
        }
    }
    free_ahead(c);
    ring_free(&c->send);
    ring_free(&c->receive);
    process_wakeup(c);
    kfree(c);
}

static bool has_buffers(const struct tcp *c) {
    return c->send.pages[0] && c->send.pages[RING_PAGES - 1] && c->receive.pages[RING_PAGES - 1];
}

static bool alloc_buffers(struct tcp *c) {
    return ring_alloc(&c->send) && ring_alloc(&c->receive);
}

static uint32_t advertised_window(const struct tcp *c) {
    return min_u32(RING_SIZE - c->receive.count, MAX_WINDOW);
}

static void send_raw(uint32_t source, uint16_t source_port, uint32_t destination, uint16_t destination_port,
                     uint32_t seq, uint32_t ack, uint8_t flags, uint16_t window, uint16_t mss,
                     const struct ring *data, uint32_t data_offset, uint32_t data_len) {
    uint8_t frame[NET_FRAME_SIZE];
    uint8_t *t = frame + NET_HEADROOM;
    size_t header = HEADER_SIZE + (mss ? 4 : 0);

    net_write16(t, source_port);
    net_write16(t + 2, destination_port);
    net_write32(t + 4, seq);
    net_write32(t + 8, ack);
    t[12] = header / 4 << 4;
    t[13] = flags;
    net_write16(t + 14, window);
    net_write16(t + 16, 0);
    net_write16(t + 18, 0);
    if (mss) {
        t[20] = OPTION_MSS;
        t[21] = 4;
        net_write16(t + 22, mss);
    }
    if (data_len)
        ring_copy_out(data, data_offset, t + header, data_len);

    size_t len = header + data_len;

    net_write16(t + 16, net_payload_checksum(source, destination, IP_PROTO_TCP, t, len));
    net_send_ip(frame, len, IP_PROTO_TCP, source, destination);
}

static void send_segment(struct tcp *c, uint32_t seq, uint8_t flags, uint32_t data_offset, uint32_t data_len) {
    uint32_t window = advertised_window(c);

    c->last_window = window;
    c->unacked_segments = 0;
    send_raw(c->local, c->local_port, c->remote, c->remote_port, seq, c->rcv_nxt, flags, window,
             flags & FLAG_SYN ? OUR_MSS : 0, &c->send, data_offset, data_len);
}

static void send_ack(struct tcp *c) {
    send_segment(c, c->snd_max, FLAG_ACK, 0, 0);
}

static void send_reset(uint32_t source, uint32_t destination, const uint8_t *t, size_t data_and_flags) {
    uint8_t flags = t[13];

    if (flags & FLAG_RST)
        return;
    if (flags & FLAG_ACK)
        send_raw(destination, net_read16(t + 2), source, net_read16(t), net_read32(t + 8), 0, FLAG_RST, 0, 0, NULL, 0, 0);
    else
        send_raw(destination, net_read16(t + 2), source, net_read16(t), 0, net_read32(t + 4) + data_and_flags,
                 FLAG_RST | FLAG_ACK, 0, 0, NULL, 0, 0);
}

// Where the data ends and a FIN, if any, goes.
static uint32_t data_end(const struct tcp *c) {
    return c->snd_una + c->send.count;
}

static void arm_retransmit(struct tcp *c) {
    c->retransmit_at = irq_ticks() + c->rto;
}

static void advance_next(struct tcp *c, uint32_t len) {
    c->snd_nxt += len;
    if (seq_lt(c->snd_max, c->snd_nxt))
        c->snd_max = c->snd_nxt;
}

static void output(struct tcp *c) {
    if (c->state < TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_2 || c->state == TCP_TIME_WAIT)
        return;

    for (;;) {
        uint32_t window = min_u32(c->cwnd, c->snd_wnd);
        uint32_t flight = c->snd_nxt - c->snd_una;
        uint32_t room = window > flight ? window - flight : 0;

        if (seq_lt(c->snd_nxt, data_end(c))) {
            uint32_t left = data_end(c) - c->snd_nxt;
            uint32_t len = min_u32(min_u32(left, c->mss), room);

            // Waits for room for a whole segment, if an answer is coming to say so.
            if (len == 0 || (len < c->mss && len < left && flight > 0))
                break;
            if (!c->timing) {
                c->timing = true;
                c->timed_seq = c->snd_nxt;
                c->timed_at = irq_ticks();
            }
            send_segment(c, c->snd_nxt, FLAG_ACK | (len == left ? FLAG_PSH : 0), c->snd_nxt - c->snd_una, len);
            advance_next(c, len);
        } else if (c->fin_queued && c->snd_nxt == data_end(c)) {
            send_segment(c, c->snd_nxt, FLAG_FIN | FLAG_ACK, 0, 0);
            advance_next(c, 1);
        } else {
            break;
        }
    }

    // Nothing is out but something's waiting on a closed window: this is what probes it.
    if (!c->retransmit_at && (c->snd_max != c->snd_una || c->send.count > 0))
        arm_retransmit(c);
}

static void finish(struct tcp *c, int error) {
    c->state = TCP_CLOSED;
    c->error = error;
    c->retransmit_at = 0;
    process_wakeup(c);
}

static void enter_time_wait(struct tcp *c) {
    c->state = TCP_TIME_WAIT;
    c->retransmit_at = 0;
    c->linger_until = irq_ticks() + TIME_WAIT_TICKS;
    if (c->orphan) {
        ring_free(&c->send);
        ring_free(&c->receive);
    }
    process_wakeup(c);
}

static void set_rto_from_rtt(struct tcp *c) {
    uint32_t rto = c->srtt / 8 + (c->rttvar ? c->rttvar : 1);

    c->rto = rto < RTO_MIN ? RTO_MIN : rto > RTO_MAX ? RTO_MAX : rto;
}

static void update_rtt(struct tcp *c, uint32_t measured) {
    if (c->srtt == 0) {
        c->srtt = measured * 8;
        c->rttvar = measured * 2;
    } else {
        int32_t difference = (int32_t)measured - (int32_t)(c->srtt / 8);
        uint32_t size = difference < 0 ? -difference : difference;

        c->rttvar += size;
        c->rttvar -= c->rttvar / 4;
        c->srtt = c->srtt + measured - c->srtt / 8;
    }

    set_rto_from_rtt(c);
}

static void enter_loss(struct tcp *c) {
    uint32_t flight = c->snd_max - c->snd_una;
    uint32_t half = flight / 2;

    c->ssthresh = half > 2u * c->mss ? half : 2u * c->mss;
}

static void process_ack(struct tcp *c, uint32_t ack, uint32_t seq, uint32_t window, bool has_data) {
    if (seq_lt(c->snd_una, ack) && seq_le(ack, c->snd_max)) {
        uint32_t acked = ack - c->snd_una;

        if (c->timing && seq_lt(c->timed_seq, ack)) {
            c->timing = false;
            update_rtt(c, irq_ticks() - c->timed_at + 1);
        }

        // What's acknowledged may include the FIN, which isn't in the ring.
        ring_drop(&c->send, min_u32(acked, c->send.count));
        c->snd_una = ack;
        if (seq_lt(c->snd_nxt, ack))
            c->snd_nxt = ack;
        c->retries = 0;
        c->duplicate_acks = 0;

        // Getting through means the waiting that built up after losses can stop.
        if (c->srtt)
            set_rto_from_rtt(c);

        if (c->recovering) {
            if (seq_le(c->recover, ack)) {
                c->recovering = false;
                c->cwnd = c->ssthresh;
            } else {
                // More of what was sent before the loss is missing.
                if (seq_lt(ack, data_end(c)))
                    send_segment(c, ack, FLAG_ACK, 0, min_u32(c->mss, data_end(c) - ack));
                c->cwnd = c->cwnd > acked ? c->cwnd - acked + c->mss : c->mss;
            }
        } else if (c->cwnd < c->ssthresh) {
            c->cwnd += min_u32(acked, c->mss);
        } else {
            c->cwnd += c->mss * c->mss / c->cwnd;
        }

        c->retransmit_at = 0;
        if (c->snd_una != c->snd_max)
            arm_retransmit(c);
        process_wakeup(c);
    } else if (ack == c->snd_una && !has_data && window == c->snd_wnd && c->snd_max != c->snd_una) {
        c->duplicate_acks++;
        if (c->recovering) {
            c->cwnd += c->mss;
        } else if (c->duplicate_acks == DUPLICATE_ACKS_TO_RETRANSMIT) {
            enter_loss(c);
            c->recover = c->snd_max;
            c->recovering = true;
            c->timing = false;
            send_segment(c, c->snd_una, FLAG_ACK, 0, min_u32(c->mss, c->send.count));
            c->cwnd = c->ssthresh + 3u * c->mss;
            arm_retransmit(c);
        }
    }

    if (seq_le(c->snd_una, ack) && (seq_lt(c->snd_wl1, seq) || (c->snd_wl1 == seq && seq_le(c->snd_wl2, ack)))) {
        bool opened = c->snd_wnd == 0 && window > 0;

        c->snd_wnd = window;
        c->snd_wl1 = seq;
        c->snd_wl2 = ack;
        if (opened)
            process_wakeup(c);
    }
}

static void read_mss_option(struct tcp *c, const uint8_t *t, size_t header) {
    for (size_t at = HEADER_SIZE; at < header;) {
        if (t[at] == OPTION_END)
            break;
        if (t[at] == OPTION_NOP) {
            at++;
            continue;
        }
        if (at + 2 > header || t[at + 1] < 2 || at + t[at + 1] > header)
            break;
        if (t[at] == OPTION_MSS && t[at + 1] == 4) {
            uint16_t mss = net_read16(t + at + 2);

            c->mss = mss == 0 ? DEFAULT_MSS : min_u32(mss, OUR_MSS);
        }
        at += t[at + 1];
    }
}

static void start_window(struct tcp *c, uint32_t seq, uint32_t ack, uint32_t window) {
    c->snd_wnd = window;
    c->snd_wl1 = seq;
    c->snd_wl2 = ack;
    c->cwnd = INITIAL_CWND_SEGMENTS * c->mss;
    c->ssthresh = MAX_WINDOW;
}

static void handle_listen(struct tcp *l, uint32_t source, uint32_t destination, const uint8_t *t, size_t header) {
    if (t[13] & FLAG_RST)
        return;
    if (t[13] & FLAG_ACK) {
        send_reset(source, destination, t, 0);
        return;
    }
    if (!(t[13] & FLAG_SYN) || l->pending >= l->backlog)
        return;

    struct tcp *c = tcp_create();

    if (!c)
        return;
    if (!alloc_buffers(c)) {
        destroy(c);
        return;
    }

    c->state = TCP_SYN_RECEIVED;
    c->local = destination;
    c->local_port = l->local_port;
    c->remote = source;
    c->remote_port = net_read16(t);
    c->parent = l;
    c->orphan = true; // until it's accepted, nothing else will close it
    c->rcv_nxt = net_read32(t + 4) + 1;
    c->iss = initial_sequence();
    c->snd_una = c->iss;
    c->snd_nxt = c->snd_max = c->iss + 1;
    read_mss_option(c, t, header);
    start_window(c, net_read32(t + 4), c->iss, net_read16(t + 14));
    l->pending++;

    send_segment(c, c->iss, FLAG_SYN | FLAG_ACK, 0, 0);
    arm_retransmit(c);
}

static void handle_syn_sent(struct tcp *c, const uint8_t *t, size_t header) {
    uint32_t seq = net_read32(t + 4);
    uint32_t ack = net_read32(t + 8);
    uint8_t flags = t[13];
    bool ack_ok = (flags & FLAG_ACK) && ack == c->iss + 1;

    if ((flags & FLAG_ACK) && !ack_ok) {
        if (!(flags & FLAG_RST))
            send_raw(c->local, c->local_port, c->remote, c->remote_port, ack, 0, FLAG_RST, 0, 0, NULL, 0, 0);
        return;
    }
    if (flags & FLAG_RST) {
        if (ack_ok)
            finish(c, ECONNREFUSED);
        return;
    }
    if (!(flags & FLAG_SYN))
        return;

    c->rcv_nxt = seq + 1;
    read_mss_option(c, t, header);
    start_window(c, seq, ack, net_read16(t + 14));

    if (ack_ok) {
        c->snd_una = ack;
        c->state = TCP_ESTABLISHED;
        c->retransmit_at = 0;
        c->retries = 0;
        send_ack(c);
        output(c);
        process_wakeup(c);
    } else {
        // Both opened at once.
        c->state = TCP_SYN_RECEIVED;
        send_segment(c, c->iss, FLAG_SYN | FLAG_ACK, 0, 0);
    }
}

static void become_established(struct tcp *c) {
    struct tcp *l = c->parent;

    c->state = TCP_ESTABLISHED;
    c->retransmit_at = 0;
    c->retries = 0;

    if (l) {
        if (l->accept_tail)
            l->accept_tail->accept_next = c;
        else
            l->accept_head = c;
        l->accept_tail = c;
        process_wakeup(l);
    }
    process_wakeup(c);
}

// Keeps a segment that's ahead of what's expected, unless it's already here.
static void hold_ahead(struct tcp *c, uint32_t seq, const uint8_t *data, uint32_t len, bool fin) {
    struct out_of_order **link = &c->ahead;

    while (*link && seq_lt((*link)->seq, seq))
        link = &(*link)->next;
    if ((*link && (*link)->seq == seq) || c->ahead_count == MAX_OUT_OF_ORDER)
        return;

    struct out_of_order *held = kmalloc(sizeof(*held) + len);

    if (!held)
        return;

    held->next = *link;
    held->seq = seq;
    held->len = len;
    held->fin = fin;
    memcpy(held->data, data, len);
    *link = held;
    c->ahead_count++;
}

// Takes what starts exactly where it should. Returns whether all of it fit.
static bool take_in_order(struct tcp *c, const uint8_t *data, uint32_t len, bool fin) {
    uint32_t room = RING_SIZE - c->receive.count;
    uint32_t keep = len < room ? len : room;

    if (keep > 0) {
        if (!c->read_closed)
            ring_append(&c->receive, data, keep);
        c->rcv_nxt += keep;
        c->unacked_segments++;
        process_wakeup(c);
    }
    if (keep < len)
        return false;

    if (fin) {
        c->rcv_nxt++;
        c->peer_finished = true;
        process_wakeup(c);

        switch (c->state) {
        case TCP_ESTABLISHED:
            c->state = TCP_CLOSE_WAIT;
            break;
        case TCP_FIN_WAIT_1:
            c->state = TCP_CLOSING;
            break;
        case TCP_FIN_WAIT_2:
            enter_time_wait(c);
            break;
        default:
            break;
        }
    }
    return true;
}

// What was waiting for the gap that just closed.
static void take_ahead(struct tcp *c) {
    while (c->ahead && seq_le(c->ahead->seq, c->rcv_nxt)) {
        struct out_of_order *held = c->ahead;
        uint32_t seen = c->rcv_nxt - held->seq;

        c->ahead = held->next;
        c->ahead_count--;
        if (seen < held->len || (seen == held->len && held->fin))
            take_in_order(c, held->data + seen, held->len - seen, held->fin);
        kfree(held);
    }
}

static void take_data(struct tcp *c, uint32_t seq, const uint8_t *data, uint32_t len, bool fin, bool pushed) {
    // Anything before what's expected has been seen before.
    if (seq_lt(seq, c->rcv_nxt)) {
        uint32_t seen = c->rcv_nxt - seq;

        data += seen;
        len -= seen;
        seq = c->rcv_nxt;
    }

    // The ACK this sends right away is what tells the other side something's missing.
    if (seq != c->rcv_nxt) {
        hold_ahead(c, seq, data, len, fin);
        send_ack(c);
        return;
    }

    // Nothing will ever read this.
    if (len > 0 && c->orphan && !c->parent) {
        send_raw(c->local, c->local_port, c->remote, c->remote_port, c->snd_max, 0, FLAG_RST, 0, 0, NULL, 0, 0);
        finish(c, ECONNRESET);
        return;
    }

    bool filled_gap = c->ahead != NULL;
    bool fit = take_in_order(c, data, len, fin);

    take_ahead(c);

    // The end of what was written means the other side may be waiting for an answer.
    if (pushed || fin || !fit || filled_gap || c->peer_finished || c->unacked_segments >= DELAYED_ACK_SEGMENTS)
        send_ack(c);
}

// Forgets one that was never accepted.
static void drop_child(struct tcp *c) {
    struct tcp *l = c->parent;
    struct tcp *before = NULL;

    for (struct tcp *q = l->accept_head; q; before = q, q = q->accept_next) {
        if (q != c)
            continue;
        if (before)
            before->accept_next = c->accept_next;
        else
            l->accept_head = c->accept_next;
        if (l->accept_tail == c)
            l->accept_tail = before;
        break;
    }
    l->pending--;
    destroy(c);
}

static void handle_open(struct tcp *c, const uint8_t *t, size_t len, size_t header) {
    uint32_t seq = net_read32(t + 4);
    uint32_t ack = net_read32(t + 8);
    uint8_t flags = t[13];
    uint32_t data_len = len - header;
    uint32_t segment_len = data_len + !!(flags & FLAG_SYN) + !!(flags & FLAG_FIN);
    uint32_t window = advertised_window(c);
    bool acceptable;

    if (c->state == TCP_TIME_WAIT)
        window = 1; // the buffers are gone, but a resent FIN still needs answering

    if (segment_len == 0)
        acceptable = window == 0 ? seq == c->rcv_nxt : seq_le(c->rcv_nxt, seq) && seq_lt(seq, c->rcv_nxt + window);
    else
        acceptable = window != 0 && seq_lt(seq, c->rcv_nxt + window) && seq_lt(c->rcv_nxt, seq + segment_len);

    if (!acceptable) {
        if (!(flags & FLAG_RST))
            send_ack(c);
        return;
    }

    if (flags & FLAG_RST) {
        if (c->parent) {
            drop_child(c);
        } else if (c->orphan) {
            destroy(c);
        } else if (c->state == TCP_TIME_WAIT) {
            finish(c, 0);
        } else {
            finish(c, c->state == TCP_CLOSE_WAIT ? EPIPE : ECONNRESET);
        }
        return;
    }
    if ((flags & FLAG_SYN) || !(flags & FLAG_ACK)) {
        if (flags & FLAG_SYN)
            send_ack(c);
        return;
    }

    if (c->state == TCP_TIME_WAIT) {
        c->linger_until = irq_ticks() + TIME_WAIT_TICKS;
        send_ack(c);
        return;
    }

    if (c->state == TCP_SYN_RECEIVED) {
        if (ack != c->snd_max) {
            send_reset(c->remote, c->local, t, 0);
            return;
        }
        c->snd_una = ack;
        become_established(c);
    }
    if (seq_lt(c->snd_max, ack)) {
        send_ack(c);
        return;
    }

    process_ack(c, ack, seq, net_read16(t + 14), segment_len != 0);

    bool fin_acked = c->fin_queued && c->snd_una == c->snd_max && c->send.count == 0;

    if (fin_acked && c->state == TCP_FIN_WAIT_1) {
        c->state = TCP_FIN_WAIT_2;
        if (c->orphan)
            c->linger_until = irq_ticks() + ORPHAN_FIN_WAIT_TICKS;
    } else if (fin_acked && c->state == TCP_CLOSING) {
        enter_time_wait(c);
        return;
    } else if (fin_acked && c->state == TCP_LAST_ACK) {
        if (c->orphan) {
            destroy(c);
        } else {
            finish(c, 0);
        }
        return;
    }

    bool can_receive = c->state == TCP_ESTABLISHED || c->state == TCP_FIN_WAIT_1 || c->state == TCP_FIN_WAIT_2;

    if (can_receive && segment_len) {
        take_data(c, seq, t + header, data_len, flags & FLAG_FIN, flags & FLAG_PSH);
        if (c->state == TCP_CLOSED) {
            if (c->orphan)
                destroy(c);
            return;
        }
    } else if (segment_len) {
        send_ack(c); // a resent FIN after ours crossed it
    }
    output(c);
}

void tcp_input(uint32_t source, uint32_t destination, const uint8_t *t, size_t len) {
    if (len < HEADER_SIZE || net_payload_checksum(source, destination, IP_PROTO_TCP, t, len) != 0)
        return;

    size_t header = (t[12] >> 4) * 4;

    if (header < HEADER_SIZE || header > len)
        return;

    struct tcp *c = find(destination, net_read16(t + 2), source, net_read16(t));

    if (!c) {
        size_t counted = len - header + !!(t[13] & FLAG_SYN) + !!(t[13] & FLAG_FIN);

        send_reset(source, destination, t, counted);
        return;
    }

    if (c->state == TCP_LISTEN)
        handle_listen(c, source, destination, t, header);
    else if (c->state == TCP_SYN_SENT)
        handle_syn_sent(c, t, header);
    else
        handle_open(c, t, len, header);
}

static void give_up(struct tcp *c) {
    if (c->parent) {
        drop_child(c);
    } else if (c->orphan) {
        destroy(c);
    } else {
        finish(c, ETIMEDOUT);
    }
}

static void retransmit(struct tcp *c) {
    bool handshake = c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED;
    bool probing = !handshake && c->snd_wnd == 0 && c->send.count > 0;

    // A window can stay closed for as long as the other side likes, unless nobody's left
    // to care what's in the buffer.
    if (!probing || c->orphan)
        c->retries++;
    if (c->retries > (handshake ? MAX_SYN_RETRIES : MAX_RETRIES)) {
        give_up(c);
        return;
    }

    c->rto = c->rto * 2 > RTO_MAX ? RTO_MAX : c->rto * 2;
    arm_retransmit(c);
    c->timing = false;

    if (handshake) {
        send_segment(c, c->iss, c->state == TCP_SYN_SENT ? FLAG_SYN : FLAG_SYN | FLAG_ACK, 0, 0);
    } else if (probing) {
        // The answer may acknowledge this byte, so it counts as sent.
        send_segment(c, c->snd_una, FLAG_ACK, 0, 1);
        if (c->snd_nxt == c->snd_una)
            advance_next(c, 1);
    } else {
        // Everything after the oldest is assumed lost too, and goes again a bit at a time.
        enter_loss(c);
        c->cwnd = c->mss;
        c->recovering = false;
        c->duplicate_acks = 0;
        c->snd_nxt = c->snd_una;
        output(c);
    }
}

void tcp_tick(void) {
    uint64_t now = irq_ticks();
    struct tcp *next;

    for (struct tcp *c = connections; c; c = next) {
        next = c->next;

        if (c->linger_until && now >= c->linger_until && (c->state == TCP_TIME_WAIT || c->state == TCP_FIN_WAIT_2)) {
            if (c->orphan) {
                destroy(c);
            } else {
                c->linger_until = 0;
                finish(c, 0);
            }
            continue;
        }
        if (c->retransmit_at && now >= c->retransmit_at) {
            c->retransmit_at = 0;
            retransmit(c);
            continue;
        }
        if (c->unacked_segments > 0 && has_buffers(c))
            send_ack(c);
    }
}

int tcp_connect(struct tcp *c, uint32_t local, uint16_t local_port, uint32_t remote, uint16_t remote_port, bool nonblocking) {
    if (c->state == TCP_SYN_SENT)
        return nonblocking ? -EALREADY : -EINPROGRESS;
    if (c->state == TCP_LISTEN)
        return -EOPNOTSUPP;
    if (c->state != TCP_CLOSED)
        return -EISCONN;

    if (!has_buffers(c) && !alloc_buffers(c))
        return -ENOMEM;

    c->local = local;
    c->local_port = local_port;
    c->remote = remote;
    c->remote_port = remote_port;
    c->error = 0;
    c->fin_queued = c->read_closed = c->peer_finished = false;
    c->state = TCP_SYN_SENT;
    c->iss = initial_sequence();
    c->snd_una = c->iss;
    c->snd_nxt = c->snd_max = c->iss + 1;
    c->retries = 0;
    c->rto = RTO_INITIAL;

    send_segment(c, c->iss, FLAG_SYN, 0, 0);
    arm_retransmit(c);

    if (nonblocking)
        return -EINPROGRESS;

    while (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED) {
        if (!process_sleep(c))
            return -EINTR;
    }
    if (c->state == TCP_CLOSED) {
        int error = c->error;

        c->error = 0;
        return -error;
    }
    return 0;
}

int tcp_listen(struct tcp *c, uint32_t local, uint16_t local_port, int backlog) {
    if (c->state != TCP_CLOSED && c->state != TCP_LISTEN)
        return -EINVAL;

    c->local = local;
    c->local_port = local_port;
    c->state = TCP_LISTEN;
    c->backlog = backlog < 1 ? 1 : backlog > SOMAXCONN ? SOMAXCONN : backlog;
    return 0;
}

int tcp_accept(struct tcp *c, bool nonblocking, struct tcp **out) {
    if (c->state != TCP_LISTEN)
        return -EINVAL;

    while (!c->accept_head) {
        if (nonblocking)
            return -EAGAIN;
        if (!process_sleep(c))
            return -EINTR;
        if (c->state != TCP_LISTEN)
            return -EINVAL;
    }

    struct tcp *accepted = c->accept_head;

    c->accept_head = accepted->accept_next;
    if (!c->accept_head)
        c->accept_tail = NULL;
    accepted->accept_next = NULL;
    accepted->parent = NULL;
    accepted->orphan = false;
    c->pending--;
    *out = accepted;
    return 0;
}

int64_t tcp_read(struct tcp *c, void *buf, size_t len, bool nonblocking, bool peek) {
    if (c->state == TCP_LISTEN)
        return -ENOTCONN;

    for (;;) {
        if (c->receive.count > 0 && !c->read_closed) {
            uint32_t got = len < c->receive.count ? len : c->receive.count;

            ring_copy_out(&c->receive, 0, buf, got);
            if (!peek) {
                ring_drop(&c->receive, got);

                // Someone could be waiting to hear there's room again, and it's worth
                // telling them once there's enough to be useful.
                if (advertised_window(c) >= c->last_window + 2u * c->mss)
                    send_ack(c);
            }
            return got;
        }
        if (c->error) {
            int error = c->error;

            c->error = 0;
            return -error;
        }
        if (c->peer_finished || c->read_closed || c->state == TCP_CLOSED) {
            bool never_connected = c->state == TCP_CLOSED && !has_buffers(c) && !c->peer_finished;

            return never_connected ? -ENOTCONN : 0;
        }
        if (c->state == TCP_SYN_SENT && nonblocking)
            return -EAGAIN;
        if (nonblocking || len == 0)
            return len == 0 ? 0 : -EAGAIN;
        if (!process_sleep(c))
            return -EINTR;
    }
}

int64_t tcp_write(struct tcp *c, const void *buf, size_t len, bool nonblocking, bool raise_sigpipe) {
    const uint8_t *from = buf;
    size_t done = 0;

    if (c->state == TCP_LISTEN)
        return -ENOTCONN;
    if (c->state == TCP_CLOSED && !has_buffers(c))
        return -ENOTCONN;

    while (done < len) {
        if (c->error) {
            int error = c->error;

            c->error = 0;
            return done ? (int64_t)done : -error;
        }
        if (c->fin_queued || c->state == TCP_CLOSED) {
            if (done)
                return done;
            if (raise_sigpipe)
                process_signal_self(SIGPIPE);
            return -EPIPE;
        }

        uint32_t room = c->state >= TCP_ESTABLISHED ? RING_SIZE - c->send.count : 0;

        if (room == 0) {
            if (nonblocking)
                return done ? (int64_t)done : -EAGAIN;
            if (!process_sleep(c))
                return done ? (int64_t)done : -EINTR;
            continue;
        }

        uint32_t part = len - done < room ? len - done : room;

        ring_append(&c->send, from + done, part);
        done += part;
        output(c);
    }
    return done;
}

static void queue_fin(struct tcp *c) {
    if (c->fin_queued)
        return;

    c->fin_queued = true;
    c->state = c->state == TCP_CLOSE_WAIT ? TCP_LAST_ACK : TCP_FIN_WAIT_1;
    output(c);
}

int tcp_shutdown(struct tcp *c, int how) {
    if (how != SHUT_RD && how != SHUT_WR && how != SHUT_RDWR)
        return -EINVAL;
    if (c->state == TCP_LISTEN || c->state == TCP_SYN_SENT || (c->state == TCP_CLOSED && !has_buffers(c)))
        return -ENOTCONN;

    if (how != SHUT_WR) {
        c->read_closed = true;
        process_wakeup(c);
    }
    if (how != SHUT_RD && (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT))
        queue_fin(c);
    return 0;
}

static void abort_child(struct tcp *c) {
    send_raw(c->local, c->local_port, c->remote, c->remote_port, c->snd_max, c->rcv_nxt, FLAG_RST | FLAG_ACK, 0, 0, NULL, 0, 0);
    destroy(c);
}

void tcp_close(struct tcp *c) {
    if (c->state == TCP_LISTEN) {
        struct tcp *next;

        for (struct tcp *child = connections; child; child = next) {
            next = child->next;
            if (child->parent == c)
                abort_child(child);
        }
        destroy(c);
        return;
    }

    if (c->state == TCP_CLOSED || c->state == TCP_SYN_SENT) {
        destroy(c);
        return;
    }

    if (c->state == TCP_TIME_WAIT) {
        c->orphan = true;
        ring_free(&c->send);
        ring_free(&c->receive);
        return;
    }

    // Data nobody read means the other side should know it wasn't all taken in.
    if (c->receive.count > 0) {
        abort_child(c);
        return;
    }

    c->orphan = true;
    if (c->state == TCP_ESTABLISHED || c->state == TCP_CLOSE_WAIT) {
        queue_fin(c);
    } else if (c->state == TCP_FIN_WAIT_2) {
        c->linger_until = irq_ticks() + ORPHAN_FIN_WAIT_TICKS;
    }
}

int tcp_poll(const struct tcp *c) {
    if (c->state == TCP_LISTEN)
        return c->accept_head ? POLLIN : 0;
    if (c->state == TCP_SYN_SENT || c->state == TCP_SYN_RECEIVED)
        return 0;
    if (c->state == TCP_CLOSED && !c->error && !has_buffers(c))
        return POLLOUT | POLLHUP;

    int ready = 0;

    if (c->receive.count > 0 || c->peer_finished || c->read_closed)
        ready |= POLLIN;
    if (!c->fin_queued && c->state != TCP_CLOSED && c->send.count < RING_SIZE)
        ready |= POLLOUT;
    if (c->state == TCP_CLOSED || (c->peer_finished && c->fin_queued))
        ready |= POLLIN | POLLHUP;
    if (c->error)
        ready |= POLLIN | POLLOUT | POLLERR;
    return ready;
}

size_t tcp_available(const struct tcp *c) {
    return c->receive.count;
}

int tcp_take_error(struct tcp *c) {
    int error = c->error;

    c->error = 0;
    return error;
}

bool tcp_is_connected(const struct tcp *c) {
    return c->state >= TCP_ESTABLISHED;
}

bool tcp_is_listening(const struct tcp *c) {
    return c->state == TCP_LISTEN;
}

void tcp_addresses(const struct tcp *c, uint32_t *local, uint16_t *local_port, uint32_t *remote, uint16_t *remote_port) {
    *local = c->local;
    *local_port = c->local_port;
    *remote = c->remote;
    *remote_port = c->remote_port;
}
