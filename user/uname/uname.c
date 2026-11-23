#include <aximos.h>
#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: uname [-asnrvm]\n"
                            "Shows the system's name, or with options, more about it:\n"
                            "  -s  the kernel's name    -n  the computer's      -r  the kernel's release\n"
                            "  -v  its version          -m  the processor       -a  all of them\n");

    struct utsname u;
    const char *letters = argc > 1 && argv[1][0] == '-' ? argv[1] + 1 : "s";
    const char *fields[] = {u.sysname, u.nodename, u.release, u.version, u.machine};
    bool first = true;

    uname(&u);
    for (int i = 0; i < 5; i++) {
        if (strchr(letters, "snrvm"[i]) || strchr(letters, 'a')) {
            printf("%s%s", first ? "" : " ", fields[i]);
            first = false;
        }
    }
    printf("\n");
    return 0;
}
