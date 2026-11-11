#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

static const char usage[] = "usage: sort [-nrufb] [-k field] [-t separator] [file...]\n";

struct line {
    char *text;
    size_t order; // where it came, which keeps equal lines as they were
};

static bool numeric, reverse, unique, fold_case;
static long key_field;
static int separator = -1;
static struct line *lines;
static size_t line_count, line_room;

// Where a line's key starts. Fields are split by the separator or else by blanks.
static const char *key_of(const char *text) {
    const char *p = text;

    for (long field = 1; field < key_field && *p; field++) {
        if (separator >= 0) {
            while (*p && *p != separator)
                p++;
            if (*p)
                p++;
        } else {
            while (*p == ' ' || *p == '\t')
                p++;
            while (*p && *p != ' ' && *p != '\t')
                p++;
        }
    }
    return p;
}

static int compare_keys(const char *a, const char *b) {
    if (numeric) {
        double x = strtod(a, NULL), y = strtod(b, NULL);

        return x < y ? -1 : x > y;
    }
    return fold_case ? strcasecmp(a, b) : strcmp(a, b);
}

static int compare(const void *left, const void *right) {
    const struct line *a = left, *b = right;
    int result = compare_keys(key_of(a->text), key_of(b->text));

    if (result)
        return reverse ? -result : result;
    if (!unique && strcmp(a->text, b->text))
        return reverse ? -strcmp(a->text, b->text) : strcmp(a->text, b->text);
    return a->order < b->order ? -1 : a->order > b->order;
}

static void read_lines(FILE *in) {
    char *text = NULL;
    size_t size = 0;
    ssize_t len;

    while ((len = getline(&text, &size, in)) >= 0) {
        if (len > 0 && text[len - 1] == '\n')
            text[len - 1] = '\0';
        if (line_count == line_room) {
            line_room = line_room ? line_room * 2 : 1024;
            lines = realloc(lines, line_room * sizeof(*lines));
            if (!lines) {
                dprintf(STDERR_FILENO, "sort: out of memory\n");
                exit(2);
            }
        }
        lines[line_count].text = strdup(text);
        lines[line_count].order = line_count;
        line_count++;
    }
    free(text);
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: sort [-nrufb] [-k field] [-t separator] [file...]\n"
                            "Sorts lines, from standard input if no files are given.\n"
                            "  -n  by number\n"
                            "  -r  backwards\n"
                            "  -u  print only the first of lines that compare equal\n"
                            "  -f  ignore case\n"
                            "  -k  compare from this field on, counting from 1\n"
                            "  -t  fields are split by this character instead of blanks\n");

    int option;

    while ((option = getopt(argc, argv, "nrufbk:t:")) != -1) {
        switch (option) {
        case 'n': numeric = true; break;
        case 'r': reverse = true; break;
        case 'u': unique = true; break;
        case 'f': fold_case = true; break;
        case 'b': break;
        case 'k': key_field = atol(optarg); break;
        case 't': separator = optarg[0]; break;
        default:
            dprintf(STDERR_FILENO, "%s", usage);
            return 2;
        }
    }

    int status = 0;

    if (optind == argc)
        read_lines(stdin);
    for (int i = optind; i < argc; i++) {
        FILE *in = strcmp(argv[i], "-") == 0 ? stdin : fopen(argv[i], "r");

        if (!in) {
            dprintf(STDERR_FILENO, "sort: %s: %s\n", argv[i], strerror(errno));
            status = 2;
            continue;
        }
        read_lines(in);
        if (in != stdin)
            fclose(in);
    }

    qsort(lines, line_count, sizeof(*lines), compare);

    for (size_t i = 0; i < line_count; i++) {
        if (unique && i > 0 && compare_keys(key_of(lines[i].text), key_of(lines[i - 1].text)) == 0)
            continue;
        puts(lines[i].text);
    }
    return status;
}
