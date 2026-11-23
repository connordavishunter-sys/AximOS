#include <aximos.h>
#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// A user or group by name or number.
static bool find(const char *name, bool group, unsigned *id) {
    char *end;
    unsigned long number = strtoul(name, &end, 10);
    struct passwd *p = group ? NULL : getpwnam(name);
    struct group *g = group ? getgrnam(name) : NULL;

    if (!*end && end != name)
        *id = number;
    else if (p || g)
        *id = p ? p->pw_uid : g->gr_gid;
    else
        return false;
    return true;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: chown owner[:group] file...\n"
                            "Changes who owns files, and with :group, their group too.\n");

    unsigned uid = -1, gid = -1;
    int failed = 0;

    if (argc < 3) {
        fprintf(stderr, "usage: chown owner[:group] file...\n");
        return 1;
    }

    char *group = strchr(argv[1], ':');

    if (group)
        *group++ = '\0';
    if ((argv[1][0] && !find(argv[1], false, &uid)) || (group && *group && !find(group, true, &gid))) {
        fprintf(stderr, "chown: no such user or group\n");
        return 1;
    }
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], uid, gid) < 0) {
            fprintf(stderr, "chown: %s: %s\n", argv[i], strerror(errno));
            failed = 1;
        }
    }
    return failed;
}
