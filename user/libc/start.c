#include <elf_types.h>
#include <string.h>
#include <sys/mman.h>
#include "lock.h"
#include "start.h"
#include "syscall.h"

const void *__tls_image;
size_t __tls_image_size;
size_t __tls_size;
size_t __tls_align = 1;

static struct thread main_thread;

static void find_tls(const struct elf64_program_header *headers, int count) {
    uintptr_t bias = 0; // where the program went relative to its addresses, if it can move

    for (int i = 0; i < count; i++) {
        if (headers[i].p_type == PT_PHDR)
            bias = (uintptr_t)headers - headers[i].p_vaddr;
    }
    for (int i = 0; i < count; i++) {
        const struct elf64_program_header *ph = &headers[i];

        if (ph->p_type != PT_TLS)
            continue;
        __tls_image = (const void *)(bias + ph->p_vaddr);
        __tls_image_size = ph->p_filesz;
        __tls_size = ph->p_memsz;
        __tls_align = ph->p_align ? ph->p_align : 1;
    }
}

// Sets up the first thread. Its __thread variables go just below the thread pointer,
// which is where the compiler looks.
void __libc_set_up(const struct elf64_program_header *headers, int count) {
    struct thread *self = &main_thread;

    find_tls(headers, count);
    if (__tls_size) {
        size_t aligned = (__tls_size + __tls_align - 1) & ~(__tls_align - 1);
        size_t total = aligned + sizeof(struct thread) + __tls_align;
        char *block = (char *)syscall6(SYS_MMAP, 0, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

        if ((long)block < 0 && (long)block > -4096)
            syscall3(SYS_EXIT, 127, 0, 0);

        uintptr_t tp = ((uintptr_t)block + aligned + __tls_align - 1) & ~(__tls_align - 1);

        self = (struct thread *)tp;
        memcpy((char *)tp - aligned, __tls_image, __tls_image_size);
    }

    self->self = self;
    self->id = syscall3(SYS_GETTID, 0, 0, 0);
    self->exited = 1;
    syscall3(SYS_SET_THREAD_POINTER, (long)self, 0, 0);
}

#ifdef __PIC__
// The dynamic linker has done all of it before the program started.
void __libc_start(void) {
}
#else
// Set by the linker: the start of the program as loaded, and its constructors.
extern const struct elf64_header __ehdr_start __attribute__((weak));
extern void (*const __init_array_start[])(void) __attribute__((weak, visibility("hidden")));
extern void (*const __init_array_end[])(void) __attribute__((weak, visibility("hidden")));

// Called by crt0 before anything else in the program.
void __libc_start(void) {
    if (&__ehdr_start) {
        const char *base = (const char *)&__ehdr_start;

        __libc_set_up((const void *)(base + __ehdr_start.e_phoff), __ehdr_start.e_phnum);
    } else {
        __libc_set_up(NULL, 0);
    }
    for (void (*const *f)(void) = __init_array_start; f < __init_array_end; f++)
        (*f)();
}
#endif
