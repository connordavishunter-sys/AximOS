// A static program, linked with libc.a, using dlopen.
#include <dlfcn.h>
#include <stdio.h>

int program_callback(int x) {
    return x + 2;
}

int main(void) {
    void *plugin = dlopen("/home/plugin.so", RTLD_NOW);
    int (*run)(int) = plugin ? (int (*)(int))dlsym(plugin, "plugin_run") : NULL;

    printf("%s static dlopen calls back into the program\n", run && run(1) == 30 ? "ok" : "FAIL");
    printf("static_dl: done\n");
    return 0;
}
