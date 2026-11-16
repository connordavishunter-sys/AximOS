#include "account.h"
#include <mbedtls/sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ROUNDS 50000 // of SHA-256, so guessing is slow

static void hash(const uint8_t salt[ACCOUNT_SALT], const char *secret, uint8_t out[ACCOUNT_HASH]) {
    uint8_t round[ACCOUNT_HASH + ACCOUNT_SALT];
    size_t length = strlen(secret);
    uint8_t *first = malloc(ACCOUNT_SALT + length);

    memcpy(first, salt, ACCOUNT_SALT);
    memcpy(first + ACCOUNT_SALT, secret, length);
    mbedtls_sha256(first, ACCOUNT_SALT + length, out, 0);
    free(first);
    memcpy(round + ACCOUNT_HASH, salt, ACCOUNT_SALT);
    for (int i = 1; i < ROUNDS; i++) {
        memcpy(round, out, ACCOUNT_HASH);
        mbedtls_sha256(round, sizeof(round), out, 0);
    }
}

static void to_hex(const uint8_t *bytes, int count, char *out) {
    for (int i = 0; i < count; i++)
        sprintf(out + 2 * i, "%02x", bytes[i]);
}

static bool from_hex(const char *hex, uint8_t *out, int count) {
    for (int i = 0; i < count; i++) {
        unsigned byte;

        if (sscanf(hex + 2 * i, "%2x", &byte) != 1)
            return false;
        out[i] = (uint8_t)byte;
    }
    return strlen(hex) == (size_t)count * 2;
}

bool account_load(struct account *a) {
    FILE *f = fopen(ACCOUNT, "r");
    char salt[2 * ACCOUNT_SALT + 1], digest[2 * ACCOUNT_HASH + 1];
    bool ok = f && fscanf(f, "%31s %32s %64s", a->name, salt, digest) == 3 && from_hex(salt, a->salt, ACCOUNT_SALT)
           && from_hex(digest, a->hash, ACCOUNT_HASH);

    if (f)
        fclose(f);
    return ok;
}

bool account_save(struct account *a, const char *name, const char *password) {
    char salt[2 * ACCOUNT_SALT + 1], digest[2 * ACCOUNT_HASH + 1];
    FILE *f;

    snprintf(a->name, sizeof(a->name), "%s", name);
    if (getentropy(a->salt, ACCOUNT_SALT) < 0)
        return false;
    hash(a->salt, password, a->hash);
    to_hex(a->salt, ACCOUNT_SALT, salt);
    to_hex(a->hash, ACCOUNT_HASH, digest);
    if (!(f = fopen(ACCOUNT, "w")))
        return false;
    fprintf(f, "%s %s %s\n", a->name, salt, digest);
    return fclose(f) == 0;
}

// Compares every byte, so how long it takes says nothing about how close it was.
bool account_check(const struct account *a, const char *password) {
    uint8_t tried[ACCOUNT_HASH], differ = 0;

    hash(a->salt, password, tried);
    for (int i = 0; i < ACCOUNT_HASH; i++)
        differ |= tried[i] ^ a->hash[i];
    return !differ;
}

bool account_name_ok(const char *s) {
    for (const char *c = s; *c; c++) {
        if (!(*c >= 'a' && *c <= 'z') && !(*c >= 'A' && *c <= 'Z') && !(*c >= '0' && *c <= '9') && !strchr("._-", *c))
            return false;
    }
    return *s;
}
