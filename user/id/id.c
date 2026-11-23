#include <aximos.h>
#include <grp.h>
#include <pwd.h>
#include <stdio.h>
#include <unistd.h>

#define MAX_GROUPS 64

static const char *group_name(gid_t gid) {
    struct group *g = getgrgid(gid);

    return g ? g->gr_name : "?";
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: id\n"
                            "Shows who you are, and the groups you're in.\n");

    struct passwd *p = getpwuid(getuid());
    gid_t groups[MAX_GROUPS];
    int count = getgroups(MAX_GROUPS, groups);

    printf("uid=%u(%s) gid=%u(%s) groups=", getuid(), p ? p->pw_name : "?", getgid(), group_name(getgid()));
    for (int i = 0; i < count; i++)
        printf("%s%u(%s)", i ? "," : "", groups[i], group_name(groups[i]));
    printf("\n");
    return 0;
}
