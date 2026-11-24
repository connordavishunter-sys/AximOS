#include <aximos.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Asks for a name and password at the terminal, then runs the account's shell. The
// first time, with no accounts yet, it makes one, which can run commands as root
// through sudo.

// Returns false if someone made an account meanwhile, as on the desktop, leaving the
// name given for logging in with.
static bool make_first_account(char *name, size_t size) {
    char password[256];

    puts("Welcome to AximOS. Make an account to log in with.");
    do {
        if (!ask("Name: ", name, size, false))
            _exit(1);
        if (account_any())
            return false;
    } while (!account_name_ok(name) && puts("A name is letters, digits, dots, dashes and underscores, starting with a letter."));
    if (!ask_new_password(password, sizeof(password)))
        _exit(1);
    if (!account_create(name, password, true)) {
        perror("ttylogin: can't make the account");
        _exit(1);
    }
    memset(password, 0, sizeof(password));
    printf("ttylogin: made %s, who can run commands as root with sudo\n\n", name);
    return true;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: ttylogin\n"
                            "Asks at the terminal for an account's name and password, then runs its shell.\n"
                            "With no accounts yet, it makes the first.\n");

    char name[64] = "", password[256], host[64] = "aximos", prompt[100];

    if (getuid() != 0) {
        fprintf(stderr, "ttylogin: only root can log anyone in\n");
        return 1;
    }

    const char *tty = ttyname(STDIN_FILENO), *console = tty && strrchr(tty, '/') ? strrchr(tty, '/') + 1 : NULL;

    account_seat(console, NULL); // whoever was here's gone
    bool named = !account_any() && !make_first_account(name, sizeof(name));

    if (!named)
        name[0] = '\0';
    gethostname(host, sizeof(host));
    snprintf(prompt, sizeof(prompt), "%s login: ", host);
    for (;;) {
        if ((!named && !ask(prompt, name, sizeof(name), false)) || !ask("Password: ", password, sizeof(password), true))
            return 1;
        named = false;

        bool right = account_check(name, password);

        memset(password, 0, sizeof(password));
        if (right) {
            account_seat(console, name);
            if (account_become(name))
                break;
            account_seat(console, NULL);
        }
        puts("Wrong name or password.\n");
        sleep(2);
    }

    // A login shell, which its name starting with - says, reads /etc/profile.
    const char *shell = getenv("SHELL");
    char login[64];

    snprintf(login, sizeof(login), "-%s", strrchr(shell, '/') ? strrchr(shell, '/') + 1 : shell);
    printf("ttylogin: logged in %s\n", name);
    execl(shell, login, (char *)NULL);
    perror(shell);
    return 127;
}
