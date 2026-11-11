#pragma once

#include <interrupt.h>
#include <signal_numbers.h>
#include <stdint.h>
#include <vfs.h>

struct shm;

// What /proc says about a process. The strings stay valid until the kernel lock goes.
struct process_info {
    int pid;
    int parent;
    int group;
    int threads;
    char state;  // R for running or ready to, S for sleeping, Z for exited
    uint64_t cpu_ticks;
    uint64_t pages;
    const char *exe;
    const char *cmdline;
    const char *cwd;
};

// The lowest process ID above the one given, or 0 if there isn't one.
int process_next_pid(int after);

bool process_get_info(int pid, struct process_info *out);

#define MAX_FDS 64

// Unless a comment says otherwise, what returns an int fails with a negative errno.

// Creates the first process from an already resolved file.
int process_spawn_init(const struct vfs_node *node, const char *path);

// What each CPU runs when it has nothing else to do. It takes over from whatever
// called it, which must hold the kernel lock.
[[noreturn]] void process_idle_loop(void);

// Lets the next runnable process run. Returns when the caller is picked again.
void schedule(void);

// Sleeps until process_wakeup on the channel, which can be any address. Callers
// must recheck what they waited for. Returns false if a signal arrived instead,
// and then the system call should fail with EINTR.
bool process_sleep(void *channel);

// The same, but also gives up after the given number of ticks, or never if that
// is 0, returning true as though woken.
bool process_sleep_timeout(void *channel, uint64_t timeout_ticks);

// Called by the timer when it interrupts the running process in ring 3.
void process_charge_tick(void);

// The caller's processor time so far.
uint64_t process_cpu_ticks(void);

// Sleeps through signals too, for what must finish before the process can react
// to one. Gives up after the given number of ticks, or never if that is 0.
void process_sleep_uninterruptible(void *channel, uint64_t timeout_ticks);

// Wakes everyone sleeping on a channel. Safe to call from an interrupt.
void process_wakeup(void *channel);

// Called on every tick to wake sleepers whose timeout has passed.
void process_wake_expired(void);

// True once the scheduler is running, when sleeping is possible.
bool process_can_sleep(void);

// Moves a process, which must be the caller or its child, to a process group.
// 0 means the caller for pid, and the process's own pid for pgid.
int process_set_group(int pid, int pgid);

int process_foreground(void);

// Sets the group that Ctrl+C ends. 0 means none, leaving Ctrl+C an ordinary key.
void process_set_foreground(int pgid);

// Sends SIGINT to the foreground group. Safe to call from an interrupt. Returns
// false if the group has no live processes.
bool process_interrupt_foreground(void);

// Sends a signal to the calling process.
void process_signal_self(int signal);

// Sends a signal the way kill() does, where pid can also name a process group.
int process_kill(int pid, int signal);

// Sets what a signal does, as sigaction() does. Either pointer may be NULL, and
// both must be validated.
int process_sigaction(int signal, const struct sigaction *action, struct sigaction *previous);
int process_sigprocmask(int how, const sigset_t *set, sigset_t *previous);
sigset_t process_signals_pending(void);
int process_sigsuspend(sigset_t mask);

// Sends SIGALRM after this many seconds, or cancels it for 0. Returns the seconds
// that were left on the previous alarm.
uint64_t process_alarm(uint64_t seconds);

// Whether what ended a system call with EINTR asked for it to be run again.
bool process_signal_restarts(void);

// Restores what a signal handler interrupted. Call for SYS_SIGRETURN.
void process_signal_return(struct interrupt_frame *frame);

// Acts on pending signals. Call just before returning to ring 3.
void process_deliver_signals(struct interrupt_frame *frame);

int process_fork(const struct interrupt_frame *frame);
// pid selects the children as waitpid() does, and options can be WNOHANG.
int process_wait(uint64_t status_ptr, int pid, int options);

// Sleeps until process_wakeup on any channel, or a signal, as process_sleep_timeout.
bool process_sleep_any(uint64_t timeout_ticks);

// Replaces the program behind frame and returns 0, or fails and leaves the
// caller untouched. argv and envp are user pointers to NULL-terminated arrays.
int process_exec(struct interrupt_frame *frame, const char *path, uint64_t argv, uint64_t envp);

// Maps memory, or a shared memory object if shm isn't NULL, returning the address.
// shm_writable is whether the descriptor it came from allows writing.
int64_t process_mmap(uint64_t len, int prot, int flags, struct shm *shm, bool shm_writable, uint64_t offset);
// Maps part of the size bytes of screen memory at phys, always shared.
int64_t process_mmap_framebuffer(uint64_t len, int prot, int flags, uint64_t phys, uint64_t size, uint64_t offset);

int process_munmap(uint64_t addr, uint64_t len);
int process_mprotect(uint64_t addr, uint64_t len, int prot);

// Moves the end of the heap, returning the end actually in effect.
uint64_t process_brk(uint64_t end);

int process_pid(void);
int process_thread_id(void);

// The path of the running program.
const char *process_exe(void);

// Sets the FS base, which is what thread-local storage is found through.
void process_set_thread_pointer(uint64_t value);

// Starts a thread that resumes from frame on the given stack, with 0 as the system
// call's result. It zeroes clear_on_exit, if not 0, as it exits, and wakes any futex
// waiters there.
int process_thread_create(const struct interrupt_frame *frame, uint64_t stack, uint64_t thread_pointer,
                          uint64_t clear_on_exit);

// Ends just the caller, and when it's the last thread the process with status 0.
// Unmaps the given range first, which can be its own user stack.
[[noreturn]] void process_thread_exit(uint64_t unmap_address, uint64_t unmap_length);

int process_signal_thread(int thread_id, int signal);
void process_yield(void);

// Like process_wakeup, for at most count sleepers. Returns how many it woke.
int process_wakeup_some(void *channel, int count);

// A page of this process was replaced by its own copy, so threads sleeping on a futex
// in it need to follow, as futexes are named by physical address.
void process_move_futex_waiters(uint64_t old_page, uint64_t new_page);

// Gives a stack page memory the first time it's touched. Returns false if the
// address isn't in the stack or there's no memory.
bool process_fault_in(uint64_t address);

// A page fault in the kernel on a user address, as a thread may have changed the
// memory after it was checked. Fixes it or ends the process.
void process_handle_kernel_fault(uint64_t address, bool present, bool write);
int process_parent_pid(void);
int process_group_of(int pid);

// The descriptors to close on exec, with bit n for descriptor n.
uint64_t *process_close_on_exec(void);
struct file **process_files(void);

// The working directory, as a normalized absolute path of up to PATH_MAX bytes.
char *process_cwd(void);

[[noreturn]] void process_exit(int status);

// Turns a CPU exception in ring 3 into the matching signal.
void process_fault(uint64_t vector);
