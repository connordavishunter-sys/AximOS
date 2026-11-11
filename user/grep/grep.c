#include <dirent.h>
#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATUS_MATCH 0
#define STATUS_NO_MATCH 1
#define STATUS_ERROR 2

static const char usage[] = "usage: grep [-EFcilnqrvH] pattern [file...]\n";

static regex_t regex;
static const char *fixed;
static bool ignore_case, invert, count_only, names_only, line_numbers, quiet, recursive, show_names;
static bool any_match, any_error;

static bool contains_fixed(const char *line) {
    size_t len = strlen(fixed);

    for (const char *p = line; *p || len == 0; p++) {
        if ((ignore_case ? strncasecmp(p, fixed, len) : strncmp(p, fixed, len)) == 0)
            return true;
        if (!*p)
            break;
    }
    return false;
}

static bool line_matches(const char *line) {
    bool found = fixed ? contains_fixed(line) : regexec(&regex, line, 0, NULL, 0) == 0;

    return found != invert;
}

static void search(FILE *in, const char *name) {
    char *line = NULL;
    size_t size = 0;
    ssize_t len;
    long number = 0, count = 0;

    while ((len = getline(&line, &size, in)) >= 0) {
        number++;
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (!line_matches(line))
            continue;

        count++;
        any_match = true;
        if (quiet)
            exit(STATUS_MATCH);
        if (names_only)
            break;
        if (count_only)
            continue;
        if (show_names)
            printf("%s:", name);
        if (line_numbers)
            printf("%ld:", number);
        printf("%s\n", line);
    }
    free(line);

    if (count_only) {
        if (show_names)
            printf("%s:", name);
        printf("%ld\n", count);
    }
    if (names_only && count)
        printf("%s\n", name);
}

static void complain(const char *path) {
    if (!quiet)
        dprintf(STDERR_FILENO, "grep: %s: %s\n", path, strerror(errno));
    any_error = true;
}

static void search_path(const char *path) {
    struct stat st;

    if (stat(path, &st) < 0) {
        complain(path);
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        if (!recursive) {
            dprintf(STDERR_FILENO, "grep: %s: Is a directory\n", path);
            return;
        }

        DIR *dir = opendir(path);
        struct dirent *entry;

        if (!dir) {
            complain(path);
            return;
        }
        while ((entry = readdir(dir))) {
            char *child;

            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 || asprintf(&child, "%s/%s", path, entry->d_name) < 0)
                continue;
            search_path(child);
            free(child);
        }
        closedir(dir);
        return;
    }

    FILE *in = fopen(path, "r");

    if (!in) {
        complain(path);
        return;
    }
    search(in, path);
    fclose(in);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: grep [-EFcilnqrvH] pattern [file...]\n"
                            "Prints the lines that match a pattern, from standard input if no files are given.\n"
                            "  -E  extended regular expression\n"
                            "  -F  plain text rather than an expression\n"
                            "  -c  print how many lines matched\n"
                            "  -i  ignore case\n"
                            "  -l  print only the names of files that match\n"
                            "  -n  print line numbers\n"
                            "  -q  print nothing, only set the exit status\n"
                            "  -r  go through directories\n"
                            "  -v  print the lines that don't match\n"
                            "  -H  always print file names\n");

    bool extended = false, plain = false, force_names = false;
    int option;

    while ((option = getopt(argc, argv, "EFcilnqrvH")) != -1) {
        switch (option) {
        case 'E': extended = true; break;
        case 'F': plain = true; break;
        case 'c': count_only = true; break;
        case 'i': ignore_case = true; break;
        case 'l': names_only = true; break;
        case 'n': line_numbers = true; break;
        case 'q': quiet = true; break;
        case 'r': recursive = true; break;
        case 'v': invert = true; break;
        case 'H': force_names = true; break;
        default:
            dprintf(STDERR_FILENO, "%s", usage);
            return STATUS_ERROR;
        }
    }
    if (optind >= argc) {
        dprintf(STDERR_FILENO, "%s", usage);
        return STATUS_ERROR;
    }

    const char *pattern = argv[optind++];

    if (plain) {
        fixed = pattern;
    } else {
        int error = regcomp(&regex, pattern, (extended ? REG_EXTENDED : 0) | (ignore_case ? REG_ICASE : 0) | REG_NOSUB);

        if (error) {
            char text[128];

            regerror(error, &regex, text, sizeof(text));
            dprintf(STDERR_FILENO, "grep: %s\n", text);
            return STATUS_ERROR;
        }
    }

    show_names = force_names || argc - optind > 1 || recursive;
    if (optind == argc && !recursive)
        search(stdin, "(standard input)");
    else if (optind == argc)
        search_path(".");
    for (int i = optind; i < argc; i++)
        search_path(argv[i]);

    if (any_error)
        return STATUS_ERROR;
    return any_match ? STATUS_MATCH : STATUS_NO_MATCH;
}
