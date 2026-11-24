#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// The desktop, which init runs on the screen's first console: the log-in screen,
// shown by wm running as the greeter account, until someone logs in, then wm as
// them until they log out, and the log-in screen again. It runs as root, so it's
// what checks passwords, and makes the first account, as the log-in screen asks
// through GUI_SESSION_FD:
//
//   log in, a name, a password             answered with ok, or wrong
//   create, a name, a password, a zone     answered with ok, or what's wrong
//
// a line each. The new account gets the look the greeter was given, in its own
// ~/.config/aximos.

#define GREETER "greeter"
#define WM "/system/bin/wm"
#define TZ "/system/bin/tz"
#define RETRY_SECONDS 2 // before the log-in screen's started again, if it stops
#define CONSOLE "tty1" // the desktop's, where seatd starts it
#define PATH "/system/bin:/system/bin/gui:/system/bin/gui/Accessories:/system/bin/gui/Games:/system/bin/gui/System:" \
             "/system/bin/gui/Demos" // with the apps, in the start menu's folders

static pid_t start_wm(const char *account, int session_fd, bool greeter) {
    pid_t pid = fork();

    if (pid == 0) {
        char fd[16];

        if (!account_become(account)) {
            fprintf(stderr, "session: can't become %s\n", account);
            _exit(127);
        }
        if (session_fd >= 0) {
            snprintf(fd, sizeof(fd), "%d", session_fd);
            setenv("GUI_SESSION_FD", fd, 1);
        }
        setenv("PATH", PATH, 1);
        execl(WM, "wm", greeter ? "--login" : (char *)NULL, (char *)NULL);
        _exit(127);
    }
    return pid;
}

static bool line(FILE *f, char *out, size_t size) {
    if (!fgets(out, size, f))
        return false;
    out[strcspn(out, "\n")] = '\0';
    return true;
}

static void run(const char *program, const char *argument) {
    pid_t pid = fork();

    if (pid == 0) {
        execl(program, program, argument, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}

// The greeter's look, copied to the new account's ~/.config/aximos.
static void give_look(const char *name) {
    struct passwd *greeter = getpwnam(GREETER), *p = getpwnam(name);
    char from[300], to[300];

    if (!greeter || !p)
        return;
    snprintf(to, sizeof(to), "%s/.config", p->pw_dir);
    mkdir(to, 0700);
    chown(to, p->pw_uid, p->pw_gid);
    snprintf(from, sizeof(from), "%s/.config/aximos", greeter->pw_dir);
    snprintf(to, sizeof(to), "%s/.config/aximos", p->pw_dir);
    mkdir(to, 0700);
    chown(to, p->pw_uid, p->pw_gid);
    for (const char *const *file = (const char *const[]){"gui.conf", "wallpaper", NULL}; *file; file++) {
        char a[400], b[400], buf[4096];
        int in, out;
        ssize_t n;

        snprintf(a, sizeof(a), "%s/%s", from, *file);
        snprintf(b, sizeof(b), "%s/%s", to, *file);
        if ((in = open(a, O_RDONLY)) < 0)
            continue;
        if ((out = open(b, O_WRONLY | O_CREAT | O_TRUNC, 0600)) >= 0) {
            while ((n = read(in, buf, sizeof(buf))) > 0 && write(out, buf, n) == n)
                ;
            fchown(out, p->pw_uid, p->pw_gid);
            close(out);
        }
        close(in);
    }
}

// Answers the log-in screen until someone's logged in, giving their name, or it
// stops, returning false.
static bool serve(int fd, char *name, size_t size) {
    FILE *f = fdopen(dup(fd), "r");
    char request[32], password[256], zone[256];
    bool in = false;

    while (!in && f && line(f, request, sizeof(request)) && line(f, name, size) && line(f, password, sizeof(password))) {
        const char *answer = "wrong\n";

        if (!strcmp(request, "log in")) {
            in = account_check(name, password);
            answer = in ? "ok\n" : answer;
        } else if (!strcmp(request, "create") && line(f, zone, sizeof(zone))) {
            if (account_any())
                answer = "There's an account already.\n";
            else if (!account_name_ok(name))
                answer = "A username is letters, digits, dots, dashes and underscores, starting with a letter.\n";
            else if (!account_create(name, password, true))
                answer = "Couldn't make the account.\n";
            else {
                if (zone[0])
                    run(TZ, zone);
                give_look(name);
                in = true;
                answer = "ok\n";
            }
        }
        memset(password, 0, sizeof(password));
        if (write(fd, answer, strlen(answer)) < 0)
            break;
    }
    if (f)
        fclose(f);
    return in;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: session\n"
                            "Runs the desktop: the log-in screen until someone logs in, then wm as them. init\n"
                            "starts it.\n");

    struct passwd *greeter = getpwnam(GREETER);

    if (getuid() != 0 || !greeter) {
        fprintf(stderr, "session: needs to be root, with a %s account\n", GREETER);
        return 1;
    }
    signal(SIGPIPE, SIG_IGN);
    mkdir(greeter->pw_dir, 0700);
    chown(greeter->pw_dir, greeter->pw_uid, greeter->pw_gid);
    for (;;) {
        int pair[2];
        char name[64];

        if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) < 0) {
            perror("session");
            return 1;
        }
        fcntl(pair[1], F_SETFD, 0); // for the greeter's wm, and the log-in screen it starts

        account_seat(CONSOLE, GREETER);

        pid_t shown = start_wm(GREETER, pair[1], true);
        bool in;

        close(pair[1]);
        in = serve(pair[0], name, sizeof(name));
        close(pair[0]);
        kill(shown, SIGTERM); // as it's done with
        waitpid(shown, NULL, 0);
        if (!in) {
            sleep(RETRY_SECONDS);
            continue;
        }
        printf("session: %s logged in\n", name);
        fflush(stdout);
        account_seat(CONSOLE, name);
        waitpid(start_wm(name, -1, false), NULL, 0);
        account_seat(CONSOLE, NULL);
        printf("session: %s logged out\n", name);
        fflush(stdout);
    }
}
