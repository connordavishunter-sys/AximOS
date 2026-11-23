#include <aximos.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Changes a password. It's setuid root, to write /etc/shadow, and asks for the old
// password first unless root's running it, who can change anyone's.

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: passwd [name]\n"
                            "Changes your password, or as root, someone else's.\n");

    struct passwd *p = getpwuid(getuid());
    const char *name = argc > 1 ? argv[1] : p ? p->pw_name : NULL;
    char old[256], password[256];

    if (!name || !getpwnam(name)) {
        fprintf(stderr, "passwd: no account called %s\n", name ? name : "that");
        return 1;
    }
    if (getuid() != 0) {
        if (strcmp(name, p->pw_name)) {
            fprintf(stderr, "passwd: only root can change someone else's\n");
            return 1;
        }
        bool right = ask("Current password: ", old, sizeof(old), true) && account_check(name, old);

        memset(old, 0, sizeof(old));
        if (!right) {
            fprintf(stderr, "passwd: wrong password\n");
            sleep(2);
            return 1;
        }
    }
    if (!ask_new_password(password, sizeof(password)))
        return 1;

    bool ok = account_set_password(name, password);

    memset(password, 0, sizeof(password));
    if (!ok) {
        perror("passwd");
        return 1;
    }
    printf("passwd: changed %s's password\n", name);
    return 0;
}
