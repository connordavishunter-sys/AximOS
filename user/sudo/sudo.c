#include <aximos.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Runs a command as root, for someone in the admin group who gives their own
// password. It's installed owned by root and setuid, so it starts as root.

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: sudo command [argument]...\n"
                            "Runs a command as root, after asking for your password. Only accounts in the\n"
                            "admin group can.\n");

    struct passwd *p = getpwuid(getuid());
    char password[256], prompt[100];

    if (argc < 2) {
        fprintf(stderr, "usage: sudo command [argument]...\n");
        return 1;
    }
    if (geteuid() != 0) {
        fprintf(stderr, "sudo: isn't setuid root, so it can't\n");
        return 1;
    }
    if (getuid() != 0) {
        if (!p || !account_is_admin(p->pw_name)) {
            fprintf(stderr, "sudo: only accounts in the %s group can\n", ADMIN_GROUP);
            return 1;
        }
        snprintf(prompt, sizeof(prompt), "Password for %s: ", p->pw_name);
        bool right = ask(prompt, password, sizeof(password), true) && account_check(p->pw_name, password);

        memset(password, 0, sizeof(password));
        if (!right) {
            fprintf(stderr, "sudo: wrong password\n");
            sleep(2);
            return 1;
        }
    }
    if (setgid(0) < 0 || setuid(0) < 0) {
        perror("sudo");
        return 1;
    }
    setenv("HOME", "/root", 1);
    setenv("USER", "root", 1);
    setenv("LOGNAME", "root", 1);
    setenv("PATH", "/system/bin", 1);
    execvp(argv[1], argv + 1);
    perror(argv[1]);
    return 127;
}
