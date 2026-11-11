// A dynamically linked program using its own libraries, libc's data and dlopen,
// printing "ok" or "FAIL" for each check.
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int shape_count, shape_ready;
extern const char shape_title[];
char *describe(int sides);

extern char **environ;

static int failures;
static int constructed;
static __thread int per_thread = 7;

static void check(const char *name, bool ok) {
    printf("%s %s\n", ok ? "ok" : "FAIL", name);
    failures += !ok;
}

__attribute__((constructor)) static void start(void) {
    constructed = 1;
}

const char *shape_hook(void) {
    return "program";
}

int program_callback(int x) {
    return x + 1;
}

int main(int argc, char **argv) {
    check("program constructor ran", constructed);
    check("library constructor ran first", shape_ready == 3);
    check("library data", shape_count == 3 && strcmp(shape_title, "shapes") == 0);
    shape_count = 5;
    check("library data is writable", shape_count == 5);

    char *text = describe(4);

    check("library chain and the program's own hook", strcmp(text, "4 sides, red, hook from the program") == 0);
    free(text);

    check("__thread variable", per_thread == 7);
    errno = 0;
    check("errno", write(-1, "", 0) < 0 && errno == EBADF);
    check("environ", getenv("PATH") && environ && environ[0]);

    // optind is libc's, and the program has its own copy that libc has to use.
    char *args[] = {"libs", "-x", "rest", NULL};

    optind = 1;
    check("getopt and optind", getopt(3, args, "x") == 'x' && getopt(3, args, "x") == -1 && optind == 2);

    void *plugin = dlopen("/tmp/plugin.so", RTLD_NOW);
    int (*run)(int) = plugin ? (int (*)(int))dlsym(plugin, "plugin_run") : NULL;

    check("dlopen calls back into the program", run && run(4) == 50);
    check("dlsym finds libc", dlsym(RTLD_DEFAULT, "printf") == (void *)printf);
    void *(*libc_malloc)(size_t) = (void *(*)(size_t))dlsym(dlopen("libc.so", RTLD_NOW), "malloc");
    char *shared = libc_malloc ? libc_malloc(16) : NULL;

    check("dlopen of libc.so is the same libc", shared && strcpy(shared, "heap") && strlen(shared) == 4);
    free(shared);
    check("dlopen of a missing library", !dlopen("nothing.so", RTLD_NOW) && strstr(dlerror(), "nothing.so"));

    printf("libs: %d failed\n", failures);
    return failures != 0;
}
