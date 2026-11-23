// Prints each timestamp given in local time, and whether mktime turns it back.
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        time_t when = atol(argv[i]);
        struct tm t;
        char text[64];

        localtime_r(&when, &t);
        strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S %Z %z", &t);

        // A time that happens twice without summer time to tell them apart can come
        // back as the other one, the same on the clock.
        struct tm copy = t;
        time_t back = mktime(&copy);
        bool same = copy.tm_isdst == t.tm_isdst && copy.tm_hour == t.tm_hour && copy.tm_min == t.tm_min
                 && copy.tm_sec == t.tm_sec && copy.tm_mday == t.tm_mday;

        printf("%ld %s %s\n", (long)when, text, back == when ? "back" : same ? "same" : "LOST");
    }
    printf("tzcheck: done\n");
    return 0;
}
