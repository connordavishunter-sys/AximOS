#include <aximos.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

// Checks the password on standard input is its caller's own, for the lock screen,
// which runs as them and so can't read /etc/shadow. It's setuid root to read it, and
// a wrong one takes a moment, so guessing is slow.

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: checkpass\n"
                            "Exits with 0 if the password on standard input is yours, as the lock screen asks.\n");

    struct passwd *p = getpwuid(getuid());
    char password[256] = "";
    bool right;

    if (!fgets(password, sizeof(password), stdin))
        return 1;
    password[strcspn(password, "\n")] = '\0';
    right = p && account_check(p->pw_name, password);
    memset(password, 0, sizeof(password));
    if (!right)
        sleep(2);
    return !right;
}
