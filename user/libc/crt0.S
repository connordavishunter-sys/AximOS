.text
.global _start
_start:
    xorl %ebp, %ebp
    movq (%rsp), %r12
    leaq 8(%rsp), %r13
    leaq 8(%r13,%r12,8), %r14
    movq %r14, environ(%rip)
    call __libc_start
    movq %r12, %rdi
    movq %r13, %rsi
    movq %r14, %rdx
    call main
    movl %eax, %edi
    call exit
