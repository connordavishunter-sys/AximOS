#pragma once

// Programs here are static, and a library is linked to what the program already has
// by name: its own global functions and variables come first, then anything else
// from libc.so in /system/lib, which is added automatically. Libraries are built
// with -fPIC -shared and --hash-style=sysv, and can't use __thread variables. All of
// a library is loaded at once, so RTLD_LAZY means the same as RTLD_NOW.
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
#define RTLD_DEFAULT ((void *)0)

// Looks in /system/lib for a name without a slash. NULL gets the program itself.
void *dlopen(const char *path, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
