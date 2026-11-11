#include <cpu.h>
#include <elf.h>
#include <elf_types.h>
#include <errno_values.h>
#include <file.h>
#include <gdt.h>
#include <irq.h>
#include <kernel.h>
#include <kmalloc.h>
#include <mm.h>
#include <net.h>
#include <process.h>
#include <serial.h>
#include <shm.h>
#include <signal_numbers.h>
#include <string.h>
#include <syscall_numbers.h>

// The unmapped pages either side of the stack catch overflows. Only the top pages,
// which hold the arguments, are mapped at the start, and the rest when first touched.
#define STACK_TOP 0x00007FFFFFFFF000UL
#define STACK_PAGES 2048
#define STACK_FIRST_PAGES 2
#define STACK_BOTTOM (STACK_TOP - STACK_PAGES * PAGE_SIZE)

// Mappings grow down from here, and the heap grows up towards them. Address space
// is plentiful enough that what munmap gives back isn't handed out again.
#define MMAP_TOP (STACK_BOTTOM - PAGE_SIZE)
#define HEAP_LIMIT 0x0000400000000000UL

#define DATA_FLAGS (PAGE_USER | PAGE_WRITE | PAGE_NO_EXEC)

#define RFLAGS_RESERVED 0x2
#define RFLAGS_DIRECTION 0x400
#define RFLAGS_INTERRUPTS 0x200

#define MAX_ARGS 64
#define CMDLINE_MAX 256

#define VECTOR_DIVIDE_ERROR 0
#define VECTOR_INVALID_OPCODE 6
#define VECTOR_X87_ERROR 16
#define VECTOR_SIMD_ERROR 19

#define FPU_STATE_SIZE 512
#define FPU_MXCSR_OFFSET 24
#define FPU_MXCSR_MASK_OFFSET 28
#define MXCSR_DEFAULT 0x1F80
#define MXCSR_DEFAULT_MASK 0xFFBF

#define RED_ZONE_SIZE 128
#define STATUS_SIGNAL_BASE 128

// The flags a program can set for itself, which sigreturn lets through: carry,
// parity, adjust, zero, sign, direction and overflow.
#define RFLAGS_USER_MASK 0xCD5

enum process_state {
    PROCESS_RUNNING,
    PROCESS_RUNNABLE,
    PROCESS_BLOCKED,
    PROCESS_ZOMBIE,
};

// What goes on the user stack around a signal handler.
struct signal_frame {
    uint64_t restorer; // the handler's return address
    uint64_t signal;
    uint64_t saved_mask;
    struct interrupt_frame saved;
    uint8_t fpu[FPU_STATE_SIZE];
};

// What the threads of a process have in common. Lives until the parent waits for it.
struct shared {
    int pid;               // of the thread that started it
    int threads;           // how many haven't exited
    int exit_status;
    bool exiting;          // all threads are on the way out
    struct shared *parent;
    uint64_t pml4;
    uint64_t heap_start;
    uint64_t heap_end;
    uint64_t mmap_next;    // the lowest address mapped so far
    struct file *files[MAX_FDS];
    uint64_t close_on_exec;
    char cwd[PATH_MAX];
    char exe[PATH_MAX];    // the program it's running
    char cmdline[CMDLINE_MAX]; // its arguments with spaces between, cut short if need be
    int pgid;
    uint64_t alarm_tick;   // when to raise SIGALRM, or 0
    uint64_t handlers[NSIG];
    uint32_t handler_masks[NSIG];
    uint32_t handler_flags[NSIG];
    uint64_t restorer;
};

_Static_assert(sizeof(struct shared) <= KMALLOC_MAX, "kmalloc can't hold shared state");

// One per thread. A single-threaded process is one of these and its shared state.
struct process {
    struct process *next; // in the list of all threads
    struct shared *s;
    uint64_t stack_top;
    uint64_t rsp;
    enum process_state state;
    int pid;               // this thread's id, the same as s->pid for the first
    void *channel;         // what this is sleeping on
    bool interruptible;    // whether a signal can end that sleep
    bool killed;           // another thread needs this one gone
    uint64_t wake_tick;    // when to give up sleeping, or 0
    uint64_t cpu_ticks;    // timer ticks that found it running in ring 3
    uint32_t signals_pending;
    uint32_t signals_masked;
    uint32_t suspend_mask; // what sigsuspend replaced
    bool suspended;        // whether sigsuspend is waiting
    uint64_t thread_pointer; // the FS base
    uint64_t clear_on_exit;  // a word to zero and wake when the thread exits, or 0
    uint8_t fpu[FPU_STATE_SIZE] __attribute__((aligned(16)));
};

// Arguments held in the kernel while the old program's memory goes away. It
// fills one page, which also bounds how much of the new stack it can use.
struct exec_args {
    int argc;
    int count; // argc arguments, then the environment
    size_t used;
    char *entries[MAX_ARGS];
    char strings[];
};

#define ARGS_STRINGS_SIZE (PAGE_SIZE - sizeof(struct exec_args))
#define SCRIPT_LINE_MAX 256

struct image {
    uint64_t pml4;
    uint64_t entry;
    uint64_t stack;
    uint64_t heap;
};

static struct process *processes;
static struct process *init;
static struct process *next_to_try; // where the scheduler looks first
static bool scheduler_running;
static int next_pid = 1;
static int foreground_pgid;

// What a new program starts with.
static uint8_t fpu_initial[FPU_STATE_SIZE] __attribute__((aligned(16)));
static uint32_t mxcsr_mask = MXCSR_DEFAULT_MASK;

#define current (this_cpu()->running)

static uint64_t page_align_up(uint64_t addr) {
    return (addr + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
}

static bool map_new_page(uint64_t virt) {
    uint64_t phys = mm_alloc_page();

    if (phys == 0)
        return false;
    if (!mm_map_page(virt, phys, DATA_FLAGS)) {
        mm_free_page(phys);
        return false;
    }
    return true;
}

// New processes go at the end, so that the scheduler reaches them in turn.
static struct process *new_process(struct shared *s) {
    struct process *p = kmalloc(sizeof(*p));
    if (!p)
        return NULL;

    memset(p, 0, sizeof(*p));
    p->stack_top = mm_alloc_kernel_stack();
    if (p->stack_top == 0) {
        kfree(p);
        return NULL;
    }

    if (!s) {
        s = kmalloc(sizeof(*s));
        if (!s) {
            mm_free_kernel_stack(p->stack_top);
            kfree(p);
            return NULL;
        }
        memset(s, 0, sizeof(*s));
    }
    s->threads++;
    p->s = s;

    struct process **link = &processes;
    while (*link)
        link = &(*link)->next;
    *link = p;
    return p;
}

static void free_process(struct process *p) {
    struct process **link = &processes;
    while (*link != p)
        link = &(*link)->next;
    *link = p->next;

    if (next_to_try == p)
        next_to_try = p->next;

    mm_free_kernel_stack(p->stack_top);
    kfree(p);
}

static void fpu_save(uint8_t *state) {
    __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t (*)[FPU_STATE_SIZE])state));
}

static void fpu_restore(const uint8_t *state) {
    __asm__ volatile("fxrstor64 %0" : : "m"(*(const uint8_t (*)[FPU_STATE_SIZE])state));
}

static void fpu_init_defaults(void) {
    uint32_t mxcsr = MXCSR_DEFAULT;
    uint32_t mask;

    __asm__ volatile("fninit; ldmxcsr %0" : : "m"(mxcsr));
    fpu_save(fpu_initial);

    memcpy(&mask, &fpu_initial[FPU_MXCSR_MASK_OFFSET], sizeof(mask));
    if (mask)
        mxcsr_mask = mask;
}

// Round robin over everything runnable, so a process that just stopped running
// doesn't get picked until the others have had a turn.
static struct process *pick_runnable(void) {
    struct process *start = next_to_try ? next_to_try : processes;
    struct process *p = start;

    do {
        if (p->state == PROCESS_RUNNABLE) {
            next_to_try = p->next;
            return p;
        }
        p = p->next ? p->next : processes;
    } while (p != start);

    return NULL;
}

static void make_runnable(struct process *p) {
    p->state = PROCESS_RUNNABLE;
    p->channel = NULL;
    p->wake_tick = 0;
    cpu_wake_one_idle();
}

// Takes on the state to run p. The caller then switches to its stack.
static void prepare_to_run(struct process *p) {
    struct cpu *cpu = this_cpu();

    p->state = PROCESS_RUNNING;
    cpu->running = p;
    cpu_set_thread_pointer(p->thread_pointer);
    cpu_set_kernel_stack(p->stack_top);
    mm_switch_address_space(p->s->pml4);
    fpu_restore(p->fpu);
}

// Prepares a new process so switching to it returns to ring 3 through frame.
static void prepare_stack(struct process *p, const struct interrupt_frame *frame) {
    struct interrupt_frame *saved = (struct interrupt_frame *)p->stack_top - 1;
    *saved = *frame;

    uint64_t *sp = (uint64_t *)saved;
    *--sp = (uint64_t)process_first_run;
    sp -= 6; // the registers switch_context pops, which start out as zero
    memset(sp, 0, 6 * sizeof(*sp));
    p->rsp = (uint64_t)sp;
}

static struct interrupt_frame user_frame(uint64_t entry, uint64_t stack) {
    return (struct interrupt_frame){
        .rip = entry,
        .cs = GDT_USER_CODE,
        .rflags = RFLAGS_RESERVED | RFLAGS_INTERRUPTS,
        .rsp = stack,
        .ss = GDT_USER_DATA,
    };
}

// Lays out the strings, argc, argv, envp and the auxiliary vector as the System V
// ABI expects at entry. Stack pages are writable and in the active address space.
static uint64_t push_args(const struct exec_args *args, const struct elf_image *elf) {
    const uint64_t aux[][2] = {
        {AT_PHDR, elf->phdr}, {AT_PHENT, sizeof(struct elf64_program_header)}, {AT_PHNUM, elf->phnum},
        {AT_PAGESZ, PAGE_SIZE}, {AT_BASE, elf->interpreter_base}, {AT_ENTRY, elf->program_entry}, {AT_NULL, 0},
    };
    uint64_t sp = STACK_TOP;
    uint64_t pointers[MAX_ARGS];

    for (int i = args->count - 1; i >= 0; i--) {
        size_t size = strlen(args->entries[i]) + 1;

        sp -= size;
        memcpy((void *)sp, args->entries[i], size);
        pointers[i] = sp;
    }

    sp = (sp - (args->count + 3 + 2 * sizeof(aux) / sizeof(*aux)) * sizeof(uint64_t)) & ~(uint64_t)15;

    uint64_t *out = (uint64_t *)sp;
    *out++ = args->argc;
    for (int i = 0; i < args->argc; i++)
        *out++ = pointers[i];
    *out++ = 0;
    for (int i = args->argc; i < args->count; i++)
        *out++ = pointers[i];
    *out++ = 0;
    memcpy(out, aux, sizeof(aux));
    return sp;
}

// Builds a program in a new address space and leaves it active. On failure the
// previous address space is active again and nothing has changed.
static int load_image(const struct vfs_node *file, const struct exec_args *args,
                      struct image *image) {
    struct process *self = this_cpu()->running;
    uint64_t previous = mm_active_address_space();

    image->pml4 = mm_create_address_space();
    if (image->pml4 == 0)
        return -ENOMEM;

    // Loading sleeps on the disk, and waking up restores the process's address
    // space, so it has to already be this one.
    if (self)
        self->s->pml4 = image->pml4;
    mm_switch_address_space(image->pml4);

    struct elf_image elf;
    int status = elf_load(file, &elf);

    for (uint64_t page = STACK_TOP - STACK_FIRST_PAGES * PAGE_SIZE; status == 0 && page < STACK_TOP; page += PAGE_SIZE)
        status = map_new_page(page) ? 0 : -ENOMEM;

    if (status != 0) {
        if (self)
            self->s->pml4 = previous;
        mm_switch_address_space(previous);
        mm_destroy_address_space(image->pml4);
        return status;
    }

    image->entry = elf.entry;
    image->stack = push_args(args, &elf);
    image->heap = page_align_up(elf.end);
    return 0;
}

static int args_add(struct exec_args *args, const char *string) {
    size_t size = strlen(string) + 1;

    if (args->count == MAX_ARGS || size > ARGS_STRINGS_SIZE - args->used)
        return -E2BIG;

    args->entries[args->count++] = &args->strings[args->used];
    args->argc = args->count;
    memcpy(&args->strings[args->used], string, size);
    args->used += size;
    return 0;
}

// Appends a user pointer's NULL-terminated array of strings, which may be NULL.
static int args_copy_from_user(struct exec_args *args, uint64_t array, int skip) {
    for (uint64_t i = skip; array != 0; i++) {
        uint64_t slot = array + i * sizeof(uint64_t);

        if (!mm_user_range_ok(slot, sizeof(uint64_t), false))
            return -EFAULT;

        uint64_t string = *(const uint64_t *)slot;
        if (string == 0)
            return 0;
        if (args->count == MAX_ARGS)
            return -E2BIG;

        int64_t len = mm_copy_user_string(&args->strings[args->used], string,
                                          ARGS_STRINGS_SIZE - args->used);
        if (len < 0)
            return len == -ENAMETOOLONG ? -E2BIG : len;

        args->entries[args->count++] = &args->strings[args->used];
        args->used += len + 1;
    }
    return 0;
}

static void save_cmdline(struct shared *s, const struct exec_args *args) {
    size_t used = 0;

    for (int i = 0; i < args->argc && used + 1 < CMDLINE_MAX; i++) {
        size_t len = strlen(args->entries[i]);

        if (i > 0)
            s->cmdline[used++] = ' ';
        if (len > CMDLINE_MAX - 1 - used)
            len = CMDLINE_MAX - 1 - used;
        memcpy(&s->cmdline[used], args->entries[i], len);
        used += len;
    }
    s->cmdline[used] = '\0';
}

int process_spawn_init(const struct vfs_node *file, const char *path) {
    fpu_init_defaults();

    uint64_t args_page = mm_alloc_kernel_page();
    if (args_page == 0)
        return -ENOMEM;

    struct exec_args *args = mm_phys_to_virt(args_page);
    struct image image;
    int status = args_add(args, path);

    if (status == 0)
        status = load_image(file, args, &image);

    struct process *p = status == 0 ? new_process(NULL) : NULL;

    if (status == 0 && !p)
        status = -ENOMEM;
    if (status == 0)
        save_cmdline(p->s, args);
    mm_free_kernel_page(args_page);
    if (status != 0)
        return status;

    struct interrupt_frame frame = user_frame(image.entry, image.stack);

    p->s->pml4 = image.pml4;
    p->pid = p->s->pid = next_pid++;
    p->s->pgid = p->pid;
    memcpy(p->s->exe, path, strlen(path) + 1);
    p->s->heap_start = p->s->heap_end = image.heap;
    p->s->mmap_next = MMAP_TOP;
    for (int fd = 0; fd <= 2; fd++)
        p->s->files[fd] = file_console();
    p->s->cwd[0] = '/';
    memcpy(p->fpu, fpu_initial, FPU_STATE_SIZE);
    prepare_stack(p, &frame);
    p->state = PROCESS_RUNNABLE;

    init = p;
    kprintf("process: created pid %u from %s\n", p->pid, path);
    return 0;
}

void process_idle_loop(void) {
    struct cpu *cpu = this_cpu();

    scheduler_running = true;

    for (;;) {
        struct process *next = pick_runnable();

        if (next) {
            prepare_to_run(next);
            switch_context(&cpu->idle_rsp, next->rsp);
            continue;
        }

        // The address space that was active may belong to a process that another
        // CPU is about to destroy.
        cpu->running = NULL;
        mm_switch_address_space(mm_kernel_address_space());

        cpu->idle = true;
        kernel_unlock();

        // sti delays interrupts until hlt has started, so none can be missed.
        __asm__ volatile("sti; hlt; cli");

        kernel_lock();
        cpu->idle = false;
    }
}

// The first thread stays for the parent to find. The others are gone once they've
// switched away, which holding the kernel lock proves of all but the caller.
static void free_exited_threads(const struct process *caller) {
    struct process *next;

    for (struct process *p = processes; p; p = next) {
        next = p->next;
        if (p->state == PROCESS_ZOMBIE && p->pid != p->s->pid && p != caller)
            free_process(p);
    }
}

// Kernel code doesn't get preempted: interrupts are off, and processes only
// switch in here. The kernel lock stays held through the switch and whatever
// runs next gives it up.
void schedule(void) {
    struct cpu *cpu = this_cpu();
    struct process *previous = cpu->running;

    net_flush();
    free_exited_threads(previous);

    if (previous->state == PROCESS_RUNNING)
        previous->state = PROCESS_RUNNABLE;

    struct process *next = pick_runnable();

    if (next == previous) {
        previous->state = PROCESS_RUNNING;
        return;
    }

    fpu_save(previous->fpu);

    if (next) {
        prepare_to_run(next);
        switch_context(&previous->rsp, next->rsp);
    } else {
        cpu->running = NULL;
        switch_context(&previous->rsp, cpu->idle_rsp);
    }
}

bool process_can_sleep(void) {
    return scheduler_running && this_cpu()->running != NULL;
}

static bool signal_is_deliverable(const struct process *p) {
    return p->signals_pending & ~p->signals_masked;
}

bool process_sleep_timeout(void *channel, uint64_t timeout_ticks) {
    if (signal_is_deliverable(current))
        return false;

    current->state = PROCESS_BLOCKED;
    current->channel = channel;
    current->interruptible = true;
    current->wake_tick = timeout_ticks ? irq_ticks() + timeout_ticks : 0;
    schedule();
    return !signal_is_deliverable(current);
}

bool process_sleep(void *channel) {
    return process_sleep_timeout(channel, 0);
}

void process_charge_tick(void) {
    current->cpu_ticks++;
}

uint64_t process_cpu_ticks(void) {
    return current->cpu_ticks;
}

void process_sleep_uninterruptible(void *channel, uint64_t timeout_ticks) {
    current->state = PROCESS_BLOCKED;
    current->channel = channel;
    current->interruptible = false;
    current->wake_tick = timeout_ticks ? irq_ticks() + timeout_ticks : 0;
    schedule();
}

static void send_to_process(struct shared *s, int signal);

[[noreturn]] static void finish_thread(void);
static void kill_other_threads(void);

// What poll sleeps on, to be woken by anything. It then looks again at what it wants.
static char any_channel;

bool process_sleep_any(uint64_t timeout_ticks) {
    return process_sleep_timeout(&any_channel, timeout_ticks);
}

void process_wakeup(void *channel) {
    for (struct process *p = processes; p; p = p->next) {
        if (p->state == PROCESS_BLOCKED && (p->channel == channel || p->channel == &any_channel))
            make_runnable(p);
    }
}

void process_wake_expired(void) {
    uint64_t now = irq_ticks();

    for (struct process *p = processes; p; p = p->next) {
        if (p->pid == p->s->pid && p->s->alarm_tick && p->s->alarm_tick <= now) {
            p->s->alarm_tick = 0;
            send_to_process(p->s, SIGALRM);
        }
        if (p->state == PROCESS_BLOCKED && p->wake_tick && p->wake_tick <= now)
            make_runnable(p);
    }
}

int process_set_group(int pid, int pgid) {
    for (struct process *p = processes; p; p = p->next) {
        bool wanted = pid == 0 ? p == current : p->pid == pid && p->pid == p->s->pid;

        if (!wanted)
            continue;
        if (p->s != current->s && p->s->parent != current->s)
            return -EPERM;

        p->s->pgid = pgid ? pgid : p->pid;
        return 0;
    }
    return -ESRCH;
}

int process_foreground(void) {
    return foreground_pgid;
}

void process_set_foreground(int pgid) {
    foreground_pgid = pgid;
}

// There's no job control, so what would stop or continue a process does nothing.
static bool ignored_by_default(int signal) {
    switch (signal) {
    case SIGCHLD:
    case SIGCONT:
    case SIGSTOP:
    case SIGTSTP:
    case SIGTTIN:
    case SIGTTOU:
    case SIGURG:
    case SIGWINCH:
        return true;
    }
    return false;
}

static bool signal_is_ignored(const struct shared *s, int signal) {
    return s->handlers[signal] == SIG_IGN_VALUE
        || (s->handlers[signal] == SIG_DFL_VALUE && ignored_by_default(signal));
}

static void send_signal(struct process *p, int signal) {
    if (p->state == PROCESS_ZOMBIE || (signal != SIGKILL && signal_is_ignored(p->s, signal)))
        return;

    p->signals_pending |= 1u << signal;

    bool can_interrupt = signal == SIGKILL || !(p->signals_masked & 1u << signal);
    if (p->state == PROCESS_BLOCKED && p->interruptible && can_interrupt)
        make_runnable(p);
}

// A signal for a process goes to a thread that isn't blocking it, if there is one.
static void send_to_process(struct shared *s, int signal) {
    struct process *fallback = NULL;

    for (struct process *p = processes; p; p = p->next) {
        if (p->s != s || p->state == PROCESS_ZOMBIE)
            continue;
        if (signal == SIGKILL || !(p->signals_masked & 1u << signal)) {
            send_signal(p, signal);
            return;
        }
        fallback = p;
    }
    if (fallback)
        send_signal(fallback, signal);
}

bool process_interrupt_foreground(void) {
    bool found = false;

    if (foreground_pgid == 0)
        return false;

    for (struct process *p = processes; p; p = p->next) {
        if (p->s->pgid != foreground_pgid || p->pid != p->s->pid || p->s->threads == 0)
            continue;

        send_to_process(p->s, SIGINT);
        found = true;
    }
    return found;
}

void process_signal_self(int signal) {
    send_signal(current, signal);
}

int process_kill(int pid, int signal) {
    if (signal < 0 || signal >= NSIG)
        return -EINVAL;

    // pid can name one process, or as a negative number a whole group. 0 means
    // the caller's own group.
    int group = pid < 0 ? -pid : pid == 0 ? current->s->pgid : 0;
    bool found = false;

    for (struct process *p = processes; p; p = p->next) {
        if (p->pid != p->s->pid || p->s->threads == 0 || (group ? p->s->pgid != group : p->pid != pid))
            continue;

        // Everything depends on init, so only it can signal itself.
        if (p->s == init->s && p->s != current->s) {
            if (!group)
                return -EPERM;
            continue;
        }

        found = true;
        if (signal != 0)
            send_to_process(p->s, signal);
    }
    return found ? 0 : -ESRCH;
}

#define UNBLOCKABLE (1u << SIGKILL | 1u << SIGSTOP)

int process_sigaction(int signal, const struct sigaction *action, struct sigaction *previous) {
    if (signal <= 0 || signal >= NSIG)
        return -EINVAL;
    if (action && (signal == SIGKILL || signal == SIGSTOP))
        return -EINVAL;

    struct process *p = current;

    if (previous) {
        *previous = (struct sigaction){
            .sa_handler = (void (*)(int))p->s->handlers[signal],
            .sa_mask = p->s->handler_masks[signal],
            .sa_flags = p->s->handler_flags[signal],
            .sa_restorer = (void (*)(void))p->s->restorer,
        };
    }
    if (!action)
        return 0;

    uint64_t handler = (uint64_t)action->sa_handler;

    if (handler != SIG_DFL_VALUE && handler != SIG_IGN_VALUE && handler >= LOWER_HALF_END)
        return -EFAULT;

    p->s->handlers[signal] = handler;
    p->s->handler_masks[signal] = action->sa_mask & ~UNBLOCKABLE;
    p->s->handler_flags[signal] = action->sa_flags;
    p->s->restorer = (uint64_t)action->sa_restorer;

    // Ignoring a signal also discards it if it's already waiting.
    if (signal_is_ignored(p->s, signal))
        p->signals_pending &= ~(1u << signal);
    return 0;
}

int process_sigprocmask(int how, const sigset_t *set, sigset_t *previous) {
    struct process *p = current;
    uint32_t mask = p->signals_masked;

    if (set) {
        if (how == SIG_BLOCK)
            mask |= *set;
        else if (how == SIG_UNBLOCK)
            mask &= ~*set;
        else if (how == SIG_SETMASK)
            mask = *set;
        else
            return -EINVAL;
    }
    if (previous)
        *previous = p->signals_masked;
    p->signals_masked = mask & ~UNBLOCKABLE;
    return 0;
}

sigset_t process_signals_pending(void) {
    return current->signals_pending & current->signals_masked;
}

int process_sigsuspend(sigset_t mask) {
    struct process *p = current;

    p->suspend_mask = p->signals_masked;
    p->suspended = true;
    p->signals_masked = mask & ~UNBLOCKABLE;
    while (process_sleep(&p->suspended))
        ;
    return -EINTR;
}

uint64_t process_alarm(uint64_t seconds) {
    struct process *p = current;
    uint64_t now = irq_ticks();
    uint64_t left = p->s->alarm_tick ? (p->s->alarm_tick - now + TIMER_HZ - 1) / TIMER_HZ : 0;

    p->s->alarm_tick = seconds ? now + seconds * TIMER_HZ : 0;
    return left;
}

bool process_signal_restarts(void) {
    struct process *p = current;
    uint32_t ready = p->signals_pending & ~p->signals_masked;

    if (!ready)
        return false;

    int signal = __builtin_ctz(ready);

    return p->s->handlers[signal] > SIG_IGN_VALUE && (p->s->handler_flags[signal] & SA_RESTART);
}

// Builds the frame on the user stack and points the process at its handler.
// Returns false if the stack isn't usable, which leaves nothing else to try.
static bool enter_signal_handler(struct process *p, struct interrupt_frame *frame, int signal) {
    if (frame->rsp >= LOWER_HALF_END)
        return false;

    // Handlers start as if called: 8 bytes past a 16-byte boundary.
    uint64_t sp = ((frame->rsp - RED_ZONE_SIZE - sizeof(struct signal_frame)) & ~(uint64_t)15) - 8;

    if (!mm_user_range_ok(sp, sizeof(struct signal_frame), true))
        return false;

    struct signal_frame *out = (struct signal_frame *)sp;

    out->restorer = p->s->restorer;
    out->signal = signal;
    out->saved_mask = p->suspended ? p->suspend_mask : p->signals_masked;
    p->suspended = false;
    out->saved = *frame;

    // Handlers are free to use floating point, and start with a clean slate.
    fpu_save(p->fpu);
    memcpy(out->fpu, p->fpu, FPU_STATE_SIZE);
    fpu_restore(fpu_initial);

    p->signals_masked |= p->s->handler_masks[signal];
    if (!(p->s->handler_flags[signal] & SA_NODEFER))
        p->signals_masked |= 1u << signal;

    frame->rip = p->s->handlers[signal];
    if (p->s->handler_flags[signal] & SA_RESETHAND)
        p->s->handlers[signal] = SIG_DFL_VALUE;
    frame->rdi = signal;
    frame->rsp = sp;
    frame->rflags &= ~(uint64_t)RFLAGS_DIRECTION;
    return true;
}

void process_deliver_signals(struct interrupt_frame *frame) {
    struct process *p = current;

    if (p->killed)
        finish_thread();

    // SIGKILL can't be blocked, even from inside a handler.
    while (signal_is_deliverable(p) || (p->signals_pending & 1u << SIGKILL)) {
        uint32_t ready = p->signals_pending & (~p->signals_masked | 1u << SIGKILL);
        int signal = __builtin_ctz(ready);

        p->signals_pending &= ~(1u << signal);

        uint64_t handler = p->s->handlers[signal];

        if (signal == SIGKILL || handler == SIG_DFL_VALUE) {
            if (signal != SIGKILL && ignored_by_default(signal))
                continue;
            process_exit(STATUS_SIGNAL_BASE + signal);
        }
        if (handler == SIG_IGN_VALUE)
            continue;

        if (!enter_signal_handler(p, frame, signal))
            process_exit(STATUS_SIGNAL_BASE + SIGSEGV);

        // Only one at a time: the next waits for this handler to return.
        return;
    }

    if (p->suspended) {
        p->signals_masked = p->suspend_mask;
        p->suspended = false;
    }
}

void process_signal_return(struct interrupt_frame *frame) {
    struct process *p = current;

    // The handler's ret popped the return address.
    uint64_t base = frame->rsp - offsetof(struct signal_frame, signal);

    if (!mm_user_range_ok(base, sizeof(struct signal_frame), false))
        process_exit(STATUS_SIGNAL_BASE + SIGSEGV);

    const struct signal_frame *in = (const struct signal_frame *)base;
    struct interrupt_frame saved = in->saved;

    // All of this was on the user stack, so nothing about it can be trusted.
    if (saved.rip >= LOWER_HALF_END || saved.rsp >= LOWER_HALF_END)
        process_exit(STATUS_SIGNAL_BASE + SIGSEGV);

    p->signals_masked = in->saved_mask & ~UNBLOCKABLE;

    // Reserved MXCSR bits would make the restore fault in the kernel.
    uint32_t mxcsr;

    memcpy(p->fpu, in->fpu, FPU_STATE_SIZE);
    memcpy(&mxcsr, &p->fpu[FPU_MXCSR_OFFSET], sizeof(mxcsr));
    mxcsr &= mxcsr_mask;
    memcpy(&p->fpu[FPU_MXCSR_OFFSET], &mxcsr, sizeof(mxcsr));
    fpu_restore(p->fpu);

    saved.cs = GDT_USER_CODE;
    saved.ss = GDT_USER_DATA;
    saved.rflags = (saved.rflags & RFLAGS_USER_MASK) | RFLAGS_RESERVED | RFLAGS_INTERRUPTS;
    saved.vector = frame->vector;
    saved.error = 0;
    *frame = saved;
}

void process_fault(uint64_t vector) {
    bool math_error = vector == VECTOR_DIVIDE_ERROR || vector == VECTOR_X87_ERROR
        || vector == VECTOR_SIMD_ERROR;
    int signal = math_error ? SIGFPE : vector == VECTOR_INVALID_OPCODE ? SIGILL : SIGSEGV;

    kprintf("process: pid %u got signal %u from exception %lu\n", current->pid, signal, vector);

    if (current->s == init->s)
        die("init crashed");

    // A handler already running for this signal can't take another, and a fault
    // can't just wait, because the program can't continue without it handled.
    uint64_t handler = current->s->handlers[signal];
    if (handler == SIG_DFL_VALUE || handler == SIG_IGN_VALUE
        || (current->signals_masked & 1u << signal))
        process_exit(STATUS_SIGNAL_BASE + signal);

    current->signals_pending |= 1u << signal;
}

int process_fork(const struct interrupt_frame *frame) {
    struct process *child = new_process(NULL);
    if (!child) {
        kprintf("process: fork failed, out of memory\n");
        return -ENOMEM;
    }

    child->s->pml4 = mm_clone_active_address_space();
    if (child->s->pml4 == 0) {
        kprintf("process: fork failed, out of memory\n");
        child->state = PROCESS_ZOMBIE;
        kfree(child->s);
        free_process(child);
        return -ENOMEM;
    }

    child->pid = child->s->pid = next_pid++;
    child->s->parent = current->s;
    child->thread_pointer = current->thread_pointer;
    memcpy(child->s->exe, current->s->exe, strlen(current->s->exe) + 1);
    memcpy(child->s->cmdline, current->s->cmdline, CMDLINE_MAX);
    child->s->pgid = current->s->pgid;
    child->signals_masked = current->signals_masked;
    child->s->restorer = current->s->restorer;
    memcpy(child->s->handlers, current->s->handlers, sizeof(child->s->handlers));
    memcpy(child->s->handler_masks, current->s->handler_masks, sizeof(child->s->handler_masks));
    memcpy(child->s->handler_flags, current->s->handler_flags, sizeof(child->s->handler_flags));
    child->s->close_on_exec = current->s->close_on_exec;

    // Only the running copy of the registers is up to date.
    fpu_save(child->fpu);
    child->s->heap_start = current->s->heap_start;
    child->s->heap_end = current->s->heap_end;
    child->s->mmap_next = current->s->mmap_next;
    for (int fd = 0; fd < MAX_FDS; fd++)
        child->s->files[fd] = current->s->files[fd] ? file_ref(current->s->files[fd]) : NULL;
    memcpy(child->s->cwd, current->s->cwd, strlen(current->s->cwd) + 1);

    struct interrupt_frame child_frame = *frame;
    child_frame.rax = 0;
    prepare_stack(child, &child_frame);
    child->state = PROCESS_RUNNABLE;

    kprintf("process: pid %u forked pid %u, %lu pages free\n",
            current->pid, child->pid, mm_free_page_count());
    return child->pid;
}

// If the file starts with #!, this gets the interpreter it names and the one argument
// that may follow, which is empty if there's none. Both point into line.
static bool read_interpreter(const struct vfs_node *file, char *line, size_t size, char **interpreter, char **argument) {
    int64_t len = vfs_read(file, 0, line, size - 1);

    if (len < 2 || line[0] != '#' || line[1] != '!')
        return false;
    line[len] = '\0';

    char *s = line + 2;

    while (*s == ' ' || *s == '\t')
        s++;
    *interpreter = s;
    while (*s && *s != ' ' && *s != '\t' && *s != '\n')
        s++;

    bool line_ended = *s == '\0' || *s == '\n';

    *s = '\0';
    if (!line_ended)
        s++;
    while (*s == ' ' || *s == '\t')
        s++;
    *argument = s;
    while (*s && *s != '\n')
        s++;
    while (s > *argument && (s[-1] == ' ' || s[-1] == '\t'))
        s--;
    *s = '\0';
    return **interpreter != '\0';
}

int process_exec(struct interrupt_frame *frame, const char *path, uint64_t argv, uint64_t envp) {
    uint64_t args_page = mm_alloc_kernel_page();
    if (args_page == 0)
        return -ENOMEM;

    struct exec_args *args = mm_phys_to_virt(args_page);
    struct vfs_node file;
    struct image image;
    uint64_t old_pml4;
    char line[SCRIPT_LINE_MAX];
    char *interpreter = NULL;
    char *argument;

    args->count = 0;
    args->used = 0;
    vfs_lock();

    int status = vfs_lookup(current->s->cwd, path, &file);
    if (status == VFS_OK && file.is_dir)
        status = -EISDIR;

    // A script runs as its interpreter, given the script's path where argv[0] was.
    int skip = 0;

    if (status == VFS_OK && read_interpreter(&file, line, sizeof(line), &interpreter, &argument)) {
        status = args_add(args, interpreter);
        if (status == VFS_OK && *argument)
            status = args_add(args, argument);
        if (status == VFS_OK)
            status = args_add(args, path);
        if (status == VFS_OK)
            status = vfs_lookup(current->s->cwd, interpreter, &file);
        if (status == VFS_OK && file.is_dir)
            status = -EISDIR;
        skip = 1;
    }
    if (status == VFS_OK)
        status = args_copy_from_user(args, argv, skip);
    args->argc = args->count;
    if (status == VFS_OK)
        status = args_copy_from_user(args, envp, 0);

    // From here the program is committed, as the other threads can't come back. They
    // need the old memory until they've gone, and may need the lock to get there.
    if (status == VFS_OK && current->s->threads > 1) {
        vfs_unlock();
        kill_other_threads();
        while (current->s->threads > 1)
            process_sleep_uninterruptible(&current->s->threads, 0);
        if (current->killed)
            finish_thread();
        vfs_lock();
    }

    old_pml4 = current->s->pml4;
    if (status == VFS_OK)
        status = load_image(&file, args, &image);

    vfs_unlock();
    if (status == 0)
        save_cmdline(current->s, args);
    mm_free_kernel_page(args_page);
    if (status != 0)
        return status;

    mm_destroy_address_space(old_pml4);
    vfs_normalize(current->s->cwd, interpreter ? interpreter : path, current->s->exe);
    current->thread_pointer = 0;
    cpu_set_thread_pointer(0);
    current->clear_on_exit = 0;
    current->s->heap_start = current->s->heap_end = image.heap;
    current->s->mmap_next = MMAP_TOP;
    *frame = user_frame(image.entry, image.stack);

    // The handlers were in the old program, though what it ignored stays ignored.
    for (int i = 0; i < NSIG; i++) {
        if (current->s->handlers[i] != SIG_IGN_VALUE)
            current->s->handlers[i] = SIG_DFL_VALUE;
        current->s->handler_masks[i] = 0;
        current->s->handler_flags[i] = 0;
    }

    // POSIX has the mask survive exec, which is how a parent can block signals for
    // the whole of a child's life.
    for (int fd = 0; fd < MAX_FDS; fd++) {
        if ((current->s->close_on_exec >> fd & 1) && current->s->files[fd]) {
            file_close(current->s->files[fd]);
            current->s->files[fd] = NULL;
        }
    }
    current->s->close_on_exec = 0;

    memcpy(current->fpu, fpu_initial, FPU_STATE_SIZE);
    fpu_restore(current->fpu);

    kprintf("process: pid %u exec %s, %lu pages free\n",
            current->pid, path, mm_free_page_count());
    return 0;
}

static bool wait_selects(const struct process *child, int pid) {
    if (pid == -1)
        return true;
    if (pid > 0)
        return child->pid == pid;
    return child->s->pgid == (pid == 0 ? current->s->pgid : -pid);
}

int process_wait(uint64_t status_ptr, int pid, int options) {
    if (status_ptr && !mm_user_range_ok(status_ptr, sizeof(int), true))
        return -EFAULT;

    for (;;) {
        bool has_children = false;

        for (struct process *p = processes; p; p = p->next) {
            if (p->s->parent != current->s || p->pid != p->s->pid || !wait_selects(p, pid))
                continue;

            has_children = true;
            if (p->s->threads == 0) {
                int found = p->pid;
                struct shared *child = p->s;

                if (status_ptr)
                    *(int *)status_ptr = child->exit_status;
                free_exited_threads(NULL);
                free_process(p);
                kfree(child);
                return found;
            }
        }

        if (!has_children)
            return -ECHILD;
        if (options & WNOHANG)
            return 0;

        if (!process_sleep(current->s))
            return -EINTR;
    }
}

int64_t process_mmap(uint64_t len, int prot, int flags, struct shm *shm, bool shm_writable, uint64_t offset) {
    bool shared = flags & MAP_SHARED;

    if (len == 0 || shared == !!(flags & MAP_PRIVATE) || (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)))
        return -EINVAL;
    if (prot == 0 || offset % PAGE_SIZE != 0 || len > MMAP_TOP - HEAP_LIMIT)
        return -EINVAL;
    if (shm && shared && (prot & PROT_WRITE) && !shm_writable)
        return -EACCES;

    uint64_t pages = page_align_up(len) / PAGE_SIZE;

    if (shm && (offset > shm_size(shm) || page_align_up(len) > page_align_up(shm_size(shm)) - offset))
        return -EINVAL;
    if (current->s->mmap_next - HEAP_LIMIT < pages * PAGE_SIZE)
        return -ENOMEM;

    // A private mapping of an object sees its contents until the first write.
    bool writable = prot & PROT_WRITE;
    bool copy_on_write = shm && !shared && writable;
    uint64_t flags_for_pages = PAGE_USER | (prot & PROT_EXEC ? 0 : PAGE_NO_EXEC)
        | (shared ? PAGE_SHARED : 0) | (copy_on_write ? PAGE_COPY_ON_WRITE : writable ? PAGE_WRITE : 0);
    uint64_t base = current->s->mmap_next - pages * PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        uint64_t phys = shm ? shm_page(shm, offset / PAGE_SIZE + i) : mm_alloc_page();
        bool mapped = phys != 0;

        if (mapped && shm)
            mm_ref_page(phys);
        if (mapped && !mm_map_page(base + i * PAGE_SIZE, phys, flags_for_pages)) {
            mm_free_page(phys);
            mapped = false;
        }

        if (!mapped) {
            for (uint64_t undo = 0; undo < i; undo++)
                mm_unmap_page(base + undo * PAGE_SIZE);
            return -ENOMEM;
        }
    }

    current->s->mmap_next = base;
    return base;
}

int64_t process_mmap_framebuffer(uint64_t len, int prot, int flags, uint64_t phys, uint64_t size, uint64_t offset) {
    if (len == 0 || !(flags & MAP_SHARED) || (flags & MAP_PRIVATE) || !(prot & PROT_READ) || (prot & PROT_EXEC))
        return -EINVAL;
    if (offset % PAGE_SIZE != 0 || offset > size || page_align_up(len) > size - offset)
        return -EINVAL;

    uint64_t pages = page_align_up(len) / PAGE_SIZE;

    if (current->s->mmap_next - HEAP_LIMIT < pages * PAGE_SIZE)
        return -ENOMEM;

    uint64_t page_flags = PAGE_USER | PAGE_NO_EXEC | PAGE_SHARED | PAGE_DEVICE | PAGE_WRITE_COMBINING
        | (prot & PROT_WRITE ? PAGE_WRITE : 0);
    uint64_t base = current->s->mmap_next - pages * PAGE_SIZE;

    for (uint64_t i = 0; i < pages; i++) {
        if (!mm_map_page(base + i * PAGE_SIZE, phys + offset + i * PAGE_SIZE, page_flags)) {
            for (uint64_t undo = 0; undo < i; undo++)
                mm_unmap_page(base + undo * PAGE_SIZE);
            return -ENOMEM;
        }
    }

    current->s->mmap_next = base;
    return base;
}

int process_munmap(uint64_t addr, uint64_t len) {
    if (addr % PAGE_SIZE != 0 || len == 0)
        return -EINVAL;

    // Only what mmap handed out. Unmapping nothing there is not an error.
    if (addr < HEAP_LIMIT || addr >= MMAP_TOP || page_align_up(len) > MMAP_TOP - addr)
        return -EINVAL;

    mm_unmap_range(addr, addr + page_align_up(len));

    // The most recent mapping's space can be used again, along with its page
    // tables. Anything else stays used until the program ends.
    if (addr == current->s->mmap_next)
        current->s->mmap_next += page_align_up(len);
    return 0;
}

uint64_t process_brk(uint64_t end) {
    if (end < current->s->heap_start || end > HEAP_LIMIT)
        return current->s->heap_end;

    uint64_t old_top = page_align_up(current->s->heap_end);
    uint64_t new_top = page_align_up(end);

    for (uint64_t page = old_top; page < new_top; page += PAGE_SIZE) {
        if (!map_new_page(page)) {
            for (uint64_t undo = old_top; undo < page; undo += PAGE_SIZE)
                mm_unmap_page(undo);
            return current->s->heap_end;
        }
    }
    mm_unmap_range(new_top, old_top);

    if (new_top != old_top)
        kprintf("process: pid %u heap %lx -> %lx, %lu pages free\n",
                current->pid, old_top, new_top, mm_free_page_count());

    current->s->heap_end = end;
    return current->s->heap_end;
}

int process_pid(void) {
    return current->s->pid;
}

int process_thread_id(void) {
    return current->pid;
}

const char *process_exe(void) {
    return current->s->exe;
}

void process_set_thread_pointer(uint64_t value) {
    current->thread_pointer = value;
    cpu_set_thread_pointer(value);
}

int process_thread_create(const struct interrupt_frame *frame, uint64_t stack, uint64_t thread_pointer,
                          uint64_t clear_on_exit) {
    struct process *t = new_process(current->s);

    if (!t)
        return -ENOMEM;

    t->pid = next_pid++;
    t->signals_masked = current->signals_masked;
    t->thread_pointer = thread_pointer;
    t->clear_on_exit = clear_on_exit;
    fpu_save(t->fpu);

    struct interrupt_frame t_frame = *frame;

    t_frame.rax = 0;
    t_frame.rsp = stack;
    prepare_stack(t, &t_frame);
    t->state = PROCESS_RUNNABLE;
    return t->pid;
}

int process_signal_thread(int thread_id, int signal) {
    if (signal < 0 || signal >= NSIG)
        return -EINVAL;

    for (struct process *p = processes; p; p = p->next) {
        if (p->s == current->s && p->pid == thread_id && p->state != PROCESS_ZOMBIE) {
            if (signal != 0)
                send_signal(p, signal);
            return 0;
        }
    }
    return -ESRCH;
}

void process_yield(void) {
    schedule();
}

int process_wakeup_some(void *channel, int count) {
    int woken = 0;

    for (struct process *p = processes; p && woken < count; p = p->next) {
        if (p->state == PROCESS_BLOCKED && p->channel == channel) {
            make_runnable(p);
            woken++;
        }
    }
    return woken;
}

void process_move_futex_waiters(uint64_t old_page, uint64_t new_page) {
    for (struct process *p = processes; p; p = p->next) {
        uint64_t channel = (uint64_t)p->channel;

        if (p->state == PROCESS_BLOCKED && p->s == current->s && channel >= old_page && channel < old_page + PAGE_SIZE)
            p->channel = (void *)(new_page + (channel - old_page));
    }
}

bool process_fault_in(uint64_t address) {
    uint64_t page = address & ~(uint64_t)(PAGE_SIZE - 1);

    if (page < STACK_BOTTOM || page >= STACK_TOP)
        return false;
    // Another thread may have touched it first.
    return mm_user_physical(page) || map_new_page(page);
}

void process_handle_kernel_fault(uint64_t address, bool present, bool write) {
    if ((present ? write && mm_handle_write_fault(address) : process_fault_in(address)))
        return;

    // Another thread unmapped what a system call was using, which is the program's
    // fault and shouldn't take the system with it.
    kprintf("process: pid %u passed the kernel a bad address %lx\n", current->pid, address);
    vfs_release_if_mine();
    process_exit(STATUS_SIGNAL_BASE + SIGSEGV);
}

int process_mprotect(uint64_t addr, uint64_t len, int prot) {
    if (addr % PAGE_SIZE || prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC))
        return -EINVAL;
    if (addr >= LOWER_HALF_END || len > LOWER_HALF_END - addr)
        return -ENOMEM;

    for (uint64_t page = addr; page < addr + page_align_up(len); page += PAGE_SIZE) {
        if (!mm_protect_page(page, prot & PROT_READ, prot & PROT_WRITE, prot & PROT_EXEC))
            return -ENOMEM;
    }
    cpu_flush_others();
    return 0;
}

void process_thread_exit(uint64_t unmap_address, uint64_t unmap_length) {
    if (unmap_length)
        process_munmap(unmap_address, unmap_length);
    finish_thread();
}

int process_next_pid(int after) {
    int best = 0;

    for (struct process *p = processes; p; p = p->next) {
        if (p->pid == p->s->pid && p->pid > after && (best == 0 || p->pid < best))
            best = p->pid;
    }
    return best;
}

bool process_get_info(int pid, struct process_info *out) {
    for (struct process *p = processes; p; p = p->next) {
        if (p->pid != pid || p->pid != p->s->pid)
            continue;

        struct shared *s = p->s;
        uint64_t ticks = 0;
        char state = s->threads == 0 ? 'Z' : 'S';

        for (struct process *t = processes; t; t = t->next) {
            if (t->s != s)
                continue;
            ticks += t->cpu_ticks;
            if (t->state == PROCESS_RUNNING || t->state == PROCESS_RUNNABLE)
                state = 'R';
        }

        *out = (struct process_info){
            .pid = pid,
            .parent = s->parent ? s->parent->pid : 0,
            .group = s->pgid,
            .threads = s->threads,
            .state = state,
            .cpu_ticks = ticks,
            .pages = s->threads ? mm_user_page_count(s->pml4) : 0,
            .exe = s->exe,
            .cmdline = s->cmdline,
            .cwd = s->cwd,
        };
        return true;
    }
    return false;
}

int process_parent_pid(void) {
    return current->s->parent ? current->s->parent->pid : 0;
}

int process_group_of(int pid) {
    for (struct process *p = processes; p; p = p->next) {
        if (pid == 0 ? p == current : p->pid == pid)
            return p->state == PROCESS_ZOMBIE ? -ESRCH : p->s->pgid;
    }
    return -ESRCH;
}

uint64_t *process_close_on_exec(void) {
    return &current->s->close_on_exec;
}

struct file **process_files(void) {
    return current->s->files;
}

char *process_cwd(void) {
    return current->s->cwd;
}

static void clear_tid_word(struct process *p) {
    uint64_t word = p->clear_on_exit;

    // The word may be gone, if the thread's memory went first.
    if (word && mm_user_range_ok(word, sizeof(uint32_t), true)) {
        *(volatile uint32_t *)word = 0;
        process_wakeup_some((void *)mm_user_physical(word), INT32_MAX);
    }
}

static void kill_other_threads(void) {
    for (struct process *p = processes; p; p = p->next) {
        if (p->s != current->s || p == current || p->state == PROCESS_ZOMBIE)
            continue;

        p->killed = true;
        send_signal(p, SIGKILL);
    }
}

static void finish_thread(void) {
    struct process *p = current;
    struct shared *s = p->s;

    if (!s->exiting)
        clear_tid_word(p);

    if (--s->threads > 0) {
        if (s->threads == 1)
            process_wakeup(&s->threads);
        p->state = PROCESS_ZOMBIE;
        schedule();
        die("exited thread was scheduled");
    }

    for (int fd = 0; fd < MAX_FDS; fd++) {
        if (s->files[fd])
            file_close(s->files[fd]);
        s->files[fd] = NULL;
    }

    // This thread keeps running on its kernel stack, which isn't part of the
    // address space, until it switches away for the last time.
    mm_switch_address_space(mm_kernel_address_space());
    mm_destroy_address_space(s->pml4);
    s->pml4 = mm_kernel_address_space();

    kprintf("process: pid %u exited with status %u, %lu pages free\n",
            s->pid, (unsigned)s->exit_status, mm_free_page_count());

    if (s == init->s) {
        kprintf("kernel: init exited, halting\n");
        cpu_stop_others();
        halt();
    }

    // Orphans become children of init, which can collect any that already exited.
    for (struct process *child = processes; child; child = child->next) {
        if (child->s->parent != s)
            continue;

        child->s->parent = init->s;
        if (child->s->threads == 0)
            process_wakeup(init->s);
    }

    p->state = PROCESS_ZOMBIE;
    process_wakeup(s->parent);
    send_to_process(s->parent, SIGCHLD);

    schedule();
    die("zombie process was scheduled");
}

void process_exit(int status) {
    struct shared *s = current->s;

    if (!s->exiting) {
        s->exiting = true;
        s->exit_status = status;
        kill_other_threads();
    }
    finish_thread();
}
