#pragma once

#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_E 2.7182818284590452354
#define M_SQRT2 1.41421356237309504880
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402

#define INFINITY __builtin_inf()
#define NAN __builtin_nan("")
#define HUGE_VAL __builtin_huge_val()

#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define isfinite(x) __builtin_isfinite(x)
#define signbit(x) __builtin_signbit(x)

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define fpclassify(x) __builtin_fpclassify(FP_NAN, FP_INFINITE, FP_NORMAL, FP_SUBNORMAL, FP_ZERO, x)
#define isnormal(x) __builtin_isnormal(x)

double expm1(double x);
double log1p(double x);
double exp2(double x);
double asinh(double x);
double acosh(double x);
double atanh(double x);
double nextafter(double x, double y);
double remainder(double x, double y);
double remquo(double x, double y, int *quotient);
double fma(double x, double y, double z); // rounds through long double, so can be off by a bit
double scalbn(double x, int exp);
double scalbln(double x, long exp);
int ilogb(double x);
double logb(double x);
long long llrint(double x);
long long llround(double x);
double fdim(double x, double y);
double nan(const char *tag);
double tgamma(double x);
double lgamma(double x);
double erf(double x);
double erfc(double x);
float log10f(float x);
float log2f(float x);
float ldexpf(float x, int exp);
float frexpf(float x, int *exp);
float modff(float x, float *whole);

double fabs(double x);
double sqrt(double x);
double floor(double x);
double ceil(double x);
double trunc(double x);
double round(double x);
double fmod(double x, double y);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double copysign(double x, double sign);
double fmin(double a, double b);
double fmax(double a, double b);
double rint(double x);
double nearbyint(double x);
long lround(double x);
long lrint(double x);
double hypot(double x, double y);
double cbrt(double x);
double modf(double x, double *whole);
double ldexp(double x, int exponent);
double frexp(double x, int *exponent);

float fabsf(float x);
float sqrtf(float x);
float floorf(float x);
float ceilf(float x);
float truncf(float x);
float roundf(float x);
float fmodf(float x, float y);
float expf(float x);
float logf(float x);
float powf(float x, float y);
float sinf(float x);
float cosf(float x);
float tanf(float x);
float atan2f(float y, float x);
float fminf(float a, float b);
float fmaxf(float a, float b);
float hypotf(float x, float y);
