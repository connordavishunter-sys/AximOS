#pragma once

#include <net/if.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>

// What netd's parts share: its cards, and Wi-Fi's part of them, in wifi.c.

#define SOCKET "/run/netd.sock"
#define RESOLV "/run/resolv.conf"
#define CONF "/system/etc/netd.conf"
#define MAX_CARDS 8
#define MAX_DNS 4
#define WIRED_METRIC 100
#define WIFI_METRIC 600
#define DHCP_MAGIC 0x63825363
#define RETRY_MS 4000   // DHCP asks again after this, doubling up to RETRY_MAX_MS
#define RETRY_MAX_MS 64000
#define SOLICITS 3      // DHCPv6 solicits before it only asks for DNS, as networks without its addresses answer

enum { IDLE, SELECTING, REQUESTING, BOUND, RENEWING, REBINDING };
enum { V6_IDLE, V6_SOLICIT, V6_REQUEST, V6_INFO, V6_DONE };

struct card {
    char name[IF_NAMESIZE];
    int index;
    uint8_t mac[6];
    bool wifi, carrier, seen;
    bool hand; // its IPv4 set by hand, from CONF
    uint32_t hand_address, hand_gateway, hand_dns;
    int hand_prefix;
    // DHCPv4, in host byte order
    int state, fd, tries;
    uint32_t xid, offered, server, address, netmask, gateway, dns[MAX_DNS];
    int64_t next, renew, rebind, expires;
    // DHCPv6
    int v6_state, v6_fd, v6_tries;
    uint8_t v6_xid[3], v6_server[130];
    int v6_server_len;
    struct in6_addr v6_address, v6_dns[MAX_DNS];
    bool has_v6_address;
    int64_t v6_next, v6_renew;
    // Wi-Fi, through a wpa_supplicant of its own
    pid_t wpa;
    bool wpa_ready;
    int64_t wpa_due;          // when to look at it, or start it again
    char joined[2 * 32 + 1];  // the network it's on, in hex
    int join_id, join_result; // a network being joined: wpa_supplicant's id, and 1 when joined, -1 when not
    char join_ssid[2 * 32 + 1];
    bool join_saved; // whether it was saved already, so isn't forgotten if it fails
    int64_t join_until;
};

extern struct card cards[MAX_CARDS];
extern int card_count;

int64_t now_ms(void);

// Wi-Fi: wpa_supplicant for each Wi-Fi card, the networks saved in WIFI_SAVED, and airplane mode.
void wifi_found(struct card *c);
void wifi_gone(struct card *c);
int wifi_tick(struct card *c, int64_t now); // how long until it's due again
void wifi_status(FILE *out, const struct card *c);
bool wifi_command(const char *word, char *rest, FILE *out); // false if it isn't Wi-Fi's
void airplane_load(void);
bool airplane_on(void);
