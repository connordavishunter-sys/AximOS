#include <aximos.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Lists the Wi-Fi networks nearby, and joins and forgets them, through netd, which
// takes their names and passwords in hex.

#define JOIN_WAIT_MS 25000

static char reply[8192];

static void to_hex(const char *in, char *out) {
    for (; *in; in++, out += 2)
        sprintf(out, "%02x", (unsigned char)*in);
    *out = '\0';
}

static void from_hex(const char *in, char *out, size_t room) {
    unsigned byte;
    size_t n = 0;

    while (in[0] && in[1] && n + 1 < room && sscanf(in, "%2x", &byte) == 1)
        out[n++] = byte >= ' ' && byte != 127 ? byte : '?', in += 2;
    out[n] = '\0';
}

static int ask_netd(const char *line) {
    int failed = net_ask(line, reply, sizeof(reply));

    if (failed)
        fprintf(stderr, "wifi: %s", reply + (strncmp(reply, "error ", 6) ? 0 : 6));
    return failed;
}

// The value after a status line's first words, as "ssid wlan0 HEX", or NULL.
static const char *status_value(const char *status, const char *start) {
    const char *at = strstr(status, start);

    return at ? at + strlen(start) : NULL;
}

// Has netd scan, then waits for what it finds, in reply: until there's something, or
// the network wanted if one is, after 2 seconds at least, and up to 6.
#define SCAN_WAIT 6
static int scan(const char *wanted) {
    if (ask_netd("scan"))
        return 1;
    for (int second = 1; second <= SCAN_WAIT; second++) {
        sleep(1);
        if (ask_netd("networks"))
            return 1;
        if (second >= 2 && (wanted ? strstr(reply, wanted) != NULL : strstr(reply, "network ") != NULL))
            break;
    }
    return 0;
}

static int list(void) {
    char status[4096], name[33], joined[66] = "";
    const char *at;

    if (ask_netd("status"))
        return 1;
    snprintf(status, sizeof(status), "%s", reply);
    if (strstr(status, "airplane on")) {
        puts("Wi-Fi is off, as airplane mode is on. wifi on turns it on.");
        return 0;
    }
    if ((at = status_value(status, "\nssid ")) && (at = strchr(at, ' ')))
        sscanf(at + 1, "%65s", joined);
    if (scan(NULL))
        return 1;
    printf("  %-32s  %-6s  %s\n", "NETWORK", "SIGNAL", "SECURITY");
    for (char *line = strtok(reply, "\n"); line; line = strtok(NULL, "\n")) {
        char ssid[66], security[8], saved[8];
        int signal;

        if (sscanf(line, "network %65s %d %7s %7s", ssid, &signal, security, saved) != 4)
            continue;
        from_hex(ssid, name, sizeof(name));
        printf("%c %-32s  %4d%%   %s%s\n", strcmp(ssid, joined) ? ' ' : '*', name, signal, security,
               !strcmp(saved, "saved") ? ", saved" : "");
    }
    return 0;
}

// Joins, asking for the password unless the network's open, then waits to hear
// whether it worked.
static int join(const char *name, bool hidden) {
    char ssid[66], password[64] = "", pass_hex[130] = "", line[256];
    bool open = false;

    if (!name[0] || strlen(name) > 32) {
        fprintf(stderr, "wifi: a network's name is 1 to 32 characters\n");
        return 1;
    }
    to_hex(name, ssid);
    if (!hidden && !ask_netd("networks")) {
        char wanted[80], *at;

        snprintf(wanted, sizeof(wanted), "network %s ", ssid);
        if (!strstr(reply, wanted)) // not seen yet, so looked for again
            scan(wanted);
        if (!(at = strstr(reply, wanted))) {
            fprintf(stderr, "wifi: %s isn't nearby. wifi lists those that are, and --hidden joins hidden ones.\n", name);
            return 1;
        }
        char security[8] = "";

        sscanf(at + strlen(wanted), "%*d %7s", security); // "network SSID SIGNAL SECURITY saved|new"
        open = !strcmp(security, "open");
    }
    if (!open && !ask(hidden ? "Password, or nothing if it's open: " : "Password: ", password, sizeof(password), true))
        return 1;
    if (password[0])
        to_hex(password, pass_hex);
    memset(password, 0, sizeof(password));
    snprintf(line, sizeof(line), "join %s %s%s", ssid, pass_hex[0] ? pass_hex : "-", hidden ? " hidden" : "");
    if (ask_netd(line))
        return 1;
    memset(line, 0, sizeof(line));
    for (int waited = 0; waited < JOIN_WAIT_MS; waited += 500) {
        char wanted[80];

        usleep(500000);
        if (net_ask("status", reply, sizeof(reply)))
            continue;
        snprintf(wanted, sizeof(wanted), " %s joined", ssid);
        if (strstr(reply, wanted)) {
            printf("wifi: joined %s\n", name);
            return 0;
        }
        snprintf(wanted, sizeof(wanted), " %s failed", ssid);
        if (strstr(reply, wanted))
            break;
    }
    fprintf(stderr, "wifi: couldn't join %s. Is the password right?\n", name);
    return 1;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: wifi\n"
                            "       wifi join NETWORK [--hidden]\n"
                            "       wifi forget NETWORK\n"
                            "       wifi on|off\n"
                            "Lists the Wi-Fi networks nearby, * by the one joined, or joins one, asking for\n"
                            "its password, which is saved, so it's joined again by itself. --hidden joins a\n"
                            "network that doesn't announce its name. off and on are airplane mode, for\n"
                            "every radio, kept across restarts.\n");

    char line[128], ssid[66];

    if (argc == 1)
        return list();
    if (!strcmp(argv[1], "join") && (argc == 3 || (argc == 4 && !strcmp(argv[3], "--hidden"))))
        return join(argv[2], argc == 4);
    if (!strcmp(argv[1], "forget") && argc == 3 && strlen(argv[2]) <= 32) {
        to_hex(argv[2], ssid);
        snprintf(line, sizeof(line), "forget %s", ssid);
        return ask_netd(line);
    }
    if ((!strcmp(argv[1], "on") || !strcmp(argv[1], "off")) && argc == 2) {
        snprintf(line, sizeof(line), "airplane %s", !strcmp(argv[1], "off") ? "on" : "off");
        return ask_netd(line);
    }
    fprintf(stderr, "wifi: try wifi --help\n");
    return 1;
}
