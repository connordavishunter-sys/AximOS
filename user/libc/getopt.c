#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>

char *optarg;
int optind = 1, opterr = 1, optopt;

static int cluster = 1;

static int short_option(int argc, char *const argv[], const char *options) {
    if (optind >= argc || !argv[optind] || argv[optind][0] != '-' || !argv[optind][1])
        return -1;
    if (strcmp(argv[optind], "--") == 0) {
        optind++;
        return -1;
    }

    const char *word = argv[optind];
    bool quiet = options[0] == ':';
    int option = word[cluster++];
    const char *found = option == ':' ? NULL : strchr(options, option);
    bool last = word[cluster] == '\0';

    optopt = option;
    if (!found) {
        if (opterr && !quiet)
            dprintf(STDERR_FILENO, "%s: unknown option -%c\n", argv[0], option);
        if (last) {
            optind++;
            cluster = 1;
        }
        return '?';
    }

    if (found[1] != ':') {
        if (last) {
            optind++;
            cluster = 1;
        }
        return option;
    }

    // The argument is the rest of this word, or else the whole of the next one,
    // unless it's optional, when only the rest of this word can be it.
    const char *rest = &word[cluster];

    optind++;
    cluster = 1;
    if (!last) {
        optarg = (char *)rest;
    } else if (found[2] == ':') {
        return option;
    } else if (optind < argc) {
        optarg = argv[optind++];
    } else {
        if (opterr && !quiet)
            dprintf(STDERR_FILENO, "%s: option -%c needs an argument\n", argv[0], option);
        return quiet ? ':' : '?';
    }
    return option;
}

static int one_option(int argc, char *const argv[], const char *options, const struct option *long_options, int *index) {
    const char *word = optind < argc ? argv[optind] : NULL;

    if (!long_options || !word || strncmp(word, "--", 2) != 0 || !word[2])
        return short_option(argc, argv, options);

    const char *name = word + 2;
    size_t len = strcspn(name, "=");
    const struct option *found = NULL;

    for (const struct option *o = long_options; o->name; o++) {
        if (strncmp(o->name, name, len) != 0)
            continue;
        bool exact = o->name[len] == '\0';

        // A unique prefix will do, and an exact name always wins.
        if (found && !exact) {
            found = NULL;
            break;
        }
        found = o;
        if (exact)
            break;
    }

    bool quiet = options[0] == ':';

    optind++;
    optarg = NULL;
    if (!found) {
        if (opterr && !quiet)
            dprintf(STDERR_FILENO, "%s: unknown option %s\n", argv[0], word);
        optopt = 0;
        return '?';
    }

    if (name[len] == '=') {
        if (found->has_arg == no_argument) {
            if (opterr && !quiet)
                dprintf(STDERR_FILENO, "%s: option --%s takes no value\n", argv[0], found->name);
            optopt = found->val;
            return '?';
        }
        optarg = (char *)name + len + 1;
    } else if (found->has_arg == required_argument) {
        if (optind == argc) {
            if (opterr && !quiet)
                dprintf(STDERR_FILENO, "%s: option --%s needs a value\n", argv[0], found->name);
            optopt = found->val;
            return quiet ? ':' : '?';
        }
        optarg = argv[optind++];
    }

    if (index)
        *index = found - long_options;
    if (found->flag) {
        *found->flag = found->val;
        return 0;
    }
    return found->val;
}

// Options can come after other arguments: those get moved along to the end as the
// options are found. A leading + in options stops at the first other argument, as
// POSIXLY_CORRECT does, and a leading - returns each as if it were option 1. Setting
// optind to 0 starts again.
static int next_option(int argc, char **argv, const char *options, const struct option *long_options, int *index) {
    if (optind == 0) {
        optind = 1;
        cluster = 1;
    }
    optarg = NULL;

    bool in_order = *options == '+' || getenv("POSIXLY_CORRECT");
    bool return_others = *options == '-';

    if (*options == '+' || *options == '-')
        options++;

    int skipped = optind;

    if (return_others && optind < argc && argv[optind] && (argv[optind][0] != '-' || !argv[optind][1])) {
        optarg = argv[optind++];
        return 1;
    }
    if (!in_order && !return_others) {
        while (optind < argc && argv[optind] && (argv[optind][0] != '-' || !argv[optind][1]))
            optind++;
        if (optind >= argc || !argv[optind]) {
            optind = skipped;
            return -1;
        }
    }

    int resumed = optind;
    int result = one_option(argc, argv, options, long_options, index);

    if (resumed > skipped) {
        int used = optind - resumed;

        for (int i = 0; i < used; i++) {
            char *moved = argv[resumed + i];

            memmove(&argv[skipped + i + 1], &argv[skipped + i], (resumed - skipped) * sizeof(*argv));
            argv[skipped + i] = moved;
        }
        optind = skipped + used;
    }
    return result;
}

int getopt(int argc, char *const argv[], const char *options) {
    return next_option(argc, (char **)argv, options, NULL, NULL);
}

int getopt_long(int argc, char *const argv[], const char *options, const struct option *long_options, int *index) {
    return next_option(argc, (char **)argv, options, long_options, index);
}
