#pragma once

#define FE_INVALID 0x01
#define FE_DIVBYZERO 0x04
#define FE_OVERFLOW 0x08
#define FE_UNDERFLOW 0x10
#define FE_INEXACT 0x20
#define FE_ALL_EXCEPT 0x3D

#define FE_TONEAREST 0x000
#define FE_DOWNWARD 0x400
#define FE_UPWARD 0x800
#define FE_TOWARDZERO 0xC00

typedef struct {
    unsigned short control;
    unsigned mxcsr;
} fenv_t;

typedef int fexcept_t;

#define FE_DFL_ENV ((const fenv_t *)0)

int feclearexcept(int flags);
int feraiseexcept(int flags);
int fetestexcept(int flags);
int fegetexceptflag(fexcept_t *out, int flags);
int fesetexceptflag(const fexcept_t *saved, int flags);
int fegetround(void);
int fesetround(int mode);
int fegetenv(fenv_t *out);
int fesetenv(const fenv_t *env);
int feholdexcept(fenv_t *out);
int feupdateenv(const fenv_t *env);
