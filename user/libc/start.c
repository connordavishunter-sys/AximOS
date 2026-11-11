#include <elf_types.h>
#include <string.h>
#include <sys/mman.h>
#include "lock.h"
#include "syscall.h"

// Set by the linker, at the start of the program as loaded.
extern const struct elf64_header __ehdr_start __attribute__((weak));

const void *__tls_image;
size_t __tls_image_size;
size_t __tls_size;
size_t __tls_align = 1;

static struct thread main_thread;

static void find_tls(void) {
    if (!&__ehdr_start)
        return;

    const char *base = (const char *)&__ehdr_start;
    const struct elf64_program_header *ph = (const void *)(base + __ehdr_start.e_phoff);

    for (int i = 0; i < __ehdr_start.e_phnum; i++, ph++) {
        if (ph->p_type != PT_TLS)
            continue;
        __tls_image = (const void *)ph->p_vaddr;
        __tls_image_size = ph->p_filesz;
        __tls_size = ph->p_memsz;
        __tls_align = ph->p_align ? ph->p_align : 1;
    }
}

// Sets up the first thread, called before anything else in the program. Its __thread
// variables go just below the thread pointer, which is where the compiler looks.
void __libc_start(void) {
    struct thread *self = &main_thread;

    find_tls();
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
