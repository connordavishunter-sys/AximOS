#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#include <netdb.h>
#include <psa/crypto.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#define BUFFER_SIZE 4096
#define MAX_REDIRECTS 5
#define HOST_MAX 256
#define PATH_LIMIT 1024
#define HEADERS_MAX 8192
#define CA_FILE "/system/etc/ca-certificates.crt"

struct url {
    bool tls;
    char host[HOST_MAX];
    char port[8];
    char path[PATH_LIMIT];
};

static bool parse_url(const char *text, struct url *out) {
    out->tls = strncmp(text, "https://", 8) == 0;
    if (out->tls)
        text += 8;
    else if (strncmp(text, "http://", 7) == 0)
        text += 7;

    const char *path = strchr(text, '/');
    size_t authority = path ? (size_t)(path - text) : strlen(text);
    const char *colon = memchr(text, ':', authority);
    size_t host_len = colon ? (size_t)(colon - text) : authority;

    if (host_len == 0 || host_len >= HOST_MAX || strlen(path ? path : "/") >= PATH_LIMIT) {
        dprintf(STDERR_FILENO, "fetch: bad address: %s\n", text);
        return false;
    }

    memcpy(out->host, text, host_len);
    out->host[host_len] = '\0';
    if (colon)
        snprintf(out->port, sizeof(out->port), "%.*s", (int)(authority - host_len - 1), colon + 1);
    else
        strcpy(out->port, out->tls ? "443" : "80");
    strcpy(out->path, path ? path : "/");
    return true;
}

static int connect_to(const struct url *url) {
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct addrinfo *found;
    int error = getaddrinfo(url->host, url->port, &hints, &found);

    if (error) {
        dprintf(STDERR_FILENO, "fetch: %s: %s\n", url->host, gai_strerror(error));
        return -1;
    }

    int fd = -1;

    for (struct addrinfo *a = found; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, 0);
        if (fd >= 0 && connect(fd, a->ai_addr, a->ai_addrlen) == 0)
            break;
        if (fd >= 0) {
            dprintf(STDERR_FILENO, "fetch: %s: %s\n", url->host, strerror(errno));
            close(fd);
        }
        fd = -1;
    }
    freeaddrinfo(found);
    return fd;
}

static bool write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t wrote = write(fd, data, len);

        if (wrote <= 0)
            return false;
        data += wrote;
        len -= wrote;
    }
    return true;
}

// A connection, which is either plain or goes through TLS.
struct connection {
    int fd;
    bool tls;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config config;
};

static int tls_send(void *context, const unsigned char *data, size_t len) {
    ssize_t wrote = write(*(int *)context, data, len);

    return wrote < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : wrote;
}

static int tls_receive(void *context, unsigned char *data, size_t len) {
    ssize_t got = read(*(int *)context, data, len);

    return got < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : got;
}

static void tls_error(const char *what, int code) {
    char text[128];

    mbedtls_strerror(code, text, sizeof(text));
    dprintf(STDERR_FILENO, "fetch: %s: %s\n", what, text);
}

// What TLS needs however many connections there are, set up the first time.
static bool tls_prepare(mbedtls_ctr_drbg_context **random, mbedtls_x509_crt **authorities) {
    static mbedtls_entropy_context entropy;
    static mbedtls_ctr_drbg_context drbg;
    static mbedtls_x509_crt roots;
    static bool prepared;

    *random = &drbg;
    *authorities = &roots;
    if (prepared)
        return true;

    // The library reads /dev/urandom, which doesn't wait for there to be enough to go on.
    char first;

    if (getentropy(&first, sizeof(first)) < 0) {
        dprintf(STDERR_FILENO, "fetch: no random numbers\n");
        return false;
    }

    psa_status_t status = psa_crypto_init();

    if (status != PSA_SUCCESS) {
        dprintf(STDERR_FILENO, "fetch: can't start the cryptography library\n");
        return false;
    }

    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);
    mbedtls_x509_crt_init(&roots);

    int error = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, NULL, 0);

    if (error) {
        tls_error("random numbers", error);
        return false;
    }

    // Some certificates failing to parse is fine, as long as there's something to trust.
    error = mbedtls_x509_crt_parse_file(&roots, CA_FILE);
    if (error < 0) {
        tls_error(CA_FILE, error);
        return false;
    }
    prepared = true;
    return true;
}

static bool tls_start(struct connection *c, const char *host) {
    mbedtls_ctr_drbg_context *drbg;
    mbedtls_x509_crt *roots;

    if (!tls_prepare(&drbg, &roots))
        return false;

    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->config);

    int error = mbedtls_ssl_config_defaults(&c->config, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);

    if (!error) {
        mbedtls_ssl_conf_authmode(&c->config, MBEDTLS_SSL_VERIFY_REQUIRED);
        mbedtls_ssl_conf_ca_chain(&c->config, roots, NULL);
        mbedtls_ssl_conf_rng(&c->config, mbedtls_ctr_drbg_random, drbg);
        error = mbedtls_ssl_setup(&c->ssl, &c->config);
    }
    if (!error)
        error = mbedtls_ssl_set_hostname(&c->ssl, host);
    if (error) {
        tls_error("setting up TLS", error);
        return false;
    }

    mbedtls_ssl_set_bio(&c->ssl, &c->fd, tls_send, tls_receive, NULL);

    while ((error = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (error == MBEDTLS_ERR_SSL_WANT_READ || error == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;

        uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);

        if (error == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED && flags != (uint32_t)-1) {
            static char reasons[512];

            mbedtls_x509_crt_verify_info(reasons, sizeof(reasons), "  ", flags);
            dprintf(STDERR_FILENO, "fetch: %s can't be trusted:\n%s", host, reasons);
        } else {
            tls_error(host, error);
        }
        return false;
    }
    c->tls = true;
    return true;
}

static ssize_t connection_read(struct connection *c, char *buf, size_t len) {
    if (!c->tls)
        return read(c->fd, buf, len);

    for (;;) {
        int got = mbedtls_ssl_read(&c->ssl, (unsigned char *)buf, len);

        if (got >= 0)
            return got;
        if (got == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            return 0;

        // Handled inside, or a ticket for reconnecting that isn't needed.
        if (got == MBEDTLS_ERR_SSL_WANT_READ || got == MBEDTLS_ERR_SSL_WANT_WRITE
            || got == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
            continue;

        tls_error("reading", got);
        return -1;
    }
}

static bool connection_write(struct connection *c, const char *data, size_t len) {
    if (!c->tls)
        return write_all(c->fd, data, len);

    while (len > 0) {
        int wrote = mbedtls_ssl_write(&c->ssl, (const unsigned char *)data, len);

        if (wrote == MBEDTLS_ERR_SSL_WANT_READ || wrote == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;
        if (wrote <= 0)
            return false;
        data += wrote;
        len -= wrote;
    }
    return true;
}

static void connection_close(struct connection *c) {
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->config);
    }
    close(c->fd);
}

// Finds a header's value in what arrived, ending at the line's end.
static const char *find_header(const char *headers, const char *name) {
    size_t len = strlen(name);

    for (const char *line = strstr(headers, "\r\n"); line; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, name, len) == 0 && line[len] == ':') {
            line += len + 1;
            while (*line == ' ')
                line++;
            return line;
        }
    }
    return NULL;
}

// Copies a chunked body, whose pieces each start with their size in hexadecimal.
static bool read_chunked(struct connection *c, char *buffer, size_t have, int out) {
    size_t used = 0;

    for (;;) {
        char *end;

        while (!(end = memmem(buffer + used, have - used, "\r\n", 2))) {
            memmove(buffer, buffer + used, have - used);
            have -= used;
            used = 0;

            ssize_t got = connection_read(c, buffer + have, BUFFER_SIZE - have);

            if (got <= 0)
                return false;
            have += got;
        }

        size_t size = strtoul(buffer + used, NULL, 16);

        used = end + 2 - buffer;
        if (size == 0)
            return true;

        // The piece and the line end that follows it.
        size_t left = size + 2;

        while (left > 0) {
            if (used == have) {
                ssize_t got = connection_read(c, buffer, BUFFER_SIZE);

                if (got <= 0)
                    return false;
                used = 0;
                have = got;
            }

            size_t part = have - used < left ? have - used : left;
            size_t data = left <= 2 ? 0 : left - 2 < part ? left - 2 : part;

            if (data && !write_all(out, buffer + used, data))
                return false;
            used += part;
            left -= part;
        }
    }
}

// Returns the status, or -1 for failure, and any redirect's target in location.
static int request(const struct url *url, const char *output, char *location, size_t location_size) {
    static char headers[HEADERS_MAX + 1];
    static char buffer[BUFFER_SIZE + 1];
    static struct connection connection;

    connection.tls = false;
    connection.fd = connect_to(url);
    if (connection.fd < 0)
        return -1;
    if (url->tls && !tls_start(&connection, url->host)) {
        close(connection.fd);
        return -1;
    }

    int len = snprintf(buffer, sizeof(buffer), "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: AximOS-fetch\r\nConnection: close\r\n\r\n",
                       url->path, url->host);

    if (!connection_write(&connection, buffer, len)) {
        dprintf(STDERR_FILENO, "fetch: can't send the request\n");
        return -1;
    }

    size_t have = 0;
    char *body = NULL;

    while (!body) {
        if (have == HEADERS_MAX) {
            dprintf(STDERR_FILENO, "fetch: headers are too long\n");
            return -1;
        }

        ssize_t got = connection_read(&connection, headers + have, HEADERS_MAX - have);

        if (got <= 0) {
            dprintf(STDERR_FILENO, "fetch: connection ended early\n");
            return -1;
        }
        have += got;
        headers[have] = '\0';
        body = strstr(headers, "\r\n\r\n");
    }
    body += 4;

    int status = 0;

    if (sscanf(headers, "HTTP/%*d.%*d %d", &status) != 1) {
        dprintf(STDERR_FILENO, "fetch: not an HTTP answer\n");
        return -1;
    }

    if (status >= 300 && status < 400) {
        const char *target = find_header(headers, "Location");

        if (target) {
            size_t n = strcspn(target, "\r\n");

            snprintf(location, location_size, "%.*s", (int)n, target);
            connection_close(&connection);
            return status;
        }
    }
    if (status >= 400) {
        dprintf(STDERR_FILENO, "fetch: the server answered %d\n", status);
        connection_close(&connection);
        return status;
    }

    int out = output ? open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644) : STDOUT_FILENO;

    if (out < 0) {
        dprintf(STDERR_FILENO, "fetch: %s: %s\n", output, strerror(errno));
        return -1;
    }

    size_t already = headers + have - body;
    const char *encoding = find_header(headers, "Transfer-Encoding");
    const char *length_text = find_header(headers, "Content-Length");
    long long expected = length_text ? atoll(length_text) : -1;
    bool ok = true;

    if (encoding && strncasecmp(encoding, "chunked", 7) == 0) {
        memcpy(buffer, body, already);
        ok = read_chunked(&connection, buffer, already, out);
    } else {
        long long total = already;

        ok = write_all(out, body, already);
        for (ssize_t got; ok && (got = connection_read(&connection, buffer, BUFFER_SIZE)) != 0; total += got) {
            if (got < 0)
                ok = false;
            else
                ok = write_all(out, buffer, got);
        }
        if (ok && expected >= 0 && total != expected)
            ok = false;
    }

    if (!ok)
        dprintf(STDERR_FILENO, "fetch: the download didn't finish\n");
    connection_close(&connection);
    if (output)
        close(out);
    return ok ? status : -1;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: fetch [-o file] url\n"
                            "Downloads an http:// or https:// address, to standard output by default.\n"
                            "Certificates are checked against " CA_FILE ".\n"
                            "  -o  save it in a file\n");

    const char *output = NULL;
    int option;

    while ((option = getopt(argc, argv, "o:")) != -1) {
        if (option != 'o') {
            dprintf(STDERR_FILENO, "usage: fetch [-o file] url\n");
            return 2;
        }
        output = optarg;
    }
    if (optind != argc - 1) {
        dprintf(STDERR_FILENO, "usage: fetch [-o file] url\n");
        return 2;
    }

    static char target[PATH_LIMIT + HOST_MAX];
    struct url url;

    snprintf(target, sizeof(target), "%s", argv[optind]);

    for (int redirects = 0; redirects <= MAX_REDIRECTS; redirects++) {
        if (target[0] == '/') {
            snprintf(url.path, sizeof(url.path), "%s", target);
        } else if (!parse_url(target, &url)) {
            return 1;
        }

        int status = request(&url, output, target, sizeof(target));

        if (status < 0 || status >= 400)
            return 1;
        if (status < 300)
            return 0;
    }
    dprintf(STDERR_FILENO, "fetch: too many redirects\n");
    return 1;
}
