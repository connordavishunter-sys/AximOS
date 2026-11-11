#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_ENTRIES 1024

static char *names[MAX_ENTRIES];
static int count;

// Inserts in sorted order, and marks directories with a trailing slash.
static void add_name(const char *name, bool is_dir) {
    if (count == MAX_ENTRIES)
        return;

    char *copy = malloc(strlen(name) + 2);
    if (!copy)
        return;

    strcpy(copy, name);
    if (is_dir)
        strcpy(&copy[strlen(copy)], "/");

    int at = count;
    while (at > 0 && strcmp(names[at - 1], copy) > 0) {
        names[at] = names[at - 1];
        at--;
    }
    names[at] = copy;
    count++;
}

static int compare_names(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

// One line per file: type, size, modification time in UTC, name.
static void print_long(const char *dir, const char *name) {
    static char path[PATH_MAX];
    struct stat st;

    if (dir && strlen(dir) + strlen(name) + 2 <= PATH_MAX) {
        strcpy(path, dir);
        strcpy(&path[strlen(path)], "/");
        strcpy(&path[strlen(path)], name);
    } else {
        strcpy(path, name);
    }

    if (stat(path, &st) < 0) {
        st.st_mode = 0;
        st.st_size = 0;
        st.st_mtime = 0;
    }

    printf("%c ", S_ISDIR(st.st_mode) ? 'd' : '-');
    print_size(st.st_size, 10);
    if (st.st_mtime == 0) {
        printf(" %16s  %s\n", "-", name);
        return;
    }

    struct tm t;
    time_t when = st.st_mtime;
    localtime_r(&when, &t);
    printf(" %d-%02d-%02d %02d:%02d  %s\n", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
           t.tm_hour, t.tm_min, name);
}

static bool one_per_line;

static void print_names_long(const char *dir) {
    for (int i = 0; i < count; i++) {
        size_t len = strlen(names[i]);

        // The sort order and name came with a marker for directories.
        if (len > 1 && names[i][len - 1] == '/')
            names[i][len - 1] = '\0';
        print_long(dir, names[i]);
        free(names[i]);
    }
    count = 0;
}

// Into a pipe or a file, each name is on its own line without the directory marker,
// so that other programs can read them.
static void print_names_plain(void) {
    for (int i = 0; i < count; i++) {
        size_t len = strlen(names[i]);

        if (len > 1 && names[i][len - 1] == '/')
            names[i][len - 1] = '\0';
        printf("%s\n", names[i]);
        free(names[i]);
    }
    count = 0;
}

static void print_names(void) {
    int rows, cols = 80;
    size_t width = 0;

    if (one_per_line)
        return print_names_plain();
    termsize(&rows, &cols);
    for (int i = 0; i < count; i++) {
        if (strlen(names[i]) + 2 > width)
            width = strlen(names[i]) + 2;
    }

    int per_row = width ? cols / width : 1;
    if (per_row < 1)
        per_row = 1;

    for (int i = 0; i < count; i++) {
        if ((i + 1) % per_row == 0 || i + 1 == count)
            printf("%s\n", names[i]);
        else
            printf("%-*s", (int)width, names[i]);
        free(names[i]);
    }
    count = 0;
}

static int list_path(const char *path, bool show_header, bool long_format, bool show_hidden) {
    struct stat st;

    if (stat(path, &st) < 0) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(errno));
        return 2;
    }

    if (!S_ISDIR(st.st_mode)) {
        add_name(path, false);
        if (long_format)
            print_names_long(NULL);
        else
            print_names();
        return 0;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        dprintf(STDERR_FILENO, "ls: %s: %s\n", path, strerror(errno));
        return 1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (show_hidden || entry->d_name[0] != '.')
            add_name(entry->d_name, entry->d_type == DT_DIR);
    }
    closedir(dir);

    if (show_header)
        printf("%s:\n", path);
    if (long_format)
        print_names_long(path);
    else
        print_names();
    return 0;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: ls [-1al] [path...]\n"
                            "Lists directories, by default the current one.\n"
                            "  -1  one name per line, as when the output isn't a terminal\n"
                            "  -a  include names starting with a dot\n"
                            "  -l  show size and date\n");

    bool long_format = false;
    bool show_hidden = false;
    int option;

    one_per_line = !isatty(STDOUT_FILENO);
    while ((option = getopt(argc, argv, "1al")) != -1) {
        if (option == '1') {
            one_per_line = true;
        } else if (option == 'a') {
            show_hidden = true;
        } else if (option == 'l') {
            long_format = true;
        } else {
            dprintf(STDERR_FILENO, "usage: ls [-1al] [path...]\n");
            return 2;
        }
    }

    if (optind == argc)
        return list_path(".", false, long_format, show_hidden);

    qsort(argv + optind, argc - optind, sizeof(*argv), compare_names);

    int status = 0;
    for (int i = optind; i < argc; i++)
        status |= list_path(argv[i], argc - optind > 1, long_format, show_hidden);
    return status;
}
