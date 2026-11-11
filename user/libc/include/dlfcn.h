#pragma once

// Programs are linked against libc.so, which is also their dynamic linker: it loads
// the libraries a program needs from /system/lib before it starts. Symbols are looked
// for in the program, then libc, then the other libraries in the order they were
// loaded; a library opened here without RTLD_GLOBAL also looks in what it needs. A
// static program can use these too, and a library it opens gets the program's
// functions and variables by name, then libc.so's for anything else.
//
// Libraries are built with -fPIC -shared and --hash-style=sysv, as are programs,
// and can't use __thread variables. All of a library is linked at once, so RTLD_LAZY
// means the same as RTLD_NOW.
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
