#pragma once

#include <stdint.h>

// The one account, kept in ACCOUNT as its name, a random salt and the password's
// hash, in hex, which login and ttylogin both check.
#define ACCOUNT "/system/etc/account"
#define ACCOUNT_SALT 16
#define ACCOUNT_HASH 32
#define ACCOUNT_NAME 32

struct account {
    char name[ACCOUNT_NAME];
    uint8_t salt[ACCOUNT_SALT], hash[ACCOUNT_HASH];
};

// Returns false if there's no account yet.
bool account_load(struct account *account);

// Makes the account, with a new salt, and saves it.
bool account_save(struct account *account, const char *name, const char *password);

// Whether a password is the account's, taking as long whatever it is.
bool account_check(const struct account *account, const char *password);

// A name is letters, digits, dots, dashes and underscores.
bool account_name_ok(const char *name);
