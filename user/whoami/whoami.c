#include <aximos.h>
#include <pwd.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: whoami\n"
                            "Shows the name you're logged in as.\n");

    struct passwd *p = getpwuid(geteuid());

    if (!p) {
        printf("%u\n", geteuid());
        return 1;
    }
    printf("%s\n", p->pw_name);
    return 0;
}
