#include "netd.h"
#include <aximos.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/rfkill.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// Wi-Fi: netd runs a wpa_supplicant for each Wi-Fi card, and tells it the networks
// saved in WIFI_SAVED, one a line, "SSID PASSWORD hidden|shown" in hex, or "-" for an
// open network's password, which it rejoins by itself, the strongest it sees. Joining
// one saves it once it's joined, and airplane mode blocks every radio with rfkill.

#define WIFI_SAVED "/system/etc/netd-wifi"
#define AIRPLANE "/system/etc/netd-airplane" // there while airplane mode's on
#define WPA_DIR "/run/wpa_supplicant"
#define WPA_CONF "/run/netd-wpa.conf"
#define WPA_PATH "/system/bin/wpa_supplicant"
#define JOIN_MS 20000 // to join a network before it's taken as not working, such as with the wrong password
#define MAX_SAVED 64
#define REPLY_MAX 16384

static bool airplane;

// --- Text ---

static void to_hex(const char *in, size_t size, char *out) {
    for (size_t i = 0; i < size; i++)
        sprintf(out + 2 * i, "%02x", (unsigned char)in[i]);
    out[2 * size] = '\0';
}

static size_t from_hex(const char *in, char *out, size_t room) {
    size_t n = 0;
    unsigned byte;

    while (in[0] && in[1] && n + 1 < room && sscanf(in, "%2x", &byte) == 1)
        out[n++] = byte, in += 2;
    out[n] = '\0';
    return n;
}

// wpa_supplicant's escaped text, as it shows SSIDs, to hex.
static void escaped_to_hex(const char *in, char *out) {
    char raw[33];
    size_t n = 0;

    while (*in && *in != '\t' && *in != '\n' && n < 32) {
        unsigned byte;

        if (in[0] == '\\' && in[1] == 'x' && sscanf(in + 2, "%2x", &byte) == 1)
            raw[n++] = byte, in += 4;
        else if (in[0] == '\\' && in[1])
            raw[n++] = in[1] == 'n' ? '\n' : in[1] == 't' ? '\t' : in[1] == 'r' ? '\r' : in[1] == 'e' ? 27 : in[1],
            in += 2;
        else
            raw[n++] = *in++;
    }
    to_hex(raw, n, out);
}

// --- wpa_supplicant ---

// Sends a command on its control socket, giving the length of the answer, or -1.
static int wpa_ask(const struct card *c, const char *command, char *reply, size_t size) {
    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un me = {.sun_family = AF_UNIX}, to = {.sun_family = AF_UNIX};
    ssize_t n = -1;

    snprintf(me.sun_path, sizeof(me.sun_path), "/run/netd-wpa-%s", c->name);
    snprintf(to.sun_path, sizeof(to.sun_path), WPA_DIR "/%s", c->name);
    unlink(me.sun_path);
    if (fd >= 0 && bind(fd, (struct sockaddr *)&me, sizeof(me)) == 0
        && sendto(fd, command, strlen(command), 0, (struct sockaddr *)&to, sizeof(to)) >= 0
        && poll(&(struct pollfd){.fd = fd, .events = POLLIN}, 1, 3000) == 1)
        n = recv(fd, reply, size - 1, 0);
    reply[n > 0 ? n : 0] = '\0';
    if (fd >= 0)
        close(fd);
    unlink(me.sun_path);
    return (int)n;
}

static bool wpa_ok(const struct card *c, const char *command) {
    char reply[64];

    return wpa_ask(c, command, reply, sizeof(reply)) > 0 && !strncmp(reply, "OK", 2);
}

// A network, added to wpa_supplicant and enabled, giving its id, or -1. A password of
// 8 to 63 characters is WPA2's or WPA3's, and others only WPA3's.
static int wpa_add(const struct card *c, const char *ssid, const char *password, bool hidden) {
    char reply[64], command[400], pass[64];
    int id;

    if (wpa_ask(c, "ADD_NETWORK", reply, sizeof(reply)) <= 0 || (id = atoi(reply)) < 0 || !isdigit(reply[0]))
        return -1;
    snprintf(command, sizeof(command), "SET_NETWORK %d ssid %s", id, ssid);
    bool ok = wpa_ok(c, command);
    size_t length = strcmp(password, "-") ? from_hex(password, pass, sizeof(pass)) : 0;

    if (!strcmp(password, "-")) {
        snprintf(command, sizeof(command), "SET_NETWORK %d key_mgmt NONE", id);
        ok = ok && wpa_ok(c, command);
    } else {
        snprintf(command, sizeof(command), "SET_NETWORK %d key_mgmt %s", id,
                 length >= 8 && length <= 63 ? "WPA-PSK WPA-PSK-SHA256 SAE" : "SAE");
        ok = ok && wpa_ok(c, command);
        snprintf(command, sizeof(command), "SET_NETWORK %d ieee80211w 1", id);
        ok = ok && wpa_ok(c, command);
        snprintf(command, sizeof(command), "SET_NETWORK %d sae_password \"%s\"", id, pass);
        ok = ok && wpa_ok(c, command);
        if (length >= 8 && length <= 63) {
            snprintf(command, sizeof(command), "SET_NETWORK %d psk \"%s\"", id, pass);
            ok = ok && wpa_ok(c, command);
        }
    }
    memset(pass, 0, sizeof(pass));
    memset(command, 0, sizeof(command));
    if (hidden) {
        snprintf(command, sizeof(command), "SET_NETWORK %d scan_ssid 1", id);
        ok = ok && wpa_ok(c, command);
    }
    snprintf(command, sizeof(command), "ENABLE_NETWORK %d", id);
    if (!ok || !wpa_ok(c, command)) {
        snprintf(command, sizeof(command), "REMOVE_NETWORK %d", id);
        wpa_ok(c, command);
        return -1;
    }
    return id;
}

// The id of the network with an SSID, in hex, or -1.
static int wpa_find(const struct card *c, const char *ssid) {
    char reply[REPLY_MAX], hex[2 * 32 + 1];

    if (wpa_ask(c, "LIST_NETWORKS", reply, sizeof(reply)) <= 0)
        return -1;
    for (char *line = strchr(reply, '\n'); line && line[1]; line = strchr(line + 1, '\n')) {
        char *tab = strchr(line + 1, '\t');

        if (!tab)
            continue;
        escaped_to_hex(tab + 1, hex);
        if (!strcmp(hex, ssid))
            return atoi(line + 1);
    }
    return -1;
}

// --- Saved networks ---

struct saved {
    char ssid[2 * 32 + 1], password[2 * 63 + 1];
    bool hidden;
};

static int read_saved(struct saved *out) {
    FILE *f = fopen(WIFI_SAVED, "r");
    char line[300], how[8];
    int n = 0;

    while (f && n < MAX_SAVED && fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%64s %126s %7s", out[n].ssid, out[n].password, how) == 3) {
            out[n].hidden = !strcmp(how, "hidden");
            n++;
        }
    }
    if (f)
        fclose(f);
    return n;
}

// WIFI_SAVED, without the network, and with it again, if password isn't NULL.
static bool write_saved(const char *ssid, const char *password, bool hidden) {
    struct saved saved[MAX_SAVED];
    int n = read_saved(saved);
    FILE *f = fopen(WIFI_SAVED ".new", "w");

    if (!f)
        return false;
    fchmod(fileno(f), 0600);
    for (int i = 0; i < n; i++) {
        if (strcmp(saved[i].ssid, ssid))
            fprintf(f, "%s %s %s\n", saved[i].ssid, saved[i].password, saved[i].hidden ? "hidden" : "shown");
    }
    if (password)
        fprintf(f, "%s %s %s\n", ssid, password, hidden ? "hidden" : "shown");
    return fclose(f) == 0 && rename(WIFI_SAVED ".new", WIFI_SAVED) == 0;
}

// --- Cards ---

void wifi_found(struct card *c) {
    c->wpa = 0;
    c->wpa_ready = false;
    c->wpa_due = now_ms();
    c->join_id = -1;
}

void wifi_gone(struct card *c) {
    if (c->wpa > 0)
        kill(c->wpa, SIGTERM);
    c->wpa = 0;
}

static void start_wpa(struct card *c) {
    FILE *f = fopen(WPA_CONF, "w");

    if (f) {
        fputs("ctrl_interface=" WPA_DIR "\n", f);
        fclose(f);
    }
    if ((c->wpa = fork()) == 0) {
        execl(WPA_PATH, "wpa_supplicant", "-q", "-i", c->name, "-D", "nl80211", "-c", WPA_CONF, (char *)NULL);
        _exit(127);
    }
    printf("netd: started wpa_supplicant on %s\n", c->name);
    fflush(stdout);
}

// It's told the saved networks once its socket's there.
static void configure(struct card *c) {
    struct saved saved[MAX_SAVED];
    int n = read_saved(saved);
    char reply[64];

    if (wpa_ask(c, "PING", reply, sizeof(reply)) <= 0 || strncmp(reply, "PONG", 4))
        return;
    for (int i = 0; i < n; i++)
        wpa_add(c, saved[i].ssid, saved[i].password, saved[i].hidden);
    c->wpa_ready = true;
    printf("netd: %s knows %d saved networks\n", c->name, n);
    fflush(stdout);
}

// The network it's on, and how a join's going: once joined it's saved, and if it
// doesn't join in time, it's forgotten, unless it was saved already.
static void follow(struct card *c, int64_t now) {
    char reply[2048], *at;
    int id = -1;
    bool done = false;

    c->joined[0] = '\0';
    if (wpa_ask(c, "STATUS", reply, sizeof(reply)) <= 0)
        return;
    done = strstr(reply, "wpa_state=COMPLETED") != NULL;
    if (done && (at = strstr(reply, "\nid=")))
        id = atoi(at + 4);
    if (done && (at = strstr(reply, "\nssid=")))
        escaped_to_hex(at + 6, c->joined);
    if (c->join_result || c->join_id < 0)
        return;
    if (done && id == c->join_id) {
        c->join_result = 1;
        wpa_ok(c, "ENABLE_NETWORK all"); // the others, which joining one disabled, to roam to
        printf("netd: %s joined its network\n", c->name);
    } else if (now >= c->join_until) {
        char command[64];

        c->join_result = -1;
        if (!c->join_saved) {
            snprintf(command, sizeof(command), "REMOVE_NETWORK %d", c->join_id);
            wpa_ok(c, command);
            write_saved(c->join_ssid, NULL, false);
        }
        wpa_ok(c, "ENABLE_NETWORK all");
        printf("netd: %s couldn't join its network\n", c->name);
    }
    fflush(stdout);
}

int wifi_tick(struct card *c, int64_t now) {
    if (c->wpa > 0 && waitpid(c->wpa, NULL, WNOHANG) == c->wpa) {
        printf("netd: wpa_supplicant on %s stopped\n", c->name);
        c->wpa = 0;
        c->wpa_ready = false;
        c->wpa_due = now + 2000;
    }
    if (now < c->wpa_due)
        return (int)(c->wpa_due - now);
    if (!c->wpa)
        start_wpa(c);
    else if (!c->wpa_ready)
        configure(c);
    else
        follow(c, now);
    c->wpa_due = now + 1000;
    return 1000;
}

void wifi_status(FILE *out, const struct card *c) {
    if (c->joined[0])
        fprintf(out, "ssid %s %s\n", c->name, c->joined);
    if (c->join_id >= 0)
        fprintf(out, "join %s %s %s\n", c->name, c->join_ssid,
                c->join_result > 0 ? "joined" : c->join_result < 0 ? "failed" : "joining");
}

// --- Airplane mode ---

static void set_radios(bool blocked) {
    int fd = open("/dev/rfkill", O_WRONLY | O_CLOEXEC);
    struct rfkill_event e = {.type = RFKILL_TYPE_ALL, .op = RFKILL_OP_CHANGE_ALL, .soft = blocked};

    if (fd < 0 || write(fd, &e, RFKILL_EVENT_SIZE_V1) != RFKILL_EVENT_SIZE_V1)
        fprintf(stderr, "netd: can't turn the radios %s: %s\n", blocked ? "off" : "on", strerror(errno));
    if (fd >= 0)
        close(fd);
}

void airplane_load(void) {
    airplane = access(AIRPLANE, F_OK) == 0;
    if (airplane)
        set_radios(true);
}

bool airplane_on(void) {
    return airplane;
}

// --- Asking it ---

static struct card *first_wifi(void) {
    for (int i = 0; i < card_count; i++) {
        if (cards[i].wifi && cards[i].wpa_ready)
            return &cards[i];
    }
    return NULL;
}

// What's nearby: "network SSID SIGNAL SECURITY saved|new", the strongest of each name,
// with its signal from 0 to 100, then "end".
static void networks(FILE *out) {
    struct card *c = first_wifi();
    static char reply[REPLY_MAX];
    struct saved saved[MAX_SAVED];
    int saved_count = read_saved(saved);
    struct { char ssid[2 * 32 + 1], security[8]; int signal; } seen[64];
    int count = 0;

    if (c && wpa_ask(c, "SCAN_RESULTS", reply, sizeof(reply)) > 0) {
        for (char *line = strchr(reply, '\n'); line && line[1]; line = strchr(line + 1, '\n')) {
            char *field[5] = {line + 1};
            int f = 1;

            for (char *p = line + 1; *p && *p != '\n' && f < 5; p++) {
                if (*p == '\t')
                    field[f++] = p + 1;
            }
            if (f < 5 || field[4][0] == '\t' || field[4][0] == '\n') // hidden ones have no name
                continue;

            char ssid[2 * 32 + 1];
            int level = atoi(field[2]), signal = (level + 100) * 2, found = -1;
            const char *flags = field[3];

            signal = signal < 0 ? 0 : signal > 100 ? 100 : signal;
            escaped_to_hex(field[4], ssid);
            for (int i = 0; i < count; i++)
                found = !strcmp(seen[i].ssid, ssid) ? i : found;
            if (found < 0 && count < 64)
                found = count++, seen[found].signal = -1;
            if (found < 0 || signal <= seen[found].signal)
                continue;
            snprintf(seen[found].ssid, sizeof(seen[found].ssid), "%s", ssid);
            snprintf(seen[found].security, sizeof(seen[found].security), "%s",
                     strstr(flags, "SAE") ? "WPA3" : strstr(flags, "WPA2") || strstr(flags, "RSN") ? "WPA2"
                     : strstr(flags, "WPA") ? "WPA" : strstr(flags, "WEP") ? "WEP" : "open");
            seen[found].signal = signal;
        }
    }
    for (int i = 0; i < count; i++) {
        bool is_saved = false;

        for (int j = 0; j < saved_count; j++)
            is_saved |= !strcmp(saved[j].ssid, seen[i].ssid);
        fprintf(out, "network %s %d %s %s\n", seen[i].ssid, seen[i].signal, seen[i].security, is_saved ? "saved" : "new");
    }
    fputs("end\n", out);
}

static void join(char *rest, FILE *out) {
    char *ssid = strtok(rest, " \t\n"), *password = strtok(NULL, " \t\n"), *how = strtok(NULL, " \t\n");
    struct card *c = first_wifi();
    char raw[64];
    bool hidden = how && !strcmp(how, "hidden");

    if (!c) {
        fputs("error there's no Wi-Fi card, or it isn't ready\n", out);
        return;
    }
    if (!ssid || !password || strlen(ssid) % 2 || strlen(ssid) > 64 || !strlen(ssid)
        || (strcmp(password, "-") && (strlen(password) % 2 || strlen(password) > 126 || from_hex(password, raw, sizeof(raw)) < 1))) {
        fputs("error that isn't a network's name and password\n", out);
        return;
    }
    if (airplane) {
        fputs("error airplane mode is on\n", out);
        return;
    }

    char command[64];
    int old = wpa_find(c, ssid);

    if (old >= 0) { // joined again, perhaps with another password
        snprintf(command, sizeof(command), "REMOVE_NETWORK %d", old);
        wpa_ok(c, command);
    }
    c->join_saved = false;
    c->join_result = 0;
    c->join_until = now_ms() + JOIN_MS;
    snprintf(c->join_ssid, sizeof(c->join_ssid), "%s", ssid);
    if ((c->join_id = wpa_add(c, ssid, password, hidden)) < 0 || !write_saved(ssid, password, hidden)) {
        c->join_id = -1;
        fputs("error wpa_supplicant wouldn't take it\n", out);
        return;
    }
    snprintf(command, sizeof(command), "SELECT_NETWORK %d", c->join_id);
    wpa_ok(c, command);
    c->join_saved = old >= 0;
    printf("netd: %s joining a network\n", c->name);
    fputs("ok\n", out);
}

static void forget(char *rest, FILE *out) {
    char *ssid = strtok(rest, " \t\n"), command[64];
    struct saved saved[MAX_SAVED];
    int n = read_saved(saved), found = 0;

    for (int i = 0; ssid && i < n; i++)
        found |= !strcmp(saved[i].ssid, ssid);
    if (!found) {
        fputs("error that network isn't saved\n", out);
        return;
    }
    write_saved(ssid, NULL, false);
    for (int i = 0; i < card_count; i++) {
        int id = cards[i].wifi && cards[i].wpa_ready ? wpa_find(&cards[i], ssid) : -1;

        if (id >= 0) {
            snprintf(command, sizeof(command), "REMOVE_NETWORK %d", id);
            wpa_ok(&cards[i], command);
        }
        if (cards[i].join_id == id)
            cards[i].join_id = -1;
    }
    fputs("ok\n", out);
}

static void set_airplane(char *rest, FILE *out) {
    char *how = strtok(rest, " \t\n");

    if (!how || (strcmp(how, "on") && strcmp(how, "off"))) {
        fputs("error airplane on or off\n", out);
        return;
    }
    airplane = !strcmp(how, "on");
    if (airplane)
        close(open(AIRPLANE, O_WRONLY | O_CREAT | O_CLOEXEC, 0644));
    else
        unlink(AIRPLANE);
    set_radios(airplane);
    printf("netd: airplane mode %s\n", how);
    fflush(stdout);
    fputs("ok\n", out);
}

bool wifi_command(const char *word, char *rest, FILE *out) {
    if (!strcmp(word, "scan")) {
        struct card *c = first_wifi();

        if (c)
            wpa_ok(c, "SCAN"); // or it's scanning already
        fputs(c ? "ok\n" : "error there's no Wi-Fi card, or it isn't ready\n", out);
    } else if (!strcmp(word, "networks")) {
        networks(out);
    } else if (!strcmp(word, "join")) {
        join(rest, out);
    } else if (!strcmp(word, "forget")) {
        forget(rest, out);
    } else if (!strcmp(word, "airplane")) {
        set_airplane(rest, out);
    } else {
        return false;
    }
    return true;
}
