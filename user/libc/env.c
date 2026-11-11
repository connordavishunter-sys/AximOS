#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SHELL_PATH "/system/bin/dash"

// The kernel puts the first environ on the stack, so the array can't grow until
// it's been copied to the heap.
static bool on_heap;

static size_t env_count(void) {
    size_t count = 0;

    while (environ && environ[count])
        count++;
    return count;
}

static char **find(const char *name, size_t len) {
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, name, len) == 0 && (*e)[len] == '=')
            return e;
    }
    return NULL;
}

static bool valid_name(const char *name, size_t len) {
    return len > 0 && !memchr(name, '=', len);
}

char *getenv(const char *name) {
    size_t len = strlen(name);
    char **entry = find(name, len);

    return entry ? *entry + len + 1 : NULL;
}

static int put_entry(const char *name, size_t len, char *entry) {
    char **slot = find(name, len);

    if (slot) {
        *slot = entry;
        return 0;
    }

    size_t count = env_count();
    char **grown = malloc((count + 2) * sizeof(char *));

    if (!grown) {
        errno = ENOMEM;
        return -1;
    }
    if (count)
        memcpy(grown, environ, count * sizeof(char *));
    grown[count] = entry;
    grown[count + 1] = NULL;
    if (on_heap)
        free(environ);
    environ = grown;
    on_heap = true;
    return 0;
}

int putenv(char *entry) {
    char *equals = strchr(entry, '=');

    if (!equals || equals == entry) {
        errno = EINVAL;
        return -1;
    }
    return put_entry(entry, equals - entry, entry);
}

int setenv(const char *name, const char *value, int overwrite) {
    size_t len = strlen(name);

    if (!valid_name(name, len)) {
        errno = EINVAL;
        return -1;
    }
    if (!overwrite && find(name, len))
        return 0;

    // Replaced entries leak, since they may not have come from here.
    char *entry = malloc(len + strlen(value) + 2);

    if (!entry) {
        errno = ENOMEM;
        return -1;
    }
    strcpy(stpcpy(stpcpy(entry, name), "="), value);
    if (put_entry(name, len, entry) < 0) {
        free(entry);
        return -1;
    }
    return 0;
}

int unsetenv(const char *name) {
    size_t len = strlen(name);

    if (!valid_name(name, len)) {
        errno = EINVAL;
        return -1;
    }

    char **slot = find(name, len);

    if (slot) {
        do
            slot[0] = slot[1];
        while (*slot++);
    }
    return 0;
}

int clearenv(void) {
    if (on_heap)
        free(environ);
    environ = NULL;
    on_heap = false;
    return 0;
}

int system(const char *command) {
    if (!command)
        return 1;

    pid_t pid = fork();

    if (pid < 0)
        return -1;
    if (pid == 0) {
        char *argv[] = {"dash", "-c", (char *)command, NULL};

        execv(SHELL_PATH, argv);
        _Exit(127);
    }

    int status;

    return waitpid(pid, &status, 0) < 0 ? -1 : status;
}
