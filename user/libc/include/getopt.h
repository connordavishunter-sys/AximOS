#pragma once

#include <unistd.h>

#define no_argument 0
#define required_argument 1
#define optional_argument 2

struct option {
    const char *name;
    int has_arg;
    int *flag;
    int val;
};

// Like getopt, also taking --name and --name=value.
int getopt_long(int argc, char *const argv[], const char *options, const struct option *long_options, int *index);
