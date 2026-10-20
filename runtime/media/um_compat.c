// The "finite math" entry points glibc dropped for new links in 2.31: Ubuntu 20.04's static
// libvorbis (the arm64 build) was compiled against an older glibc and still names them.
#include <math.h>

double __exp_finite(double x);
double __log_finite(double x);
double __pow_finite(double x, double y);
float __acosf_finite(float x);
double __exp_finite(double x) { return exp(x); }
double __log_finite(double x) { return log(x); }
double __pow_finite(double x, double y) { return pow(x, y); }
float __acosf_finite(float x) { return acosf(x); }
