#pragma once

#include <sys/types.h>

struct passwd {
    char *pw_name;
    char *pw_passwd;
    uid_t pw_uid;
    gid_t pw_gid;
    char *pw_gecos;
    char *pw_dir;
    char *pw_shell;
};

// The only user is root.
struct passwd *getpwuid(uid_t uid);
struct passwd *getpwnam(const char *name);

// root is the only user.
struct passwd *getpwent(void);
void setpwent(void);
void endpwent(void);
