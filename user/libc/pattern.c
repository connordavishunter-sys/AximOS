#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>
#include <libgen.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

char *basename(char *path) {
    if (!path || !*path)
        return ".";

    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/')
        path[--len] = '\0';
    if (len == 1 && path[0] == '/')
        return path;

    char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

char *dirname(char *path) {
    if (!path || !*path)
        return ".";

    size_t len = strlen(path);

    while (len > 1 && path[len - 1] == '/')
        len--;
    while (len > 0 && path[len - 1] != '/')
        len--;
    if (len == 0)
        return ".";
    while (len > 1 && path[len - 1] == '/')
        len--;
    path[len] = '\0';
    return path;
}

// Matches a [...] set against c, setting where the pattern goes next. Returns -1 if
// there's no closing bracket, and then the [ is taken literally.
static int match_set(const char *p, char c, int flags, const char **rest) {
    bool negated = *p == '!' || *p == '^';
    bool found = false;

    if (negated)
        p++;
    for (bool first = true; first || *p != ']'; first = false) {
        char low = *p;

        if (!low)
            return -1;
        if (low == '\\' && !(flags & FNM_NOESCAPE) && p[1])
            low = *++p;
        p++;

        char high = low;

        if (p[0] == '-' && p[1] && p[1] != ']') {
            high = p[1];
            if (high == '\\' && !(flags & FNM_NOESCAPE) && p[2])
                high = *++p;
            p += 2;
        }
        if (c >= low && c <= high)
            found = true;
    }
    *rest = p + 1;
    return found != negated;
}

static bool leading_period(const char *s, const char *start, int flags) {
    if (*s != '.' || !(flags & FNM_PERIOD))
        return false;
    return s == start || ((flags & FNM_PATHNAME) && s[-1] == '/');
}

static int match_from(const char *p, const char *s, const char *start, int flags) {
    for (;; p++, s++) {
        switch (*p) {
        case '\0':
            return *s ? FNM_NOMATCH : 0;
        case '?':
            if (!*s || (*s == '/' && (flags & FNM_PATHNAME)) || leading_period(s, start, flags))
                return FNM_NOMATCH;
            break;
        case '*':
            if (leading_period(s, start, flags))
                return FNM_NOMATCH;
            while (*p == '*')
                p++;
            for (;; s++) {
                if (match_from(p, s, start, flags) == 0)
                    return 0;
                if (!*s || (*s == '/' && (flags & FNM_PATHNAME)))
                    return FNM_NOMATCH;
            }
        case '[': {
            const char *rest;

            if (!*s || (*s == '/' && (flags & FNM_PATHNAME)) || leading_period(s, start, flags))
                return FNM_NOMATCH;

            int result = match_set(p + 1, *s, flags, &rest);

            if (result < 0) {
                if (*s != '[')
                    return FNM_NOMATCH;
                break;
            }
            if (!result)
                return FNM_NOMATCH;
            p = rest - 1;
            break;
        }
        case '\\':
            if (!(flags & FNM_NOESCAPE) && p[1])
                p++;
            [[fallthrough]];
        default:
            if (*p != *s)
                return FNM_NOMATCH;
        }
    }
}

int fnmatch(const char *pattern, const char *string, int flags) {
    return match_from(pattern, string, string, flags);
}

static bool add_result(glob_t *g, const char *path, bool mark) {
    char **list = realloc(g->gl_pathv, (g->gl_pathc + 2) * sizeof(*list));

    if (!list)
        return false;
    g->gl_pathv = list;

    size_t len = strlen(path);
    char *copy = malloc(len + 2);

    if (!copy)
        return false;
    strcpy(copy, path);
    if (mark) {
        copy[len] = '/';
        copy[len + 1] = '\0';
    }
    list[g->gl_pathc++] = copy;
    list[g->gl_pathc] = NULL;
    return true;
}

static bool has_wildcards(const char *s, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '*' || s[i] == '?' || s[i] == '[')
            return true;
    }
    return false;
}

// Works through a pattern one path component at a time, with prefix what's been
// matched so far.
static int expand(const char *prefix, const char *pattern, int flags, int (*on_error)(const char *, int), glob_t *g) {
    while (*pattern == '/')
        pattern++;

    size_t len = strcspn(pattern, "/");
    const char *next = pattern + len;
    char path[PATH_MAX];

    if (len == 0) {
        struct stat st;

        if (stat(prefix[0] ? prefix : ".", &st) != 0)
            return 0;
        return add_result(g, prefix, (flags & GLOB_MARK) && S_ISDIR(st.st_mode)) ? 0 : GLOB_NOSPACE;
    }

    size_t prefix_len = strlen(prefix);
    bool needs_slash = prefix_len > 0 && prefix[prefix_len - 1] != '/';

    if (prefix_len + len + 2 > sizeof(path))
        return 0;

    if (!has_wildcards(pattern, len)) {
        char *end = path + prefix_len + needs_slash;

        strcpy(path, prefix);
        if (needs_slash)
            path[prefix_len] = '/';
        memcpy(end, pattern, len);
        end[len] = '\0';
        return expand(path, next, flags, on_error, g);
    }

    char part[NAME_MAX + 1];

    if (len > NAME_MAX)
        return 0;
    memcpy(part, pattern, len);
    part[len] = '\0';

    DIR *dir = opendir(prefix[0] ? prefix : ".");

    if (!dir) {
        if ((on_error && on_error(prefix, errno)) || (flags & GLOB_ERR))
            return GLOB_ABORTED;
        return 0;
    }

    int match_flags = FNM_PERIOD | (flags & GLOB_NOESCAPE ? FNM_NOESCAPE : 0);
    int status = 0;
    struct dirent *entry;

    while (status == 0 && (entry = readdir(dir))) {
        if (fnmatch(part, entry->d_name, match_flags) != 0)
            continue;
        if (prefix_len + strlen(entry->d_name) + 2 > sizeof(path))
            continue;

        strcpy(path, prefix);
        if (needs_slash)
            strcat(path, "/");
        strcat(path, entry->d_name);
        status = expand(path, next, flags, on_error, g);
    }
    closedir(dir);
    return status;
}

static int compare_paths(const void *a, const void *b) {
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags, int (*on_error)(const char *, int), glob_t *g) {
    if (!(flags & GLOB_APPEND)) {
        g->gl_pathc = 0;
        g->gl_pathv = NULL;
    }

    size_t before = g->gl_pathc;
    int status = expand(*pattern == '/' ? "/" : "", pattern, flags, on_error, g);

    if (status)
        return status;
    if (g->gl_pathc == before) {
        if (!(flags & GLOB_NOCHECK))
            return GLOB_NOMATCH;
        if (!add_result(g, pattern, false))
            return GLOB_NOSPACE;
    }
    if (!(flags & GLOB_NOSORT))
        qsort(g->gl_pathv + before, g->gl_pathc - before, sizeof(char *), compare_paths);
    return 0;
}

void globfree(glob_t *g) {
    for (size_t i = 0; i < g->gl_pathc; i++)
        free(g->gl_pathv[i]);
    free(g->gl_pathv);
    g->gl_pathc = 0;
    g->gl_pathv = NULL;
}
