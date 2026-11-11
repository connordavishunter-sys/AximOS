#include <fenv.h>

#define ROUND_MASK FE_TOWARDZERO
#define SSE_ROUND_SHIFT 3
#define SSE_MASKS_SHIFT 7

static unsigned read_mxcsr(void) {
    unsigned value;

    __asm__ volatile("stmxcsr %0" : "=m"(value));
    return value;
}

static void write_mxcsr(unsigned value) {
    __asm__ volatile("ldmxcsr %0" : : "m"(value));
}

static unsigned short read_control(void) {
    unsigned short value;

    __asm__ volatile("fnstcw %0" : "=m"(value));
    return value;
}

static void write_control(unsigned short value) {
    __asm__ volatile("fldcw %0" : : "m"(value));
}

static unsigned short read_status(void) {
    unsigned short value;

    __asm__ volatile("fnstsw %0" : "=a"(value));
    return value;
}

int fetestexcept(int flags) {
    return (read_status() | read_mxcsr()) & flags & FE_ALL_EXCEPT;
}

int feclearexcept(int flags) {
    // The x87 flags can only be cleared together, so the others are put back in SSE.
    int others = read_status() & FE_ALL_EXCEPT & ~flags;

    __asm__ volatile("fnclex");
    write_mxcsr((read_mxcsr() | others) & ~(flags & FE_ALL_EXCEPT));
    return 0;
}

int feraiseexcept(int flags) {
    write_mxcsr(read_mxcsr() | (flags & FE_ALL_EXCEPT));
    return 0;
}

int fegetexceptflag(fexcept_t *out, int flags) {
    *out = fetestexcept(flags);
    return 0;
}

int fesetexceptflag(const fexcept_t *saved, int flags) {
    feclearexcept(flags);
    return feraiseexcept(*saved & flags);
}

int fegetround(void) {
    return read_control() & ROUND_MASK;
}

int fesetround(int mode) {
    if (mode & ~ROUND_MASK)
        return -1;
    write_control((read_control() & ~ROUND_MASK) | mode);
    write_mxcsr((read_mxcsr() & ~(ROUND_MASK << SSE_ROUND_SHIFT)) | mode << SSE_ROUND_SHIFT);
    return 0;
}

int fegetenv(fenv_t *out) {
    out->control = read_control();
    out->mxcsr = read_mxcsr() | (read_status() & FE_ALL_EXCEPT);
    return 0;
}

int fesetenv(const fenv_t *env) {
    __asm__ volatile("fnclex");
    if (env == FE_DFL_ENV) {
        __asm__ volatile("fninit");
        write_mxcsr(0x1F80);
        return 0;
    }
    write_control(env->control);
    write_mxcsr(env->mxcsr);
    return 0;
}

int feholdexcept(fenv_t *out) {
    fegetenv(out);
    return feclearexcept(FE_ALL_EXCEPT);
}

int feupdateenv(const fenv_t *env) {
    int raised = fetestexcept(FE_ALL_EXCEPT);

    fesetenv(env);
    return feraiseexcept(raised);
}
