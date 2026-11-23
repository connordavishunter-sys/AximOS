#include <aximos.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Asks for a name and password at the terminal, then runs the account's shell. The
// first time, with no accounts yet, it makes one, which can run commands as root
// through sudo.

static void make_first_account(void) {
    char name[64], password[256];

    puts("Welcome to AximOS. Make an account to log in with.");
    do {
        if (!ask("Name: ", name, sizeof(name), false))
            _exit(1);
    } while (!account_name_ok(name) && puts("A name is letters, digits, dots, dashes and underscores, starting with a letter."));
    if (!ask_new_password(password, sizeof(password)))
        _exit(1);
    if (!account_create(name, password, true)) {
        perror("ttylogin: can't make the account");
        _exit(1);
    }
    memset(password, 0, sizeof(password));
    printf("ttylogin: made %s, who can run commands as root with sudo\n\n", name);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: ttylogin\n"
                            "Asks at the terminal for an account's name and password, then runs its shell.\n"
                            "With no accounts yet, it makes the first.\n");

    char name[64], password[256], host[64] = "aximos", prompt[100];

    if (getuid() != 0) {
        fprintf(stderr, "ttylogin: only root can log anyone in\n");
        return 1;
    }
    if (!account_any())
        make_first_account();
    gethostname(host, sizeof(host));
    snprintf(prompt, sizeof(prompt), "%s login: ", host);
    for (;;) {
        if (!ask(prompt, name, sizeof(name), false) || !ask("Password: ", password, sizeof(password), true))
            return 1;

        bool right = account_check(name, password);

        memset(password, 0, sizeof(password));
        if (right && account_become(name))
            break;
        puts("Wrong name or password.\n");
        sleep(2);
    }

    // A login shell, which its name starting with - says, reads /etc/profile.
    const char *shell = getenv("SHELL");

    printf("ttylogin: logged in %s\n", name);
    execl(shell, "-sh", (char *)NULL);
    perror(shell);
    return 127;
}
