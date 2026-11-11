#include <stdio.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: date\n"
                            "Prints the date and time in UTC.\n");

    static const char *const days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                         "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    time_t now = time(NULL);
    struct tm t;

    gmtime_r(&now, &t);
    printf("%s %s %2d %02d:%02d:%02d UTC %d\n", days[t.tm_wday], months[t.tm_mon], t.tm_mday,
           t.tm_hour, t.tm_min, t.tm_sec, t.tm_year + 1900);
    return 0;
}
