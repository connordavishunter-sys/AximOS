#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char usage[] = "usage: find [path...] [-name pattern] [-iname pattern] [-type f|d] [-maxdepth n] [-mindepth n]\n";

static const char *name_pattern;
static bool ignore_case;
static char wanted_type;
static long max_depth = -1, min_depth;
static int status;

static const char *base_name(const char *path) {
    const char *end = path + strlen(path);

    while (end > path + 1 && end[-1] == '/')
        end--;

    const char *start = end;

    while (start > path && start[-1] != '/')
        start--;
    return start;
}

static char *lower_copy(const char *text) {
    char *copy = strdup(text);

    for (char *p = copy; p && *p; p++)
        *p = tolower((unsigned char)*p);
    return copy;
}

static bool name_matches(const char *name) {
    if (!ignore_case)
        return fnmatch(name_pattern, name, 0) == 0;

    char *lower_pattern = lower_copy(name_pattern);
    char *lower_name = lower_copy(name);
    bool matched = lower_pattern && lower_name && fnmatch(lower_pattern, lower_name, 0) == 0;

    free(lower_pattern);
    free(lower_name);
    return matched;
}

static void visit(const char *path, long depth) {
    struct stat st;

    if (stat(path, &st) < 0) {
        dprintf(STDERR_FILENO, "find: %s: %s\n", path, strerror(errno));
        status = 1;
        return;
    }

    bool is_dir = S_ISDIR(st.st_mode);
    bool type_ok = !wanted_type || (wanted_type == 'd') == is_dir;
    bool name_ok = !name_pattern || name_matches(base_name(path));

    if (type_ok && name_ok && depth >= min_depth)
        printf("%s\n", path);
    if (!is_dir || (max_depth >= 0 && depth >= max_depth))
        return;

    DIR *dir = opendir(path);
    struct dirent *entry;

    if (!dir) {
        dprintf(STDERR_FILENO, "find: %s: %s\n", path, strerror(errno));
        status = 1;
        return;
    }
    while ((entry = readdir(dir))) {
        char *child;
        bool has_slash = path[strlen(path) - 1] == '/';

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0
            || asprintf(&child, "%s%s%s", path, has_slash ? "" : "/", entry->d_name) < 0)
            continue;
        visit(child, depth + 1);
        free(child);
    }
    closedir(dir);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: find [path...] [-name pattern] [-iname pattern] [-type f|d] [-maxdepth n] [-mindepth n]\n"
                            "Lists what's under the paths, by default the current directory.\n"
                            "  -name      only names matching a pattern like *.txt\n"
                            "  -iname     the same, ignoring case\n"
                            "  -type      only files (f) or directories (d)\n"
                            "  -maxdepth  go no deeper than this\n"
                            "  -mindepth  print nothing shallower than this\n");

    int paths_end = 1;

    while (paths_end < argc && argv[paths_end][0] != '-')
        paths_end++;

    for (int i = paths_end; i < argc; i += 2) {
        if (i + 1 >= argc) {
            dprintf(STDERR_FILENO, "%s", usage);
            return 1;
        }
        if (strcmp(argv[i], "-name") == 0) {
            name_pattern = argv[i + 1];
        } else if (strcmp(argv[i], "-iname") == 0) {
            name_pattern = argv[i + 1];
            ignore_case = true;
        } else if (strcmp(argv[i], "-type") == 0 && (argv[i + 1][0] == 'f' || argv[i + 1][0] == 'd') && !argv[i + 1][1]) {
            wanted_type = argv[i + 1][0];
        } else if (strcmp(argv[i], "-maxdepth") == 0) {
            max_depth = atol(argv[i + 1]);
        } else if (strcmp(argv[i], "-mindepth") == 0) {
            min_depth = atol(argv[i + 1]);
        } else {
            dprintf(STDERR_FILENO, "%s", usage);
            return 1;
        }
    }

    if (paths_end == 1)
        visit(".", 0);
    for (int i = 1; i < paths_end; i++)
        visit(argv[i], 0);
    return status;
}
