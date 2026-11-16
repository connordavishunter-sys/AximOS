#include "../login/account.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#define SHELL "/system/bin/dash"

// A line typed at the terminal, without its newline, and with echo off for a
// password. Returns false at the end of the input.
static bool ask(const char *prompt, char *out, int size, bool secret) {
    struct termios was, quiet;
    bool hidden = secret && tcgetattr(STDIN_FILENO, &was) == 0;
    bool got;

    printf("%s", prompt);
    fflush(stdout);
    if (hidden) {
        quiet = was;
        quiet.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &quiet);
    }
    got = fgets(out, size, stdin) != NULL;
    if (hidden) {
        tcsetattr(STDIN_FILENO, TCSANOW, &was);
        printf("\n");
    }
    out[strcspn(out, "\n")] = '\0';
    return got;
}

// Asks for the username and password until they're right, then runs the shell as
// that user. With no account yet, there's no one to log in as, so the shell starts.
int main(int argc, char **argv) {
    help_option(argc, argv, "usage: ttylogin [shell]\n"
                            "Asks at the terminal for the username and password the desktop logs in with,\n"
                            "then runs the shell. Until there's an account, the shell just starts.\n");

    const char *shell = argc > 1 ? argv[1] : SHELL;
    struct account account;
    char name[ACCOUNT_NAME], password[128];

    if (!account_load(&account)) {
        printf("ttylogin: no account yet, which the desktop makes the first time\n");
        execl(shell, shell, (char *)NULL);
        return 127;
    }
    for (;;) {
        if (!ask("AximOS login: ", name, sizeof(name), false) || !ask("Password: ", password, sizeof(password), true))
            return 1;

        // The password's checked whatever the name, so a wrong one takes as long.
        bool right = account_check(&account, password) && !strcmp(name, account.name);

        memset(password, 0, sizeof(password));
        if (right)
            break;
        printf("Wrong username or password.\n\n");
        sleep(2);
    }
    printf("ttylogin: logged in %s\n", account.name);
    setenv("USER", account.name, 1);
    execl(shell, shell, (char *)NULL);
    printf("ttylogin: can't run %s\n", shell);
    return 127;
}
