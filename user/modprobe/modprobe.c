#include <aximos.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>

// Loads kernel modules by name or by alias, as devices ask for them, with each that
// it needs first, from what the kernel's build wrote in /lib/modules/<release>:
// modules.dep, "path: needed...", and modules.alias, "alias pattern module".

#define COMPRESSED 4 // MODULE_INIT_COMPRESSED_FILE
#define LINE_MAX 4096
#define MAX_DEPTH 16

static char dir[300];
static bool quiet;

// Module names are the same with - or _.
static void normalize(char *name) {
    for (; *name; name++)
        *name = *name == '-' ? '_' : *name;
}

// The module's name from a path like kernel/drivers/foo/bar-baz.ko.zst.
static void name_of(const char *path, char *out, size_t size) {
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;

    snprintf(out, size, "%.*s", (int)strcspn(base, "."), base);
    normalize(out);
}

// The module an alias stands for, from the first pattern it matches.
static bool alias(const char *wanted, char *out, size_t size) {
    char path[sizeof(dir) + 32], line[LINE_MAX], pattern[LINE_MAX], module[256];
    FILE *f;

    snprintf(path, sizeof(path), "%s/modules.alias", dir);
    if (!(f = fopen(path, "r")))
        return false;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "alias %s %255s", pattern, module) == 2 && fnmatch(pattern, wanted, 0) == 0) {
            snprintf(out, size, "%s", module);
            normalize(out);
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

// Its line in modules.dep, or modules.builtin's if it's part of the kernel already.
static bool find(const char *name, char *line, size_t size, bool *builtin) {
    char path[sizeof(dir) + 32], found[256];

    *builtin = false;
    for (int pass = 0; pass < 2; pass++) {
        FILE *f;

        snprintf(path, sizeof(path), "%s/%s", dir, pass ? "modules.builtin" : "modules.dep");
        if (!(f = fopen(path, "r")))
            continue;
        while (fgets(line, size, f)) {
            name_of(strtok(strdupa(line), ":\n"), found, sizeof(found));
            if (!strcmp(found, name)) {
                fclose(f);
                *builtin = pass;
                return true;
            }
        }
        fclose(f);
    }
    return false;
}

static bool loaded(const char *name) {
    char path[300];
    struct stat st;

    snprintf(path, sizeof(path), "/sys/module/%s", name);
    return stat(path, &st) == 0;
}

static bool load(const char *name, const char *options, int depth) {
    char line[LINE_MAX], path[sizeof(dir) + LINE_MAX];
    bool builtin;

    if (loaded(name))
        return true;
    if (depth > MAX_DEPTH || !find(name, line, sizeof(line), &builtin)) {
        if (!quiet)
            fprintf(stderr, "modprobe: no module called %s\n", name);
        return false;
    }
    if (builtin)
        return true;

    // What it needs, each needing those after it, so the last is loaded first.
    char *file = strtok(line, ":\n"), *needed[64];
    int count = 0;

    for (char *n = strtok(NULL, " \n"); n && count < 64; n = strtok(NULL, " \n"))
        needed[count++] = n;
    file = strdupa(file);
    while (count--) {
        char dependency[256];

        name_of(needed[count], dependency, sizeof(dependency));
        if (!load(dependency, "", depth + 1))
            return false;
    }

    snprintf(path, sizeof(path), "%s/%s", dir, file);

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    bool ok = fd >= 0 && (syscall(SYS_finit_module, fd, options, strstr(file, ".ko.") ? COMPRESSED : 0) == 0 || errno == EEXIST);

    if (!ok && !quiet)
        fprintf(stderr, "modprobe: %s: %s\n", name, strerror(errno));
    if (fd >= 0)
        close(fd);
    return ok;
}

int main(int argc, char **argv) {
    help_option(argc, argv, "usage: modprobe [-q] [-r] module [option=value]...\n"
                            "       modprobe -a [-q] module-or-alias...\n"
                            "Loads a kernel module, and the modules it needs first. It can be named by\n"
                            "an alias, as devices name the drivers they want.\n"
                            "  -r  unload it instead\n"
                            "  -a  load each one named, without options\n"
                            "  -q  say nothing when one can't be found\n");

    bool remove = false, all = false;
    char options[1024] = "", name[256];
    struct utsname u;
    int option, failed = 0;

    while ((option = getopt(argc, argv, "qra")) != -1) {
        if (option == 'q')
            quiet = true;
        else if (option == 'r')
            remove = true;
        else if (option == 'a')
            all = true;
        else
            return 1;
    }
    if (optind == argc) {
        fprintf(stderr, "usage: modprobe [-q] [-r] module [option=value]...\n");
        return 1;
    }
    uname(&u);
    snprintf(dir, sizeof(dir), "/lib/modules/%s", u.release);
    for (int i = optind + 1; !all && i < argc; i++)
        snprintf(options + strlen(options), sizeof(options) - strlen(options), "%s%s", options[0] ? " " : "", argv[i]);

    for (int i = optind; i < (all ? argc : optind + 1); i++) {
        bool builtin;
        char line[LINE_MAX];

        snprintf(name, sizeof(name), "%s", argv[i]);
        normalize(name);
        if (!find(name, line, sizeof(line), &builtin) && !alias(argv[i], name, sizeof(name))) {
            if (!quiet)
                fprintf(stderr, "modprobe: no module called %s\n", argv[i]);
            failed = 1;
        } else if (remove) {
            if (syscall(SYS_delete_module, name, O_NONBLOCK) < 0) {
                fprintf(stderr, "modprobe: %s: %s\n", name, strerror(errno));
                failed = 1;
            }
        } else {
            failed |= !load(name, options, 0);
        }
    }
    return failed;
}
