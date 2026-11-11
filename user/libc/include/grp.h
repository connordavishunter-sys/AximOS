#pragma once

#include <stddef.h>
#include <sys/types.h>

struct group {
    char *gr_name;
    char *gr_passwd;
    gid_t gr_gid;
    char **gr_mem;
};

// The only group is root.
struct group *getgrgid(gid_t gid);
struct group *getgrnam(const char *name);

// root is the only group and the only member of it.
struct group *getgrent(void);
void setgrent(void);
void endgrent(void);
int initgroups(const char *user, gid_t group);
int setgroups(size_t count, const gid_t *groups);
int getgrouplist(const char *user, gid_t group, gid_t *groups, int *count);
