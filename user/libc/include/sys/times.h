#pragma once

#include <time.h>

struct tms {
    clock_t tms_utime;
    clock_t tms_stime;
    clock_t tms_cutime;
    clock_t tms_cstime;
};

// All processor time counts as user time, and children aren't added up.
clock_t times(struct tms *out);
