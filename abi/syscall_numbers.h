#pragma once

// Shared by the kernel and libc. Invoked with the syscall instruction, the number
// in rax, arguments in rdi, rsi, rdx, r10, r8 and r9, and the result returned in
// rax. Failures return a negative errno. rcx and r11 are overwritten.

#define SYS_EXIT 0       // exit(status)
#define SYS_WRITE 1      // write(fd, buf, len): bytes written
#define SYS_BRK 2        // brk(end): the resulting end of the heap
#define SYS_FORK 3       // fork(): child pid in the parent, 0 in the child
#define SYS_EXEC 4       // exec(path, argv, envp): only returns on failure
#define SYS_WAIT 5       // wait(status, pid, options): pid of a child that exited
#define SYS_GETPID 6     // getpid(): pid of the caller
#define SYS_OPEN 7       // open(path, flags): file descriptor
#define SYS_CLOSE 8      // close(fd): 0
#define SYS_READ 9       // read(fd, buf, len): bytes read, 0 at end of file
#define SYS_CHDIR 10     // chdir(path): 0
#define SYS_GETCWD 11    // getcwd(buf, len): length of the path
#define SYS_READDIR 12   // readdir(fd, dirent): 1, or 0 after the last entry
#define SYS_STAT 13      // stat(path, stat): 0
#define SYS_STATFS 14    // statfs(path, statfs): 0
#define SYS_TERMSIZE 15  // termsize(rows, cols): 0
#define SYS_REBOOT 16    // reboot(command): only returns on failure
#define SYS_PIPE 17      // pipe(fds): 0, with the read end in fds[0]
#define SYS_DUP 18       // dup(fd): the lowest free descriptor
#define SYS_DUP2 19      // dup2(fd, target): target
#define SYS_SETPGID 20   // setpgid(pid, pgid): 0, with 0 meaning the caller's pid
#define SYS_LSEEK 22     // lseek(fd, offset, whence): the new offset
#define SYS_UNLINK 23    // unlink(path): 0
#define SYS_MKDIR 24     // mkdir(path): 0
#define SYS_RMDIR 25     // rmdir(path): 0
#define SYS_TIME 26      // time(): seconds since 1970 in UTC
#define SYS_RENAME 27    // rename(old, new): 0, replacing a file at new
#define SYS_KILL 28      // kill(pid, signal): 0
#define SYS_SIGACTION 29 // sigaction(number, action, previous): 0
#define SYS_SIGRETURN 30 // sigreturn(): only for returning from a handler
#define SYS_MEMINFO 31   // meminfo(info): 0
#define SYS_MMAP 32      // mmap(addr, len, prot, flags, fd, offset): the address
#define SYS_MUNMAP 33    // munmap(addr, len): 0
#define SYS_SHM_OPEN 34  // shm_open(name, flags): file descriptor
#define SYS_SHM_UNLINK 35 // shm_unlink(name): 0
#define SYS_FTRUNCATE 36 // ftruncate(fd, size): 0
#define SYS_NANOSLEEP 37 // nanosleep(request, remaining): 0
#define SYS_CLOCK_GETTIME 38 // clock_gettime(clock, timespec): 0
#define SYS_FB_INFO 39   // fb_info(fd, info): 0
#define SYS_FB_SET_MODE 40 // fb_set_mode(fd, width, height): 0
#define SYS_FB_DRAW 41   // fb_draw(fd, rect): 0
#define SYS_FSTAT 42     // fstat(fd, stat): 0
#define SYS_FCNTL 43     // fcntl(fd, command, value): depends on the command
#define SYS_IOCTL 44     // ioctl(fd, request, pointer): 0
#define SYS_POLL 45      // poll(fds, count, timeout in ms): how many have events
#define SYS_SIGPROCMASK 46 // sigprocmask(how, set, previous): 0
#define SYS_SIGPENDING 47 // sigpending(set): 0
#define SYS_SIGSUSPEND 48 // sigsuspend(mask): only ever fails with EINTR
#define SYS_ALARM 49     // alarm(seconds): the seconds left on the previous one
#define SYS_GETPPID 50   // getppid(): pid of the parent
#define SYS_GETPGID 51   // getpgid(pid): its process group, with 0 meaning the caller
#define SYS_THREAD_CREATE 52 // thread_create(stack, thread pointer, word to clear): thread id, 0 in the new thread
#define SYS_THREAD_EXIT 53 // thread_exit(address, length): unmaps the range and ends the thread
#define SYS_FUTEX 54     // futex(address, operation, value, timeout in ns or 0): depends on the operation
#define SYS_SET_THREAD_POINTER 55 // set_thread_pointer(address): 0
#define SYS_GETTID 56    // gettid(): id of the calling thread
#define SYS_TGKILL 57    // tgkill(thread, signal): 0
#define SYS_YIELD 58     // yield(): 0
#define SYS_EXE_PATH 59  // exe_path(buf, len): length of the running program's path
#define SYS_SOCKET 60    // socket(domain, type, protocol): file descriptor
#define SYS_SOCKETPAIR 61 // socketpair(domain, type, protocol, fds): 0
#define SYS_BIND 62      // bind(fd, address, length): 0
#define SYS_LISTEN 63    // listen(fd, backlog): 0
#define SYS_ACCEPT 64    // accept(fd, address, length, flags): file descriptor
#define SYS_CONNECT 65   // connect(fd, address, length): 0
#define SYS_SHUTDOWN 66  // shutdown(fd, how): 0
#define SYS_SOCKET_NAME 67 // socket_name(fd, address, length, peer): 0
#define SYS_SENDMSG 68   // sendmsg(fd, message, flags): bytes sent
#define SYS_RECVMSG 69   // recvmsg(fd, message, flags): bytes received
#define SYS_MPROTECT 70  // mprotect(addr, len, prot): 0
#define SYS_SOCKET_INFO 71 // socket_info(fd, SO_ERROR or SO_TYPE): its value
#define SYS_NET_INFO 72  // net_info(info): 0
#define SYS_NET_SET 73   // net_set(address, netmask, gateway, dns): 0
#define SYS_GETRANDOM 74 // getrandom(buf, len, flags): bytes written
#define SYS_SETSID 75    // setsid(): the new session and group, which is the caller's pid

#define O_RDONLY 0
#define O_WRONLY 1
#define O_RDWR 2
#define O_ACCMODE 3
#define O_CREAT 0x40
#define O_EXCL 0x80
#define O_NOCTTY 0x100 // has no effect, as opening never takes a controlling terminal
#define O_TRUNC 0x200
#define O_APPEND 0x400
#define O_NONBLOCK 0x800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC 0x80000

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1

#define WNOHANG 1

#define FUTEX_WAIT 0 // sleeps if the word equals value, failing with EAGAIN if not
#define FUTEX_WAKE 1 // wakes up to value sleepers, returning how many

#define PROT_NONE 0x0
#define PROT_READ 0x1
#define PROT_WRITE 0x2
#define PROT_EXEC 0x4

#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_ANONYMOUS 0x20

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define RB_AUTOBOOT 1
#define RB_POWER_OFF 2

// Exit statuses of processes the kernel ended, following the Unix convention
// of 128 plus a signal number.
#define STATUS_INTERRUPTED 130
#define STATUS_ILLEGAL 132
#define STATUS_ABORTED 134
#define STATUS_MATH 136
#define STATUS_KILLED 137
#define STATUS_CRASHED 139
#define STATUS_BROKEN_PIPE 141
#define STATUS_TERMINATED 143
