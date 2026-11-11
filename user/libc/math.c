#include <string.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>

// Most of this leans on x87 instructions, which already do the hard parts to
// 80-bit accuracy. The trigonometric ones lose accuracy for very large inputs.

#define TWO_TO_52 4503599627370496.0

double fabs(double x) {
    return __builtin_fabs(x);
}

double sqrt(double x) {
    double result;

    __asm__ volatile("sqrtsd %1, %0" : "=x"(result) : "x"(x));
    return result;
}

double trunc(double x) {
    // Anything this large already has no fraction.
    if (!(fabs(x) < TWO_TO_52))
        return x;

    double result = (double)(long)x;

    return result == 0.0 ? __builtin_copysign(0.0, x) : result;
}

double floor(double x) {
    double result = trunc(x);

    return result > x ? result - 1.0 : result;
}

double ceil(double x) {
    double result = trunc(x);

    return result < x ? result + 1.0 : result;
}

double round(double x) {
    double result = trunc(x);

    if (fabs(x - result) >= 0.5)
        result += x < 0 ? -1.0 : 1.0;
    return result;
}

double fmod(double x, double y) {
    long double result = x;

    __asm__ volatile("1: fprem; fnstsw %%ax; testb $4, %%ah; jnz 1b" : "+t"(result) : "u"((long double)y) : "ax", "cc");
    return result;
}

// y * log2(x), which is what the x87 computes directly.
static long double y_log2_x(long double y, long double x) {
    long double result;

    __asm__ volatile("fyl2x" : "=t"(result) : "0"(x), "u"(y) : "st(1)");
    return result;
}

// 2 to the power of x.
static double two_to(long double x) {
    long double whole, fraction, result;

    __asm__ volatile("frndint" : "=t"(whole) : "0"(x));
    fraction = x - whole;
    __asm__ volatile("f2xm1" : "=t"(result) : "0"(fraction));
    result += 1.0L;
    __asm__ volatile("fscale" : "+t"(result) : "u"(whole));
    return result;
}

double log2(double x) {
    if (x < 0)
        return NAN;
    if (x == 0)
        return -INFINITY;
    if (isinf(x) || isnan(x))
        return x;
    return y_log2_x(1.0L, x);
}

double log(double x) {
    if (x < 0)
        return NAN;
    if (x == 0)
        return -INFINITY;
    if (isinf(x) || isnan(x))
        return x;

    long double ln2;

    __asm__ volatile("fldln2" : "=t"(ln2));
    return y_log2_x(ln2, x);
}

double log10(double x) {
    if (x < 0)
        return NAN;
    if (x == 0)
        return -INFINITY;
    if (isinf(x) || isnan(x))
        return x;

    long double lg2;

    __asm__ volatile("fldlg2" : "=t"(lg2));
    return y_log2_x(lg2, x);
}

double exp(double x) {
    if (isnan(x))
        return x;
    if (x > 710.0)
        return INFINITY;
    if (x < -746.0)
        return 0.0;

    long double log2e;

    __asm__ volatile("fldl2e" : "=t"(log2e));
    return two_to(x * log2e);
}

double pow(double x, double y) {
    if (y == 0.0 || x == 1.0)
        return 1.0;
    if (isnan(x) || isnan(y))
        return NAN;

    bool y_is_integer = trunc(y) == y;
    bool y_is_odd = y_is_integer && fmod(y, 2.0) != 0.0;

    if (isinf(y)) {
        double magnitude = fabs(x);

        if (magnitude == 1.0)
            return 1.0;
        return (magnitude > 1.0) == (y > 0) ? INFINITY : 0.0;
    }

    if (x == 0.0 || isinf(x)) {
        bool to_zero = (x == 0.0) == (y > 0);
        double result = to_zero ? 0.0 : INFINITY;

        return x < 0 || __builtin_signbit(x) ? (y_is_odd ? -result : result) : result;
    }

    if (x < 0 && !y_is_integer)
        return NAN;

    double result = two_to(y_log2_x(y, fabs(x)));

    return x < 0 && y_is_odd ? -result : result;
}

double sin(double x) {
    long double result;

    if (isinf(x))
        return NAN;
    __asm__ volatile("fsin" : "=t"(result) : "0"((long double)x));
    return result;
}

double cos(double x) {
    long double result;

    if (isinf(x))
        return NAN;
    __asm__ volatile("fcos" : "=t"(result) : "0"((long double)x));
    return result;
}

double tan(double x) {
    long double result, one;

    if (isinf(x))
        return NAN;
    __asm__ volatile("fptan" : "=t"(one), "=u"(result) : "0"((long double)x));
    return result;
}

double atan2(double y, double x) {
    long double result;

    __asm__ volatile("fpatan" : "=t"(result) : "0"((long double)x), "u"((long double)y) : "st(1)");
    return result;
}

double atan(double x) {
    return atan2(x, 1.0);
}

double asin(double x) {
    if (fabs(x) > 1.0)
        return NAN;
    return atan2(x, sqrt((1.0 - x) * (1.0 + x)));
}

double acos(double x) {
    if (fabs(x) > 1.0)
        return NAN;
    return atan2(sqrt((1.0 - x) * (1.0 + x)), x);
}

double sinh(double x) { return (exp(x) - exp(-x)) / 2; }
double cosh(double x) { return (exp(x) + exp(-x)) / 2; }

double tanh(double x) {
    // The exponentials overflow long before the answer stops being exactly 1.
    if (fabs(x) > 20)
        return __builtin_copysign(1.0, x);
    return sinh(x) / cosh(x);
}

double copysign(double x, double sign) { return __builtin_copysign(x, sign); }
double fmin(double a, double b) { return isnan(a) || b < a ? b : a; }
double fmax(double a, double b) { return isnan(a) || b > a ? b : a; }

// Rounds to the nearest integer, with ties to even as the rounding mode says.
double rint(double x) {
    if (!(fabs(x) < TWO_TO_52))
        return x;

    double result = __builtin_copysign(TWO_TO_52, x);

    result = x + result - result;
    return __builtin_copysign(result, x);
}

double nearbyint(double x) { return rint(x); }
long lround(double x) { return (long)round(x); }
long lrint(double x) { return (long)rint(x); }

double hypot(double x, double y) {
    x = fabs(x);
    y = fabs(y);

    double large = fmax(x, y), small = fmin(x, y);

    if (large == 0 || isinf(large))
        return large;
    small /= large;
    return large * sqrt(1 + small * small);
}

double cbrt(double x) {
    if (x == 0 || !isfinite(x))
        return x;

    double y = pow(fabs(x), 1.0 / 3.0);

    // One Newton step, which makes perfect cubes come out exact.
    y -= (y * y * y - fabs(x)) / (3 * y * y);
    return __builtin_copysign(y, x);
}

double modf(double x, double *whole) {
    *whole = trunc(x);
    return isinf(x) ? __builtin_copysign(0.0, x) : __builtin_copysign(x - *whole, x);
}

double ldexp(double x, int exponent) {
    long double result = x;

    __asm__ volatile("fscale" : "+t"(result) : "u"((long double)exponent));
    return result;
}

double frexp(double x, int *exponent) {
    *exponent = 0;
    if (x == 0 || !isfinite(x))
        return x;

    // Subnormals have no exponent to read, so scale them up to normal first.
    int scale = 0;

    if (fabs(x) < 2.2250738585072014e-308) {
        x *= 18014398509481984.0;
        scale = -54;
    }

    uint64_t bits;

    __builtin_memcpy(&bits, &x, sizeof(bits));
    *exponent = (int)(bits >> 52 & 0x7FF) - 1022 + scale;
    bits = (bits & ~(0x7FFULL << 52)) | (1022ULL << 52);
    __builtin_memcpy(&x, &bits, sizeof(bits));
    return x;
}

float fabsf(float x) { return __builtin_fabsf(x); }
float sqrtf(float x) { return sqrt(x); }
float floorf(float x) { return floor(x); }
float ceilf(float x) { return ceil(x); }
float truncf(float x) { return trunc(x); }
float roundf(float x) { return round(x); }
float fmodf(float x, float y) { return fmod(x, y); }
float expf(float x) { return exp(x); }
float logf(float x) { return log(x); }
float powf(float x, float y) { return pow(x, y); }
float sinf(float x) { return sin(x); }
float cosf(float x) { return cos(x); }
float tanf(float x) { return tan(x); }
float atan2f(float y, float x) { return atan2(y, x); }
float fminf(float a, float b) { return fmin(a, b); }
float fmaxf(float a, float b) { return fmax(a, b); }
float hypotf(float x, float y) { return hypot(x, y); }

double expm1(double x) {
    double u = exp(x);

    // Dividing by log(u) cancels the rounding error in u.
    if (u == 1.0)
        return x;
    if (u - 1.0 == -1.0 || isinf(u))
        return u - 1.0;
    return (u - 1.0) * x / log(u);
}

double log1p(double x) {
    double u = 1.0 + x;

    if (u == 1.0)
        return x;
    if (isinf(u))
        return u;
    return log(u) * x / (u - 1.0);
}

double exp2(double x) {
    return pow(2.0, x);
}

#define LARGE_FOR_HYPERBOLIC 1e150
#define LN2 0.693147180559945309417

double asinh(double x) {
    double a = fabs(x);
    double result = a > LARGE_FOR_HYPERBOLIC ? log(a) + LN2 : log1p(a + a * a / (1.0 + sqrt(1.0 + a * a)));

    return x == 0.0 ? x : copysign(result, x);
}

double acosh(double x) {
    if (x > LARGE_FOR_HYPERBOLIC)
        return log(x) + LN2;
    return x < 1.0 ? NAN : log1p(x - 1.0 + sqrt((x - 1.0) * (x + 1.0)));
}

double atanh(double x) {
    if (x == 0.0)
        return x;
    return copysign(0.5 * log1p(2.0 * fabs(x) / (1.0 - fabs(x))), x);
}

double nextafter(double x, double y) {
    if (isnan(x) || isnan(y))
        return x + y;
    if (x == y)
        return y;
    if (x == 0.0)
        return copysign(__DBL_DENORM_MIN__, y);

    uint64_t bits;

    memcpy(&bits, &x, sizeof(bits));
    bits += (x < y) == (x > 0.0) ? 1 : -1;
    memcpy(&x, &bits, sizeof(bits));
    return x;
}

double remquo(double x, double y, int *quotient) {
    if (isnan(x) || isnan(y) || isinf(x) || y == 0.0)
        return NAN;

    // Only the last bits of the quotient are returned, so going around at 8 keeps them.
    double a = fabs(y);
    double r = isinf(a) ? fabs(x) : fmod(fabs(x), 8.0 * a);
    int q = isinf(a) ? 0 : (int)(r / a);

    r -= q * a;
    if (r * 2.0 > a || (r * 2.0 == a && (q & 1))) {
        r -= a;
        q++;
    }

    *quotient = signbit(x) != signbit(y) ? -q : q;
    return signbit(x) ? -r : r;
}

double remainder(double x, double y) {
    int quotient;

    return remquo(x, y, &quotient);
}

double fma(double x, double y, double z) {
    return (long double)x * y + z;
}

double scalbn(double x, int exp) {
    return ldexp(x, exp);
}

double scalbln(double x, long exp) {
    return ldexp(x, exp > INT_MAX ? INT_MAX : exp < INT_MIN ? INT_MIN : exp);
}

int ilogb(double x) {
    int exp;

    if (x == 0.0 || isnan(x))
        return INT_MIN;
    if (isinf(x))
        return INT_MAX;
    frexp(x, &exp);
    return exp - 1;
}

double logb(double x) {
    if (x == 0.0)
        return -HUGE_VAL;
    if (isinf(x))
        return HUGE_VAL;
    return isnan(x) ? x : ilogb(x);
}

long long llrint(double x) {
    return lrint(x);
}

long long llround(double x) {
    return lround(x);
}

double fdim(double x, double y) {
    if (isnan(x) || isnan(y))
        return x + y;
    return x > y ? x - y : 0.0;
}

double nan(const char *tag) {
    (void)tag;
    return NAN;
}

// The Lanczos approximation, good to about 15 digits.
#define LANCZOS_G 7.0
static const double lanczos[] = {
    0.99999999999980993,  676.5203681218851,    -1259.1392167224028,   771.32342877765313,
    -176.61502916214059,  12.507343278686905,   -0.13857109526572012,  9.9843695780195716e-6,
    1.5056327351493116e-7,
};

#define SQRT_2PI 2.50662827463100050242

static double lanczos_sum(double x) {
    double sum = lanczos[0];

    for (int i = 1; i < 9; i++)
        sum += lanczos[i] / (x + i);
    return sum;
}

#define GAMMA_OVERFLOW 171.7
#define PI_VALUE 3.14159265358979323846

double lgamma(double x) {
    if (isnan(x) || isinf(x))
        return isnan(x) ? x : HUGE_VAL;
    if (x <= 0.0 && x == floor(x))
        return HUGE_VAL;
    if (x < 0.5)
        return log(PI_VALUE / fabs(sin(PI_VALUE * x))) - lgamma(1.0 - x);

    double y = x - 1.0;
    double t = y + LANCZOS_G + 0.5;

    return log(SQRT_2PI) + (y + 0.5) * log(t) - t + log(lanczos_sum(y));
}

double tgamma(double x) {
    if (isnan(x) || x == HUGE_VAL)
        return x;
    if (x == 0.0)
        return copysign(HUGE_VAL, x);
    if ((x < 0.0 && x == floor(x)) || x == -HUGE_VAL)
        return NAN;
    if (x >= GAMMA_OVERFLOW)
        return HUGE_VAL;
    if (x == floor(x) && x < 23.0) {
        double product = 1.0;

        for (int i = 2; i < x; i++)
            product *= i;
        return product;
    }
    if (x < 0.5)
        return PI_VALUE / (sin(PI_VALUE * x) * tgamma(1.0 - x));

    double y = x - 1.0;
    double t = y + LANCZOS_G + 0.5;

    // Split in two, so that the power can't overflow before the division brings it back.
    double half = pow(t, (y + 0.5) / 2.0);

    return SQRT_2PI * half / exp(t) * half * lanczos_sum(y);
}

#define ERF_SERIES_LIMIT 2.5
#define ERFC_ZERO_LIMIT 27.5
#define ERF_TERMS 200
#define ERFC_TERMS 200
#define TWO_OVER_SQRT_PI 1.12837916709551257390
#define SERIES_EPSILON 1e-17

// Sums without any cancellation, which keeps it accurate up to the limit.
static double erf_series(double x) {
    double term = x;
    double sum = x;

    for (int n = 1; n < ERF_TERMS && fabs(term) > SERIES_EPSILON * fabs(sum); n++) {
        term *= 2.0 * x * x / (2 * n + 1);
        sum += term;
    }
    return TWO_OVER_SQRT_PI * exp(-x * x) * sum;
}

// A continued fraction, evaluated from the far end. Needs x past the series limit.
static double erfc_fraction(double x) {
    double f = 0.0;

    for (int n = ERFC_TERMS; n > 0; n--)
        f = (n / 2.0) / (x + f);
    return exp(-x * x) / (x + f) * (TWO_OVER_SQRT_PI / 2.0);
}

double erf(double x) {
    if (isnan(x))
        return x;

    double a = fabs(x);
    double result = a < ERF_SERIES_LIMIT ? erf_series(a) : 1.0 - erfc_fraction(a);

    return x == 0.0 ? x : copysign(result, x);
}

double erfc(double x) {
    if (isnan(x))
        return x;
    if (x > ERFC_ZERO_LIMIT)
        return 0.0;
    if (x < 0.0)
        return 2.0 - erfc(-x);
    return x < ERF_SERIES_LIMIT ? 1.0 - erf_series(x) : erfc_fraction(x);
}

float log10f(float x) {
    return log10(x);
}

float log2f(float x) {
    return log2(x);
}

float ldexpf(float x, int exp) {
    return ldexp(x, exp);
}

float frexpf(float x, int *exp) {
    return frexp(x, exp);
}

float modff(float x, float *whole) {
    double w;
    double fraction = modf(x, &w);

    *whole = w;
    return fraction;
}
