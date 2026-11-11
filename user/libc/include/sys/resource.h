#pragma once

#include <sys/time.h>

#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_CORE 4
#define RLIMIT_NOFILE 7
#define RLIMIT_STACK 3
#define RLIM_INFINITY (~0ul)

typedef unsigned long rlim_t;

struct rlimit {
    rlim_t rlim_cur;
    rlim_t rlim_max;
};

// Reports the real limits. Nothing can change them, so setting only checks that
// the values asked for are what they already are or more.
struct rusage {
    struct timeval ru_utime;
    struct timeval ru_stime;
    long ru_maxrss;
    long ru_minflt;
    long ru_majflt;
    long ru_nvcsw;
    long ru_nivcsw;
};

#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1

int getrlimit(int resource, struct rlimit *out);
int setrlimit(int resource, const struct rlimit *limit);
