#include <aximos.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: useradd [-a] name\n"
                            "Makes an account, with a home folder, asking for its password. Only root can,\n"
                            "as through sudo.\n"
                            "  -a  in the admin group, so it can use sudo too\n");

    bool admin = argc > 2 && !strcmp(argv[1], "-a");
    const char *name = argv[argc - 1];
    char password[256];

    if (argc < 2 || (argc > 2 && !admin)) {
        fprintf(stderr, "usage: useradd [-a] name\n");
        return 1;
    }
    if (getuid() != 0) {
        fprintf(stderr, "useradd: only root can, as with sudo useradd\n");
        return 1;
    }
    if (!account_name_ok(name)) {
        fprintf(stderr, "useradd: a name is letters, digits, dots, dashes and underscores, starting with a letter\n");
        return 1;
    }
    if (!ask_new_password(password, sizeof(password)))
        return 1;

    bool ok = account_create(name, password, admin);

    memset(password, 0, sizeof(password));
    if (!ok) {
        fprintf(stderr, "useradd: can't make %s, which may already be taken\n", name);
        return 1;
    }
    printf("useradd: made %s%s\n", name, admin ? ", in the admin group" : "");
    return 0;
}
